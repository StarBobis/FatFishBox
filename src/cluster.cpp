#include "diagnostics.h"
#include "cluster.h"
#include "dsh.h"
#include "goal_projection.h"
#include "dsh_history.h"
#include "legacy.h"
#include "state_file.h"
#include "handoff_artifacts.h"
#include "event_journal.h"
#include "work_items.h"
#include "evidence.h"
#include "team.h"
#include <windows.h>
#include <objbase.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <ctime>
#include <fstream>
#include <mutex>
#include <limits>
#include <optional>
#include <ranges>
#include <set>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <vector>

namespace fatfish {
struct HandoffObservations {
    std::shared_ptr<LegacyObservations> queue(const std::string& id) {
        const std::lock_guard guard(lock);
        auto& value=queues[id];
        if (!value) value=std::make_shared<LegacyObservations>();
        return value;
    }
private:
    std::mutex lock;
    std::map<std::string,std::shared_ptr<LegacyObservations>> queues;
};
std::wstring widen(std::string_view value) {
    if (value.empty()) return {};
    const auto count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), int(value.size()), nullptr, 0);
    if (!count) throw std::runtime_error("UTF-8 输入无效");
    std::wstring output(count, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), int(value.size()), output.data(), count);
    return output;
}
std::string narrow(std::wstring_view value) {
    if (value.empty()) return {};
    const auto count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), int(value.size()), nullptr, 0, nullptr, nullptr);
    if (!count) throw std::runtime_error("Unicode 输入无效");
    std::string output(count, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), int(value.size()), output.data(), count, nullptr, nullptr);
    return output;
}
std::string utf8(const fs::path& path) { return narrow(path.native()); }
std::string id() {
    GUID guid;
    if (CoCreateGuid(&guid) < 0) throw std::runtime_error("无法生成操作标识");
    wchar_t text[40]{};
    StringFromGUID2(guid, text, 40);
    return narrow(std::wstring_view(text + 1, 36));
}
std::int64_t now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

