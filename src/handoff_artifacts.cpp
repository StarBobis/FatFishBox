#include "handoff_artifacts.h"
#include "dsh.h"
#include "state_file.h"
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <limits>

namespace fatfish {
namespace {
constexpr std::size_t report_limit=32*1024*1024;
fs::path artifact_path(const fs::path& data,const std::string& queue,const std::string& session,const std::string& version) {
    return data/L"results"/path_from_utf8(queue)/path_from_utf8(sha256(session+"\n"+version)+".json");
}
bool sha(const Json& value) {
    if (value.is_null()) return true;
    if (!value.is_string()) return false;
    const auto& text=value.get_ref<const std::string&>();
    return (text.size()==40 || text.size()==64) && text.find_first_not_of("0123456789abcdefABCDEF")==text.npos;
}
Json delivery_from(Json& messages) {
    for (auto it=messages.rbegin();it!=messages.rend();++it) {
        if (it->value("type","")!="assistant/message" && it->value("type","")!="assistant/response") continue;
        auto text=it->value("text","");
        constexpr std::string_view fence="```fatfish-delivery";
        const auto begin=text.rfind(fence);
        if (begin==text.npos) continue;
        const auto start=begin+fence.size(),end=text.find("```",start);
        if (end==text.npos || end-start>64000) continue;
        auto value=Json::parse(std::string_view(text).substr(start,end-start),nullptr,false);
        if (!value.is_object() || !value.contains("candidateSha") || !sha(value.at("candidateSha"))) continue;
        if (value.contains("baseSha") && !sha(value.at("baseSha"))) continue;
        if (value.contains("worktreeClean") && !value.at("worktreeClean").is_boolean()) continue;
        const auto tests=value.value("tests",Json::array()),paths=value.value("evidencePaths",Json::array());
        if (!tests.is_array() || tests.size()>64 || !paths.is_array() || paths.size()>64) continue;
        bool valid=true;
        for (const auto& test:tests) valid=valid && test.is_object() && test.contains("command") &&
            test.at("command").is_string() && test.at("command").get_ref<const std::string&>().size()<=2000 &&
            test.contains("exitCode") && test.at("exitCode").is_number_integer();
        for (const auto& test:tests) if (test.contains("logPath"))
            valid=valid && test.at("logPath").is_string() && test.at("logPath").get_ref<const std::string&>().size()<=4000 &&
                path_from_utf8(test.at("logPath").get_ref<const std::string&>()).is_absolute();
        for (const auto& path:paths) valid=valid && path.is_string() && path.get_ref<const std::string&>().size()<=4000 &&
            path_from_utf8(path.get_ref<const std::string&>()).is_absolute();
        if (!valid) continue;
        const auto status=value.value("status",Json("reported"));
        if (!status.is_string() || (status!="reported" && status!="passed" && status!="failed" && status!="blocked")) continue;
        const auto issues=value.value("issues",Json::array());
        if (!issues.is_array() || issues.size()>64) continue;
        Json result{{"candidateSha",value.at("candidateSha")},{"baseSha",value.value("baseSha",Json())},
            {"status",status},{"issues",issues},{"tests",tests},{"evidencePaths",paths},{"source","agent_reported"},
            {"checksManifest",nullptr},{"evidenceManifest",nullptr},{"nextAction","inspect_result"}};
        for (const auto* key:{"worktreeClean","checksManifest","evidenceManifest","nextAction"})
            if (value.contains(key)) result[key]=value.at(key);
        text.erase(begin,end+3-begin);
        (*it)["text"]=std::move(text);
        return result;
    }
    return Json();
}
std::string prefix(std::string_view text,std::size_t max) {
    auto end=std::min(text.size(),max);
    while (end<text.size() && end && (static_cast<unsigned char>(text[end])&0xc0)==0x80) --end;
    return std::string(text.substr(0,end));
}
}
std::string sha256(std::string_view bytes) {
    std::array<unsigned char,32> digest{};
    if (bytes.size()>std::numeric_limits<ULONG>::max() || BCryptHash(BCRYPT_SHA256_ALG_HANDLE,nullptr,0,
        reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())),static_cast<ULONG>(bytes.size()),digest.data(),
        static_cast<ULONG>(digest.size()))<0) throw std::runtime_error("无法计算 SHA256");
    constexpr char hex[]="0123456789abcdef";
    std::string result;
    for (const auto value:digest) {result+=hex[value>>4];result+=hex[value&15];}
    return result;
}
Json load_result_artifact(const fs::path& data,const std::string& queue,const std::string& session,const std::string& version) {
    if (version.empty()) return Json();
    const auto file=artifact_path(data,queue,session,version);
    if (!fs::exists(file)) return Json();
    const auto result=read_json(file,report_limit);
    if (result.value("sessionId","")!=session || result.value("resultVersion","")!=version || result.value("queueId","")!=queue)
        throw std::runtime_error("交付归档身份不匹配");
    if (!result.value("archiveComplete",true)) return Json();
    return result;
}
Json save_result_artifact(const fs::path& data,const std::string& queue,const Json& row,Json report) {
    const auto session=row.at("id").get<std::string>(),version=report.value("resultVersion","");
    if (version.empty()) return report;
    const auto file=artifact_path(data,queue,session,version);
    fs::create_directories(file.parent_path());
    const ExclusiveStateLock lock(file.wstring()+L".lock");
    if (auto existing=load_result_artifact(data,queue,session,version);!existing.is_null()) return existing;
    auto delivery=delivery_from(report.at("messages"));
    const auto work=row.value("workItemId",session);
    if (delivery.is_null()) delivery={{"status",report.contains("failure")?"failed":"missing"},
        {"candidateSha",nullptr},{"baseSha",row.value("baseSha",Json())},
        {"issues",Json::array({{{"code","delivery_missing"},{"message","未找到有效结构化交付，请按需读取原始结果"}}})},
        {"source","unavailable"},{"nextAction",report.contains("failure")?"redispatch":"inspect_result"}};
    delivery["workItemId"]=work;
    if (delivery.value("baseSha",Json()).is_null() && row.contains("baseSha")) {
        delivery["baseSha"]=row.at("baseSha");delivery["baseShaSource"]="dispatch_record";
    }
    if (!delivery.contains("checksManifest")) delivery["checksManifest"]=nullptr;
    if (!delivery.contains("evidenceManifest")) delivery["evidenceManifest"]=nullptr;
    if (!delivery.contains("nextAction")) delivery["nextAction"]="inspect_result";
    if (report.contains("failure") && row.value("status","")=="failed" && delivery.value("status","")!="failed") {
        delivery["reportedStatus"]=delivery.at("status");delivery["status"]="failed";
        delivery["reportedNextAction"]=delivery.at("nextAction");delivery["nextAction"]="inspect_failure";
    }
    report["delivery"]=delivery;
    report["session"]=row;
    report["queueId"]=queue;
    report["workItemId"]=work;
    report["logPath"]=utf8(file);
    report["savedAt"]=now();
    report["archiveComplete"]=!report.contains("readError") && !report.value("truncated",false);
    write_state_file(file,report);
    return report;
}
Json handoff_summary(const Json& artifact) {
    Json result{{"delivery",artifact.at("delivery")},{"result",Json::array()},{"logPath",artifact.at("logPath")},
        {"workItemId",artifact.at("workItemId")},{"resultTruncated",artifact.value("truncated",false)}};
    // Keep a short final assistant excerpt for existing clients. Never include prior user messages.
    const auto& messages=artifact.at("messages");
    for (auto it=messages.rbegin();it!=messages.rend();++it) {
        const auto type=it->value("type","");
        if (type!="assistant/message" && type!="assistant/response" && type!="dispatch/error") continue;
        const auto text=it->value("text","");
        const auto summary=prefix(text,600);
        result["result"].push_back({{"type",type},{"text",summary},{"truncated",summary.size()<text.size()}});
        result["resultTruncated"]=result.at("resultTruncated").get<bool>() || summary.size()<text.size() || messages.size()>1;
        break;
    }
    // Unusually large reports stay on disk rather than expanding every claim/re-read.
    if (result.at("delivery").dump().size()>1800) {
        const auto full=result.at("delivery");
        Json compact=Json::object();
        for (const auto* key:{"workItemId","candidateSha","baseSha","status","source","worktreeClean"})
            if (full.contains(key)) compact[key]=full.at(key);
        for (const auto* key:{"checksManifest","evidenceManifest"})
            compact[key]=full.contains(key) && full.at(key).is_string() && full.at(key).get_ref<const std::string&>().size()<=240?full.at(key):Json();
        compact["nextAction"]=full.contains("nextAction") && full.at("nextAction").is_string()?Json(prefix(full.at("nextAction").get_ref<const std::string&>(),240)):Json("inspect_result");
        compact["issues"]=Json::array();
        if (full.contains("issues") && full.at("issues").is_array()) for (const auto& issue:full.at("issues")) {
            if (compact.at("issues").size()>=2) break;
            if (issue.is_string()) compact["issues"].push_back(prefix(issue.get_ref<const std::string&>(),300));
            else if (issue.is_object()) {
                Json preview=Json::object();
                for (const auto* member:{"issueId","code","title","message","issue"})
                    if (issue.contains(member) && issue.at(member).is_string()) preview[member]=prefix(issue.at(member).get_ref<const std::string&>(),200);
                if (!preview.empty()) compact["issues"].push_back(std::move(preview));
            }
        }
        for (const auto* key:{"tests","evidencePaths","issues"})
            if (full.contains(key) && full.at(key).is_array()) compact[std::string(key)+"Count"]=full.at(key).size();
        compact["detailsTruncated"]=true;
        result["delivery"]=std::move(compact);
    }
    for (const auto* key:{"failure","resultSource","readError","evidence"}) if (artifact.contains(key)) result[key]=artifact.at(key);
    if (result.contains("failure") && result.at("failure").dump().size()>1200) {
        const auto failure=result.at("failure");
        result["failure"]=Json::object();
        for (const auto* key:{"kind","code","stage","message"})
            if (failure.contains(key) && failure.at(key).is_string())
                result["failure"][key]=prefix(failure.at(key).get_ref<const std::string&>(),480);
        result["failure"]["detailsTruncated"]=true;
    }
    if (result.contains("evidence")) {
        result["evidence"].erase("checksManifest");
        result["evidence"].erase("evidenceManifest");
        if (result["evidence"].contains("error") && result["evidence"].at("error").is_string())
            result["evidence"]["error"]=prefix(result["evidence"].at("error").get_ref<const std::string&>(),480);
    }
    return result;
}
Json result_page(const Json& artifact,const Json& input) {
    const auto offset=input.value("offset",std::int64_t(0));
    const auto limit=input.value("limit",6000);
    if (offset<0 || limit<4 || limit>24000) throw std::runtime_error("offset 须非负，limit 须为 4 至 24000 字节");
    std::string text;
    for (const auto& message:artifact.at("messages")) {
        if (!text.empty()) text+="\n";
        text+=message.value("text","");
    }
    if (static_cast<std::uint64_t>(offset)>text.size() || (static_cast<std::size_t>(offset)<text.size() &&
        (static_cast<unsigned char>(text[static_cast<std::size_t>(offset)])&0xc0)==0x80))
        throw std::runtime_error("offset 超出日志或不是 UTF-8 字符边界，请使用 nextOffset");
    const auto page=prefix(std::string_view(text).substr(static_cast<std::size_t>(offset)),static_cast<std::size_t>(limit));
    const auto next=static_cast<std::size_t>(offset)+page.size();
    Json result=artifact;
    result["messages"]=Json::array({{{"type","assistant/message"},{"text",page},{"truncated",next<text.size()}}});
    result["offset"]=offset;
    result["nextOffset"]=next<text.size()?Json(next):Json();
    result["hasMore"]=next<text.size();
    return result;
}
}