namespace {
void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
std::wstring environment(const wchar_t* name) {
    auto size = GetEnvironmentVariableW(name, nullptr, 0);
    if (!size) return {};
    std::wstring value(size, L'\0');
    value.resize(GetEnvironmentVariableW(name, value.data(), size));
    return value;
}
const std::set<std::string> roles{"developer","reviewer","repair","support","lead","integrator"};
bool active(const Json& row) {
    const auto state = row.value("status", "");
    return state != "succeeded" && state != "failed" && state != "cancelled" && state != "interrupted";
}
Json settings_template() {
    return {{"revision",0},{"configPath",""},{"authPath",""},{"permission","danger-full-access"},{"preset","ptc"},
        {"accounts",Json::object()},{"pools",Json::object()}};
}
std::string dsh_profile_id(const Json& settings) {
    return settings.value("dshProfile","default");
}
Json dsh_profile_fields(const Json& settings) {
    return {{"name",settings.value("dshName","")},{"configPath",settings.value("configPath","")},
        {"authPath",settings.value("authPath","")},{"automaticAuth",settings.value("automaticAuth",false)},
        {"permission",settings.value("permission","")},{"preset",settings.value("preset","")}};
}
Json dsh_profile_settings(const Json& state,const std::string& profile) {
    const auto& current=state.at("settings");
    if (profile.empty() || profile==dsh_profile_id(current)) return current;
    Json settings=settings_template();
    if (profile!="new") {
        require(state.contains("dshProfiles") && state.at("dshProfiles").contains(profile),"DSH 连接配置不存在");
        settings=state.at("dshProfiles").at(profile);
    }
    settings["revision"]=current.at("revision");
    settings["dshProfile"]=profile=="new"?id():profile;
    return settings;
}
void save_dsh_profile(Json& state,Json settings) {
    const auto& previous=state.at("settings");
    const auto old_id=dsh_profile_id(previous), new_id=dsh_profile_id(settings);
    if (old_id!=new_id) {
        auto& profiles=state["dshProfiles"];
        if (profiles.is_null()) profiles=Json::object();
        profiles[old_id]=previous;
        profiles.erase(new_id);
    }
    settings["revision"]=previous.at("revision").get<int>()+1;
    state["settings"]=std::move(settings);
}
Json dsh_profiles(const Json& state) {
    const auto& current=state.at("settings");
    auto result=Json::object();
    result[dsh_profile_id(current)]=dsh_profile_fields(current);
    if (state.contains("dshProfiles"))
        for (const auto& [key,settings]:state.at("dshProfiles").items()) result[key]=dsh_profile_fields(settings);
    return result;
}
Json legacy_connection_settings(const Json& state) {
    auto settings=state.at("settings");
    settings["dshConnections"]=dsh_profiles(state);
    return settings;
}
fs::path dispatch_auth(const Json& settings) {
    return settings.value("automaticAuth",false)?discover(settings,Json::object()).auth:
        path_from_utf8(settings.at("authPath").get<std::string>());
}
class Database {
public:
    Database(const fs::path& folder, bool writable) : folder_(folder), writable_(writable) {
        lock_ = CreateFileW((folder / L"state.lock").c_str(), GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (lock_ == INVALID_HANDLE_VALUE) throw std::runtime_error("无法打开本地状态锁");
        if (!LockFileEx(lock_, writable ? LOCKFILE_EXCLUSIVE_LOCK : 0, 0, 1, 0, &range_)) {
            CloseHandle(lock_);
            throw std::runtime_error("无法锁定本地状态");
        }
        try {
            const auto file = folder / L"state.json";
            state = fs::exists(file) ? read_json(file, 64 * 1024 * 1024)
                : Json{{"settings",settings_template()},{"sessions",Json::object()}};
            auto& settings=state.at("settings");
            const auto defaults=settings_template();
            for (const auto& [key,value]:defaults.items())
                if (!settings.contains(key)) settings[key]=value;
            previous_sessions_=state.at("sessions");
            settings.erase("balance");
            for (auto& row:settings.at("accounts")) {
                row.erase("roles");
                row.erase("enabled");
                row.erase("concurrency");
            }
            for (auto& row:settings.at("pools")) {
                row.erase("roles");
                row.erase("priority");
                row.erase("weight");
            }
        } catch (...) {
            UnlockFileEx(lock_, 0, 1, 0, &range_);
            CloseHandle(lock_);
            throw;
        }
    }
    Database(const Database&) = delete;
    ~Database() {
        UnlockFileEx(lock_, 0, 1, 0, &range_);
        CloseHandle(lock_);
    }
    void save() {
        require(writable_, "本地状态锁没有写权限");
        synchronize_work_items(state);
        record_session_changes(state,previous_sessions_);
        const auto temp = folder_ / path_from_utf8("state-" + id() + ".tmp");
        try {
            std::ofstream output(temp, std::ios::binary | std::ios::trunc);
            const auto data = state.dump();
            output.write(data.data(), std::streamsize(data.size()));
            output.close();
            require(bool(output), "本地状态写入失败");
            require(MoveFileExW(temp.c_str(), (folder_ / L"state.json").c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH), "无法提交本地状态");
            previous_sessions_=state.at("sessions");
        } catch (...) {
            std::error_code ignored;
            fs::remove(temp, ignored);
            throw;
        }
    }
    Json state;
private:
    fs::path folder_;
    Json previous_sessions_;
    bool writable_;
    HANDLE lock_{};
    OVERLAPPED range_{};
};
bool queue_scoped_tool(std::string_view tool) {
    return tool=="cluster_overview" || tool=="cluster_get_session_status" || tool=="cluster_get_session_result" ||
        tool=="cluster_review_issues" || tool=="cluster_retry_review" || tool=="cluster_start_session" ||
        tool=="cluster_next_handoff" || tool=="cluster_resume_handoff" || tool=="cluster_wait_handoff" || tool=="cluster_repair_handoff" || tool=="cluster_attach_handoff_repair" ||
        tool=="cluster_detach_handoff_repair" || tool=="cluster_refresh_sessions" || tool=="cluster_cancel_session" || tool=="cluster_create_work_item" ||
        tool=="cluster_get_work_item" || tool=="cluster_list_work_items" || tool=="cluster_update_work_item" ||
        tool=="cluster_send_message" || tool=="cluster_get_message" || tool=="cluster_capture_evidence" || tool=="cluster_get_evidence_manifest";
}
bool in_queue(const Json& row,std::string_view queue,const std::string& legacy) {
    return row.value("queueId",legacy)==queue;
}
Json queue_sessions(const Json& state,std::string_view queue,const std::string& legacy) {
    Json result=Json::object();
    for (const auto& [key,row]:state.at("sessions").items()) if (in_queue(row,queue,legacy)) result[key]=row;
    return result;
}
LegacyFilter legacy_filter(const Json& state,const std::string& queue,const std::string& legacy) {
    std::set<std::string,std::less<>> protected_ids;
    for (const auto& row:state.at("sessions")) {
        const auto sid=row.value("dshId","");
        if (!sid.empty() && (row.contains("precheck") || row.contains("precheckFor"))) protected_ids.insert(sid);
    }
    return [assignments=state.value("legacyQueues",Json::object()),protected_ids=std::move(protected_ids),queue,legacy](std::string_view id) {
        if (id.starts_with("codex:")) {
            const auto sid=id.substr(6);
            if (protected_ids.contains(sid.substr(0,sid.find('@')))) return false;
        }
        return assignments.value(std::string(id),legacy)==queue;
    };
}
Json queue_info(Json row) {
    row.erase("requestId");
    row.erase("initialName");
    row.erase("initialWorkspace");
    row.erase("batchPools");
    auto& workspaces=row.at("workspaces").get_ref<Json::array_t&>();
    row["workspaceCount"]=workspaces.size();
    if (workspaces.size()>12) workspaces.resize(12);
    return row;
}
bool open_queue(const Json& state,const std::string& key) {
    const auto& queues=state.at("queues");
    return queues.contains(key) && !queues.at(key).contains("deletedAt");
}
bool same_workspace(const std::string& left,const std::string& right) {
    return _wcsicmp(path_from_utf8(left).lexically_normal().c_str(),path_from_utf8(right).lexically_normal().c_str())==0;
}
void remember_workspace(Json& queue,const std::string& path) {
    auto& paths=queue.at("workspaces");
    if (!path.empty() && std::ranges::none_of(paths,[&](const Json& item){
        return same_workspace(item.get_ref<const std::string&>(),path);
    })) paths.push_back(path);
}
std::string field(const Json& source, std::string_view key, std::size_t max) {
    const auto value = source.at(key).get<std::string>();
    require(!value.empty() && value.size() <= max && value.find('\0') == std::string::npos, "缺少字段或长度无效");
    return value;
}
bool commit_sha(std::string_view sha) {
    return (sha.size()==40 || sha.size()==64) && std::ranges::all_of(sha,[](unsigned char ch) {
        return (ch>='0' && ch<='9') || (ch>='a' && ch<='f') || (ch>='A' && ch<='F');
    });
}
bool evidence_paths(const Json& paths) {
    return paths.is_array() && paths.size()<=16 && std::ranges::all_of(paths,[](const Json& path) {
        if (!path.is_string()) return false;
        const auto& value=path.get_ref<const std::string&>();
        return !value.empty() && value.size()<=1000 && value.find('\0')==std::string::npos &&
            path_from_utf8(value).is_absolute();
    });
}
Json extract_delivery(Json& messages) {
    for (auto it=messages.rbegin();it!=messages.rend();++it) {
        const auto kind=it->value("type","");
        if (kind!="assistant/message" && kind!="assistant/response") continue;
        auto output=it->value("text","");
        constexpr std::string_view fence="```fatfish-delivery";
        const auto begin=output.rfind(fence);
        if (begin==std::string::npos) continue;
        const auto after=begin+fence.size();
        const auto end=output.find("```",after);
        if (end==std::string::npos || end-after>6000) continue;
        const auto parsed=Json::parse(std::string_view(output).substr(after,end-after),nullptr,false);
        if (!parsed.is_object()) continue;
        const auto sha=parsed.value("candidateSha",Json());
        const auto clean=parsed.value("worktreeClean",Json());
        const auto tests=parsed.value("tests",Json());
        const auto paths=parsed.value("evidencePaths",Json());
        if (!parsed.contains("candidateSha") || (!sha.is_null() && (!sha.is_string() || !commit_sha(sha.get_ref<const std::string&>()))) ||
            !clean.is_boolean() || !tests.is_array() || tests.size()>16 || !evidence_paths(paths)) continue;
        Json checks=Json::array();
        for (const auto& test:tests) {
            if (!test.is_object()) break;
            const auto command=test.value("command",Json()),code=test.value("exitCode",Json());
            if (!command.is_string() || command.get_ref<const std::string&>().empty() ||
                command.get_ref<const std::string&>().size()>600 || !code.is_number_integer()) break;
            checks.push_back({{"command",command},{"exitCode",code}});
        }
        if (tests.size()!=checks.size()) continue;
        output.erase(begin,end+3-begin);
        (*it)["text"]=std::move(output);
        return {{"candidateSha",sha},{"worktreeClean",clean},{"tests",checks},{"evidencePaths",paths},{"source","agent_reported"}};
    }
    return nullptr;
}
void validate_configuration(const Json& settings) {
    for (const auto name : {"configPath","authPath"}) {
        auto path = settings.at(name).get<std::string>();
        require(path.empty() || path_from_utf8(path).is_absolute(), "配置文件必须使用绝对路径");
    }
    require(settings.value("dshName","").size()<=240,"DSH 连接名称过长");
    auto permission = settings.at("permission").get<std::string>();
    require(permission.size() <= 80 && std::all_of(permission.begin(), permission.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    }), "权限预设只能包含字母、数字、连字符与下划线");
    auto limits = [](const Json& row) {
        require(row.at("enabled").is_boolean(), "启用字段必须为布尔值");
        require(row.at("concurrency").is_number_integer(), "并发必须为整数");
        const auto cap = row.at("concurrency").get<int>();
        require(cap >= 1 && cap <= 1000, "总并发必须在 1 至 1000 之间");
    };
    for (const auto& pool : settings.at("pools")) {
        limits(pool);
        require(settings.at("accounts").contains(pool.at("provider").get<std::string>()), "模型池的账号无效");
        require(pool.at("effort").is_string(), "思考强度必须为字符串");
    }
}
void set_model_field(Json& pool,const std::string& field,const Json& value) {
    require(field=="enabled" || field=="concurrency" || field=="effort","未知模型设置");
    if (field!="enabled") require(pool.at("enabled").get<bool>(),"请先启用模型");
    if (field=="effort") {
        const auto effort=value.get<std::string>();
        const auto& efforts=pool.at("efforts");
        require(effort.empty() || std::find(efforts.begin(),efforts.end(),effort)!=efforts.end(),"DSH 不提供该思考强度");
    }
    pool[field]=value;
}
Json repair_links(const Json& row) {
    if (row.contains("repairLinks")) return row.at("repairLinks");
    if (row.contains("repairOf")) return Json::array({row.at("repairOf")});
    return Json::array();
}
Json repair_requests(const Json& row) {
    if (row.contains("repairRequests")) return row.at("repairRequests");
    if (row.contains("repairRequest")) return Json::array({row.at("repairRequest")});
    return Json::array();
}
bool review_worker(const Json& row) { return row.contains("precheckFor"); }
bool failed_handoff(const Json& row) {
    return row.value("status","")=="failed" && !row.value("resultVersion","").empty();
}
bool archived_dispatch(const Json& row) {
    return row.value("resultVersion","")=="dispatch:failed" &&
        row.value("dispatch",Json::object()).value("reason","")=="unconfirmed_submission";
}
std::string review_state(const Json& row) {
    if (!row.contains("precheck") || failed_handoff(row)) return {};
    const auto& check=row.at("precheck");
    if (row.value("resultVersion","").empty() || check.value("version","")!=row.value("resultVersion",""))
        return "pending";
    return check.value("state","pending");
}
bool result_visible(const Json&) { return true; }
Json review_notice(const Json& row) {
    const auto& check=row.at("precheck");
    Json notice{{"state",review_state(row)}};
    for (const auto* key:{"candidateSha","code","findings","checks","reviewSessionId"})
        if (check.contains(key)) notice[key]=check.at(key);
    return notice;
}
Json public_row(Json row) {
    if (review_worker(row)) row["internalReview"]=true;
    if (row.contains("precheck") && !failed_handoff(row)) {
        row["reviewState"]=review_state(row);
        row["review"]=review_notice(row);
    }
    if (!result_visible(row)) {
        row.erase("failure");
        row.erase("error");
        row.erase("dispatch");
    }
    row.erase("precheck");
    row.erase("precheckFor");
    row.erase("reviewTask");
    row.erase("owner");
    row.erase("dshAuthPath");
    row.erase("dispatchOperation");
    if (row.contains("pendingPrompt")) row["dispatchRetryAt"]=row.at("pendingPrompt").at("retryAt");
    row.erase("pendingPrompt");
    row.erase("repairRequest");
    row.erase("repairRequests");
    row.erase("messageOutbox");
    row.erase("repairQueued");
    row.erase("repairQueuedAt");
    if (row.contains("repairOf") && !row.contains("repairLinks")) row["repairLinks"]=repair_links(row);
    return row;
}
using CatalogModels=std::map<std::pair<std::string,std::string>,Json>;
CatalogModels catalog_models(const Json& catalog) {
    CatalogModels models;
    for (const auto& group:catalog.at("groups")) {
        if (!group.value("enabled",true) || !group.value("available",true)) continue;
        for (const auto& model:group.at("models"))
            if (model.value("enabled",true) && model.value("available",true))
                models.emplace(std::pair(group.at("id").get<std::string>(),model.at("id").get<std::string>()),model);
    }
    return models;
}

std::set<std::string> batch_allowlist(const Json& input) {
    if (!input.contains("allowedPoolIds")) return {};
    const auto& pools=input.at("allowedPoolIds");
    require(pools.is_array() && !pools.empty() && pools.size()<=64,"批次允许池需要 1 至 64 个池 ID");
    std::set<std::string> keys;
    for (const auto& item:pools) {
        require(item.is_string() && !item.get_ref<const std::string&>().empty() &&
            item.get_ref<const std::string&>().size()<=100,"批次允许池 ID 无效");
        keys.insert(item.get<std::string>());
    }
    return keys;
}

struct PoolSelection {
    std::string key;
    Json diagnostics;
};
PoolSelection choose_pool(const Json& settings,const Json& sessions,const CatalogModels& models,
    const std::set<std::string>& allowed,const std::string& pinned,const Json& cooldowns,
    const std::map<std::string,std::string>& rejected,bool automatic_team=false) {
    std::vector<std::tuple<int,int,std::string>> scores;
    Json diagnostics{{"pools",Json::array()},{"availablePools",Json::array()}};
    for (const auto& [key, pool] : settings.at("pools").items()) {
        const auto provider = pool.at("provider").get<std::string>();
        const auto model=pool.at("model").get<std::string>();
        const auto enabled=pool.at("enabled").get<bool>();
        const bool in_batch=allowed.empty() || allowed.contains(key);
        if (!enabled && key!=pinned && !allowed.contains(key)) continue;
        int account_count=0,pool_count=0,unverified=0;
        for (const auto& row : sessions) {
            if (!active(row)) continue;
            if (row.at("provider")==provider && row.value("dshProfile","default")==dsh_profile_id(settings)) ++account_count;
            const auto legacy_model = row.value("source","") == "codex" && row.at("provider") == provider &&
                row.value("model","") == pool.at("model").get<std::string>();
            if (row.value("poolId","")==key || legacy_model) {
                ++pool_count;
                if (row.value("stale",false)) ++unverified;
            }
        }
        const auto cap = automatic_team?64:pool.at("concurrency").get<int>();
        const auto cooldown=cooldowns.value(provider,std::int64_t(0));
        const auto refusal=rejected.find(key);
        const std::string_view reason=!in_batch?"batch_restricted":!enabled?"disabled":
            cooldown>now()?"account_cooldown":!models.contains({provider,model})?"model_unavailable":
            refusal!=rejected.end()?std::string_view(refusal->second):pool_count>=cap?"slots_full":"available";
        Json diagnostic{{"poolId",key},{"account",settings.at("accounts").at(provider).value("name",provider)},
            {"model",pool.value("name",model)},{"occupied",pool_count},{"concurrency",cap},{"reason",reason}};
        if (unverified) diagnostic["unverifiedSlots"]=unverified;
        if (reason=="account_cooldown") diagnostic["retryAt"]=cooldown;
        if (diagnostics.at("pools").size()<64 || key==pinned) diagnostics["pools"].push_back(diagnostic);
        if (reason!="available") continue;
        if (diagnostics.at("availablePools").size()<24)
            diagnostics["availablePools"].push_back(diagnostic);
        if (!pinned.empty() && key!=pinned) continue;
        const auto load = 100000 * pool_count / cap;
        scores.emplace_back(account_count,load,key);
    }
    if (!pinned.empty() && !settings.at("pools").contains(pinned))
        diagnostics["pools"].push_back({{"poolId",pinned},{"reason","unknown_pool"}});
    return {scores.empty()?"":std::get<2>(*std::min_element(scores.begin(),scores.end())),std::move(diagnostics)};
}

std::string selection_rejection(const DshError& error) {
    if (error.code=="http_429" || error.code=="rate_limited" || error.code=="account_cooldown" ||
        error.code=="quota_exceeded") return "account_cooldown";
    if (error.code=="model_unavailable" || error.code=="model_not_found" ||
        error.code=="session/model-unavailable" ||
        error.code=="provider_unavailable" || error.code=="provider_not_found") return "model_unavailable";
    return {};
}

std::string pool_failure(const Json& diagnostics,const std::string& pinned) {
    const auto& pools=diagnostics.at("pools");
    const auto selected=std::ranges::find_if(pools,[&](const Json& entry) {return entry.at("poolId")==pinned;});
    const auto describe=[](const Json& entry) {
        const auto reason=entry.at("reason").get<std::string>();
        if (reason=="unknown_pool") return std::string("池 ID 不存在");
        const auto name=entry.at("account").get<std::string>()+" / "+entry.at("model").get<std::string>();
        if (reason=="slots_full") return name+" 槽位已满（"+entry.at("occupied").dump()+"/"+entry.at("concurrency").dump()+"）";
        if (reason=="account_cooldown") return name+" 的账号正在冷却";
        if (reason=="batch_restricted") return name+" 不在批次允许池清单中";
        if (reason=="model_unavailable") return name+" 在 DSH 中不可用";
        return name+" 未启用";
    };
    std::string message;
    if (selected!=pools.end()) message=describe(*selected);
    else {
        for (const auto& entry:pools) {
            if (entry.at("reason")=="batch_restricted") continue;
            if (!message.empty()) message+="。";
            message+=describe(entry);
            if (message.size()>400) break;
        }
        if (message.empty()) message="派发范围内没有启用的模型池";
    }
    const auto& available=diagnostics.at("availablePools");
    if (!available.empty()) {
        message+="。可用池";
        for (std::size_t i=0;i<std::min<std::size_t>(3,available.size());++i) {
            const auto& entry=available.at(i);
            message+=(i?", ":" ")+entry.at("account").get<std::string>()+" / "+entry.at("model").get<std::string>()+
                " ("+entry.at("poolId").get<std::string>()+")";
        }
    }
    return message;
}
std::pair<std::size_t,std::size_t> catalog_size(const Json& catalog) {
    const auto& groups = catalog.at("groups");
    require(groups.is_array(), "DSH 模型目录格式无效");
    std::size_t models=0;
    for (const auto& group:groups) {
        require(group.at("models").is_array(), "DSH 模型目录格式无效");
        models+=group.at("models").size();
    }
    return {groups.size(),models};
}

Json read_import_catalog(Dsh& dsh,const DshConnection& connection,bool recover) {
    try { return dsh.request("session/modelCatalog"); }
    catch (const DshError& error) {
        if (!error.code.starts_with("winhttp_") && error.code!="http_401" && error.code!="http_403") throw;
        if (recover && dsh.desktop_pid() && error.code=="winhttp_12029")
            recover_desktop_dsh({{"configPath",utf8(connection.config)},{"authPath",utf8(connection.auth)}});
        std::optional<Dsh> refreshed;
        try { refreshed.emplace(connection.auth,30000,40000); }
        catch (const std::exception&) {}
        if (!refreshed || refreshed->owner()==dsh.owner()) throw;
        dsh=std::move(*refreshed);
        return dsh.request("session/modelCatalog");
    }
}

std::size_t add_configured_models(Json& catalog,const Dsh& dsh) {
    const auto directory=dsh.request("llm/listConfigurableProviders");
    require(directory.is_array(),"DSH 提供方目录格式无效");
    if (directory.empty()) return 0;
    const auto settings=dsh.request("settings/describe");
    const auto& namespaces=settings.at("namespaces");
    require(namespaces.is_array(),"DSH 模型配置格式无效");
    auto& groups=catalog.at("groups");
    std::size_t added=0;
    for (const auto& entry:directory) {
        if (!entry.is_object()) continue;
        const auto provider=entry.value("provider","");
        const auto ns=entry.value("settingsNs","");
        const auto path=entry.find("settingsPath");
        if (provider.empty() || provider.size()>512 || ns.empty() || path==entry.end() || !path->is_array()) continue;
        const auto section=std::ranges::find_if(namespaces,[&](const Json& item) {
            return item.is_object() && item.value("ns","")==ns;
        });
        if (section==namespaces.end() || !section->contains("value")) continue;
        const Json* profile=&section->at("value");
        for (const auto& part:*path) {
            if (!part.is_string() || !profile->is_object()) { profile=nullptr; break; }
            const auto next=profile->find(part.get_ref<const std::string&>());
            if (next==profile->end()) { profile=nullptr; break; }
            profile=&*next;
        }
        if (!profile || !profile->is_object() || !profile->contains("models") || !profile->at("models").is_array()) continue;
        auto group=std::ranges::find_if(groups,[&](const Json& item){return item.at("id")==provider;});
        for (const auto& model:profile->at("models")) {
            if (!model.is_object() || !model.contains("id") || !model.at("id").is_string()) continue;
            const auto& key=model.at("id").get_ref<const std::string&>();
            if (key.empty() || key.size()>512) continue;
            if (group==groups.end()) {
                groups.push_back({{"id",provider},{"name",entry.value("displayName",provider)},{"models",Json::array()}});
                group=std::prev(groups.end());
            }
            auto& models=group->at("models");
            if (std::ranges::any_of(models,[&](const Json& live){return live.at("id")==key;})) continue;
            const auto name=model.find("name");
            models.push_back({{"id",key},{"name",name!=model.end() && name->is_string()?*name:Json(key)},{"configuredOnly",true}});
            ++added;
        }
    }
    return added;
}

Json catalog_import(Json& settings, const Json& catalog) {
    const auto& groups=catalog.at("groups");
    std::set<std::string> accounts;
    std::set<std::pair<std::string,std::string>> imported;
    for (const auto& group : groups) {
        auto provider = field(group, "id", 512);
        auto& account = settings["accounts"][provider];
        if (account.is_null()) account = {{"id",provider}};
        account["name"] = group.value("name", provider);
        accounts.insert(provider);
        for (const auto& model : group.at("models")) {
            auto model_id = field(model, "id", 512);
            if (!imported.emplace(provider,model_id).second) continue;
            auto& pools = settings["pools"];
            auto it = std::find_if(pools.begin(), pools.end(), [&](const Json& value) {
                return value.at("provider") == provider && value.at("model") == model_id;
            });
            if (it == pools.end()) {
                auto key = id();
                pools[key] = {{"id",key},{"provider",provider},{"model",model_id},{"enabled",false},
                    {"concurrency",1},{"effort",""},{"name",model_id},{"efforts",Json::array()},{"defaultEffort",""}};
                it = pools.find(key);
            }
            auto& pool = *it;
            pool["name"] = model.value("name", model_id);
            if (model.value("configuredOnly",false)) continue;
            Json efforts = Json::array();
            const auto reasoning=model.value("reasoning",Json::object());
            if (reasoning.is_object() && reasoning.contains("efforts") && reasoning.at("efforts").is_array())
                for (const auto& value : reasoning.at("efforts"))
                    if (value.is_object() && value.contains("id") && value["id"].is_string()) efforts.push_back(value["id"]);
            pool["efforts"] = efforts;
            const auto fallback=reasoning.is_object()?reasoning.value("defaultEffort",Json("")):Json("");
            pool["defaultEffort"] = fallback.is_string()?fallback:Json("");
        }
    }
    return {{"accounts",accounts.size()},{"models",imported.size()}};
}
Json rpc_state(const Dsh& dsh, const Json& row) {
    require(row.contains("dshId") && row.at("dshId").is_string(), "会话尚未取得 DSH ID");
    require(dsh.matches_owner(row.value("owner","")), "没有连接到该会话最初所属的 DSH 服务");
    return dsh.request("session/state", {{"sessionId",row.at("dshId")}});
}
Dsh session_connection(const Json& profiles,const Json& row,int timeout,int total_timeout=0) {
    const auto owner=row.value("owner","");
    std::set<fs::path> tried;
    const auto match=[&](const fs::path& auth)->std::optional<Dsh> {
        if (!auth.is_absolute() || !tried.insert(auth).second) return {};
        try {
            Dsh dsh(auth,timeout,total_timeout);
            if (dsh.matches_owner(owner)) return dsh;
        } catch (const std::exception&) {}
        return {};
    };
    if (auto dsh=match(path_from_utf8(row.value("dshAuthPath","")))) return std::move(*dsh);
    const auto match_profile=[&](const Json& profile)->std::optional<Dsh> {
        if (auto dsh=match(path_from_utf8(profile.at("authPath").get<std::string>()))) return dsh;
        try { return match(dispatch_auth(profile)); }
        catch (const std::exception&) { return {}; }
    };
    const auto profile=profiles.find(row.value("dshProfile","default"));
    if (profile!=profiles.end()) {
        if (auto dsh=match_profile(*profile)) return std::move(*dsh);
    }
    for (auto it=profiles.begin();it!=profiles.end();++it)
        if (it!=profile)
            if (auto dsh=match_profile(*it)) return std::move(*dsh);
    throw std::runtime_error("没有连接到该会话最初所属的 DSH 服务");
}
bool dispatch_pending(const Json& row) {
    const auto state=row.value("dispatchState","");
    return state=="submitting" || state=="unconfirmed" || (state.empty() && row.value("status","")=="unknown" &&
        row.value("dispatch",Json::object()).value("mayHaveDispatched",false));
}
bool prompt_retry_due(const Json& row,std::int64_t time) {
    return dispatch_pending(row) && row.contains("pendingPrompt") &&
        row.at("pendingPrompt").at("retryAt").get<std::int64_t>()<=time;
}
fs::path prompt_lock_file(const fs::path& data,const std::string& key) {
    return data/path_from_utf8("prompt-"+key+".lock");
}
void require_ready_dsh(const Json& settings) {
    if (!recover_desktop_dsh(settings,{},false))
        throw DispatchError("DSH 正在自动重启，请稍后用原参数重试",
            {{"reason","dsh_recovering"},{"retryable",true},{"stage","connect"},{"retryAfterMs",2000}});
}
Json report(const Dsh& dsh, const Json& row) {
    Json records=Json::array();
    std::int64_t before=std::numeric_limits<std::int64_t>::max();
    std::size_t bytes=0;
    bool more=false;
    for (int page=0;page<128;++page) {
        Json request{{"address",{{"kind","session"},{"sessionId",row.at("dshId")}}},{"maxMessages",8}};
        if (row.contains("cursor")) request["throughSeq"]=row.at("cursor");
        if (page) request["beforeSeq"]=before;
        const auto response=dsh.request("session/page",request);
        auto earliest=before;
        for (const auto& record:response.at("records")) {
            if (!record.contains("event")) continue;
            const auto seq=record.at("event").value("seq",std::int64_t(-1));
            if (seq<0 || seq>=before || seq>row.value("cursor",std::numeric_limits<std::int64_t>::max())) continue;
            earliest=std::min(earliest,seq);
            if (seq<=row.value("startCursor",std::int64_t(-1))) continue;
            const auto size=record.dump().size();
            if (bytes+size>16*1024*1024) {more=true;break;}
            bytes+=size;
            records.push_back(record);
        }
        if (more) break;
        more=response.value("hasMore",false) && earliest>row.value("startCursor",std::int64_t(-1));
        if (!more || earliest>=before) break;
        before=earliest;
        more=false;
        if (page==127) more=true;
    }
    std::sort(records.begin(),records.end(),[](const Json& a,const Json& b) {
        return a.at("event").at("seq")<b.at("event").at("seq");
    });
    return {{"records",records},{"truncated",more}};
}
constexpr std::array usage_keys{"uncachedInputTokens","outputTokens","cacheReadTokens","cacheWriteTokens"};
using TokenCounts=std::array<std::uint64_t,usage_keys.size()>;
Json token_totals(const TokenCounts& counts) {
    Json result=Json::object();
    for (std::size_t i=0;i<counts.size();++i) result[usage_keys[i]]=counts[i];
    return result;
}
TokenCounts token_counts(const Json& usage,bool from_event=false) {
    TokenCounts counts{};
    for (std::size_t i=0;i<counts.size();++i) {
        const auto key=i==0 && from_event?"inputTokens":usage_keys[i];
        if (from_event && i>=2 && !usage.contains(key)) continue;
        const auto& value=usage.at(key);
        require(value.is_number_unsigned(),"DSH 用量格式无效");
        counts[i]=value.get<std::uint64_t>();
    }
    return counts;
}
std::int64_t midnight() {
    auto stamp = std::time(nullptr);
    std::tm local{};
    if (localtime_s(&local,&stamp) != 0) return 0;
    local.tm_hour = local.tm_min = local.tm_sec = 0;
    local.tm_isdst = -1;
    return std::int64_t(std::mktime(&local)) * 1000;
}
std::optional<TokenCounts> token_sample(const Json& event) {
    const auto& data=event.at("data");
    if (data.contains("usage") && data.at("usage").is_object()) {
        return token_counts(data.at("usage"),true);
    }
    const auto stream=data.value("stream",Json::array());
    if (stream.is_array()) for (auto it=stream.rbegin();it!=stream.rend();++it) {
        if (!it->contains("chunk") || it->at("chunk").value("type","")!="usage") continue;
        return token_counts(it->at("chunk").at("usage"),true);
    }
    return {};
}
TokenCounts day_usage(const Dsh& dsh,const Json& row,std::int64_t cursor,std::int64_t start,std::stop_token stop={}) {
    using Attempt=std::pair<int,int>;
    std::vector<std::pair<Attempt,std::optional<TokenCounts>>> events;
    auto before=cursor+1;
    int page_size=16;
    for (int pages=0;pages<32;++pages) {
        require(!stop.stop_requested(),"已停止同步用量");
        Json request{{"address",{{"kind","session"},{"sessionId",row.at("dshId")}}},
            {"throughSeq",cursor},{"maxMessages",page_size}};
        if (before<=cursor) request["beforeSeq"]=before;
        Json page;
        for (;;) {
            try {page=dsh.request("session/page",request);break;}
            catch (const std::length_error&) {
                if (page_size<=1) throw;
                page_size/=2;
                request["maxMessages"]=page_size;
            }
        }
        auto earliest=before;
        bool old=false;
        const auto& records=page.at("records");
        std::vector<const Json*> ordered;
        for (const auto& record:records)
            if (record.contains("event") && record.at("event").is_object() &&
                record.at("event").value("seq",before)<before)
                ordered.push_back(&record.at("event"));
        std::ranges::sort(ordered,[](const Json* a,const Json* b){return a->at("seq")>b->at("seq");});
        for (const auto* value:ordered) {
            const auto& event=*value;
            auto seq=event.value("seq",before);
            if (seq>=before) continue;
            earliest=std::min(earliest,seq);
            auto stamp=event.value("time",std::int64_t(0));
            if (stamp>0 && stamp<start) {old=true;continue;}
            const auto kind=event.value("type","");
            if (kind!="assistant/message" && kind!="assistant/attempt" && kind!="llm/retry-started") continue;
            require(stamp>0,"用量事件没有时间");
            const auto& data=event.at("data");
            Attempt attempt{data.at("turn").get<int>(),data.at("step").get<int>()};
            if (kind=="llm/retry-started") events.emplace_back(attempt,std::nullopt);
            else if (auto sample=token_sample(event)) events.emplace_back(attempt,sample);
        }
        if (old || !page.value("hasMore",false)) {
            TokenCounts total{},previous{};
            std::optional<Attempt> last;
            const auto settle=[&] {
                if (!last) return;
                for (std::size_t i=0;i<total.size();++i) total[i]+=previous[i];
                last.reset();
            };
            for (auto it=events.rbegin();it!=events.rend();++it) {
                const auto& [attempt,sample]=*it;
                if (sample) {
                    if (last!=attempt) settle();
                    previous=*sample;
                    last=attempt;
                } else if (last==attempt) settle();
            }
            settle();
            return total;
        }
        require(earliest<before,"用量分页无法继续");
        before=earliest;
    }
    throw std::runtime_error("今日历史用量超过扫描范围");
}
Json legacy_usage_cache(const fs::path& data) {
    try {
        auto cache=read_json(data/L"legacy-usage.json",8*1024*1024);
        if (cache.is_object()) return cache;
    } catch (const std::exception&) {}
    return Json::object();
}
Json telemetry_overview(const Json& sessions,const Json& legacy_cache) {
    const auto today=midnight();
    std::uint64_t total=0;
    int missing=0,sampled=0,today_sampled=0;
    TokenCounts lifetime{},today_usage{};
    for (const auto& row:sessions) {
        const Json* sample=&row;
        const auto legacy=row.value("readOnly",false);
        const auto completed=row.value("completed",std::int64_t(0));
        const bool relevant_today=active(row) || !completed || completed>=today;
        if (legacy) {
            const auto found=legacy_cache.find(row.at("id").get<std::string>());
            if (found==legacy_cache.end()) {
                if (relevant_today) ++missing;
                continue;
            }
            sample=&*found;
        }
        const auto tokens=sample->value("usage",Json::object());
        if (!tokens.empty()) ++sampled;
        for (std::size_t i=0;i<lifetime.size();++i) lifetime[i]+=tokens.value(usage_keys[i],std::uint64_t(0));
        const bool same_day=sample->value("tokenDay",std::int64_t(0))==today && sample->contains("todayUsage");
        if (same_day) {
            const auto counts=token_counts(sample->at("todayUsage"));
            for (std::size_t i=0;i<counts.size();++i) {
                today_usage[i]+=counts[i];
                total+=counts[i];
            }
            ++today_sampled;
        }
        if (!relevant_today) continue;
        if (legacy) {
            if (!same_day || sample->value("stale",false) || sample->value("updated",std::int64_t(0))!=
                row.value("registryUpdated",row.value("updated",std::int64_t(0))) ||
                (active(row) && now()-sample->value("observed",std::int64_t(0))>120000)) ++missing;
        } else if (row.contains("startCursor") &&
                   (!same_day || row.value("tokenCursor",Json())!=row.value("cursor",Json()))) ++missing;
    }
    const auto hours=double(now()-today)/3600000.0;
    return {{"total",token_totals(lifetime)},{"today",token_totals(today_usage)},{"todayTokens",total},
        {"hourlyTokens",hours>0 && (today_sampled || !missing)?Json(double(total)/hours):Json(nullptr)},
        {"missing",missing},{"sampled",sampled},{"todaySampled",today_sampled}};
}
void clear_dispatch_diagnostics(Json& row) {
    if (row.contains("error")) row["dispatch"]["lastError"]=row.at("error");
    for (const auto* member:{"error","retryable","retryAfterMs","retryMode","failureStage"}) row.erase(member);
}
void fail_dispatch(Json& row,const std::string& code,const std::string& message) {
    row.update({{"status","failed"},{"dispatchState","failed"},{"resultVersion","dispatch:failed"},
        {"completed",now()},{"updated",now()},{"read",false},{"processed",false},{"error",message},
        {"failure",{{"kind","dispatch"},{"code",code},{"message",message}}}});
    row["retryable"]=code=="startup_interrupted";
    row["retryAfterMs"]=0;
    row["failureStage"]=code=="startup_interrupted"?"prepare_dispatch":"manual_failure";
    row["retryMode"]="new_request";
    row["failure"].update({{"retryable",row.at("retryable")},{"retryAfterMs",0},
        {"stage",row.at("failureStage")},{"retryMode","new_request"}});
    if (row.contains("messageOutbox")) for (auto& entry:row.at("messageOutbox")) {
        if (entry.value("state","")=="pending" || entry.value("state","")=="unconfirmed") {
            entry["state"]=entry.contains("sentAt")?"unconfirmed_terminal":"not_sent";
            entry["reason"]="session_failed";entry["retryable"]=false;entry.erase("retryAt");
        }
    }
    row.erase("pendingPrompt");
    row.erase("startCursor");
}
Json refresh(const fs::path& data,const std::string& key,int timeout_ms=15000,bool include_history_usage=true,
             std::stop_token stop={}) {
    Json profiles, prior;
    {
        Database db(data, false);
        profiles = dsh_profiles(db.state);
        prior = db.state.at("sessions").at(key);
    }
    if (prior.value("status","")=="starting" && !prior.contains("startCursor") &&
        now()-prior.value("updated",now())>120000) {
        // A live dispatcher holds this lock from reservation through submission.
        const ExclusiveStateLock lock(prompt_lock_file(data,key));
        Database db(data,true);
        auto& row=db.state.at("sessions").at(key);
        if (row.value("status","")=="starting" && !row.contains("startCursor") &&
            now()-row.value("updated",now())>120000) {
            fail_dispatch(row,"startup_interrupted","派发准备超过两分钟且派发进程已停止，任务未提交，请重新派发");
            db.save();
        }
        return public_row(row);
    }
    if (!prior.contains("startCursor") || prior.value("resultVersion","")=="dispatch:failed") return public_row(prior);
    std::optional<Dsh> connection;
    std::optional<DshHistory> local_history;
    Json live;
    try {
        connection.emplace(session_connection(profiles,prior,timeout_ms,
            timeout_ms<15000?2*timeout_ms:0));
        live=rpc_state(*connection,prior);
    } catch (const std::exception&) {
        const auto sid=prior.value("dshId","");
        const auto start=prior.at("startCursor").get<std::int64_t>();
        local_history=read_dsh_history(dsh_history_file(sid,prior.value("workspace","")),sid,start);
        if (!local_history || local_history->cursor<prior.value("cursor",start)) throw;
        live={{"cursor",local_history->cursor},{"running",false},{"pendingInput",Json::array()},{"queue",Json::array()}};
    }
    Json manager_goal;
    if (prior.value("role","")=="lead" && (prior.contains("goal") || prior.contains("managerBootstrapPath")) && !local_history) {
        const auto view=connection->request("session/projections",{{"sessionId",prior.at("dshId")}});
        manager_goal=goal_snapshot(view);
        require(!manager_goal.is_null(),"项目经理 goal 投影缺失，不能推断会话完成");
        if (manager_goal.value("phase","")=="active") live["running"]=true;
    }
    auto cursor = live.at("cursor").get<std::int64_t>();
    const auto start = prior.at("startCursor").get<std::int64_t>();
    const auto today=midnight();
    const bool has_today=prior.value("tokenDay",std::int64_t(0))==today && prior.contains("todayUsage") &&
        prior.value("tokenCursor",std::int64_t(-1))==cursor;
    std::string status;
    std::string version;
    Json failure=Json::object();
    auto completed = now();
    if (!live.value("pendingInput", Json::array()).empty()) status = "waiting_input";
    else if (live.at("running").get<bool>()) status = "running";
    else if (cursor<=start && has_queued_prompt(live,prior.at("id").get_ref<const std::string&>())) status = "queued";
    else if (!active(prior) && prior.value("cursor", start) == cursor) {
        if (has_today || !include_history_usage || prior.value("completed",std::int64_t(0))<today)
            return public_row(prior);
        status=prior.at("status").get<std::string>();
        version=prior.at("resultVersion").get<std::string>();
    }
    else if (cursor <= start) status = prior.at("status")=="starting"?"starting":"unknown";
    else {
        status = "unknown";
        const auto ending=local_history?std::optional<TurnEnd>(local_history->ending):
            latest_turn_end(*connection,prior.at("dshId"),start,cursor);
        if (ending) {
            status=ending->status;
            version=std::to_string(start)+":"+std::to_string(ending->seq);
            if (ending->time>0) completed=ending->time;
            failure=ending->failure;
        } else if (has_queued_prompt(live,prior.at("id").get_ref<const std::string&>())) status="queued";
    }
    Json usage;
    std::optional<TokenCounts> today_usage;
    try {
        if (local_history) throw std::runtime_error("本机历史同步");
        const auto projections=connection->request("session/projections",{{"sessionId",prior.at("dshId")}});
        const auto counts=token_counts(projections.at("/values/tokenUsage"_json_pointer));
        usage=token_totals(counts);
        if (prior.value("created",std::int64_t(0))>=today) today_usage=counts;
    } catch (const std::exception&) {}
    if (!local_history && !has_today && include_history_usage && prior.value("created",std::int64_t(0))<today) {
        try {today_usage=day_usage(*connection,prior,cursor,today,stop);}
        catch (const std::exception&) {}
    }
    Database db(data, true);
    auto& row = db.state["sessions"].at(key);
    if (row.value("resultVersion","")!="dispatch:failed" && row.value("cursor",start)<=cursor && row.value("startCursor",Json())==start &&
        (row.value("cursor",start)<cursor || row.at("status")==prior.at("status"))) {
        const auto before=row;
        auto dispatch=row.value("dispatch",Json::object());
        const bool archive=status=="unknown" && cursor==start && live.contains("queue") &&
            (live.at("queue").is_array() || live.at("queue").is_null()) && !has_queued_prompt(live) &&
            !row.contains("pendingPrompt") && dispatch.value("stage","")=="submit_prompt" &&
            dispatch.value("mayHaveDispatched",false);
        const bool observed=status=="queued" || status=="running" || status=="waiting_input" || !version.empty();
        if (archive) {
            status="failed";
            version="dispatch:failed";
            failure={{"kind","dispatch"},{"code","unconfirmed_submission"},
                {"message","DSH 未开始执行，旧记录没有可供重试的提交请求"}};
            dispatch["reason"]="unconfirmed_submission";
            row["dispatch"]=std::move(dispatch);
            row["dispatchState"]="failed";
            row["retryable"]=true;row["retryAfterMs"]=0;row["retryMode"]="new_request";row["failureStage"]="submit_prompt";
            failure.update({{"retryable",true},{"retryAfterMs",0},{"retryMode","new_request"},{"stage","submit_prompt"}});
            row.erase("startCursor");
        }
        if (!manager_goal.is_null()) row["goal"]=manager_goal;
        row["status"] = status;
        row["cursor"] = cursor;
        if (observed && !archive) clear_dispatch_diagnostics(row);
        if (observed && dispatch_pending(row)) {
            row["dispatchState"]="confirmed";
            row.erase("pendingPrompt");
        }
        else if (!archive && !observed && cursor==start && before.at("status")=="queued" &&
            row.value("dispatch",Json::object()).value("mayHaveDispatched",false))
            row["dispatchState"]="unconfirmed";
        if (usage.is_object()) row["usage"]=usage;
        if (today_usage) {
            row["tokenDay"]=today;
            row["todayUsage"]=token_totals(*today_usage);
            row["tokenCursor"]=cursor;
        }
        if (row.value("resultVersion","") != version) {
            row["resultVersion"] = version;
            row["read"] = false;
            row["processed"] = false;
            row["completed"] = version.empty() ? 0 : completed;
            row.erase("failure");
            if (!failure.empty()) row["failure"]=std::move(failure);
        }
        if (status=="failed" && !archive) {
            const auto diagnostics=row.value("failure",Json::object());
            row["retryable"]=diagnostics.value("retryable",Json());
            row["retryAfterMs"]=diagnostics.value("retryAfterMs",0);
            row["retryMode"]="new_request";row["failureStage"]="execution";
        }
        if (row!=before) {
            if (row.at("status")!=before.at("status") || row.at("cursor")!=before.value("cursor",Json())) row["updated"]=now();
            db.save();
        }
    }
    return public_row(row);
}
void retry_prompt(const fs::path& data,const std::string& key) {
    const ExclusiveStateLock lock(prompt_lock_file(data,key));
    Json request,profiles,prior;
    {
        Database db(data,true);
        auto& row=db.state.at("sessions").at(key);
        const auto time=now();
        if (!prompt_retry_due(row,time) || !open_queue(db.state,row.at("queueId").get<std::string>())) return;
        profiles=dsh_profiles(db.state);
        prior=row;
        auto& pending=row.at("pendingPrompt");
        const auto attempt=std::min(4,pending.value("attempt",0)+1);
        pending["attempt"]=attempt;
        pending["retryAt"]=time+(4000LL<<attempt);
        // DSH finds the original requestId in the session's inbox and event history.
        request=pending.at("request");
        row["status"]="unknown";
        row["dispatchState"]="unconfirmed";
        db.save();
    }
    const auto dsh=session_connection(profiles,prior,3500);
    require(dsh.request("session/prompt",request).value("accepted",false),"DSH 未确认接收任务");
    Database db(data,true);
    auto& row=db.state.at("sessions").at(key);
    if (!dispatch_pending(row)) return;
    row["dispatchState"]="confirmed";
    row["status"]="queued";
    clear_dispatch_diagnostics(row);
    row["updated"]=now();
    row.erase("pendingPrompt");
    db.save();
}

Json message_result(const Json& row,const std::string& request,const Json& entry) {
    const auto state=entry.at("state").get<std::string>();
    Json result{{"sessionId",row.at("id")},{"workItemId",row.value("workItemId","")},
        {"requestId",request},{"messageId",entry.at("messageId")},{"mode",entry.at("mode")},
        {"state",state},{"accepted",state=="accepted"},{"executionConfirmed",false},
        {"retryable",entry.value("retryable",false)},{"retryAfterMs",std::max<std::int64_t>(0,entry.value("retryAt",std::int64_t(0))-now())},
        {"mayHaveBeenDelivered",entry.contains("sentAt")},{"messagePath",entry.at("messagePath")}};
    for (const auto* member:{"reason","stage","error","attempts","acceptedAt","needsInspection"})
        if (entry.contains(member)) result[member]=entry.at(member);
    return result;
}
bool message_due(const Json& entry,std::int64_t time) {
    const auto status=entry.value("state","");
    return (status=="pending" || status=="unconfirmed") && entry.value("retryable",true) &&
        entry.value("retryAt",std::int64_t(0))<=time;
}
void message_event(Json& state,Json& row,const std::string& request,Json& entry) {
    row["lastMessageAt"]=now();
    record_event(state,row.at("queueId").get<std::string>(),"session.message",
        {{"sessionId",row.at("id")},{"workItemId",row.value("workItemId","")},{"requestId",request},
         {"messageId",entry.at("messageId")},{"state",entry.at("state")},{"mode",entry.at("mode")}});
}
void deliver_message(const fs::path& data,const std::string& key,const std::string& request_id,bool force=false) {
    // Serialize supplements with dispatch, retries and manual failure for this session.
    const ExclusiveStateLock lock(prompt_lock_file(data,key));
    Json prior,entry,profiles,settings;
    {
        Database db(data,false);
        prior=db.state.at("sessions").at(key);
        entry=prior.at("messageOutbox").at(request_id);
        if (!message_due(entry,force?std::numeric_limits<std::int64_t>::max():now())) return;
        profiles=dsh_profiles(db.state);
        try {settings=dsh_profile_settings(db.state,prior.value("dshProfile","default"));}
        catch (const std::exception&) {}
    }
    const auto finish=[&](const std::string& state,const std::string& reason,bool retryable,const std::string& stage,const std::string& error="") {
        Database db(data,true);
        auto& row=db.state.at("sessions").at(key);
        auto& current=row.at("messageOutbox").at(request_id);
        current.update({{"state",state},{"reason",reason},{"retryable",retryable},{"stage",stage},{"error",error},
            {"needsInspection",state=="unconfirmed_terminal"}});
        if (!retryable) current.erase("retryAt");
        message_event(db.state,row,request_id,current);
        db.save();
    };
    if (!active(prior) || ![&] {Database db(data,false);return open_queue(db.state,prior.at("queueId").get<std::string>());}()) {
        finish(entry.contains("sentAt")?"unconfirmed_terminal":"not_sent","session_terminal",false,"message_preflight");
        return;
    }
    std::string stage="message_archive";
    try {
        const auto body=read_json(path_from_utf8(entry.at("messagePath").get<std::string>()),512*1024);
        require(body.at("inputDigest")==entry.at("inputDigest") && body.at("messageId")==entry.at("messageId") &&
            sha256(body.at("input").dump())==entry.at("inputDigest").get<std::string>() &&
            sha256(body.at("request").dump())==entry.at("requestDigest").get<std::string>() &&
            body.at("request").at("requestId")==entry.at("messageId") &&
            body.at("request").at("sessionId")==prior.at("dshId"),"追加消息归档身份不匹配");
        stage="message_connect";
        if (!settings.is_null()) require_ready_dsh(settings);
        const auto dsh=session_connection(profiles,prior,1500,6000);
        stage="message_preflight";
        const auto live=rpc_state(dsh,prior);
        const bool busy=live.value("running",false) || !live.value("pendingInput",Json::array()).empty() || has_queued_prompt(live);
        if (!busy) {
            finish(entry.contains("sentAt")?"unconfirmed_terminal":"not_sent",
                entry.contains("sentAt")?"message_unconfirmed_session_idle":"session_idle",false,"message_preflight");
            return;
        }
        {
            Database db(data,true);
            auto& row=db.state.at("sessions").at(key);
            auto& current=row.at("messageOutbox").at(request_id);
            const auto attempt=current.value("attempts",0)+1;
            current["attempts"]=attempt;
            current["retryAt"]=now()+(2000LL<<std::min(4,attempt-1));
            current["sentAt"]=now();
            // This durable reservation precedes the possibly ambiguous network write.
            db.save();
        }
        stage="message_admission";
        const auto reply=dsh.request("session/prompt",body.at("request"));
        require(reply.value("accepted",false),"DSH 未确认接收追加消息");
        Database db(data,true);
        auto& row=db.state.at("sessions").at(key);
        auto& current=row.at("messageOutbox").at(request_id);
        current.update({{"state","accepted"},{"acceptedAt",now()},{"retryable",false},{"stage","message_admission"}});
        current.erase("retryAt");current.erase("reason");current.erase("error");
        message_event(db.state,row,request_id,current);
        db.save();
    } catch (const DispatchError& error) {
        {
            Database db(data,true);
            auto& current=db.state.at("sessions").at(key).at("messageOutbox").at(request_id);
            current["retryAt"]=now()+error.details.value("retryAfterMs",2000);
            db.save();
        }
        finish("unconfirmed",error.details.value("reason",std::string("dsh_recovering")),true,stage,error.what());
    } catch (const DshError& error) {
        const auto diagnostics=dsh_error_diagnostics(error);
        const auto retryable=diagnostics.at("retryable").get<bool>();
        {
            Database db(data,true);
            auto& current=db.state.at("sessions").at(key).at("messageOutbox").at(request_id);
            if (!current.contains("retryAt") || current.at("retryAt").get<std::int64_t>()<=now()) current["retryAt"]=now()+2000;
            db.save();
        }
        finish(retryable?"unconfirmed":"failed",error.code,retryable,error.stage,error.what());
    } catch (const std::exception& error) {
        {
            Database db(data,true);
            auto& current=db.state.at("sessions").at(key).at("messageOutbox").at(request_id);
            current["retryAt"]=now()+4000;
            db.save();
        }
        const bool retryable=stage!="message_archive";
        finish(retryable?"unconfirmed":"failed",retryable?"message_delivery_unconfirmed":"message_archive_invalid",retryable,stage,error.what());
    }
}

Json reconcile_dispatch(const fs::path& data,Json row,bool resend=true) {
    if (!dispatch_pending(row)) return row;
    try {
        const auto key=row.at("id").get<std::string>();
        Json settings;
        {
            Database db(data,false);
            const auto& stored=db.state.at("sessions").at(key);
            settings=dsh_profile_settings(db.state,stored.value("dshProfile","default"));
        }
        settings["authPath"]=utf8(dispatch_auth(settings));
        require_ready_dsh(settings);
        try { row=refresh(data,key,1800,false); }
        catch (const std::exception&) {}
        if (resend && dispatch_pending(row)) {
            bool due;
            {
                Database db(data,false);
                due=prompt_retry_due(db.state.at("sessions").at(key),now());
            }
            if (due) {
                retry_prompt(data,key);
                row=refresh(data,key,1800,false);
            }
        }
    }
    catch (const std::exception&) {}
    if (dispatch_pending(row)) row["dispatchState"]="unconfirmed";
    return row;
}
Json dispatch_request(const Json& rows,const Json& operation,const std::string& queue,const std::string& legacy) {
    if (operation.is_null()) return nullptr;
    for (const auto& row:rows) {
        if (!in_queue(row,queue,legacy)) continue;
        const auto existing=row.find("dispatchOperation");
        if (existing==row.end() || !existing->is_object() || existing->at("kind")!=operation.at("kind") ||
            existing->at("requestId")!=operation.at("requestId")) continue;
        require(existing->at("input")==operation.at("input"),"该 requestId 已用于其他派发参数，请用原参数重试");
        return public_row(row);
    }
    return nullptr;
}
}

Cluster::Cluster(fs::path data) : data_(std::move(data)),handoff_observations_(std::make_shared<HandoffObservations>()) {
    if (data_.empty()) data_ = environment(L"FATFISHBOX_DATA_DIR");
    if (data_.empty()) data_ = fs::path(environment(L"LOCALAPPDATA")) / L"FatFishBox";
    require(data_.is_absolute(), "应用数据目录必须使用绝对路径");
    fs::create_directories(data_);
    Database db(data_,true);
    if (!db.state.contains("legacyQueueId")) {
        const auto legacy=id();
        const auto created=now();
        std::set<std::string> workspaces;
        for (const auto& row:db.state.at("sessions")) if (!row.value("workspace","").empty())
            workspaces.insert(row.at("workspace").get<std::string>());
        const auto external=read_legacy_sessions(legacy_connection_settings(db.state));
        for (const auto& row:external.rows) if (!row.value("workspace","").empty())
            workspaces.insert(row.at("workspace").get<std::string>());
        db.state["legacyQueueId"]=legacy;
        db.state["queues"][legacy]={{"queueId",legacy},{"name","原有队列"},{"legacy",true},
            {"created",created},{"updated",created},{"workspaces",workspaces}};
        db.save();
    }
    legacy_queue_id_=db.state.at("legacyQueueId").get<std::string>();
    queue_id_=legacy_queue_id_;
    if (synchronize_work_items(db.state)) db.save();
}

Cluster Cluster::for_queue(std::string_view queue_id) const {
    const bool valid=queue_id.size()==36 && std::ranges::all_of(std::views::iota(std::size_t{0},queue_id.size()),[&](auto i) {
        const auto ch=queue_id[i];
        if (i==8 || i==13 || i==18 || i==23) return ch=='-';
        return (ch>='0' && ch<='9') || (ch>='A' && ch<='F') || (ch>='a' && ch<='f');
    });
    require(valid,"queueId 必须是队列 UUID");
    std::string key(queue_id);
    for (auto& ch:key) if (ch>='a' && ch<='f') ch=char(ch-'a'+'A');
    Database db(data_,false);
    require(open_queue(db.state,key),"找不到该队列，请核对 queueId");
    Cluster result=*this;
    result.queue_id_=std::move(key);
    return result;
}

fs::path Cluster::handoff_file(bool lock) const {
    const auto base=queue_id_==legacy_queue_id_?"handoff":"handoff-"+queue_id_;
    return data_/path_from_utf8(base+(lock?".lock":".json"));
}

void Cluster::require_open_queue() const {
    Database db(data_,false);
    require(open_queue(db.state,queue_id_),"该队列已删除");
}

void Cluster::delete_desktop_queue(std::string_view queue_id) const {
    const auto deleting=for_queue(queue_id);
    const auto& key=deleting.queue_id_;
    const auto team=invoke("team_get_queue",{{"queueId",key}}).value("project",Json());
    require(!team.is_object() || (team.value("status","")!="running" && team.value("status","")!="starting" && team.value("status","")!="finishing"),"执行中的团队队列不能删除，请等待任务结束");
    require(key!=legacy_queue_id_,"原有队列保存历史任务，不能删除");
    const ExclusiveStateLock lock(deleting.handoff_file(true));
    Database db(data_,true);
    if (!open_queue(db.state,key)) return;
    db.state.at("queues").at(key)["deletedAt"]=now();
    if (db.state.value("desktopQueueId",legacy_queue_id_)==key) db.state["desktopQueueId"]=legacy_queue_id_;
    // Keep the sessions, receipts and dispatch request IDs with their original queue UUID.
    db.save();
}

Json Cluster::invoke_mcp(std::string_view tool,const Json& input) const {
    require(input.is_object(),"工具参数必须为 JSON 对象");
    if (!queue_scoped_tool(tool)) return invoke(tool,input);
    Cluster queue=*this;
    Json arguments=input;
    if (input.contains("queueId")) {
        queue=for_queue(field(input,"queueId",36));
        arguments.erase("queueId");
    } else {
        // Preserve continuations that were issued before the upgrade.
        bool migrated_claim=false;
        if (input.contains("claimToken") &&
            (tool!="cluster_next_handoff" || (!input.at("claimToken").is_null() && input.at("claimToken")!="")) &&
            (tool=="cluster_next_handoff" || tool=="cluster_repair_handoff" ||
            tool=="cluster_attach_handoff_repair" || tool=="cluster_detach_handoff_repair")) {
            const auto token=field(input,"claimToken",100);
            const auto file=handoff_file();
            if (fs::exists(file)) {
                const auto state=read_json(file);
                const auto claim=state.value("claim",Json::object());
                migrated_claim=claim.value("claimToken","")==token ||
                    (tool=="cluster_next_handoff" && state.value("lastToken","")==token);
            }
            if (!migrated_claim && (tool=="cluster_repair_handoff" || tool=="cluster_attach_handoff_repair")) {
                Database db(data_,false);
                migrated_claim=std::ranges::any_of(db.state.at("sessions"),[&](const Json& row) {
                    if (!in_queue(row,legacy_queue_id_,legacy_queue_id_)) return false;
                    return std::ranges::any_of(repair_requests(row),[&](const Json& entry) {
                        const auto& request=entry.contains("input")?entry.at("input"):entry;
                        return request.value("claimToken","")==token;
                    });
                });
            }
        }
        if (!migrated_claim) throw DispatchError("请提供 queueId。用 cluster_create_queue 创建独立队列，或用 cluster_find_queues 查找已有队列",
            {{"reason","queue_id_required"}});
    }
    if (team_managed_queue(data_,queue.queue_id_) &&
        (tool=="cluster_start_session" || tool=="cluster_repair_handoff" || tool=="cluster_update_work_item" ||
         tool=="cluster_create_work_item" || tool=="cluster_send_message" || tool=="cluster_cancel_session" ||
         tool=="cluster_next_handoff" || tool=="cluster_attach_handoff_repair" || tool=="cluster_detach_handoff_repair"))
        throw DispatchError("此队列由团队执行器管理。主代理使用 team_* 决策通道，负责人使用带领域凭据的 team_* 工具。",{{"reason","team_managed_queue"}});
    auto result=queue.invoke(tool,arguments);
    result["queueId"]=queue.queue_id_;
    return result;
}

Json Cluster::update_model_field(const std::string& id,const std::string& field,
                                const Json& expected,const Json& value) const {
    Database db(data_,true);
    auto& settings=db.state.at("settings");
    auto& pool=settings.at("pools").at(id);
    if (pool.at(field)==value) return settings;
    require(pool.at(field)==expected,"这项模型配置已在别处更新");
    set_model_field(pool,field,value);
    validate_configuration(settings);
    settings["revision"]=settings.at("revision").get<int>()+1;
    db.save();
    return settings;
}

namespace {
using HandoffVersions=std::set<std::pair<std::string,std::string>>;
std::pair<std::string,std::string> handoff_version(const Json& row) {
    return {row.at("id").get<std::string>(),row.value("resultVersion","")};
}
HandoffVersions replaced_versions(const Json& rows) {
    HandoffVersions versions;
    for (const auto& row:rows) if (row.value("repairQueued",false) && !archived_dispatch(row)) {
        for (const auto& parent:repair_links(row))
            versions.emplace(parent.at("sessionId").get<std::string>(),parent.at("resultVersion").get<std::string>());
    }
    return versions;
}
bool pending_handoff(const Json& row,const HandoffVersions& replaced) {
    return result_visible(row) && !active(row) && !row.value("resultVersion","").empty() && !row.value("processed",false) &&
        (failed_handoff(row) || !row.contains("repairOf") || row.value("repairQueued",false)) &&
        !replaced.contains(handoff_version(row));
}
bool review_repair_needed(const Json& row) {
    const auto phase=review_state(row);
    return phase=="needs_repair" ||
        (phase=="blocked" && row.at("precheck").value("code","")=="candidate_unavailable");
}
bool handoff_candidate(const Json& row,const HandoffVersions& replaced) {
    if (row.value("resultVersion","").empty()) return false;
    return pending_handoff(row,replaced) || (review_repair_needed(row) && !active(row) &&
        !row.value("processed",false) && !replaced.contains(handoff_version(row)));
}
auto handoff_order(const Json& row) {
    return std::pair(std::max({row.value("completed",std::int64_t(0)),row.value("repairQueuedAt",std::int64_t(0)),
        row.value("precheck",Json::object()).value("readyAt",std::int64_t(0))}),
        row.at("id").get<std::string>());
}
bool launched_repair(const Json& child) {
    return child.contains("dshId") && child.contains("startCursor") && !child.value("processed",false) &&
        child.value("status","")!="starting" && (!dispatch_pending(child) || child.value("repairQueued",false));
}
void record_repair_link(Json& child,const Json& parent,const Json& request) {
    auto links=repair_links(child);
    if (std::ranges::none_of(links,[&](const Json& link) {
        return link.at("sessionId")==parent.at("sessionId") && link.at("resultVersion")==parent.at("resultVersion");
    })) links.push_back(parent);
    if (!child.contains("repairOf")) child["repairOf"]=parent;
    child["repairLinks"]=std::move(links);
    child["repairQueued"]=true;
    if (!child.contains("repairQueuedAt")) child["repairQueuedAt"]=now();
    if (request.is_object()) {
        auto requests=repair_requests(child);
        const Json entry{{"input",request},{"parentSessionId",parent.at("sessionId")}};
        if (std::ranges::find(requests,entry)==requests.end()) requests.push_back(entry);
        child["repairRequests"]=std::move(requests);
        child.erase("repairRequest");
    }
}
bool repair_cycle(const Json& rows,const std::string& parent,const std::string& child) {
    std::set<std::string> seen;
    std::vector<std::string> pending{parent};
    while (!pending.empty()) {
        auto current=std::move(pending.back());
        pending.pop_back();
        if (current==child) return true;
        if (!seen.insert(current).second) continue;
        const auto row=rows.find(current);
        if (row!=rows.end()) for (const auto& link:repair_links(*row))
            pending.push_back(link.at("sessionId").get<std::string>());
    }
    return false;
}
void release_replaced_claim(Json& queue,const Json& parent) {
    const auto claim=queue.value("claim",Json::object());
    if (claim.value("sessionId",Json())!=parent.at("sessionId") ||
        claim.value("resultVersion",Json())!=parent.at("resultVersion")) return;
    queue.erase("claim");
    queue.erase("nextToken");
}
bool migrate_repair(const fs::path& data,Json& queue,const std::string& queue_id,const std::string& legacy) {
    if (!queue.contains("repair")) return false;
    const auto old=queue.at("repair");
    Database db(data,true);
    auto& rows=db.state.at("sessions");
    const auto child=rows.find(old.at("sessionId").get<std::string>());
    if (child!=rows.end() && in_queue(*child,queue_id,legacy) && launched_repair(*child)) {
        const auto parent=old.value("reference",Json{{"sessionId",old.at("parentId")},{"resultVersion",old.at("parentVersion")}});
        record_repair_link(*child,parent,old.value("request",Json()));
        db.save();
        release_replaced_claim(queue,parent);
    }
    else if (child!=rows.end() && in_queue(*child,queue_id,legacy) && dispatch_pending(*child)) return false;
    queue.erase("repair");
    return true;
}

Json session_overview(const Json& settings,const Json& sessions,const LegacySessions& legacy,const fs::path& data,const Json& claim) {
    int running=0, pending=0, processed=0, claimed=0;
    const bool has_claim=claim.value("expiresAt",std::int64_t(0))>now();
    const auto claimed_version=std::pair(claim.value("sessionId",""),claim.value("resultVersion",""));
    const auto replaced=replaced_versions(sessions);
    for (const auto& row:sessions) {
        if (active(row)) ++running;
        else if (row.value("processed",false)) ++processed;
        else if (pending_handoff(row,replaced)) {
            ++pending;
            if (has_claim && handoff_version(row)==claimed_version) ++claimed;
        }
    }
    return {{"active",running},{"unprocessed",pending},{"processed",processed},{"claimed",claimed},{"accounts",settings.at("accounts").size()},
        {"models",settings.at("pools").size()},{"connected",!settings.at("authPath").get<std::string>().empty()},
        {"codexSessions",legacy.rows.size()},{"codexSources",legacy.sources},{"unreadableCodexSources",legacy.unreadable},
        {"usage",telemetry_overview(sessions,legacy_usage_cache(data))}};
}
std::pair<int,int> refresh_desktop_native(const fs::path& data,DesktopMonitor& monitor,std::stop_token stop,
                                          const std::string& queue,const std::string& legacy) {
    std::vector<std::pair<std::int64_t,std::string>> candidates;
    {
        Database db(data,false);
        const auto& rows=db.state.at("sessions");
        std::erase_if(monitor.native_attempts,[&](const auto& item) {return !rows.contains(item.first);});
        for (const auto& row:rows) {
            if (!in_queue(row,queue,legacy) || !active(row) || (!row.contains("startCursor") && row.value("status","")!="starting")) continue;
            const auto key=row.at("id").get<std::string>();
            candidates.emplace_back(monitor.native_attempts[key],key);
        }
    }
    std::sort(candidates.begin(),candidates.end());
    if (candidates.size()>24) candidates.resize(24);
    std::atomic_size_t next{};
    std::atomic_int checked{},failed{};
    const auto deadline=GetTickCount64()+3000;
    std::vector<std::jthread> workers;
    for (std::size_t i=0;i<std::min<std::size_t>(8,candidates.size());++i) {
        workers.emplace_back([&] {
            while (!stop.stop_requested() && GetTickCount64()<deadline) {
                const auto index=next.fetch_add(1);
                if (index>=candidates.size()) return;
                try {refresh(data,candidates[index].second,1100,false); ++checked;}
                catch (const std::exception&) {++failed;}
            }
        });
    }
    for (auto& worker:workers) worker.join();
    for (std::size_t i=0;i<std::min(next.load(),candidates.size());++i)
        monitor.native_attempts.at(candidates[i].second)=now();
    return {checked.load(),failed.load()};
}
}

void Cluster::migrate_repair_queue() const {
    const auto file=handoff_file();
    if (!fs::exists(file)) return;
    const ExclusiveStateLock lock(handoff_file(true));
    auto queue=read_json(file);
    if (migrate_repair(data_,queue,queue_id_,legacy_queue_id_)) write_state_file(file,queue);
}

Json Cluster::mark_desktop_failed(const std::string& key) const {
    require_open_queue();
    const ExclusiveStateLock lock(prompt_lock_file(data_,key));
    Json prior,profiles;
    {
        Database db(data_,false);
        prior=db.state.at("sessions").at(key);
        require(in_queue(prior,queue_id_,legacy_queue_id_) && active(prior),"只能标记当前队列的活动会话");
        profiles=dsh_profiles(db.state);
    }
    std::string message="用户手动标记为失败，请检查后重新派发";
    if (prior.contains("dshId")) {
        try {
            const auto dsh=session_connection(profiles,prior,1500,3000);
            dsh.request("session/cancel",{{"sessionId",prior.at("dshId")}});
        } catch (const std::exception&) {
            message+="；未能确认远端取消，重新派发前请核对 DSH 会话";
        }
    }
    Database db(data_,true);
    auto& row=db.state.at("sessions").at(key);
    require(active(row),"会话状态已变化，请刷新后重试");
    fail_dispatch(row,"manual_failure",message);
    db.save();
    return public_row(row);
}

Json Cluster::desktop_snapshot(DesktopMonitor* monitor,std::stop_token stop) const {
    Json settings,legacy_settings,sessions,queues=Json::array();
    LegacyFilter include;
    std::pair<int,int> native{};
    std::jthread native_worker;
    std::exception_ptr native_error;
    {
        Database db(data_,false);
        settings=db.state.at("settings");
        legacy_settings=legacy_connection_settings(db.state);
        for (const auto& row:db.state.at("queues")) if (!row.contains("deletedAt")) queues.push_back(queue_info(row));
        include=legacy_filter(db.state,queue_id_,legacy_queue_id_);
        if (!monitor) sessions=queue_sessions(db.state,queue_id_,legacy_queue_id_);
    }
    if (monitor) {
        recover_desktop_dsh(settings,stop);
        native_worker=std::jthread([&] {
            try { native=refresh_desktop_native(data_,*monitor,stop,queue_id_,legacy_queue_id_); }
            catch (...) { native_error=std::current_exception(); }
        });
    }
    const auto legacy=read_legacy_sessions(legacy_settings,false,monitor?&monitor->legacy:nullptr,stop,LegacyRefresh::incremental,include);
    if (monitor) {
        native_worker.join();
        if (native_error) std::rethrow_exception(native_error);
        Database db(data_,false);
        settings=db.state.at("settings");
        sessions=queue_sessions(db.state,queue_id_,legacy_queue_id_);
    }
    if (monitor && !legacy.rows.empty()) {
        Database db(data_,true);
        const auto changed=record_external_observations(db.state,queue_id_,legacy.rows);
        const auto before=db.state.value("workExternalSessions",Json::object());
        observe_work_sessions(db.state,queue_id_,legacy.rows);
        if (changed || before!=db.state.at("workExternalSessions")) db.save();
    }
    sessions.update(legacy.rows);
    const auto replaced=replaced_versions(sessions);
    const auto queue_file=handoff_file();
    const auto claim=fs::exists(queue_file)?read_json(queue_file).value("claim",Json::object()):Json::object();
    const bool has_claim=claim.value("expiresAt",std::int64_t(0))>now();
    const auto claimed_version=std::pair(claim.value("sessionId",""),claim.value("resultVersion",""));
    std::vector<Json> running,pending,processed;
    for (const auto& row:sessions) {
        if (active(row) || (!result_visible(row) && !replaced.contains(handoff_version(row))))
            running.push_back(public_row(row));
        else if (pending_handoff(row,replaced)) {
            auto entry=public_row(row);
            if (has_claim && claimed_version==handoff_version(row)) entry["handoffClaimed"]=true;
            pending.push_back(std::move(entry));
        } else if (row.value("processed",false)) processed.push_back(public_row(row));
    }
    const auto order=[](const Json& row,const char* stamp) {
        return std::pair(row.value(stamp,std::int64_t(0)),row.at("id").get<std::string>());
    };
    std::sort(running.begin(),running.end(),[&](const Json& a,const Json& b) {return order(a,"updated")>order(b,"updated");});
    std::sort(pending.begin(),pending.end(),[](const Json& a,const Json& b) {return handoff_order(a)<handoff_order(b);});
    std::sort(processed.begin(),processed.end(),[&](const Json& a,const Json& b){return order(a,"completed")>order(b,"completed");});
    return {{"overview",session_overview(settings,sessions,legacy,data_,claim)},{"settings",settings},
        {"running",std::move(running)},{"pending",std::move(pending)},{"processed",std::move(processed)},
        {"queueId",queue_id_},{"queues",std::move(queues)},
        {"liveChecked",legacy.live_checked+native.first},{"liveStale",legacy.live_stale+native.second}};
}

void Cluster::refresh_usage(std::stop_token stop) const {
    const auto day=midnight(), observed=now();
    std::vector<std::string> native;
    {
        Database db(data_,false);
        for (const auto& [key,row]:db.state.at("sessions").items()) {
            if (!row.contains("startCursor") || (!active(row) && row.value("completed",std::int64_t(0))<day)) continue;
            if (row.value("tokenDay",std::int64_t(0))!=day || !row.contains("todayUsage") ||
                row.value("tokenCursor",Json())!=row.value("cursor",Json())) native.push_back(key);
        }
    }
    std::atomic_size_t next_native{};
    std::vector<std::jthread> native_workers;
    for (std::size_t i=0;i<std::min<std::size_t>(4,native.size());++i) {
        native_workers.emplace_back([&] {
            while (!stop.stop_requested()) {
                const auto index=next_native.fetch_add(1);
                if (index>=native.size()) return;
                try {refresh(data_,native[index],1500,true,stop);}
                catch (const std::exception&) {}
            }
        });
    }
    const auto settings=[&] { Database db(data_,false); return legacy_connection_settings(db.state); }();
    const auto legacy=read_legacy_sessions(settings,true);
    if (legacy.rows.empty()) return;
    const auto connection=discover(settings,Json::object());
    if (connection.auth.empty()) return;
    const Dsh dsh(connection.auth,2500);
    const auto cache=legacy_usage_cache(data_);
    struct Job {
        std::string id;
        const Json* row;
        const Json* candidate;
        Json sample;
    };
    std::vector<Job> jobs;
    for (const auto& [id,row]:legacy.rows.items()) {
        const auto& candidate=legacy.usage_candidates.at(id);
        if (candidate.at("origin")!=dsh.origin()) continue;
        const auto old=cache.find(id);
        if (old!=cache.end() && !active(row) && !old->value("stale",false) &&
            old->value("observed",std::int64_t(0))>=observed-5*60*1000 &&
            old->value("updated",std::int64_t(0))==row.at("updated") &&
            old->value("tokenDay",std::int64_t(0))==day && old->contains("todayUsage")) continue;
        jobs.push_back({id,&row,&candidate,old==cache.end()?Json::object():*old});
    }
    std::sort(jobs.begin(),jobs.end(),[](const Job& a,const Job& b){return a.row->at("updated")>b.row->at("updated");});
    std::atomic_size_t next{};
    std::vector<std::jthread> fetchers;
    for (std::size_t i=0;i<std::min<std::size_t>(6,jobs.size());++i) {
        fetchers.emplace_back([&] {
            while (!stop.stop_requested()) {
                const auto index=next.fetch_add(1);
                if (index>=jobs.size()) return;
                auto& job=jobs[index];
                const auto& row=*job.row;
                const auto& candidate=*job.candidate;
                try {
                    const auto projections=dsh.request("session/projections",{{"sessionId",candidate.at("dshId")}});
                    const auto counts=token_counts(projections.at("/values/tokenUsage"_json_pointer));
                    job.sample["usage"]=token_totals(counts);
                    job.sample["observed"]=observed;
                    job.sample["updated"]=row.at("updated");
                    TokenCounts today_counts{};
                    if (candidate.value("first",std::int64_t(0))>=day) today_counts=counts;
                    else {
                        // The live cursor covers turns that resumed after a registry snapshot.
                        const auto state=dsh.request("session/state",{{"sessionId",candidate.at("dshId")}});
                        const auto cursor=state.at("cursor").get<std::int64_t>();
                        if (cursor>=0) today_counts=day_usage(dsh,row,cursor,day,stop);
                    }
                    job.sample["tokenDay"]=day;
                    job.sample["todayUsage"]=token_totals(today_counts);
                    job.sample["stale"]=false;
                } catch (const std::exception&) {
                    job.sample["stale"]=true;
                }
            }
        });
    }
    for (auto& thread:fetchers) thread.join();
    if (stop.stop_requested() || jobs.empty()) return;
    const ExclusiveStateLock lock(data_/L"legacy-usage.lock");
    auto updated=legacy_usage_cache(data_);
    for (auto& job:jobs) {
        if (updated.contains(job.id) && updated.at(job.id).value("observed",std::int64_t(0))>observed) continue;
        updated[job.id]=std::move(job.sample);
    }
    for (auto it=updated.begin();it!=updated.end();) {
        if (!legacy.rows.contains(it.key())) it=updated.erase(it);
        else ++it;
    }
    write_state_file(data_/L"legacy-usage.json",updated);
}

Json Cluster::start_session(const Json& input,const std::string& key,const Json& repair,const Json& operation) const {
    if (!operation.is_null()) {
        Json replay;
        {
            Database db(data_,false);
            replay=dispatch_request(db.state.at("sessions"),operation,queue_id_,legacy_queue_id_);
        }
        if (!replay.is_null()) return reconcile_dispatch(data_,std::move(replay));
    }
    const auto title = field(input,"title",400), batch = field(input,"batch",400);
    const auto role=input.contains("role")?field(input,"role",32):"developer", task=field(input,"task",input.contains("teamProjectId")?128000:64000);
    require(roles.contains(role), "未知角色");
    const auto work = path_from_utf8(field(input,"workspace",4000));
    require(work.is_absolute() && fs::is_directory(work) && work.has_relative_path(), "工作区必须是现有的绝对子目录");
    const auto workspace = fs::canonical(work);
    require(workspace.has_relative_path(), "工作区不能指向磁盘根目录");
    const auto settings=invoke("cluster_get_settings");
    const auto auth=dispatch_auth(settings);
    auto resolved=settings;
    resolved["authPath"]=utf8(auth);
    require_ready_dsh(resolved);
    // Team roles share all enabled models; ignore legacy role pool references.
    // Dispatch replay above is intentionally unchanged: never move an existing session.
    const auto pinned=!input.contains("teamProjectId") && input.contains("poolId")?field(input,"poolId",100):std::string();
    const auto requested=batch_allowlist(input);
    const Dsh dsh(auth,3500,12000);
    CatalogModels models;
    try {models=catalog_models(dsh.request("session/modelCatalog"));}
    catch (const DshError& error) {
        auto details=dsh_error_diagnostics(error);
        details["cause"]=details.at("reason");
        details["reason"]="catalog_unavailable";
        details["availablePools"]=Json::array();
        throw DispatchError("无法读取 DSH 实时模型目录",std::move(details));
    }
    catch (const std::exception&) {
        throw DispatchError("无法读取 DSH 实时模型目录",{{"reason","catalog_unavailable"},{"availablePools",Json::array()}});
    }
    LegacyObservations observations;
    const auto legacy_settings=[&] { Database db(data_,false); return legacy_connection_settings(db.state); }();
    const auto legacy=read_legacy_sessions(legacy_settings,false,&observations);
    Json pool,row,diagnostics;
    std::map<std::string,std::string> rejected;
    const auto reserve=[&](const std::string& cooling_account={}) -> Json {
        Database db(data_, true);
        const auto& current=db.state.at("settings");
        require(dsh_profile_id(current)==dsh_profile_id(settings) && dsh_profile_fields(current)==dsh_profile_fields(settings) &&
            dispatch_auth(current)==auth,"DSH 连接设置已变化，请重新派发");
        auto& sessions = db.state["sessions"];
        if (row.empty()) {
            auto replay=dispatch_request(sessions,operation,queue_id_,legacy_queue_id_);
            if (!replay.is_null()) return replay;
            require(!sessions.contains(key),"此派发请求已存在");
        }
        auto occupied = sessions;
        occupied.erase(key);
        occupied.update(legacy.rows);
        check_team_reservation(data_,occupied,input);
        for (const auto& value : occupied) {
            if (active(value)) require(_wcsicmp(widen(value.at("workspace").get<std::string>()).c_str(),
                workspace.c_str()) != 0, "该工作区已有活动会话");
        }
        auto& queue=db.state.at("queues").at(queue_id_);
        require(!queue.contains("deletedAt"),"该队列已删除");
        auto& batches=queue_id_==legacy_queue_id_?db.state["batchPools"]:queue["batchPools"];
        auto allowed=requested;
        if (batches.contains(batch)) {
            allowed=batches.at(batch).get<std::set<std::string>>();
            if (!requested.empty() && requested!=allowed)
                throw DispatchError("此批次已有允许池清单",{{"reason","batch_allowlist_conflict"},{"allowedPoolIds",allowed}});
        }
        for (const auto& pool_id:allowed)
            if (!current.at("pools").contains(pool_id))
                throw DispatchError("批次清单包含未知池",{{"reason","unknown_pool"},{"poolId",pool_id}});
        auto& cooldowns=dsh_profile_id(settings)=="default"?db.state["accountCooldowns"]:
            db.state["dshProfileCooldowns"][dsh_profile_id(settings)];
        if (cooldowns.is_null()) cooldowns=Json::object();
        if (!cooling_account.empty()) {
            cooldowns[cooling_account]=now()+60000;
            db.save();
        }
        auto choice=choose_pool(current,occupied,models,allowed,pinned,cooldowns,rejected,input.contains("teamProjectId"));
        diagnostics=std::move(choice.diagnostics);
        if (choice.key.empty()) throw DispatchError(pool_failure(diagnostics,pinned),diagnostics);
        if (!allowed.empty()) batches[batch]=allowed;
        pool=current.at("pools").at(choice.key);
        if (row.empty()) row={{"id",key},{"title",title},{"role",role},{"batch",batch},{"workspace",utf8(workspace)},
            {"status","starting"},{"created",now()},{"resultVersion",""},{"read",false},{"processed",false}};
        row.update({{"poolId",choice.key},{"provider",pool.at("provider")},{"model",pool.at("model")},{"updated",now()},{"queueId",queue_id_}});
        for (const auto* member:{"teamProjectId","teamDomainId","teamJobId","teamCharterVersion"})
            if (input.contains(member)) row[member]=input.at(member);
        row["dshProfile"]=dsh_profile_id(settings);
        row["dshAuthPath"]=utf8(auth);
        if (!repair.is_null()) row["repairOf"]=repair;
        const auto operation_kind=operation.is_object()?operation.value("kind",""):"";
        if (operation_kind=="precheck") row["precheckFor"]=operation.at("source");
        if (!operation.is_null()) {
            row["dispatchOperation"]=operation;
            row["requestId"]=operation.at("requestId");
        }
        if (role=="lead" && row.contains("teamProjectId")) {
            row["workItemId"]="team-lead-"+key;
            row["taskRef"]="";
        } else register_work_session(db.state,row,input,queue_id_);
        row["dispatchState"]="starting";
        remember_workspace(queue,utf8(workspace));
        queue["updated"]=row.at("updated");
        sessions[key]=row;
        synchronize_work_items(db.state);
        if (role!="lead" || !row.contains("teamProjectId"))
            row["taskRef"]=publish_work_item_spec(data_,db.state.at("workItems").at(row.at("workItemId").get<std::string>()));
        sessions[key] = row;
        db.save();
        return nullptr;
    };
    std::optional<ExclusiveStateLock> prompt_lock;
    prompt_lock.emplace(prompt_lock_file(data_,key));
    if (auto replay=reserve(); !replay.is_null()) {
        prompt_lock.reset();
        return reconcile_dispatch(data_,std::move(replay));
    }
    bool may_have_dispatched = false;
    std::string stage="create_session";
    try {
        Json create{{"cwd",utf8(workspace)}};
        create["agentPreset"]="ptc";
        const auto created = dsh.request("session/create", create);
        const auto sid = field(created, "sessionId", 200);
        row["dshId"] = sid;
        row["owner"] = dsh.owner();
        if (created.contains("agentPreset"))
            require(created.at("agentPreset") == "ptc", "DSH 未保留 Agent 预设");
        stage="permission_preset";
        const std::string permission="danger-full-access";
        const auto applied = dsh.call("commands/execute", {{"agentId",sid},{"line","/permission " + permission},
            {"submittedAttachments",Json::array()}});
        require(applied.value("result", applied).value("kind","") == "success", "DSH 没有应用权限预设");
        bool verified = false;
        for (int attempt = 0; attempt < 8 && !verified; ++attempt) {
            const auto value = dsh.request("session/projections", {{"sessionId",sid}});
            const auto path = "/values/permissions/currentValue"_json_pointer;
            verified = value.contains(path) && value.at(path) == permission;
            if (!verified) Sleep(100);
        }
        require(verified, "DSH 权限预设复核失败");
        stage="select_model";
        for (;;) {
            const auto provider=pool.at("provider").get<std::string>(),model=pool.at("model").get<std::string>();
            const auto reasoning=models.at({provider,model}).value("reasoning",Json::object());
            const auto efforts=reasoning.is_object()?reasoning.value("efforts",Json::array()):Json::array();
            const auto supports=[&](const std::string& value) {
                return !value.empty() && std::ranges::any_of(efforts,[&](const Json& item) {
                    return item.is_object() && item.value("id","")==value;
                });
            };
            auto effort=pool.at("effort").get<std::string>();
            if (!supports(effort)) effort=reasoning.is_object()?reasoning.value("defaultEffort",""):"";
            if (!supports(effort)) effort.clear();
            Json select{{"sessionId",sid},{"provider",provider},{"model",model}};
            if (!effort.empty()) select["reasoningEffort"]=effort;
            try {
                const auto result=dsh.request("session/selectModel",select);
                const auto& selected=result.at("selected");
                require(selected.at("provider")==provider && selected.at("model")==model &&
                    (effort.empty() || selected.value("reasoningEffort","")==effort),"DSH 所选模型或思考强度与配置不符");
                const auto reported_effort=selected.find("reasoningEffort");
                row["effort"]=reported_effort!=selected.end() && reported_effort->is_string()?reported_effort->get<std::string>():effort;
                break;
            } catch (const DshError& error) {
                const auto reason=selection_rejection(error);
                if (reason.empty()) throw;
                rejected[row.at("poolId").get<std::string>()]=reason;
                reserve(reason=="account_cooldown"?provider:"");
            }
        }
        if (role=="lead" && input.contains("teamGoal")) {
            stage="set_goal";
            const auto user_objective=normalized_goal_objective(field(input,"teamGoal",12000));
            require(!user_objective.empty(),"项目经理goal目标不可为空");
            stage="prepare_manager_bootstrap";
            const auto bootstrap=workspace/L"manager-bootstrap.md";
            const auto content=task+"\n\nComplete charter and task plan\n"+input.value("teamBootstrapContext",Json::object()).dump(2);
            std::ofstream document(bootstrap,std::ios::binary|std::ios::trunc);
            document.write(content.data(),static_cast<std::streamsize>(content.size()));document.close();
            require(bool(document),"无法保存完整项目经理启动文档，禁止激活goal");
            const auto objective=std::string("You are the only project manager: dispatch developers, arrange independent review, perform final acceptance and coordinate repairs. Do not modify product code or write audit scripts.\n")+
                "First read the bootstrap document below for the complete instructions, current charter, task plan and credentials. Do not begin product work before reading it.\nBootstrap document: "+
                utf8(bootstrap)+"\nImmediately call team_domain_context, then team_dispatch_ready to queue every ready task from the principal's plan. Do not re-split the plan, dispatch just one independent item and wait, or implement changes yourself. Developers must build/test before review; you test only after review passes. Use MCP waits exclusively.\nProject objective: "+user_objective;
            // The first goal round may start before commands/execute returns.
            // Persist everything it needs and capture the cursor BEFORE activation.
            const auto before=rpc_state(dsh,row);
            row["startCursor"]=before.at("cursor");row["cursor"]=before.at("cursor");
            row["managerBootstrapPath"]=utf8(bootstrap);row["managerObjective"]=user_objective;
            {Database db(data_,true);db.state["sessions"][key]=row;db.save();}
            stage="set_goal";
            // One mutation only. Projection propagation is allowed a bounded read-only wait.
            const Dsh goal_connection(auth,1000,5000);
            const auto set=goal_connection.call("commands/execute",{{"agentId",sid},{"line","/goal "+objective},{"submittedAttachments",Json::array()}});
            require(set.value("result",set).value("kind","")=="success","DSH 未创建项目经理 goal");
            Json observed;std::string transient;
            int attempts=0;
            for (;attempts<20;++attempts) {
                try {
                    observed=goal_snapshot(goal_connection.request("session/projections",{{"sessionId",sid}}));
                    transient.clear();
                    if (goal_matches(observed,objective)) break;
                } catch (const DshError& error) {
                    if (!dsh_error_diagnostics(error).value("retryable",false)) throw;
                    transient=error.code;
                    if (error.code=="deadline") break;
                }
                if (attempts<19) Sleep(100);
            }
            if (!goal_matches(observed,objective)) {
                const auto phase=observed.is_null()?"missing":observed.value("phase","missing");
                const bool matches=!observed.is_null() && normalized_goal_objective(observed.at("objective").get<std::string>())==objective;
                throw std::runtime_error("DSH 项目经理 goal 回读校验失败（已有限轮询；phase="+phase+
                    ", objectiveMatches="+(matches?std::string("true"):std::string("false"))+", transport="+(transient.empty()?"none":transient)+"）");
            }
            row["goal"]=observed;
            row["status"]="running";row["dispatchState"]="confirmed";
            // /goal is the sole execution trigger for a manager. A later queued
            // prompt could remain unread for the entire goal and must not exist.
        } else {
            stage="session_state";
            const auto live = rpc_state(dsh, row);
            row["startCursor"] = live.at("cursor");
            row["cursor"] = live.at("cursor");
            row["dispatchState"]="submitting";
            const auto task_context="Task ledger (read-only metadata): workItemId="+row.at("workItemId").get<std::string>()+
                "; original task and issue definitions: "+row.at("taskRef").get<std::string>()+". Read that metadata as needed; "
                "code changes remain restricted to the assigned workspace. Base SHA: "+row.value("baseSha",std::string("unspecified"))+".\n\n";
            const auto prompt = role=="lead"?task:(review_worker(row)?task:task_context+"Work in the assigned workspace only. Do not change provider/model, start nested agents, "
                "or publish changes without authorization. Return verifiable evidence. In the final answer include a fenced "
                "```fatfish-delivery JSON block with workItemId, baseSha (full commit hash or null), candidateSha (full commit hash or null), "
                "status (passed/failed/blocked), issues, nextAction, optional checksManifest and evidenceManifest, worktreeClean (boolean), "
                "tests (array of {command,exitCode}), and evidencePaths (array of local absolute paths). "
                "Only report tests you actually ran.\n\n" + task);
            const Json request{{"sessionId",sid},{"requestId",key},{"mode","queue"},
                {"content",Json::array({{{"type","text"},{"text",prompt}}})}};
            row["pendingPrompt"]={{"request",request},{"retryAt",now()+15000}};
            {
                Database db(data_, true);
                db.state["sessions"][key] = row;
                db.save();
            }
            stage="submit_prompt";
            const Dsh submission(auth,3500);
            require(submission.matches_owner(dsh.owner()),"没有连接到该会话最初所属的 DSH 服务");
            may_have_dispatched = true;
            const auto reply = submission.request("session/prompt",request);
            may_have_dispatched = reply.value("accepted",false);
            require(may_have_dispatched, "DSH 拒绝了任务");
            row["status"] = "queued";
            row["dispatchState"]="confirmed";
            row.erase("pendingPrompt");
        }
        if (!rejected.empty()) row["poolFallbacks"]=rejected;
    } catch (const std::exception& e) {
        // /goal may arm a round before the task prompt. A rejected setup must
        // not leave an orphan manager running in its newly created session.
        Json goal_cleanup;
        if (stage=="set_goal" && row.contains("dshId")) {
            goal_cleanup={{"pauseConfirmed",false},{"cancelConfirmed",false}};
            const Dsh cleanup(auth,1000,3500);
            try {
                const auto paused=cleanup.call("commands/execute",{{"agentId",row.at("dshId")},{"line","/goal pause"},{"submittedAttachments",Json::array()}});
                goal_cleanup["pauseConfirmed"]=paused.value("result",paused).value("kind","")=="success";
            } catch (const std::exception&) {}
            try {
                cleanup.request("session/cancel",{{"sessionId",row.at("dshId")}});
                const auto state=cleanup.request("session/state",{{"sessionId",row.at("dshId")}});
                goal_cleanup["cancelConfirmed"]=!state.value("running",true) && state.value("queue",Json::array()).empty() && state.value("pendingInput",Json::array()).empty();
            } catch (const std::exception&) {}
        }
        row["status"] = may_have_dispatched ? "unknown" : "failed";
        row["dispatchState"] = may_have_dispatched ? "unconfirmed" : "failed";
        row["error"] = e.what();
        row["dispatch"]={{"stage",stage},{"diagnostics",diagnostics},{"mayHaveDispatched",may_have_dispatched}};
        if (!goal_cleanup.is_null()) {
            row["dispatch"]["goalCleanup"]=goal_cleanup;
            if (goal_cleanup.at("pauseConfirmed")!=true || goal_cleanup.at("cancelConfirmed")!=true)
                row["error"]=row.at("error").get<std::string>()+"；新建goal的停止状态未完全确认，须核对该DSH会话";
        }
        row["failureStage"]=stage;
        row["retryMode"]=may_have_dispatched?"same_request":"new_request";
        row["retryable"]=may_have_dispatched;
        row["retryAfterMs"]=may_have_dispatched?2000:0;
        if (const auto* failure=dynamic_cast<const DshError*>(&e)) {
            const auto details=dsh_error_diagnostics(*failure);
            row["retryable"]=details.at("retryable");
            row["retryAfterMs"]=details.at("retryAfterMs");
            row["dispatch"]["code"]=failure->code;
            const auto reason=selection_rejection(*failure);
            if (!reason.empty()) row["dispatch"]["reason"]=reason;
        }
        if (may_have_dispatched) row.at("pendingPrompt")["retryAt"]=now()+2000;
        else {
            row["resultVersion"] = "dispatch:failed";
            row["completed"] = now();
            row.erase("startCursor");
            row.erase("pendingPrompt");
        }
    }
    Json result;
    {
        Database db(data_, true);
        auto& stored = db.state["sessions"].at(key);
        if (stored.at("status")=="starting" || row.at("status")=="failed" ||
            (stored.at("status")=="unknown" && dispatch_pending(stored) && row.at("status")=="unknown")) {
            stored = row;
            db.save();
        } else if (row.value("dispatchState","")=="confirmed" || row.contains("poolFallbacks") ||
                   (dispatch_pending(stored) && row.value("dispatchState","")=="unconfirmed")) {
            if (row.value("dispatchState","")=="confirmed") {
                if (row.contains("managerBootstrapPath")) {
                    for (const auto* field:{"goal","managerBootstrapPath","managerObjective"}) stored[field]=row.at(field);
                }
                if (dispatch_pending(stored)) stored["status"]=row.contains("managerBootstrapPath")?"running":"queued";
                stored["dispatchState"]="confirmed";
                clear_dispatch_diagnostics(stored);
                stored.erase("pendingPrompt");
            }
            if (dispatch_pending(stored) && row.value("dispatchState","")=="unconfirmed")
                stored.update({{"dispatchState","unconfirmed"},{"dispatch",row.at("dispatch")},{"error",row.at("error")}});
            if (row.contains("poolFallbacks")) stored["poolFallbacks"]=row.at("poolFallbacks");
            db.save();
        }
        result=public_row(stored);
    }
    prompt_lock.reset();
    return reconcile_dispatch(data_,std::move(result),false);
}

namespace {
Json parse_precheck(const Json& messages,const Json& sha) {
    for (auto it=messages.rbegin();it!=messages.rend();++it) {
        const auto kind=it->value("type","");
        if (kind!="assistant/message" && kind!="assistant/response") continue;
        const auto output=it->value("text","");
        constexpr std::string_view marker="\x60\x60\x60" "fatfish-review";
        const auto begin=output.rfind(marker);
        if (begin==std::string::npos) continue;
        const auto after=begin+marker.size(), end=output.find("\x60\x60\x60",after);
        if (end==std::string::npos || end-after>5500) return nullptr;
        const auto value=Json::parse(std::string_view(output).substr(after,end-after),nullptr,false);
        if (!value.is_object() || value.value("candidateSha",Json())!=sha) return nullptr;
        const auto verdict=value.value("verdict",Json());
        if (verdict=="pass") {
            const auto checks=value.value("checks",Json());
            if (checks.is_array() && !checks.empty() && checks.size()<=8 &&
                std::ranges::all_of(checks,[](const Json& check) {
                    return check.is_string() && !check.get_ref<const std::string&>().empty() &&
                        check.get_ref<const std::string&>().size()<=400;
                })) return {{"state","passed"},{"readyAt",now()},{"checks",checks}};
            return nullptr;
        }
        if (verdict!="fail") return nullptr;
        const auto findings=value.value("findings",Json());
        if (!findings.is_array() || findings.empty() || findings.size()>6) return nullptr;
        Json trimmed=Json::array();
        for (const auto& finding:findings) {
            if (!finding.is_object()) return nullptr;
            Json item=Json::object();
            for (const auto* key:{"issue","evidence","acceptance"}) {
                const auto v=finding.value(key,Json());
                if (!v.is_string() || v.get_ref<const std::string&>().empty() ||
                    v.get_ref<const std::string&>().size()>500) return nullptr;
                item[key]=v;
            }
            trimmed.push_back(std::move(item));
        }
        return {{"state","needs_repair"},{"findings",std::move(trimmed)}};
    }
    return nullptr;
}
}

void Cluster::run_prechecks(std::stop_token stop) const {
    while (!stop.stop_requested()) {
        try {advance_teams(*this,data_,stop);} catch (const std::exception&) {diagnostics::caught();}
        try {
            std::vector<std::string> stale;
            {
                Database db(data_,false);
                for (const auto& row:db.state.at("sessions"))
                    if (row.value("status","")=="starting" && !row.contains("startCursor") &&
                        now()-row.value("updated",now())>120000)
                        stale.push_back(row.at("id").get<std::string>());
            }
            for (const auto& key:stale) {
                if (stop.stop_requested()) break;
                try {refresh(data_,key,1100,false,stop);} catch (const std::exception&) {diagnostics::caught();}
            }
            Json due;
            {
                Database db(data_,false);
                const auto time=now();
                for (const auto& row:db.state.at("sessions")) {
                    if (!prompt_retry_due(row,time) || !open_queue(db.state,row.at("queueId").get<std::string>())) continue;
                    if (due.is_null() || row.at("pendingPrompt").at("retryAt")<due.at("dispatchRetryAt"))
                        due=public_row(row);
                }
            }
            if (!stop.stop_requested() && !due.is_null()) reconcile_dispatch(data_,std::move(due));
            std::pair<std::string,std::string> message;
            {
                Database db(data_,false);
                auto earliest=std::numeric_limits<std::int64_t>::max();
                for (const auto& row:db.state.at("sessions")) {
                    if (!row.contains("messageOutbox")) continue;
                    for (const auto& [request,entry]:row.at("messageOutbox").items())
                        if (message_due(entry,now()) && entry.value("retryAt",std::int64_t(0))<earliest) {
                            earliest=entry.value("retryAt",std::int64_t(0));
                            message={row.at("id").get<std::string>(),request};
                        }
                }
            }
            if (!stop.stop_requested() && !message.first.empty()) deliver_message(data_,message.first,message.second);
        } catch (const std::exception&) {diagnostics::caught();}
        if (stop.stop_requested()) break;
        for (int i=0;i<20 && !stop.stop_requested();++i) Sleep(100);
    }
}

void Cluster::advance_prechecks(std::stop_token stop) const {
    std::vector<std::pair<std::string,std::string>> work;
    {
        Database db(data_,false);
        const auto& rows=db.state.at("sessions");
        const auto replaced=replaced_versions(rows);
        for (const auto& row:rows) {
            if (!row.contains("precheck") || failed_handoff(row) || row.value("processed",false) ||
                replaced.contains(handoff_version(row))) continue;
            const auto phase=review_state(row);
            if (phase=="passed" || phase=="needs_repair" || phase=="blocked") continue;
            const auto queue=row.value("queueId",legacy_queue_id_);
            if (open_queue(db.state,queue)) work.emplace_back(queue,row.at("id").get<std::string>());
        }
    }
    if (work.empty()) return;
    const ExclusiveStateLock lock(data_/L"prechecks.lock");
    for (const auto& [queue,key]:work) {
        if (stop.stop_requested()) return;
        try {
            const auto scoped=for_queue(queue);
            Json row;
            {
                Database db(data_,false);
                row=db.state.at("sessions").at(key);
            }
            if (active(row)) {
                if (!row.contains("startCursor")) continue;
                refresh(data_,key,1200,false,stop);
                Database db(data_,false);
                row=db.state.at("sessions").at(key);
            }
            const auto version=row.value("resultVersion","");
            const auto phase=review_state(row);
            if (version.empty() || active(row) || failed_handoff(row) || phase=="passed" ||
                phase=="needs_repair" || phase=="blocked") continue;
            auto check=phase=="pending"?Json{{"version",version}}:row.at("precheck");
            const auto save=[&] {
                Database db(data_,true);
                auto& current=db.state.at("sessions").at(key);
                if (active(current) || current.at("resultVersion")!=version ||
                    current.at("precheck")!=row.at("precheck")) return false;
                current["precheck"]=check;
                db.save();
                row["precheck"]=check;
                return true;
            };
            const auto block=[&](const char* code) {
                check.update({{"state","blocked"},{"code",code}});
                save();
            };
            const auto read_for_review=[&](const Json& session,const char* code) -> Json {
                try {
                    const auto& result_version=session.at("resultVersion");
                    auto report=scoped.invoke_impl("cluster_read_result",
                        {{"sessionId",session.at("id")},{"verifiedVersion",result_version}},true);
                    if (report.at("resultVersion")==result_version) return report;
                } catch (const std::exception&) {
                    const auto completed=session.value("completed",std::int64_t(0));
                    if (completed>0 && now()-completed>60000) block(code);
                }
                return nullptr;
            };
            if (!check.contains("candidateSha")) {
                auto report=read_for_review(row,"candidate_result_unavailable");
                if (report.is_null()) continue;
                const auto delivery=extract_delivery(report.at("messages"));
                if (delivery.is_null() || !delivery.at("candidateSha").is_string() || !delivery.at("worktreeClean").get<bool>()) {
                    block("candidate_unavailable");
                    continue;
                }
                check["candidateSha"]=delivery.at("candidateSha");
            }
            if (!check.contains("reviewSessionId")) {
                check["reviewSessionId"]=id();
                check["state"]="scheduling";
                if (!save()) continue;
            }
            const auto reviewer=check.at("reviewSessionId").get<std::string>();
            const Json source{{"sessionId",key},{"resultVersion",version}};
            Json child;
            {
                Database db(data_,false);
                child=db.state.at("sessions").value(reviewer,Json());
            }
            if (child.is_null()) {
                const auto sha=check.at("candidateSha").get<std::string>();
                const auto requirements=row.value("reviewTask","");
                const auto task=std::string("Independently adversarially review the exact commit ")+sha+
                    ". Inspect the committed diff and independently verify acceptance conditions. Never modify the candidate "
                    "worktree or switch its HEAD. If executing a build or test writes files, use a separate detached git "
                    "worktree at that SHA. Do not publish, start nested agents, or change provider/model. "
                    "Do not rely on the developer's claims. Look for regressions and missing evidence. "
                    "Finish with exactly one fenced block named fatfish-review containing JSON with candidateSha (the exact SHA), "
                    "verdict (pass, fail, or blocked). On pass include checks (a nonempty list of short checks actually run or "
                    "inspected). On fail include findings (up to six objects with short issue, evidence, acceptance strings). "
                    "On blocked use verdict blocked. Do not quote or reproduce the developer's result. Requirements follow.\n"+
                    requirements;
                const Json launch{{"title","独立对抗审查"},{"role","reviewer"},{"batch",row.at("batch")},
                    {"workspace",row.at("workspace")},{"task",task}};
                const Json operation{{"kind","precheck"},{"requestId",reviewer},{"input",launch},
                    {"source",source}};
                scoped.start_session(launch,reviewer,Json(),operation);
                continue;
            }
            if (child.value("precheckFor",Json())!=source) {
                block("review_identity_mismatch");
                continue;
            }
            if (active(child)) {
                const auto stage=now()-child.value("created",now())>30*60*1000?"overdue":"reviewing";
                if (check.value("state","")!=stage) {
                    check["state"]=stage;
                    if (!save()) continue;
                }
                if (!child.contains("startCursor")) continue;
                refresh(data_,reviewer,1200,false,stop);
                Database db(data_,false);
                child=db.state.at("sessions").at(reviewer);
            }
            if (active(child)) continue;
            if (child.at("status")!="succeeded") {
                block("review_execution_failed");
                continue;
            }
            const auto report=read_for_review(child,"review_result_unavailable");
            if (report.is_null()) continue;
            const auto verdict=parse_precheck(report.at("messages"),check.at("candidateSha"));
            if (verdict.is_null()) block("review_inconclusive");
            else {
                check.update(verdict);
                save();
            }
        } catch (...) { diagnostics::caught(); }
    }
}

Json Cluster::invoke(std::string_view tool, const Json& input) const {
    auto result=invoke_impl(tool,input,false);
    if (tool=="cluster_read_result" && !result.value("resultVersion","").empty()) {
        const auto row=invoke_impl("cluster_get_session",{{"sessionId",input.at("sessionId")}},false);
        if (!result.contains("delivery")) result=save_result_artifact(data_,queue_id_,row,std::move(result));
        if (row.contains("workItemId")) {
            result["workItemId"]=row.at("workItemId");
            result["delivery"]["workItemId"]=row.at("workItemId");
        }
        result=attach_result_evidence(data_,queue_id_,row,std::move(result));
    }
    return result;
}

Json Cluster::invoke_impl(std::string_view tool, const Json& input,bool internal_read) const {
    if (tool.starts_with("team_")) return team_invoke(*this,data_,tool,input);
    if (tool=="cluster_get_evidence_manifest")
        return evidence_manifest_page(data_,read_evidence_manifest(data_,queue_id_,field(input,"manifestId",200)),input);
    if (tool=="cluster_capture_evidence") {
        const auto key=field(input,"sessionId",256);
        auto row=invoke("cluster_get_session",{{"sessionId",key}});
        if (input.contains("resultVersion")) require(input.at("resultVersion")==row.at("resultVersion"),"结果版本已变化，请重新核对采集来源");
        auto artifact=load_result_artifact(data_,queue_id_,key,row.value("resultVersion",""));
        auto delivery=artifact.is_null()?Json{{"candidateSha",nullptr},{"tests",Json::array()},{"evidencePaths",Json::array()}}:
            artifact.value("delivery",Json{{"candidateSha",nullptr},{"tests",Json::array()},{"evidencePaths",Json::array()}});
        if (input.contains("candidateSha")) {
            require(input.at("candidateSha").is_string() && commit_sha(input.at("candidateSha").get<std::string>()),"candidateSha 须为完整 Git SHA");
            delivery["candidateSha"]=input.at("candidateSha");
        }
        if (input.contains("tests")) delivery["tests"]=input.at("tests");
        const auto& tests=delivery.at("tests");
        require(tests.is_array() && tests.size()<=64,"tests 须为最多 64 项的数组");
        for (const auto& test:tests) {
            require(test.is_object(),"test 须为对象");field(test,"command",2000);
            require(test.contains("exitCode") && test.at("exitCode").is_number_integer(),"test.exitCode 须为整数");
            if (test.contains("logPath")) require(path_from_utf8(field(test,"logPath",4000)).is_absolute(),"测试日志须为绝对路径");
        }
        Json options=Json::object();
        options["testSource"]=input.contains("tests")?"caller_reported":"agent_reported";
        if (input.contains("configuration")) options["configuration"]=field(input,"configuration",120);
        if (input.contains("files")) {
            require(input.at("files").is_array() && input.at("files").size()<=128,"files 须为最多 128 项的数组");
            for (const auto& entry:input.at("files")) {
                const auto path=entry.is_string()?entry.get<std::string>():field(entry,"path",4000);
                require(path.size()<=4000 && path.find('\0')==path.npos && path_from_utf8(path).is_absolute(),"证据须为有效绝对路径");
                if (entry.is_object() && entry.contains("kind")) {
                    const auto kind=field(entry,"kind",32);
                    require(kind=="screenshot" || kind=="executable" || kind=="test_log" || kind=="build_config" || kind=="other","证据 kind 无效");
                }
            }
            options["files"]=input.at("files");
        }
        Json corrections=Json::object();
        if(row.contains("evidenceManifest") && row.at("evidenceManifest").is_string()) try {
            const auto previous=read_evidence_manifest(data_,queue_id_,row.at("evidenceManifest").get<std::string>());
            if(previous.at("sessionId")==key && previous.at("resultVersion")==row.at("resultVersion") && previous.at("candidateSha")==delivery.at("candidateSha"))
                for(const auto& c:previous.value("fileCorrections",Json::array())) corrections[c.at("reportedPath").get<std::string>()]=c;
        } catch(const std::exception&) { /* An unavailable old manifest cannot authorize a correction. */ }
        if(input.contains("corrections")) {
            require(input.at("corrections").is_array() && input.at("corrections").size()<=128,"corrections must contain at most 128 entries");
            for(const auto& c:input.at("corrections")) {const auto reported=field(c,"reportedPath",4000),replacement=field(c,"replacementPath",4000);field(c,"reason",4000);require(path_from_utf8(reported).is_absolute() && path_from_utf8(replacement).is_absolute(),"Correction paths must be absolute");corrections[reported]=c;}
        }
        if(!corrections.empty()) {options["corrections"]=Json::array();for(const auto& c:corrections) options["corrections"].push_back(c);}
        auto result=capture_evidence(data_,queue_id_,row,delivery,options);
        remember_result_evidence(data_,queue_id_,row,result);
        {
            Database db(data_,true);
            auto& rows=db.state.at("sessions");
            if (rows.contains(key) && rows.at(key).value("resultVersion","")==row.value("resultVersion","")) {
                rows.at(key)["checksManifest"]=result.at("checksManifest");
                rows.at(key)["evidenceManifest"]=result.at("evidenceManifest");
                db.save();
            }
        }
        result["sessionId"]=key;result["workItemId"]=row.at("workItemId");
        return result;
    }
    if (tool=="cluster_send_message" || tool=="cluster_get_message") {
        const auto key=field(input,"sessionId",256),request=field(input,"requestId",160);
        require(!key.starts_with("codex:"),"追加消息只支持肥鱼重工派发的 DSH 会话");
        {
            const ExclusiveStateLock lock(prompt_lock_file(data_,key));
            Database db(data_,true);
            auto& sessions=db.state.at("sessions");
            require(sessions.contains(key) && in_queue(sessions.at(key),queue_id_,legacy_queue_id_),"当前队列没有该会话");
            auto& row=sessions.at(key);
            auto& outbox=row["messageOutbox"];
            if (outbox.is_null()) outbox=Json::object();
            if (tool=="cluster_get_message") {
                require(outbox.contains(request),"找不到该追加消息 requestId");
                auto result=message_result(row,request,outbox.at(request));
                if (input.value("includeMessage",false)) result["input"]=read_json(path_from_utf8(outbox.at(request).at("messagePath").get<std::string>()),512*1024).at("input");
                return result;
            }
            require(open_queue(db.state,queue_id_),"该队列已删除");
            const auto digest=sha256(input.dump());
            if (outbox.contains(request)) require(outbox.at(request).at("inputDigest")==digest,"该消息 requestId 已用于其他参数，请用完全相同的参数重试");
            else {
                require(active(row) && row.contains("dshId") && row.contains("startCursor") && !dispatch_pending(row),"只能向已确认派发的活动会话追加消息");
                require(outbox.size()<128,"该会话已达到 128 条追加消息上限");
                const auto mode=input.value("mode",std::string("steer"));
                require(mode=="steer" || mode=="queue","mode 须为 steer 或 queue");
                const auto message=input.contains("message")?field(input,"message",16000):"";
                require(message.empty() || message.find_first_not_of(" \t\r\n")!=message.npos,"追加消息不能仅包含空白");
                const auto baseline=input.contains("baseSha")?field(input,"baseSha",64):"";
                require(baseline.empty() || commit_sha(baseline),"baseSha 须为完整 Git SHA");
                const auto issues=input.value("issueIds",Json::array());
                require(issues.is_array() && issues.size()<=32,"issueIds 须为最多 32 项的数组");
                require(!message.empty() || !baseline.empty() || !issues.empty(),"请提供 message、baseSha 或 issueIds");
                const auto work=row.at("workItemId").get<std::string>();
                const bool team_lead=row.value("role","")=="lead" && row.contains("teamProjectId");
                std::string reference;
                if (team_lead) {
                    require(issues.empty() && baseline.empty(),"负责人通知不能替代执行工作项的基线/缺陷更新");
                    reference="team_domain_context (current project charter and domain worklist)";
                } else {
                    const auto& item=db.state.at("workItems").at(work);
                    require(item.at("state")=="open" || item.at("state")=="blocked","事项已验收或关闭，请先 reopen");
                    for (const auto& issue:issues) require(issue.is_string() && item.at("issues").contains(issue.get<std::string>()),"缺陷 ID 不属于该事项");
                    reference=publish_work_item_spec(data_,item);
                }
                const auto message_id="ff-message-"+sha256(queue_id_+"\n"+key+"\n"+request);
                const auto file=data_/L"messages"/path_from_utf8(queue_id_)/path_from_utf8(sha256(key))/path_from_utf8(message_id+".json");
                fs::create_directories(file.parent_path());
                const std::string supplement="Supplement to the existing task; original scope, workspace and model remain unchanged. "
                    "Read-only task/issue reference: "+reference+".\n"+
                    Json{{"workItemId",work},{"newBaseSha",baseline.empty()?Json():Json(baseline)},{"issueIds",issues}}.dump()+
                    "\nAdditional constraints or findings:\n"+message;
                const Json prompt{{"sessionId",row.at("dshId")},{"requestId",message_id},{"mode",mode},
                    {"content",Json::array({{{"type","text"},{"text",supplement}}})}};
                const Json body{{"messageId",message_id},{"inputDigest",digest},{"input",input},{"request",prompt}};
                if (fs::exists(file)) require(read_json(file,512*1024).at("inputDigest")==digest,"该消息 requestId 的归档参数不同");
                else write_state_file(file,body);
                outbox[request]={{"messageId",message_id},{"inputDigest",digest},{"requestDigest",sha256(prompt.dump())},{"messagePath",utf8(file)},
                    {"mode",mode},{"state","pending"},{"retryAt",now()},{"retryable",true},{"created",now()},{"attempts",0}};
                message_event(db.state,row,request,outbox.at(request));
                db.save();
            }
        }
        deliver_message(data_,key,request,true);
        Database db(data_,false);
        const auto& row=db.state.at("sessions").at(key);
        return message_result(row,request,row.at("messageOutbox").at(request));
    }
    if (tool=="cluster_create_work_item" || tool=="cluster_get_work_item" || tool=="cluster_list_work_items" || tool=="cluster_update_work_item") {
        Json observed,external=Json::object();
        if (tool=="cluster_get_work_item" || tool=="cluster_list_work_items") {
            Json settings;
            LegacyFilter include;
            {Database db(data_,false);settings=legacy_connection_settings(db.state);include=legacy_filter(db.state,queue_id_,legacy_queue_id_);}
            external=read_legacy_sessions(settings,false,nullptr,{},LegacyRefresh::incremental,include).rows;
        }
        if (tool=="cluster_update_work_item" &&
            (input.value("action","")=="attach_session" || input.value("action","")=="record_review")) {
            const auto work=field(input,"workItemId",100),request=field(input,"requestId",160);
            bool replay=false;
            {
                Database db(data_,false);
                const auto& rows=db.state.value("workItems",Json::object());
                replay=rows.contains(work) && rows.at(work).at("queueId")==queue_id_ && rows.at(work).at("operations").contains(request);
            }
            if (!replay) {
                const auto session=field(input.at("details"),"sessionId",256);
                observed=invoke(input.at("action")=="record_review"?"cluster_refresh_session":"cluster_get_session",{{"sessionId",session}});
            }
        }
        Database db(data_,true);
        require(open_queue(db.state,queue_id_),"该队列已删除");
        if (!observed.is_null() && observed.at("id").get_ref<const std::string&>().starts_with("codex:"))
            require(legacy_filter(db.state,queue_id_,legacy_queue_id_)(observed.at("id").get<std::string>()),"外部会话队列归属已变化");
        const auto external_before=db.state.value("workExternalSessions",Json::object());
        observe_work_sessions(db.state,queue_id_,external);
        if (!observed.is_null() && observed.at("id").get_ref<const std::string&>().starts_with("codex:"))
            observe_work_sessions(db.state,queue_id_,Json{{observed.at("id").get<std::string>(),observed}});
        const auto changed=synchronize_work_items(db.state) || external_before!=db.state.at("workExternalSessions");
        if (tool=="cluster_create_work_item") {
            auto result=create_work_item(db.state,queue_id_,input);
            db.save();
            result["taskRef"]=publish_work_item_spec(data_,db.state.at("workItems").at(result.at("workItemId").get<std::string>()));
            return result;
        }
        if (tool=="cluster_update_work_item") {
            const auto& details=input.value("details",Json::object());
            const auto work=field(input,"workItemId",100),request=field(input,"requestId",160);
            const auto& saved=db.state.at("workItems").at(work);
            const bool replay=saved.at("queueId")==queue_id_ && saved.at("operations").contains(request);
            if (!replay) for (const auto* reference:{"checksManifest","evidenceManifest"}) if (details.contains(reference)) {
                const auto manifest=read_evidence_manifest(data_,queue_id_,field(details,reference,200));
                const auto& item=db.state.at("workItems").at(field(input,"workItemId",100));
                require(item.at("queueId")==queue_id_ && item.at("sessions").contains(manifest.at("sessionId").get<std::string>()),"证据不属于该事项的会话");
                if (details.contains("candidateSha")) require(manifest.at("candidateSha")==details.at("candidateSha"),"证据候选 SHA 不匹配");
                require(manifest.at("kind")==std::string(reference[0]=='c'?"checks":"evidence"),"证据清单类型不匹配");
            }
            auto result=update_work_item(db.state,queue_id_,input,observed);
            db.save();
            result["taskRef"]=publish_work_item_spec(data_,db.state.at("workItems").at(result.at("workItemId").get<std::string>()));
            return result;
        }
        if (changed) db.save();
        auto result=query_work_items(db.state,queue_id_,input,tool=="cluster_get_work_item");
        if (tool=="cluster_get_work_item") result["taskRef"]=publish_work_item_spec(data_,db.state.at("workItems").at(result.at("workItemId").get<std::string>()));
        return result;
    }
    if (tool=="cluster_wait_events") {
        require(input.is_object() && input.contains("targets") && input.at("targets").is_array() &&
            !input.at("targets").empty() && input.at("targets").size()<=8,"targets 须包含 1 至 8 个队列");
        const auto timeout=input.value("timeoutMs",25000),limit=input.value("limit",32);
        require(timeout>=0 && timeout<=30000 && limit>=1 && limit<=100,"timeoutMs 须为 0 至 30000，limit 须为 1 至 100");
        std::vector<Cluster> queues;
        std::vector<DesktopMonitor> monitors(input.at("targets").size());
        std::set<std::string> seen;
        for (const auto& target:input.at("targets")) {
            require(target.is_object(),"targets 元素须为对象");
            auto queue=for_queue(field(target,"queueId",36));
            require(seen.insert(queue.queue_id_).second,"targets 不得重复同一队列");
            if (target.contains("afterCursor")) require(target.at("afterCursor").is_string(),"afterCursor 须为字符串");
            queues.push_back(std::move(queue));
        }
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(timeout);
        for (;;) {
            Json results=Json::array();
            bool ready=false;
            std::vector<Json> snapshots(queues.size());
            std::vector<std::exception_ptr> errors(queues.size());
            std::vector<std::jthread> readers;
            for (std::size_t index=0;index<queues.size();++index) readers.emplace_back([&,index] {
                try { snapshots[index]=queues[index].desktop_snapshot(&monitors[index]); }
                catch (...) { errors[index]=std::current_exception(); }
            });
            for (auto& reader:readers) reader.join();
            for (std::size_t index=0;index<queues.size();++index) {
                auto& queue=queues[index];
                queue.require_open_queue();
                if (errors[index]) std::rethrow_exception(errors[index]);
                const auto& snapshot=snapshots[index];
                Json observed=Json::object();
                for (const auto* section:{"running","pending","processed"}) for (const auto& row:snapshot.at(section))
                    if (row.at("id").get_ref<const std::string&>().starts_with("codex:")) observed[row.at("id").get<std::string>()]=row;
                Database db(data_,true);
                const auto initialized=db.state.contains("eventJournal");
                const auto changed=record_external_observations(db.state,queue.queue_id_,observed);
                if (!initialized || changed) db.save();
                auto result=events_after(db.state,queue.queue_id_,input.at("targets").at(index),limit);
                result["counts"]={{"active",snapshot.at("running").size()},{"pending",snapshot.at("pending").size()},
                    {"processed",snapshot.at("processed").size()}};
                result["stale"]=snapshot.value("liveStale",0)>0;
                if (result.at("initial").get<bool>() || result.at("resetRequired").get<bool>()) {
                    result["sessions"]=Json::array();
                    for (const auto* section:{"running","pending"}) for (const auto& row:snapshot.at(section)) {
                        if (result.at("sessions").size()>=static_cast<std::size_t>(limit)) break;
                        result["sessions"].push_back(event_projection(row));
                    }
                    result["snapshotTruncated"]=snapshot.at("running").size()+snapshot.at("pending").size()>result.at("sessions").size();
                }
                ready=ready || result.at("initial").get<bool>() || result.at("resetRequired").get<bool>() || !result.at("events").empty();
                results.push_back(std::move(result));
            }
            const auto remaining=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-std::chrono::steady_clock::now());
            if (ready || remaining.count()<=0) return {{"targets",results},{"timedOut",!ready}};
            std::this_thread::sleep_for(std::min(remaining,std::chrono::milliseconds(500)));
        }
    }
    if (tool=="cluster_review_issues" || tool=="cluster_retry_review") {
        Database db(data_,false);
        auto& rows=db.state.at("sessions");
        const auto replaced=replaced_versions(rows);
        if (tool=="cluster_review_issues") {
            Json issues=Json::array();
            for (const auto& row:rows) {
                if (!in_queue(row,queue_id_,legacy_queue_id_) || replaced.contains(handoff_version(row)) ||
                    row.value("processed",false)) continue;
                const auto state=review_state(row);
                if (state!="blocked" && state!="needs_repair" && state!="overdue") continue;
                auto issue=review_notice(row);
                issue["historical"]=true;
                issue["automaticReviewEnabled"]=false;
                issue["action"]="Read the delivery and existing reviewer results before deciding whether another review is needed.";
                issue.update({{"sessionId",row.at("id")},{"title",row.at("title")},{"resultVersion",row.at("resultVersion")}});
                issues.push_back(std::move(issue));
            }
            return {{"issues",std::move(issues)}};
        }
        throw DispatchError("请由主代理用 cluster_start_session 设置 role 为 reviewer 主动安排复审",
            {{"reason","manual_review_required"}});
    }
    require(input.is_object(), "工具参数必须为 JSON 对象");
    if (tool=="cluster_create_queue") {
        const auto name=field(input,"name",240),request=field(input,"requestId",160);
        require(name.find_first_not_of(" \t\r\n")!=std::string::npos,"请输入队列名称");
        const auto work_key=input.contains("workKey")?field(input,"workKey",240):"";
        const auto workspace=input.contains("workspace")?field(input,"workspace",4000):"";
        require(workspace.empty() || path_from_utf8(workspace).is_absolute(),"工作区须使用绝对路径");
        Database db(data_,true);
        auto& queues=db.state.at("queues");
        for (const auto& row:queues) if (row.value("requestId","")==request) {
            require(!row.contains("deletedAt"),"此请求创建的队列已删除，创建新队列请使用新的 requestId");
            require(row.value("initialName",row.at("name").get<std::string>())==name &&
                row.value("workKey","")==work_key && row.value("initialWorkspace","")==workspace,
                "此 requestId 已用于创建队列，请用原参数重试");
            return queue_info(row);
        }
        const auto key=id();
        const auto created=now();
        queues[key]={{"queueId",key},{"name",name},{"initialName",name},{"legacy",false},{"created",created},{"updated",created},
            {"workKey",work_key},{"initialWorkspace",workspace},{"requestId",request},
            {"workspaces",workspace.empty()?Json::array():Json::array({workspace})}};
        db.save();
        return queue_info(queues.at(key));
    }
    if (tool=="cluster_rename_queue") {
        const auto scoped=for_queue(field(input,"queueId",36));
        const auto name=field(input,"name",240);
        require(name.find_first_not_of(" \t\r\n")!=std::string::npos,"请输入队列名称");
        Database db(data_,true);
        auto& row=db.state.at("queues").at(scoped.queue_id_);
        require(!row.contains("deletedAt"),"该队列已删除");
        if (row.at("name")!=name) {
            if (!row.contains("initialName")) row["initialName"]=row.at("name");
            row["name"]=name;
            row["updated"]=now();
            db.save();
        }
        return queue_info(row);
    }
    if (tool=="cluster_find_queues") {
        const auto client=input.contains("clientKey")?field(input,"clientKey",240):"";
        const auto work=input.contains("workKey")?field(input,"workKey",240):"";
        const auto workspace=input.contains("workspace")?field(input,"workspace",4000):"";
        require(workspace.empty() || path_from_utf8(workspace).is_absolute(),"工作区须使用绝对路径");
        Database db(data_,false);
        auto last=client.empty()?"":db.state.value("queueClients",Json::object()).value(client,"");
        if (!open_queue(db.state,last)) last.clear();
        std::vector<Json> matches;
        for (const auto& row:db.state.at("queues")) {
            if (row.contains("deletedAt")) continue;
            const bool bound=!last.empty() && row.at("queueId")==last;
            const bool by_work=!work.empty() && row.value("workKey","")==work;
            const auto& paths=row.at("workspaces");
            const bool by_workspace=!workspace.empty() && std::ranges::any_of(paths,[&](const Json& value){
                return same_workspace(value.get_ref<const std::string&>(),workspace);
            });
            if ((!client.empty() || !work.empty() || !workspace.empty()) && !bound && !by_work && !by_workspace) continue;
            auto info=queue_info(row);
            info["matchedBy"]=Json::array();
            if (bound) info["matchedBy"].push_back("clientKey");
            if (by_work) info["matchedBy"].push_back("workKey");
            if (by_workspace) info["matchedBy"].push_back("workspace");
            matches.push_back(std::move(info));
        }
        std::ranges::sort(matches,[](const Json& a,const Json& b) {
            return std::pair(a.at("updated"),a.at("queueId"))>std::pair(b.at("updated"),b.at("queueId"));
        });
        return {{"queues",std::move(matches)},{"lastQueueId",last.empty()?Json():Json(last)}};
    }
    if (tool=="cluster_get_queue" || tool=="cluster_bind_queue") {
        const auto scoped=for_queue(field(input,"queueId",36));
        if (tool=="cluster_bind_queue") {
            const auto client=field(input,"clientKey",240);
            Database db(data_,true);
            require(open_queue(db.state,scoped.queue_id_),"该队列已删除");
            db.state["queueClients"][client]=scoped.queue_id_;
            db.save();
            return {{"queueId",scoped.queue_id_},{"clientKey",client}};
        }
        auto snapshot=scoped.desktop_snapshot();
        Database db(data_,false);
        auto info=queue_info(db.state.at("queues").at(scoped.queue_id_));
        const auto& overview=snapshot.at("overview");
        for (const auto field:{"active","unprocessed","processed","claimed"}) info[field]=overview.at(field);
        return info;
    }
    if (tool=="cluster_desktop_onboarding") {
        const bool updating=input.contains("completed") || input.contains("dismissed");
        Database db(data_,updating);
        auto value=db.state.value("desktopOnboarding",Json{{"completed",false},{"dismissed",false}});
        if (updating) {for (const auto* key:{"completed","dismissed"}) if (input.contains(key)) value[key]=input.at(key).get<bool>();db.state["desktopOnboarding"]=value;db.save();}
        return value;
    }
    if (tool=="cluster_desktop_settings_expanders") {
        const bool updating=input.contains("section");
        Database db(data_,updating);
        auto expanded=db.state.value("desktopSettingsExpanders",Json{{"general",true}});
        if (updating) {
            const auto section=field(input,"section",20);
            require(section=="general" || section=="dsh" || section=="mcp" || section=="team","未知设置分组");
            expanded[section]=input.at("expanded").get<bool>();
            db.state["desktopSettingsExpanders"]=expanded;
            db.save();
        }
        return expanded;
    }
    if (tool=="cluster_desktop_close_behavior") {
        const bool updating=input.contains("closeBehavior");
        Database db(data_,updating);
        if (updating) {
            const auto value=field(input,"closeBehavior",12);
            require(value=="ask" || value=="tray" || value=="exit","关闭行为无效");
            db.state["desktopCloseBehavior"]=value;
            db.save();
        }
        return {{"closeBehavior",db.state.value("desktopCloseBehavior","ask")}};
    }
    if (tool=="cluster_desktop_queue") {
        if (!input.contains("queueId")) {
            Database db(data_,false);
            const auto saved=db.state.value("desktopQueueId",legacy_queue_id_);
            return {{"queueId",open_queue(db.state,saved)?saved:legacy_queue_id_}};
        }
        const auto scoped=for_queue(field(input,"queueId",36));
        Database db(data_,true);
        require(open_queue(db.state,scoped.queue_id_),"该队列已删除");
        db.state["desktopQueueId"]=scoped.queue_id_;
        db.save();
        return {{"queueId",scoped.queue_id_}};
    }
    if (tool=="cluster_assign_legacy_session") {
        const auto target=for_queue(field(input,"queueId",36));
        const auto key=field(input,"sessionId",256);
        require(key.starts_with("codex:") && target.queue_id_!=legacy_queue_id_,"请提供外部 Codex 会话 ID 和目标新队列");
        const auto legacy=for_queue(legacy_queue_id_);
        const ExclusiveStateLock source_lock(legacy.handoff_file(true));
        const ExclusiveStateLock target_lock(target.handoff_file(true));
        const auto file=legacy.handoff_file();
        auto handoff=fs::exists(file)?read_json(file):Json::object();
        if (migrate_repair(data_,handoff,legacy_queue_id_,legacy_queue_id_)) write_state_file(file,handoff);
        Database db(data_,true);
        require(open_queue(db.state,target.queue_id_),"目标队列已删除");
        auto& assignments=db.state["legacyQueues"];
        if (assignments.is_null()) assignments=Json::object();
        const auto current=assignments.value(key,legacy_queue_id_);
        if (current==target.queue_id_) return {{"queueId",current},{"sessionId",key},{"assigned",false}};
        require(current==legacy_queue_id_,"该外部会话已归属其他队列");
        const auto claim=handoff.value("claim",Json::object());
        require(claim.value("sessionId","")!=key || claim.value("expiresAt",std::int64_t(0))<=now(),"已领取的外部会话不能换队列");
        for (const auto& child:db.state.at("sessions")) for (const auto& link:repair_links(child))
            require(link.at("sessionId")!=key,"已有返修关联的外部会话不能换队列");
        const auto rows=read_legacy_sessions(legacy_connection_settings(db.state)).rows;
        require(rows.contains(key) && !rows.at(key).value("processed",false),"找不到待处理或进行中的外部会话");
        reassign_work_session_queue(db.state,key,target.queue_id_);
        assignments[key]=target.queue_id_;
        auto& destination=db.state.at("queues").at(target.queue_id_);
        remember_workspace(destination,rows.at(key).value("workspace",""));
        destination["updated"]=now();
        db.save();
        return {{"queueId",target.queue_id_},{"sessionId",key},{"assigned",true}};
    }
    if (tool == "cluster_overview" || tool == "cluster_get_settings") {
        Json settings,sessions;
        LegacyFilter include;
        {
            Database db(data_,false);
            if (tool=="cluster_get_settings") return db.state.at("settings");
            settings=legacy_connection_settings(db.state);
            sessions=queue_sessions(db.state,queue_id_,legacy_queue_id_);
            include=legacy_filter(db.state,queue_id_,legacy_queue_id_);
        }
        LegacyObservations observations;
        const auto legacy=read_legacy_sessions(settings,false,&observations,{},LegacyRefresh::incremental,include);
        sessions.update(legacy.rows);
        const auto file=handoff_file();
        const auto claim=fs::exists(file)?read_json(file).value("claim",Json::object()):Json::object();
        return session_overview(settings,sessions,legacy,data_,claim);
    }
    if (tool=="cluster_get_session_status") {
        const auto supplied=input.find("sessionId");
        require(supplied!=input.end() && supplied->is_string(),"请提供 sessionId，可以填写派发结果中的 id 或 dshId");
        const auto& requested=supplied->get_ref<const std::string&>();
        require(!requested.empty() && requested.size()<=256 && requested.find('\0')==std::string::npos,
            "sessionId 无效，请填写派发结果中的 id 或 dshId");
        std::string key;
        Json settings;
        LegacyFilter include;
        std::vector<std::string> matches;
        {
            Database db(data_,false);
            const auto rows=queue_sessions(db.state,queue_id_,legacy_queue_id_);
            if (rows.contains(requested)) key=requested;
            else {
                settings=legacy_connection_settings(db.state);
                include=legacy_filter(db.state,queue_id_,legacy_queue_id_);
                for (const auto& [id,row]:rows.items())
                    if (row.value("dshId","")==requested) matches.push_back(id);
            }
        }
        if (key.empty()) {
            const auto legacy=read_legacy_sessions(settings,false,nullptr,{},LegacyRefresh::incremental,include);
            if (legacy.rows.contains(requested)) key=requested;
            else {
                for (const auto& [id,row]:legacy.rows.items())
                    if (row.value("dshId","")==requested) matches.push_back(id);
                require(!matches.empty(),"找不到该会话，请检查派发结果中的 id 或 dshId");
                require(matches.size()==1,"同一个 dshId 对应多条登记，请使用派发结果中的 FatFishBox id 查询");
                key=matches.front();
            }
        }
        auto row=invoke("cluster_get_session",{{"sessionId",key}});
        bool stale=false;
        Json sync_error;
        try {
            row=invoke("cluster_refresh_session",{{"sessionId",key}});
            stale=row.value("stale",false) || (key.starts_with("codex:") && !row.contains("liveCursor") &&
                row.value("resultSource","")!="dsh_history");
        } catch (const DshError& error) {stale=true;sync_error=dsh_error_diagnostics(error);}
        catch (const std::exception& error) {stale=true;sync_error={{"message",error.what()}};}
        Json result{{"sessionId",key},{"title",row.value("title","")},{"status",row.at("status")},{"terminal",!active(row)},
            {"updated",row.value("updated",std::int64_t(0))},{"stale",stale}};
        if (!sync_error.is_null()) result["syncError"]=sync_error;
        if (row.contains("dispatchState")) result["dispatchState"]=dispatch_pending(row)?"unconfirmed":row.value("dispatchState","");
        if (row.contains("dispatchRetryAt")) result["dispatchRetryAt"]=row.at("dispatchRetryAt");
        if (row.contains("requestId")) result["requestId"]=row.at("requestId");
        for (const auto* member:{"retryable","retryAfterMs","retryMode","failureStage","failure","workItemId","baseSha","resultVersion","lastMessageAt","checksManifest","evidenceManifest","goal"})
            if (row.contains(member)) result[member]=row.at(member);
        if (row.contains("resultSource")) result["resultSource"]=row.at("resultSource");
        if (row.contains("reviewState")) result["reviewState"]=row.at("reviewState");
        if (row.contains("internalReview")) result["internalReview"]=true;
        auto links=repair_links(row);
        if (!links.empty()) result["repairLinks"]=std::move(links);
        return result;
    }
    if (tool=="cluster_detach_handoff_repair") {
        const auto key=field(input,"sessionId",100);
        const auto original=field(input,"originalSessionId",256);
        const auto version=field(input,"originalVersion",100);
        const auto token=input.contains("claimToken")?field(input,"claimToken",100):"";
        const auto queue_file=handoff_file();
        const ExclusiveStateLock lock(handoff_file(true));
        require_open_queue();
        auto queue=fs::exists(queue_file)?read_json(queue_file):Json::object();
        if (migrate_repair(data_,queue,queue_id_,legacy_queue_id_)) write_state_file(queue_file,queue);
        const auto claim=queue.value("claim",Json::object());
        const bool claimed=claim.value("sessionId","")==key && claim.value("expiresAt",std::int64_t(0))>now();
        require(claimed?token==claim.value("claimToken",""):token.empty(),"返修已领取时必须提供该返修的当前领取凭据");

        Database db(data_,true);
        auto& sessions=db.state.at("sessions");
        require(sessions.contains(key) && in_queue(sessions.at(key),queue_id_,legacy_queue_id_) &&
            launched_repair(sessions.at(key)),"当前队列没有该未确认的返修会话");
        auto& child=sessions.at(key);
        auto links=repair_links(child);
        const auto removed=std::ranges::find_if(links,[&](const Json& link) {
            return link.at("sessionId")==original && link.at("resultVersion")==version;
        });
        const bool detached=removed!=links.end();
        if (detached) {
            links.erase(removed);
            if (links.empty()) {
                child.erase("repairOf");
                child.erase("repairLinks");
                child.erase("repairQueued");
                child.erase("repairQueuedAt");
            } else {
                child["repairOf"]=links.front();
                child["repairLinks"]=links;
            }
            Json requests=Json::array();
            for (const auto& entry:repair_requests(child)) {
                const auto& request=entry.contains("input")?entry.at("input"):entry;
                // Old replay records only identify the version. Retire ambiguous replays for that version.
                if (request.value("originalVersion","")!=version || entry.value("parentSessionId",original)!=original)
                    requests.push_back(entry);
            }
            child.erase("repairRequest");
            child["repairRequests"]=std::move(requests);
            db.state.at("queues").at(queue_id_)["updated"]=now();
            db.save();
        }
        return {{"sessionId",key},{"detached",detached},{"repairLinks",std::move(links)}};
    }
    if (tool=="cluster_repair_handoff" || tool=="cluster_attach_handoff_repair") {
        const bool attaching=tool=="cluster_attach_handoff_repair";
        const auto token=field(input,"claimToken",100),version=field(input,"originalVersion",100);
        const auto queue_file=handoff_file();
        const ExclusiveStateLock lock(handoff_file(true));
        require_open_queue();
        auto queue=fs::exists(queue_file)?read_json(queue_file):Json::object();
        if (migrate_repair(data_,queue,queue_id_,legacy_queue_id_)) write_state_file(queue_file,queue);
        const auto respond=[&](const Json& child,bool replayed,bool released) {
            Json result{{"replayed",replayed},{"claimReleased",released}};
            if (attaching) {
                result.update({{"sessionId",child.at("id")},{"status",child.at("status")},
                    {"handoffState",active(child)?"repairing":"queued_for_review"},
                    {"linkedHandoffs",repair_links(child).size()}});
            } else {
                result["session"]=public_row(child);
                if (child.contains("dispatchState")) result["dispatchState"]=child.at("dispatchState");
            }
            return result;
        };
        {
            Database db(data_,false);
            for (const auto& child:db.state.at("sessions")) {
                if (!in_queue(child,queue_id_,legacy_queue_id_)) continue;
                for (const auto& entry:repair_requests(child)) {
                    const auto& request=entry.contains("input")?entry.at("input"):entry;
                    if (request.value("claimToken","")!=token) continue;
                    require(request==input,"该领取凭据已用于返修，请复用原参数重试");
                    return respond(child,true,true);
                }
            }
        }
        auto operation=attaching?Json():Json{{"kind","repair"},{"requestId",field(input,"requestId",100)},{"input",input}};
        if (queue.contains("repair")) {
            const auto& pending=queue.at("repair");
            require(!attaching && pending.value("request",Json())==input,
                "该队列有尚待确认的返修，请用原参数和 requestId 重试");
            const Json lookup{{"sessionId",pending.at("sessionId")}};
            auto child=reconcile_dispatch(data_,invoke("cluster_get_session",lookup));
            if (!launched_repair(child)) return respond(child,true,false);
            if (migrate_repair(data_,queue,queue_id_,legacy_queue_id_)) write_state_file(queue_file,queue);
            return respond(invoke("cluster_get_session",lookup),true,true);
        }
        if (!attaching) {
            Database db(data_,false);
            const auto previous=dispatch_request(db.state.at("sessions"),operation,queue_id_,legacy_queue_id_);
            if (!previous.is_null()) return respond(previous,true,false);
        }
        const auto claim=queue.value("claim",Json::object());
        require(claim.value("claimToken","")==token && claim.value("expiresAt",std::int64_t(0))>now(),
            "需要当前队首的有效领取凭据");
        require(claim.at("resultVersion")==version,"原交接版本不匹配");
        require_ready_dsh(invoke("cluster_get_settings"));
        const auto parent_id=claim.at("sessionId").get<std::string>();
        const auto parent=parent_id.starts_with("codex:")?invoke("cluster_refresh_session",{{"sessionId",parent_id}}):
            refresh(data_,parent_id,1900,false);
        require(!active(parent) && !parent.value("stale",false) && !parent.value("processed",false) &&
            parent.at("resultVersion")==version,"原交接已变化或无法确认");
        std::string parent_work;
        {
            Database db(data_,true);
            if (parent_id.starts_with("codex:")) {
                observe_work_sessions(db.state,queue_id_,Json{{parent_id,parent}});
                db.save();
            }
            parent_work=session_work_item(db.state,parent,queue_id_);
        }
        Json reference{{"sessionId",parent.at("id")},{"resultVersion",version},{"workItemId",parent_work}};
        const auto release=[&](const std::string& key) {
            Database db(data_,true);
            auto& child=db.state.at("sessions").at(key);
            require(in_queue(child,queue_id_,legacy_queue_id_) && launched_repair(child),"返修会话必须已派发且尚未确认");
            const auto phase=db.state.at("workItems").at(parent_work).at("state");
            require(phase=="open" || phase=="blocked","原事项已验收或关闭，请先 reopen 再关联返修");
            record_repair_link(child,reference,input);
            db.state.at("queues").at(queue_id_)["updated"]=now();
            db.save();
            release_replaced_claim(queue,reference);
            queue.erase("repair");
            write_state_file(queue_file,queue);
            return respond(child,false,true);
        };
        if (attaching) {
            const auto key=field(input,"sessionId",100);
            require(parent.at("id")!=key,"原交接不能绑定自身为返修");
            {
                Database db(data_,false);
                const auto& sessions=db.state.at("sessions");
                require(sessions.contains(key) && in_queue(sessions.at(key),queue_id_,legacy_queue_id_),"当前队列没有该返修会话");
                const auto& child=sessions.at(key);
                require(launched_repair(child),"返修会话必须已派发且尚未确认");
                const auto batch=parent.value("batch","");
                require(!batch.empty() && child.at("batch")==batch,"返修会话必须与原交接处于同一批次");
                require(!repair_cycle(sessions,parent.at("id").get<std::string>(),key),"关联会造成返修循环");
            }
            queue["repair"]={{"parentId",parent.at("id")},{"parentVersion",version},{"sessionId",key},{"request",input}};
            write_state_file(queue_file,queue);
            return release(key);
        }
        const auto sha=field(input,"candidateSha",64);
        require(commit_sha(sha),"候选 SHA 必须是完整的 Git 提交哈希");
        if (claim.value("kind","")=="review_repair") {
            Database db(data_,false);
            const auto& original=db.state.at("sessions").at(parent_id);
            require(review_repair_needed(original) && (!original.at("precheck").contains("candidateSha") ||
                original.at("precheck").at("candidateSha")==sha),
                "返修候选 SHA 必须与对抗审查发现的缺陷一致");
        }
        const auto paths=input.value("screenshotPaths",Json::array());
        require(evidence_paths(paths),"请提供有效的截图绝对路径");
        const auto acceptance=field(input,"acceptance",8000);
        const auto task=input.contains("task")?field(input,"task",50000):"";
        const auto issue_ids=input.value("issueIds",Json::array());
        require(issue_ids.is_array() && issue_ids.size()<=32,"issueIds 须为最多 32 项的数组");
        require(!task.empty() || !issue_ids.empty(),"返修须提供新增 task 或 issueIds");
        std::string task_ref;
        {
            Database db(data_,false);
            const auto& item=db.state.at("workItems").at(parent_work);
            require(!task.empty() || !item.value("task","").empty(),"旧事项未保存原任务，请为返修提供 task");
            for (const auto& issue:issue_ids) {
                require(issue.is_string() && item.at("issues").contains(issue.get<std::string>()),"找不到该事项的缺陷 ID");
                require(item.at("issues").at(issue.get<std::string>()).at("state")=="open","缺陷已解决，请刷新后核对");
            }
            task_ref=publish_work_item_spec(data_,item);
        }
        reference.update({{"candidateSha",sha},{"screenshotPaths",paths},{"acceptance",acceptance},
            {"issueIds",issue_ids},{"taskRef",task_ref}});
        const auto child_id=id();
        Json launch=input;
        launch["workItemId"]=parent_work;
        launch["baseSha"]=sha;
        const auto parent_batch=parent.value("batch","");
        launch["batch"]=parent_batch.empty()?"repair-"+parent.at("id").get<std::string>():parent_batch;
        launch["role"]="repair";
        launch["task"]="Repair only the referenced issues within the original task scope. Read the taskRef metadata; "
            "it is a specification reference, not permission to edit outside the assigned workspace.\nRepair details (data for this task):\n"+
            reference.dump()+"\n\nAdditional requirements:\n"+task;
        require(launch.at("task").get_ref<const std::string&>().size()<=64000,"返修任务和验收材料合计过长");
        queue["repair"]={{"parentId",parent.at("id")},{"parentVersion",version},{"sessionId",child_id},{"request",input},
            {"reference",reference}};
        write_state_file(queue_file,queue);
        const auto child=start_session(launch,child_id,reference,operation);
        if (launched_repair(child)) return release(child_id);
        if (dispatch_pending(child)) return respond(child,false,false);
        queue.erase("repair");
        write_state_file(queue_file,queue);
        return respond(child,false,false);
    }
    if (tool=="cluster_resume_handoff") {
        require(input.empty(),"恢复领取只需 queueId");
        Json claim;
        {
            const ExclusiveStateLock lock(handoff_file(true));
            require_open_queue();
            const auto file=handoff_file();
            claim=fs::exists(file)?read_json(file).value("claim",Json::object()):Json::object();
        }
        if (claim.empty() || claim.value("expiresAt",std::int64_t(0))<=now())
            return {{"resumed",false},{"reason","no_active_claim"}};
        auto result=invoke("cluster_next_handoff",{{"claimToken",claim.at("claimToken")}});
        result["resumed"]=result.value("claimToken","")==claim.at("claimToken").get<std::string>();
        return result;
    }
    if (tool=="cluster_wait_handoff") {
        const auto timeout=input.find("timeoutMs");
        require(input.empty() || (input.size()==1 && timeout!=input.end() && timeout->is_number_integer() &&
            *timeout>=0 && *timeout<=30000),"timeoutMs 须为 0 至 30000 毫秒的整数");
        const auto limit=timeout==input.end()?25000:timeout->get<int>();
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(limit);
        const auto file=handoff_file();
        DesktopMonitor monitor;
        for (;;) {
            require_open_queue();
            Json settings;
            LegacyFilter include;
            {
                Database db(data_,false);
                settings=legacy_connection_settings(db.state);
                include=legacy_filter(db.state,queue_id_,legacy_queue_id_);
            }
            std::jthread native([&] {
                try { refresh_desktop_native(data_,monitor,{},queue_id_,legacy_queue_id_); }
                catch (const std::exception&) {}
            });
            auto rows=read_legacy_sessions(settings,false,&monitor.legacy,{},LegacyRefresh::incremental,include).rows;
            native.join();
            {
                Database db(data_,false);
                rows.update(queue_sessions(db.state,queue_id_,legacy_queue_id_));
            }
            const auto claim=fs::exists(file)?read_json(file).value("claim",Json::object()):Json::object();
            const bool busy=claim.value("expiresAt",std::int64_t(0))>now();
            const auto replaced=replaced_versions(rows);
            if (!busy && std::ranges::any_of(rows,[&](const Json& row) {
                return !row.value("stale",false) && handoff_candidate(row,replaced);
            })) return {{"ready",true},{"timedOut",false}};
            const auto remaining=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-std::chrono::steady_clock::now());
            if (remaining.count()<=0) return {{"ready",false},{"timedOut",true},{"busy",busy}};
            std::this_thread::sleep_for(std::min(remaining,std::chrono::milliseconds(1000)));
        }
    }
    if (tool == "cluster_next_handoff") {
        require(std::ranges::all_of(input.items(),[](const auto& entry) {
            return entry.key()=="claimToken" || entry.key()=="outcome" || entry.key()=="claimNext";
        }),"只能凭当前领取凭据提交处理结果");
        const auto claim_token=input.find("claimToken");
        const auto token=claim_token==input.end() || claim_token->is_null() || *claim_token==""?"":field(input,"claimToken",100);
        const auto receipt=input.find("outcome");
        const bool has_receipt=receipt!=input.end() && !receipt->is_null() && (!token.empty() || *receipt!="");
        const auto outcome=has_receipt?field(input,"outcome",2000):"";
        require(!has_receipt || outcome.find_first_not_of(" \t\r\n")!=std::string::npos,"请说明实际采取的处置");
        require(outcome.empty() || !token.empty(),"提交处理结果需要当前领取凭据");
        const auto next=input.find("claimNext");
        const bool has_next=next!=input.end() && !next->is_null();
        require(!has_next || next->is_boolean(),"claimNext 须为布尔值或 null");
        const bool claim_next=outcome.empty() || !has_next || next->get<bool>();
        const auto queue_file=handoff_file();
        const auto previous=fs::exists(queue_file)?read_json(queue_file):Json::object();
        const auto previous_claim=previous.value("claim",Json::object());
        if (!previous.contains("repair") && previous_claim.value("expiresAt",std::int64_t(0))>now() &&
            previous_claim.value("claimToken","")!=token && (token.empty() || previous.value("lastToken","")!=token))
            return {{"busy",true},{"expiresAt",previous_claim.at("expiresAt")}};
        const auto settings=[&] { Database db(data_,false); return legacy_connection_settings(db.state); }();
        const bool dsh_ready=recover_desktop_dsh(settings,{},false);
        const ExclusiveStateLock lock(handoff_file(true));
        require_open_queue();
        auto queue=fs::exists(queue_file)?read_json(queue_file):Json::object();
        if (migrate_repair(data_,queue,queue_id_,legacy_queue_id_)) write_state_file(queue_file,queue);
        auto claim=queue.value("claim",Json::object());
        const auto observations=handoff_observations_->queue(queue_id_);
        auto snapshot=[&] {
            Json rows;
            LegacyFilter include;
            {
                Database db(data_,false);
                include=legacy_filter(db.state,queue_id_,legacy_queue_id_);
            }
            std::jthread native([&] {
                if (!dsh_ready) return;
                try {
                    DesktopMonitor monitor;
                    refresh_desktop_native(data_,monitor,{},queue_id_,legacy_queue_id_);
                } catch (const std::exception&) {}
            });
            auto legacy=read_legacy_sessions(settings,false,observations.get(),{},
                dsh_ready?LegacyRefresh::handoff:LegacyRefresh::archive,include);
            native.join();
            {
                Database db(data_,false);
                rows=queue_sessions(db.state,queue_id_,legacy_queue_id_);
            }
            rows.update(legacy.rows);
            return rows;
        };
        auto present=[&](const Json& current,bool selecting=false) {
            const auto key=current.at("sessionId").get<std::string>();
            const bool legacy=key.starts_with("codex:");
            auto row=legacy?live_legacy_session(settings,key):refresh(data_,key,1900,false);
            const bool valid=row.at("resultVersion")==current.at("resultVersion") && !active(row) && !row.value("stale",false);
            if (legacy && !valid) observations->erase(key);
            if (selecting && (!valid || row.value("processed",false))) return Json();
            require(valid,"领取的结果版本已变化或无法确认");
            Json approved;
            if (legacy) {
                Database db(data_,true);
                observe_work_sessions(db.state,queue_id_,Json{{key,row}});
                db.save();
                row["workItemId"]=session_work_item(db.state,row,queue_id_);
            }
            if (!legacy) {
                Database db(data_,false);
                const auto& source=db.state.at("sessions").at(key);
                if (source.at("resultVersion")!=current.at("resultVersion") || active(source)) {
                    if (selecting) return Json();
                    throw std::runtime_error("领取的结果版本已变化");
                }
                if (source.contains("precheck")) approved=review_notice(source);
            }
            auto report=legacy?legacy_result(settings,key,&row):
                invoke("cluster_read_result",{{"sessionId",key},{"verifiedVersion",row.at("resultVersion")}});
            if (selecting && report.at("resultVersion")!=current.at("resultVersion")) return Json();
            require(report.at("resultVersion")==current.at("resultVersion"),"领取的结果版本已变化");
            auto artifact=report.contains("delivery")?report:save_result_artifact(data_,queue_id_,row,std::move(report));
            if (!artifact.contains("evidence")) artifact=attach_result_evidence(data_,queue_id_,row,std::move(artifact));
            Json compact;
            for (const auto* member:{"id","dshId","title","batch","role","workspace","status","resultVersion","completed","reviewState","workItemId"})
                if (row.contains(member)) compact[member]=row.at(member);
            const auto links=repair_links(row);
            if (!links.empty()) {
                compact["repairLinkCount"]=links.size();
                compact["repairLinks"]=Json::array();
                for (const auto& link:links) {
                    if (compact.at("repairLinks").size()>=8) break;
                    compact["repairLinks"].push_back({{"sessionId",link.at("sessionId")},{"resultVersion",link.at("resultVersion")}});
                }
                if (links.size()>8) compact["repairLinksTruncated"]=true;
            }
            if (row.contains("repairOf")) {
                compact["repairOf"]=Json::object();
                for (const auto* member:{"sessionId","resultVersion","candidateSha"})
                    if (row.at("repairOf").contains(member)) compact["repairOf"][member]=row.at("repairOf").at(member);
            }
            Json result=handoff_summary(artifact);
            result.update({{"claimToken",current.at("claimToken")},{"expiresAt",current.at("expiresAt")},{"session",compact}});
            if (row.contains("workItemId")) {
                result["workItemId"]=row.at("workItemId");
                result["delivery"]["workItemId"]=row.at("workItemId");
            }
            if (!approved.is_null()) result["review"]=std::move(approved);
            return result;
        };
        if (!claim.empty() && claim.value("expiresAt",std::int64_t(0))<=now()) {
            claim=Json::object();
            queue.erase("claim");
            write_state_file(queue_file,queue);
        }
        if (!token.empty() && token==claim.value("claimToken","") && outcome.empty()) return present(claim);
        if (!token.empty() && token==queue.value("lastToken","")) {
            if (!claim_next) return {{"completed",true},{"claimReleased",true}};
            if (!claim.empty() && queue.value("nextToken","")==claim.value("claimToken","")) {
                auto response=present(claim);
                response["completed"]=true;
                return response;
            }
            return {{"completed",true},{"busy",!claim.empty()}};
        }
        if (!claim.empty()) {
            if (claim.at("claimToken")!=token) return {{"busy",true},{"expiresAt",claim.at("expiresAt")}};
            const auto key=claim.at("sessionId").get<std::string>();
            const auto version=claim.at("resultVersion").get<std::string>();
            const auto repair=queue.value("repair",Json::object());
            if (repair.value("parentId","")==key && repair.value("parentVersion","")==version)
                throw DispatchError("请先用原参数和 requestId 核对未确认的返修派发",
                    {{"reason","repair_dispatch_unconfirmed"},{"claimReleased",false}});
            const bool legacy=key.starts_with("codex:");
            bool changed;
            if (legacy) changed=!complete_legacy_handoff(settings,key,version,outcome);
            else {
                const auto current=refresh(data_,key,1900,false);
                require(!current.value("stale",false),"暂时无法确认已领取的结果，请重试");
                changed=current.at("resultVersion")!=version || active(current);
            }
            if (changed) {
                queue.erase("claim");
                write_state_file(queue_file,queue);
                return {{"changed",true}};
            }
            if (legacy) {
                Database db(data_,true);
                db.state.at("queues").at(queue_id_)["updated"]=now();
                db.save();
            } else {
                Database db(data_,true);
                auto& row=db.state["sessions"].at(key);
                require(result_visible(row),"结果尚未通过对抗审查");
                require(!active(row) && row.at("resultVersion")==version,"会话的结果版本已变化");
                row["read"]=true;
                row["processed"]=true;
                row["handoffOutcome"]=outcome;
                db.state.at("queues").at(queue_id_)["updated"]=now();
                db.save();
            }
            queue["lastToken"]=token;
            queue.erase("nextToken");
            queue.erase("claim");
            write_state_file(queue_file,queue);
            if (!claim_next) return {{"completed",true},{"claimReleased",true}};
        } else require(token.empty(),"领取凭据过期，请重新领取");

        auto rows=snapshot();
        const auto replaced=replaced_versions(rows);
        std::vector<Json> pending;
        std::size_t unconfirmed{};
        for (auto& row:rows) {
            if (!handoff_candidate(row,replaced)) continue;
            if (row.value("stale",false)) ++unconfirmed;
            else pending.push_back(std::move(row));
        }
        std::ranges::sort(pending,[](const Json& a,const Json& b) {
            return std::pair(!review_repair_needed(a),handoff_order(a))<
                std::pair(!review_repair_needed(b),handoff_order(b));
        });
        std::string result_error;
        const auto result_deadline=GetTickCount64()+5000;
        for (const auto& row:pending) {
            if (GetTickCount64()>=result_deadline) {
                ++unconfirmed;
                break;
            }
            claim={{"source",row.value("source","fatfishbox")},{"sessionId",row.at("id")},
                {"resultVersion",row.at("resultVersion")},{"claimToken",id()},
                {"expiresAt",now()+30*60*1000}};

            Json result;
            try { result=present(claim,true); }
            catch (const DshError& error) {
                result_error=error.what();
                continue;
            } catch (const DispatchError& error) {
                const auto reason=error.details.value("reason","");
                if (reason!="result_embargoed" && reason!="approved_delivery_unavailable") throw;
                result_error=error.what();
                continue;
            }
            if (result.is_null()) continue;
            queue["claim"]=claim;
            if (!token.empty()) queue["nextToken"]=claim.at("claimToken");
            write_state_file(queue_file,queue);
            result["completed"]=!token.empty();
            return result;
        }
        if (!dsh_ready) throw DispatchError("DSH 正在自动重启，请稍后重试",
            {{"reason","dsh_recovering"},{"retryAfterMs",2000},{"completed",!token.empty()}});
        if (unconfirmed || !result_error.empty()) throw DispatchError("待处理交接的结果暂时无法读取，请稍后重试",
            {{"reason","handoff_result_unavailable"},{"unconfirmed",unconfirmed},{"detail",result_error},{"completed",!token.empty()}});
        return {{"empty",true},{"completed",!token.empty()}};
    }
    if (tool=="cluster_list_dsh_profiles" || tool=="cluster_detect_dsh_connection") {
        Json profiles;
        std::string current;
        {
            Database db(data_,false);
            profiles=dsh_profiles(db.state);
            current=dsh_profile_id(db.state.at("settings"));
        }
        if (tool=="cluster_list_dsh_profiles") return {{"current",current},{"profiles",profiles}};
        Json hints=Json::array({input});
        for (const auto& profile:profiles) hints.push_back(profile);
        return discover_dsh_connections(hints);
    }
    if (tool=="cluster_save_dsh_profile") {
        Database db(data_,true);
        require(db.state.at("settings").at("revision")==input.at("revision"),"配置已变化，请刷新后重试");
        auto settings=dsh_profile_settings(db.state,input.at("profileId").get<std::string>());
        for (const auto member:{"configPath","authPath","permission","preset","automaticAuth","dshName"})
            settings[member]=input.at(member);
        require(settings.at("configPath")!="" || settings.at("authPath")!="","请填写 DSH 配置文件或本机授权文件");
        validate_configuration(settings);
        save_dsh_profile(db.state,std::move(settings));
        db.save();
        return db.state.at("settings");
    }
    if (tool=="cluster_get_presets" || tool=="cluster_import_models" ||
        tool=="cluster_test_dsh_connection" || tool=="cluster_check_dsh_version") {
        Json settings;
        {
            Database db(data_, false);
            settings=dsh_profile_settings(db.state,input.value("profileId",""));
            if (input.contains("revision"))
                require(settings.at("revision")==input.at("revision"),"配置已变化，请刷新后重试");
        }
        const auto connection = discover(settings, input);
        if (tool == "cluster_get_presets") {
            Json permissions=Json::array(), agents=Json::array();
            const auto append=[](Json& list,const std::string& key,const std::string& label) {
                if (!key.empty() && std::ranges::none_of(list,[&](const Json& row){return row.at("id")==key;}))
                    list.push_back({{"id",key},{"label",label.empty()?key:label}});
            };
            if (!connection.auth.empty()) {
                Dsh dsh(connection.auth);
                const auto catalog=dsh.request("permissionPresets/catalog");
                for (const auto& row:catalog.at("defaultOptions"))
                    append(permissions,row.at("value").get<std::string>(),row.value("name",""));
                const auto roster=dsh.request("agentPresets/list");
                for (const auto& row:roster.at("presets"))
                    if (row.value("broken","").empty())
                        append(agents,row.at("id").get<std::string>(),row.value("name",""));
            } else {
                append(permissions,connection.permission,"");
                append(agents,connection.preset,"");
            }
            return {{"permissions",permissions},{"agents",agents},{"connected",!connection.auth.empty()}};
        }
        require(!connection.auth.empty(), "未找到 DSH 本机授权文件，请检查 DSH 配置或填写授权文件路径");
        if (tool=="cluster_check_dsh_version") {
            const Dsh dsh(connection.auth,4000,6000);
            dsh.probe();
            const auto current=dsh.runtime_version();
            return {{"version",current},{"supported",current==supported_dsh_version}};
        }
        const bool importing=tool=="cluster_import_models";
        Dsh dsh(connection.auth,30000,40000);
        auto catalog=read_import_catalog(dsh,connection,importing);
        const auto [accounts,models]=catalog_size(catalog);
        const auto failures=catalog.find("failures");
        const auto failed_providers=failures!=catalog.end() && failures->is_array()?failures->size():0;
        if (!importing)
            return {{"accounts",accounts},{"models",models},{"failedProviders",failed_providers}};

        std::size_t configured_models=0;
        bool configuration_unavailable=false;
        try {
            auto combined=catalog;
            configured_models=add_configured_models(combined,dsh);
            catalog=std::move(combined);
        } catch (const DshError& error) {
            configuration_unavailable=error.code!="http_404" || failed_providers>0;
        } catch (const std::exception&) {
            configuration_unavailable=true;
        }
        require(models || configured_models || !failed_providers,
            "已连接 DSH，但模型目录查询失败。请在 DSH 中检查模型提供方配置和连接");
        auto summary = catalog_import(settings, catalog);
        summary["configuredModels"]=configured_models;
        summary["failedProviders"]=failed_providers;
        summary["configurationUnavailable"]=configuration_unavailable;
        settings["authPath"] = utf8(connection.auth);
        settings["configPath"] = utf8(connection.config);
        if (input.contains("automaticAuth")) settings["automaticAuth"]=input.at("automaticAuth");
        else if (input.contains("authPath") || input.contains("configPath"))
            settings["automaticAuth"]=input.value("authPath","").empty() && !connection.config.empty();
        if (input.contains("dshName")) settings["dshName"]=input.at("dshName");
        for (const auto member:{"permission","preset"})
            if (input.contains(member)) settings[member]=input.at(member);
        if (settings["permission"] == "") settings["permission"] = connection.permission;
        if (settings["preset"] == "") settings["preset"] = connection.preset;
        validate_configuration(settings);
        Database db(data_, true);
        require(db.state["settings"]["revision"] == settings["revision"], "设置已变化，请重试导入");
        save_dsh_profile(db.state,std::move(settings));
        db.save();
        summary["settings"] = db.state.at("settings");
        return summary;
    }
    if (tool == "cluster_configure" || tool == "cluster_update_pool") {
        Database db(data_, true);
        auto& settings = db.state["settings"];
        require(settings.at("revision") == input.at("revision"), "配置已变化，请刷新后重试");
        if (tool == "cluster_configure") {
            for (const auto member : {"configPath","authPath","permission","preset"})
                if (input.contains(member)) settings[member] = input.at(member);
            if (input.contains("authPath") || input.contains("configPath"))
                settings["automaticAuth"]=settings.at("authPath")=="" && settings.at("configPath")!="";
        } else {
            auto& pool = settings.at("pools").at(field(input, "id", 512));
            for (const auto member : {"enabled","concurrency","effort"})
                if (input.contains(member)) set_model_field(pool,member,input.at(member));
        }
        validate_configuration(settings);
        settings["revision"] = settings["revision"].get<int>() + 1;
        db.save();
        return settings;
    }
    if (tool == "cluster_start_session") {
        const auto operation=input.contains("requestId")?
            Json{{"kind","start"},{"requestId",field(input,"requestId",100)},{"input",input}}:Json();
        return start_session(input,id(),Json(),operation);
    }
    if (tool == "cluster_refresh_sessions") {
        const auto limit = input.value("limit", 12);
        require(limit >= 1 && limit <= 20, "同步数量必须在 1 至 20 之间");
        std::vector<std::pair<std::int64_t,std::string>> candidates;
        {
            Database db(data_, false);
            for (const auto& row : db.state.at("sessions"))
                if (in_queue(row,queue_id_,legacy_queue_id_) && active(row) && row.contains("startCursor"))
                    candidates.emplace_back(row.at("updated").get<std::int64_t>(), row.at("id").get<std::string>());
        }
        std::sort(candidates.begin(),candidates.end());
        Json sessions=Json::array(), errors=Json::array();
        for (std::size_t i=0; i<std::min(std::size_t(limit),candidates.size()); ++i) {
            const auto& current=candidates[i].second;
            try { sessions.push_back(refresh(data_,current)); }
            catch (const std::exception& e) { errors.push_back({{"sessionId",current},{"error",e.what()}}); }
        }
        return {{"sessions",sessions},{"errors",errors},{"total",candidates.size()}};
    }
    if (tool=="cluster_get_session_result") {
        const auto key=field(input,"sessionId",256);
        const auto row=invoke("cluster_get_session",{{"sessionId",key}});
        const auto version=input.contains("resultVersion")?field(input,"resultVersion",100):row.value("resultVersion","");
        auto artifact=load_result_artifact(data_,queue_id_,key,version);
        if (artifact.is_null()) {
            require(version==row.value("resultVersion",""),"该历史结果尚未归档");
            artifact=invoke("cluster_read_result",{{"sessionId",key}});
        }
        if (row.contains("workItemId")) {
            artifact["workItemId"]=row.at("workItemId");
            if (artifact.contains("delivery")) artifact["delivery"]["workItemId"]=row.at("workItemId");
        }
        artifact=attach_result_evidence(data_,queue_id_,row,std::move(artifact));
        return result_page(artifact,input);
    }
    const auto key = field(input,"sessionId",256);
    Json session_settings;
    {
        Database db(data_,false);
        if (key.starts_with("codex:")) {
            require(legacy_filter(db.state,queue_id_,legacy_queue_id_)(key),"当前队列没有该会话");
            session_settings=legacy_connection_settings(db.state);
        } else {
            const auto& sessions=db.state.at("sessions");
            require(sessions.contains(key) && in_queue(sessions.at(key),queue_id_,legacy_queue_id_),"当前队列没有该会话");
        }
    }
    if (key.starts_with("codex:")) {
        const auto& settings=session_settings;
        if (tool == "cluster_read_result") {
            const auto row=read_legacy_sessions(settings).rows.at(key);
            if (auto saved=load_result_artifact(data_,queue_id_,key,row.value("resultVersion",""));!saved.is_null()) return saved;
            return legacy_result(settings,key);
        }
        if (tool == "cluster_refresh_session") return live_legacy_session(settings,key);
        if (tool == "cluster_get_session") {
            auto row=read_legacy_sessions(settings).rows.at(key);
            Database db(data_,false);
            row["workItemId"]=session_work_item(db.state,row,queue_id_);
            return row;
        }
        throw std::runtime_error("此会话属于原 Codex 插件，请在原插件中取消或确认");
    }
    if (tool == "cluster_refresh_session") return refresh(data_,key);
    if (tool == "cluster_mark_receipt") {
        const ExclusiveStateLock lock(handoff_file(true));
        refresh(data_, key);
        Database db(data_, true);
        auto& row = db.state["sessions"].at(key);
        require(result_visible(row),"该结果尚未通过对抗审查");
        const auto work=row.value("workItemId","");
        const bool retired=db.state.value("workItems",Json::object()).contains(work) && db.state.at("workItems").at(work).at("state")=="cancelled";
        require(retired || !replaced_versions(db.state.at("sessions")).contains(handoff_version(row)),"请验收排队中的返修交接");
        require(!active(row) && row.at("resultVersion") == field(input,"resultVersion",100), "结果版本已变化，无法确认");
        row["read"] = true;
        row["processed"] = row.at("processed").get<bool>() || input.value("processed", false);
        db.save();
        return public_row(row);
    }
    Json profiles, row;
    {
        Database db(data_, false);
        profiles = dsh_profiles(db.state);
        row = db.state.at("sessions").at(key);
    }
    if (tool == "cluster_get_session") return public_row(row);
    if (tool=="cluster_read_result" && !active(row))
        if (auto saved=load_result_artifact(data_,queue_id_,key,row.value("resultVersion",""));!saved.is_null()) return saved;
    if (tool=="cluster_read_result" && !internal_read && !result_visible(row))
        throw DispatchError("对抗审查通过后才可读取开发结果",
            {{"reason","result_embargoed"},{"reviewState",review_worker(row)?"internal":review_state(row)}});
    const auto visible_report=[&](Json result) {
        if (row.contains("precheck")) result["review"]=review_notice(row);
        return result;
    };
    if (tool=="cluster_read_result" && row.value("resultVersion","")=="dispatch:failed") {
        auto failure=row.value("failure",Json{{"kind","dispatch"},{"message",row.value("error","派发失败")},
            {"stage",row.value("dispatch",Json::object()).value("stage","")}});
        for (const auto* member:{"retryable","retryAfterMs","retryMode"}) if (row.contains(member)) failure[member]=row.at(member);
        if (row.contains("failureStage")) failure["stage"]=row.at("failureStage");
        if (!failure.contains("code") && row.value("dispatch",Json::object()).contains("code")) failure["code"]=row.at("dispatch").at("code");
        return {{"sessionId",key},{"resultVersion","dispatch:failed"},{"messages",Json::array({
            {{"type","dispatch/error"},{"text",row.value("error","派发失败")}}})},{"failure",failure}};
    }
    if (tool=="cluster_read_result" && !internal_read && row.contains("precheck") && failed_handoff(row)) {
        const auto failure=row.value("failure",Json{{"kind","error"},{"message","DSH 会话执行失败"}});
        return {{"sessionId",key},{"resultVersion",row.at("resultVersion")},{"messages",Json::array()},{"failure",failure}};
    }
    if (tool=="cluster_read_result" && !active(row) && !row.value("resultVersion","").empty() &&
        row.contains("startCursor") && row.contains("cursor")) {
        const auto start=row.at("startCursor").get<std::int64_t>();
        const auto sid=row.value("dshId","");
        const auto history=read_dsh_history(dsh_history_file(sid,row.value("workspace","")),sid,start,
            row.at("cursor").get<std::int64_t>());
        if (history && row.at("resultVersion")==std::to_string(start)+":"+std::to_string(history->ending.seq)) {
            auto output=history->output;
            const bool clipped=false;
            Json result{{"sessionId",key},{"resultVersion",row.at("resultVersion")},
                {"resultSource","dsh_history"},{"messages",Json::array()}};
            if (!output.empty()) result["messages"].push_back({{"type","assistant/message"},{"text",output},{"truncated",clipped}});
            if (!history->ending.failure.empty()) result["failure"]=history->ending.failure;
            return visible_report(std::move(result));
        }
    }
    const bool verified=tool=="cluster_read_result" && !row.value("resultVersion","").empty() &&
        input.value("verifiedVersion",Json())==row.at("resultVersion");
    const auto dsh=session_connection(profiles,row,verified?1900:15000,verified?3500:0);
    const auto live=verified?Json::object():rpc_state(dsh,row);
    require(!verified || dsh.matches_owner(row.value("owner","")),"没有连接到该会话最初所属的 DSH 服务");
    if (tool == "cluster_cancel_session") {
        const ExclusiveStateLock lock(prompt_lock_file(data_,key));
        {
            Database db(data_,false);
            row=db.state.at("sessions").at(key);
        }
        require(active(row) && row.contains("startCursor"), "只能取消已派发的活动会话");
        if (row.contains("goal")) {
            const auto paused=dsh.call("commands/execute",{{"agentId",row.at("dshId")},{"line","/goal pause"},{"submittedAttachments",Json::array()}});
            require(paused.value("result",paused).value("kind","")=="success","停止项目经理前暂停goal失败");
        }
        dsh.request("session/cancel", {{"sessionId",row.at("dshId")}});
        if (row.contains("pendingPrompt")) {
            Database db(data_,true);
            db.state.at("sessions").at(key).erase("pendingPrompt");
            db.save();
        }
        return refresh(data_,key);
    }
    if (tool == "cluster_read_result") {
        const auto version=row.value("resultVersion","");
        if (version.empty()) row["cursor"]=live.at("cursor");
        auto failure=row.value("failure",Json::object());
        const auto status=row.value("status","");
        if (failure.empty() && (status=="failed" || status=="interrupted") && row.contains("startCursor")) {
            const auto start=row.at("startCursor").get<std::int64_t>();
            try {
                const auto end=latest_turn_end(dsh,row.at("dshId"),start,row.at("cursor").get<std::int64_t>());
                if (end && version==std::to_string(start)+":"+std::to_string(end->seq)) failure=end->failure;
            } catch (const std::exception&) {}
        }
        Json result{{"sessionId",key},{"resultVersion",version},{"messages",Json::array()}};
        if (!failure.empty()) result["failure"]=std::move(failure);
        Json snapshot;
        try { snapshot=report(dsh,row); }
        catch (const std::exception& error) {
            if (!result.contains("failure")) throw;
            result["readError"]=error.what();
            return visible_report(std::move(result));
        }
        Json messages = Json::array();
        std::size_t remaining = 16*1024*1024;
        const auto& records = snapshot.at("records");
        for (auto it = records.rbegin(); it != records.rend() && remaining; ++it) {
            if (!it->contains("event")) continue;
            const auto& event = it->at("event");
            if (row.contains("startCursor") && event.value("seq",std::int64_t(-1))<=row.at("startCursor").get<std::int64_t>())
                continue;
            const auto type = event.value("type","");
            if (type!="assistant/message" && type!="assistant/response" && type!="user/message") continue;
            const auto message_path="/data/message/content"_json_pointer;
            const auto direct_path="/data/content"_json_pointer;
            const auto path=event.contains(message_path)?message_path:direct_path;
            if (!event.contains(path)) continue;
            const auto& content=event.at(path);
            std::string text;
            if (content.is_string()) text = content.get<std::string>();
            else if (content.is_array()) for (const auto& part : content)
                if (part.is_object() && part.contains("text") && part.at("text").is_string()) text += part.at("text").get<std::string>();
            if (text.empty()) continue;
            const bool truncated = text.size() > remaining;
            if (truncated) {
                auto start = text.size() - remaining;
                while (start < text.size() && (static_cast<unsigned char>(text[start]) & 0xc0) == 0x80) ++start;
                text.erase(0, start);
            }
            remaining -= text.size();
            messages.push_back({{"type",type},{"text",text},{"truncated",truncated}});
        }
        std::reverse(messages.begin(), messages.end());
        result["messages"]=std::move(messages);
        result["truncated"]=snapshot.value("truncated",false) || remaining==0;
        return visible_report(std::move(result));
    }
    throw std::runtime_error("未知肥鱼重工操作");
}

Json Cluster::tools() {
    Json tools = Json::array();
    const Json text{{"type","string"}}, integer{{"type","integer"}}, boolean{{"type","boolean"}},
        pool_list{{"type","array"},{"items",text},{"minItems",1},{"maxItems",64}},
        path_list{{"type","array"},{"items",text},{"maxItems",16}};
    auto add = [&](const char* name, const char* description, Json fields = Json::object(), Json required = Json::array()) {
        if (queue_scoped_tool(name)) {
            fields["queueId"]={{"type","string"},{"description","Queue UUID."}};
            required.push_back("queueId");
        }
        tools.push_back({{"name",name},{"description",description},
            {"inputSchema",{{"type","object"},{"properties",fields},{"required",required},{"additionalProperties",false}}}});
    };
    add("cluster_create_queue","Create a queue for independent work. Reuse requestId on uncertain retries; workKey locates the same work across chats.",
        {{"name",text},{"requestId",text},{"workKey",text},{"workspace",text}},{"name","requestId"});
    add("cluster_rename_queue","Change the display name of the specified queue.",{{"queueId",text},{"name",text}},{"queueId","name"});
    add("cluster_find_queues","Find local queues by stable clientKey, workKey or absolute workspace. lastQueueId is the previous client binding. Verify ownership when multiple queues match.",
        {{"clientKey",text},{"workKey",text},{"workspace",text}});
    add("cluster_get_queue","Read a queue name, workspace, recent activity and pending counts.",{{"queueId",text}},{"queueId"});
    add("cluster_bind_queue","Bind a stable client/chat key to a queue for recovery. Every queue operation still requires queueId.",
        {{"queueId",text},{"clientKey",text}},{"queueId","clientKey"});
    add("cluster_assign_legacy_session","Assign an unclaimed legacy Codex session without repair links to a queue, using its original codex: ID.",
        {{"queueId",text},{"sessionId",text}},{"queueId","sessionId"});
    add("cluster_overview","Read queue activity, handoffs, unread counts and global account/model summaries.");
    add("cluster_get_settings","Read the current DSH connection, accounts, model pools and configuration revision.");
    add("cluster_get_presets","Read available DSH permission and agent presets for legacy callers; new sessions use danger-full-access and ptc.",
        {{"authPath",text},{"configPath",text}});
    add("cluster_import_models","Import accounts and models from the live DSH catalog and configured model roster.",
        {{"authPath",text},{"configPath",text}});
    add("cluster_configure","Update DSH connection configuration with revision protection. New sessions use fixed danger-full-access and ptc defaults.",
        {{"revision",integer},{"configPath",text},{"authPath",text},{"permission",text},{"preset",text}}, {"revision"});
    add("cluster_update_pool","Update enabled state, legacy pool capacity or reasoning effort. Team roles share the global maximum concurrency.",
        {{"revision",integer},{"id",text},{"enabled",boolean},{"concurrency",integer},{"effort",text}}, {"revision","id"});
    const Json nullable_text{{"type",Json::array({"string","null"})}},
        nullable_boolean{{"type",Json::array({"boolean","null"})}};
    add("cluster_create_work_item","Create a durable work item with requestId deduplication. task stores the original assignment; development, repair and review reference the same workItemId.",
        {{"title",text},{"requestId",text},{"task",text},{"workspace",text},{"baseSha",text}},{"title","requestId"});
    add("cluster_get_work_item","Read work, issues, reviews, acceptance and integration records. includeTask=true includes the original assignment.",
        {{"workItemId",text},{"includeTask",boolean}},{"workItemId"});
    add("cluster_list_work_items","Page the ledger with complete counts. Remaining includes accepted candidates awaiting integration; reconcile historical merge state.",
        {{"offset",integer},{"limit",integer},{"state",text},{"remainingOnly",boolean}});
    add("cluster_update_work_item","Update the ledger with revision protection and stable requestId. Acceptance requires an independent passing review; integrate records a real commit without performing a merge.",
        {{"workItemId",text},{"requestId",text},{"expectedRevision",integer},
         {"action",{{"type","string"},{"enum",Json::array({"add_issue","resolve_issue","record_review","accept","integrate","reopen","block","cancel","attach_session"})}}},
         {"details",{{"type","object"}}}},{"workItemId","requestId","expectedRevision","action","details"});
    add("cluster_next_handoff","Claim the oldest completed handoff summary. Reuse claimToken to read it; provide outcome to acknowledge it. claimNext=false only acknowledges the current claim.",
        {{"claimToken",nullable_text},{"outcome",nullable_text},{"claimNext",nullable_boolean}});
    tools.back()["title"]="获取下一个需处理对话";
    add("cluster_resume_handoff","Resume an existing claim without extending it. no_active_claim means a new claim is needed.");
    const Json event_target{{"type","object"},{"properties",{{"queueId",text},{"afterCursor",text}}},
        {"required",Json::array({"queueId"})},{"additionalProperties",false}};
    add("cluster_wait_events","Wait for batched cursor changes without claiming results. Omit afterCursor initially to establish a baseline; resetRequired means re-read current summaries.",
        {{"targets",{{"type","array"},{"minItems",1},{"maxItems",8},{"items",event_target}}},
         {"timeoutMs",{{"type","integer"},{"minimum",0},{"maximum",30000},{"default",25000}}},
         {"limit",{{"type","integer"},{"minimum",1},{"maximum",100},{"default",32}}}},{"targets"});
    add("cluster_wait_handoff","Wait for an available handoff, then claim when ready. timedOut means only that this wait expired; timeoutMs=0 checks immediately.",
        {{"timeoutMs",{{"type","integer"},{"minimum",0},{"maximum",30000},{"default",25000}}}});
    const Json evidence_file{{"type","object"},{"properties",{{"path",text},{"kind",{{"type","string"},
        {"enum",Json::array({"screenshot","executable","test_log","build_config","other"})}}}}},{"required",Json::array({"path"})},{"additionalProperties",false}};
    const Json reported_test{{"type","object"},{"properties",{{"command",text},{"exitCode",integer},{"logPath",text}}},
        {"required",Json::array({"command","exitCode"})},{"additionalProperties",false}};
    add("cluster_capture_evidence","Capture source identity, build metadata, file hashes and reported checks into reusable manifests. Explicit files may be external. This does not execute tests.",
        {{"sessionId",text},{"resultVersion",text},{"candidateSha",text},{"configuration",text},
         {"corrections",{{"type","array"},{"items",{{"type","object"},{"properties",{{"reportedPath",text},{"replacementPath",text},{"reason",text}}},{"required",Json::array({"reportedPath","replacementPath","reason"})}}}}},{"files",{{"type","array"},{"items",evidence_file},{"maxItems",128}}},
         {"tests",{{"type","array"},{"items",reported_test},{"maxItems",64}}}},{"sessionId"});
    add("cluster_get_evidence_manifest","Page evidence metadata by manifest reference, verifying content hash and queue ownership.",
        {{"manifestId",text},{"offset",integer},{"limit",integer}},{"manifestId"});
    add("cluster_send_message","Append constraints, baselines or defects to a locally dispatched active session. Default mode=steer. Stable requestId deduplicates persistently; accepted means queued only.",
        {{"sessionId",text},{"requestId",text},{"message",text},{"baseSha",text},
         {"issueIds",{{"type","array"},{"items",text},{"maxItems",32}}},
         {"mode",{{"type","string"},{"enum",Json::array({"steer","queue"})},{"default","steer"}}}},{"sessionId","requestId"});
    add("cluster_get_message","Read message enqueue state; includeMessage=true includes the original message arguments.",
        {{"sessionId",text},{"requestId",text},{"includeMessage",boolean}},{"sessionId","requestId"});
    add("cluster_start_session","Dispatch a session. Reuse requestId and exact arguments after uncertain responses; allowedPoolIds restricts a batch. Team jobs automatically balance all enabled models.",
        {{"title",text},{"batch",text},{"role",text},{"workspace",text},{"task",text},{"poolId",text},{"allowedPoolIds",pool_list},{"requestId",text},{"workItemId",text},{"baseSha",text}},
        {"title","batch","workspace","task"});
    add("cluster_repair_handoff","Dispatch a repair linked to the original handoff. claimReleased confirms claim release; unconfirmed retains the original claim.",
        {{"claimToken",text},{"originalVersion",text},{"requestId",text},{"candidateSha",text},{"screenshotPaths",path_list},
         {"acceptance",text},{"title",text},{"workspace",text},{"task",text},
         {"issueIds",{{"type","array"},{"items",text},{"maxItems",32}}},{"poolId",text},{"allowedPoolIds",pool_list}},
        {"claimToken","originalVersion","requestId","candidateSha","acceptance","title","workspace"});
    add("cluster_refresh_sessions","Synchronize the oldest locally registered active sessions, up to 20 per call.",{{"limit",integer}});
    add("cluster_get_session_status","Read status, title and repair links using returned id or dshId. Use id for ambiguous dshId; stale=true returns the last known status.",
        {{"sessionId",text}},{"sessionId"});
    add("cluster_get_session_result","Read archived delivery and log excerpts for an exact resultVersion. Page with nextOffset; this does not change claims.",
        {{"sessionId",text},{"resultVersion",text},{"offset",{{"type","integer"},{"minimum",0}}},
         {"limit",{{"type","integer"},{"minimum",4},{"maximum",24000}}}},{"sessionId"});
    add("cluster_review_issues","Read historical automatic-review issues and blockers. Inspect delivery and existing reviewer results before deciding whether a fresh review is needed.");
    add("cluster_attach_handoff_repair","Link an existing same-batch repair covering current issues. Check its status and handoff first. After claimReleased=true, immediately claim the next item.",
        {{"claimToken",text},{"originalVersion",text},{"sessionId",text}},{"claimToken","originalVersion","sessionId"});
    add("cluster_detach_handoff_repair","Detach an incorrect, unacknowledged repair link using the exact originalSessionId/originalVersion. If claimed, supply the repair claimToken. Eligible original handoffs return to the queue.",
        {{"sessionId",text},{"originalSessionId",text},{"originalVersion",text},{"claimToken",text}},
        {"sessionId","originalSessionId","originalVersion"});
    add("cluster_cancel_session","Cancel a known active session. Already terminal sessions do not need cancellation; close obsolete ledger items explicitly instead.",{{"sessionId",text}},{"sessionId"});
    for (auto& entry:team_tools()) tools.push_back(std::move(entry));
    return tools;
}
}
