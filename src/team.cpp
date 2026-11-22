#include "team.h"
#include "team_scheduler.h"
#include "diagnostics.h"
#include "dsh.h"
#include "state_file.h"
#include "team_prompts.h"
#include "handoff_artifacts.h"
#include <windows.h>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <fstream>
#include <ranges>
#include <set>
#include <thread>
#include <vector>

namespace fatfish {
namespace {
void check(bool yes,const char* message) { if (!yes) throw std::runtime_error(message); }
std::string text(const Json& j,const char* key,std::size_t max=8000) {
    check(j.contains(key) && j.at(key).is_string(),"缺少必需文本字段");
    const auto s=j.at(key).get<std::string>();
    check(!s.empty() && s.size()<=max && s.find('\0')==s.npos && s.find_first_not_of(" \r\n\t")!=s.npos,"文本为空或超出长度限制");
    return s;
}
bool terminal(const std::string& s) { return s=="succeeded" || s=="failed" || s=="cancelled" || s=="interrupted"; }
bool sha(const std::string& s) { return (s.size()==40 || s.size()==64) && s.find_first_not_of("0123456789abcdefABCDEF")==s.npos; }
Json defaults() {
    return {{"revision",0},{"totalConcurrency",8},{"developerConcurrency",4},{"reviewerConcurrency",2},
        {"leadPool",""},{"developerPool",""},{"reviewerPool",""},{"integratorPool",""}};
}
class Store {
public:
    explicit Store(const fs::path& data):file(data/L"teams.json"),lock(data/L"teams.lock") {
        value=fs::exists(file)?read_json(file,32*1024*1024):Json{{"settings",defaults()},{"projects",Json::object()},{"requests",Json::object()}};
    }
    void save() { write_state_file(file,value); }
    Json value;
private:
    fs::path file;
    ExclusiveStateLock lock;
};
// Errors belong to their operation and scope; a successful unrelated step must
// never hide a still-failing cancellation or receipt. No network I/O under Store.
class RuntimeHealth {
public:
    RuntimeHealth(const fs::path& data,std::string project,const Json& errors)
        :data_(data),project_(std::move(project)) {
        for (const auto& [key,value]:errors.items()) known_.insert(key);
    }
    template<class Work> bool run(const std::string& operation,const std::string& scope,Work&& work) {
        const auto key=operation+":"+scope;
        try {
            work();
            resolve(operation,scope);
            return true;
        } catch (const std::exception& error) { failed(key,operation,scope,error.what()); }
          catch (...) { failed(key,operation,scope,"Unknown non-standard exception"); }
        return false;
    }
    void resolve(const std::string& operation,const std::string& scope) {
        const auto key=operation+":"+scope;
        if (!known_.contains(key)) return;
        Store s(data_);s.value["projects"][project_]["runtimeErrors"].erase(key);s.save();known_.erase(key);
    }
private:
    void failed(const std::string& key,const std::string& operation,const std::string& scope,std::string message) noexcept {
        try {
            if (message.size()>1000) {
                std::size_t end=1000;
                while (end>0 && (static_cast<unsigned char>(message[end])&0xc0)==0x80) --end;
                message.resize(end);
            }
            Store s(data_);auto& errors=s.value["projects"][project_]["runtimeErrors"];
            if (errors.is_null()) errors=Json::object();
            const auto previous=errors.value(key,Json::object());const auto time=now();
            const bool log=previous.value("message","")!=message || time-previous.value("lastLoggedAt",0LL)>=60000;
            if (log) diagnostics::record("team-runtime",Json{{"projectId",project_},{"operation",operation},{"scope",scope},{"message",message}}.dump());
            errors[key]={{"operation",operation},{"scope",scope},{"message",message},
                {"failures",previous.value("failures",0LL)+1},{"firstFailureAt",previous.value("firstFailureAt",time)},
                {"lastFailureAt",time},{"retryAt",time+2000},{"lastLoggedAt",log?time:previous.value("lastLoggedAt",0LL)}};
            while (errors.size()>32) {
                auto oldest=errors.begin();
                for (auto i=errors.begin();i!=errors.end();++i) if (i.value().at("lastFailureAt")<oldest.value().at("lastFailureAt")) oldest=i;
                known_.erase(oldest.key());errors.erase(oldest);
            }
            s.save();known_.insert(key);
        } catch (...) {diagnostics::caught();}
    }
    fs::path data_;
    std::string project_;
    std::set<std::string> known_;
};
Json& project(Store& s,const Json& in) {
    const auto key=text(in,"projectId",100);
    check(s.value.at("projects").contains(key),"团队项目不存在");
    return s.value.at("projects").at(key);
}
Json& domain(Json& p,const Json& in) {
    const auto key=text(in,"domainId",100);
    check(p.at("domains").contains(key),"项目中不存在该领域");
    auto& d=p.at("domains").at(key);
    check(d.at("token")==text(in,"token",100),"领域凭据不匹配");
    return d;
}
void event(Json& p,const std::string& kind,const Json& detail) {
    const auto sequence=p.value("sequence",0LL)+1;
    p["sequence"]=sequence;p["updated"]=now();
    p["events"].push_back({{"sequence",sequence},{"kind",kind},{"detail",detail},{"time",now()}});
    if (p["events"].size()>256) p["events"].erase(p["events"].begin());
}
Json project_summary(const Json& p) {
    Json result;
    for (const auto* k:{"projectId","queueId","title","workspace","baseSha","status","charterVersion","updated","sequence"})
        if (p.contains(k)) result[k]=p.at(k);
    result["domains"]=Json::array();
    int complete=0,pending=0,running=0;
    for (const auto& d:p.at("domains")) {
        Json row;for (const auto* k:{"domainId","name","mission","state","summary","evidence","acceptanceResults","lastError"})
            if (d.contains(k)) row[k]=d.at(k);
        std::int64_t latest=-1;
        for (const auto& job:p.at("jobs")) if (job.at("role")=="lead" && job.at("domainId")==d.at("domainId") && job.value("created",0LL)>=latest) {
            latest=job.value("created",0LL);row["leadState"]=job.at("state");row["leadSessionId"]=job.value("sessionId","");
        }
        if (d.at("state")=="completed") ++complete;
        result["domains"].push_back(std::move(row));
    }
    for (const auto& j:p.at("jobs")) {
        if (j.at("role")=="lead") continue;
        if (j.at("state")=="queued") ++pending;
        else if (!terminal(j.at("state").get<std::string>())) ++running;
    }
    result["progress"]={{"completedDomains",complete},{"totalDomains",p.at("domains").size()},{"queuedJobs",pending},{"runningJobs",running}};
    if (p.contains("backlog")) result["backlog"]={{"pending",p.at("backlog").value("pending",0)},{"actionable",p.at("backlog").value("actionable",0)},{"waiting",p.at("backlog").value("waiting",0)},{"processed",p.at("backlog").value("processed",0)}};
    result["runtimeErrors"]=p.value("runtimeErrors",Json::object());
    result["priorityResults"]=p.value("priorityResults",Json::array());
    if (p.contains("history")) result["historyCounts"]=p.at("history").value("counts",Json::object());
    result["decisions"]=Json::array();
    for (const auto& decision:p.at("decisions")) if (decision.at("state")=="open") result["decisions"].push_back(decision);
    return result;
}
void valid_charter(const Json& c) {
    check(c.is_object(),"charter 必须为对象");
    for (const auto* k:{"objective","acceptance","constraints","architecture","authority","escalation"}) text(c,k,12000);
    check(c.dump().size()<48000,"授权书总长度须小于 48000 字节");
}
std::wstring quote(const std::wstring& arg) {
    std::wstring out=L"\"";std::size_t slashes=0;
    for (const auto ch:arg) {
        if (ch==L'\\') {++slashes;continue;}
        if (ch==L'"') out.append(slashes*2+1,L'\\');else out.append(slashes,L'\\');
        slashes=0;out+=ch;
    }
    out.append(slashes*2,L'\\');return out+L'"';
}
std::string git(const fs::path& root,const std::vector<std::wstring>& args) {
    // No shell and no command interpolation. Output goes to a file so a full pipe
    // cannot deadlock the wait. A job object kills hung git and its descendants.
    auto path=fs::temp_directory_path()/path_from_utf8("fatfish-git-"+id()+".log");
    SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};
    HANDLE log=CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ,&security,CREATE_ALWAYS,FILE_ATTRIBUTE_TEMPORARY,nullptr);
    check(log!=INVALID_HANDLE_VALUE,"无法创建 Git 诊断文件");
    HANDLE input=CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&security,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    HANDLE job=CreateJobObjectW(nullptr,nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (job) SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limits,sizeof(limits));
    std::wstring command=L"git.exe -C "+quote(root.native());for (const auto& a:args) command+=L" "+quote(a);
    STARTUPINFOW start{};start.cb=sizeof(start);start.dwFlags=STARTF_USESTDHANDLES;start.hStdOutput=log;start.hStdError=log;
    start.hStdInput=input;
    PROCESS_INFORMATION process{};
    const bool created=CreateProcessW(nullptr,command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW|CREATE_SUSPENDED,nullptr,nullptr,&start,&process);
    if (created) {
        if (job) AssignProcessToJobObject(job,process.hProcess);
        ResumeThread(process.hThread);
    }
    DWORD exit=1;
    if (created) {
        if (WaitForSingleObject(process.hProcess,30000)!=WAIT_OBJECT_0) {TerminateProcess(process.hProcess,1);WaitForSingleObject(process.hProcess,5000);}
        GetExitCodeProcess(process.hProcess,&exit);CloseHandle(process.hThread);CloseHandle(process.hProcess);
    }
    CloseHandle(log);if (input!=INVALID_HANDLE_VALUE) CloseHandle(input);if (job) CloseHandle(job);
    std::ifstream file(path,std::ios::binary);std::string output(8192,'\0');file.read(output.data(),output.size());output.resize(static_cast<std::size_t>(file.gcount()));file.close();
    std::error_code ignored;fs::remove(path,ignored);
    if (!created || exit!=0) throw std::runtime_error("Git 操作失败："+output);
    while (!output.empty() && (output.back()=='\n' || output.back()=='\r')) output.pop_back();
    return output;
}
std::string role_pool(const Json& settings,const std::string& role) {
    return settings.at(role=="repair"?"developerPool":role+"Pool").get<std::string>();
}
Json& authorize_job(Json& p,const Json& d,const std::string& key) {
    check(p.at("jobs").contains(key) && p.at("jobs").at(key).at("domainId")==d.at("domainId"),"任务不属于当前领域");
    return p.at("jobs").at(key);
}
void running_project(const Json& p) { check(p.at("status")=="running","项目已暂停或结束，不能派发或改变执行状态"); }
std::string backlog_owner(const Json& p) {
    if (p.contains("backlogOwnerDomainId")) return p.at("backlogOwnerDomainId").get<std::string>();
    for (const auto& d:p.at("domains")) if (d.at("dependencies").empty()) return d.at("domainId").get<std::string>();
    return p.at("domains").begin().key();
}
Json backlog_snapshot(const Cluster& cluster,const Json& p) {
    const auto snapshot=cluster.for_queue(p.at("queueId").get<std::string>()).desktop_snapshot();
    Json result={{"pending",0},{"actionable",0},{"waiting",0},{"signature",""}};
    std::string signature;
    for (const auto& row:snapshot.at("pending")) {
        if (row.value("role","")=="lead") continue;
        result["pending"]=result.at("pending").get<int>()+1;
        bool linked=false;const auto work=row.value("workItemId","");
        if (!work.empty()) for (const auto& job:p.at("jobs")) if (job.at("role")!="lead" && !terminal(job.at("state").get<std::string>()) && job.value("workItemId","")==work) linked=true;
        const auto key=row.at("id").get<std::string>()+"@"+row.value("resultVersion","");
        if (p.contains("backlog") && p.at("backlog").contains("blocked")) {
            const auto& blocked=p.at("backlog").at("blocked");
            if (blocked.contains(key)) {
                const auto decision=blocked.at(key).value("decisionId","");
                linked=linked || (!decision.empty() && p.at("decisions").contains(decision) && p.at("decisions").at(decision).at("state")=="open");
            }
        }
        const auto field=linked?"waiting":"actionable";result[field]=result.at(field).get<int>()+1;
        if (!linked) signature+=key+"\n";
    }
    result["signature"]=sha256(signature);return result;
}
Json priority_results(const Cluster& cluster,const Json& p,const std::string& owner={}) {
    // Derive readiness from current ledger/result versions, not model promises
    // or receipt order. Historical archives must not starve the delivery path.
    std::map<std::string,Json> latest;
    for (const auto& job:p.at("jobs")) {
        if (job.at("role")=="lead" || job.value("historical",false) || !job.contains("workItemId")) continue;
        if (!owner.empty() && job.at("domainId")!=owner) continue;
        const auto key=job.at("workItemId").get<std::string>();
        if (!latest.contains(key) || latest.at(key).value("created",0LL)<job.value("created",0LL)) latest[key]=job;
    }
    std::vector<Json> results;const auto scoped=cluster.for_queue(p.at("queueId").get<std::string>());
    std::map<std::string,Json> ledger;
    const auto read=[&](const std::string& key)->const Json& {if (!ledger.contains(key)) ledger[key]=scoped.invoke("cluster_get_work_item",{{"workItemId",key}});return ledger.at(key);};
    for (const auto& [work,job]:latest) {
        if (!terminal(job.at("state").get<std::string>()) || job.value("resultVersion","").empty()) continue;
        const auto& item=read(work);
        const auto state=item.at("state").get<std::string>();
        if (state=="integrated" || state=="cancelled" || state=="superseded") continue;
        bool pending=false;
        bool integration_candidate=false;std::string completed_integration;
        for (const auto& other:p.at("jobs")) if (other.at("role")=="integrator") {
            if (other.value("workItemId","")==work) integration_candidate=true;
            if (other.at("state")=="cancelled") continue;
            for (const auto& source:other.value("sourceWorkItemIds",Json::array())) if (source==work) {
                if (!other.contains("workItemId")) pending=true;
                else {
                    const auto& child=read(other.at("workItemId").get<std::string>());
                    if (child.at("state")=="integrated") completed_integration=child.at("workItemId").get<std::string>();
                    else if (child.at("state")!="cancelled") pending=true;
                }
            }
        }
        for (const auto& other:p.at("jobs")) if (other.at("role")!="lead" && !terminal(other.at("state").get<std::string>())) {
            if (other.value("workItemId","")==work) pending=true;
            for (const auto& source:other.value("sourceWorkItemIds",Json::array())) if (source==work) pending=true;
        }
        for (const auto& decision:p.at("decisions")) if (decision.at("state")=="open" && decision.value("jobId","")==job.at("jobId").get<std::string>()) pending=true;
        if (pending) continue;
        std::string reason;
        if (state=="accepted") reason=integration_candidate?"ready_for_main_merge":completed_integration.empty()?"ready_for_integration":"record_source_integration";
        else if (job.at("role")=="reviewer") {
            Json review;
            for (const auto& r:item.at("reviews")) if (r.at("sessionId")==job.at("sessionId") && r.at("resultVersion")==job.at("resultVersion")) review=r;
            if (review.is_null()) reason="review_result_unhandled";
            else if (review.at("verdict")!="passed") reason="review_requires_repair";
            else reason="verify_issues_and_accept";
        } else reason=job.at("state")=="succeeded"?"candidate_needs_review":"execution_failed";
        results.push_back({{"jobId",job.at("jobId")},{"domainId",job.at("domainId")},{"workItemId",work},{"role",job.at("role")},
            {"sessionId",job.value("sessionId","")},{"resultVersion",job.at("resultVersion")},{"reason",reason},{"integrationWorkItemId",completed_integration},{"created",job.value("created",0LL)}});
    }
    std::ranges::sort(results,[](const Json& a,const Json& b){return a.at("created").get<long long>()<b.at("created").get<long long>();});
    return Json(results);
}
// A domain can have many independent chains. Waiting on one chain must not
// park owned, idle work or the backlog owner's unassigned historical inventory.
Json domain_worklist(const Cluster& cluster,const Json& p,const std::string& owner) {
    Json ready=Json::array();const auto& d=p.at("domains").at(owner);
    std::set<std::string> owned;
    for (const auto& key:d.value("workItemIds",Json::array())) owned.insert(key.get<std::string>());
    for (const auto& j:p.at("jobs")) if (j.at("domainId")==owner && j.at("role")!="lead" && j.contains("workItemId")) owned.insert(j.at("workItemId").get<std::string>());
    const auto scoped=cluster.for_queue(p.at("queueId").get<std::string>());
    for (const auto& key:owned) {
        bool linked=false;
        for (const auto& j:p.at("jobs")) if (j.at("domainId")==owner && j.at("role")!="lead" && j.value("workItemId","")==key) {
            if (!terminal(j.at("state").get<std::string>())) linked=true;
            for (const auto& dec:p.at("decisions")) if (dec.at("state")=="open" && dec.value("jobId","")==j.at("jobId").get<std::string>()) linked=true;
        }
        if (linked) continue;
        const auto item=scoped.invoke("cluster_get_work_item",{{"workItemId",key}});
        const auto state=item.at("state").get<std::string>();
        if (state=="integrated" || state=="accepted" || state=="cancelled" || state=="superseded") continue;
        ready.push_back({{"workItemId",key},{"title",item.at("title")},{"action","inspect_and_dispatch"}});
    }
    if (owner==backlog_owner(p) && p.contains("history"))
        for (const auto& [key,item]:p.at("history").at("items").items()) if (item.value("domainId","").empty())
            ready.push_back({{"workItemId",key},{"title",item.at("title")},{"action","claim_or_assign_or_defer"}});
    return ready;
}
// One durable FIFO for the coordinator. A finished child only adds a ready
// action; it never interrupts the action currently being handled.
Json coordinator_queue(const Cluster& cluster,const fs::path& data,const Json& p,const std::string& owner) {
    const auto results=priority_results(cluster,p,owner);
    const auto worklist=domain_worklist(cluster,p,owner);
    std::map<std::string,Json> candidates;std::set<std::string> covered;
    for (const auto& result:results) {
        auto row=result;row["kind"]="result";
        const auto key="result:"+result.at("jobId").get<std::string>()+":"+result.at("resultVersion").get<std::string>()+":"+result.at("reason").get<std::string>();
        row["actionId"]=key;candidates[key]=row;covered.insert(result.at("workItemId").get<std::string>());
    }
    for (const auto& row:worklist) if (!covered.contains(row.at("workItemId").get<std::string>())) {
        auto item=row;const auto key="work:"+row.at("workItemId").get<std::string>();item["kind"]="work";item["actionId"]=key;candidates[key]=item;
    }
    if (owner==backlog_owner(p)) {
        const auto snapshot=cluster.for_queue(p.at("queueId").get<std::string>()).desktop_snapshot();
        for (const auto& row:snapshot.at("pending")) {
            if (row.value("role","")=="lead") continue;
            const auto work=row.value("workItemId","");bool waiting=covered.contains(work);
            for (const auto& job:p.at("jobs")) if (job.at("role")!="lead" && job.value("workItemId","")==work) {
                if (!terminal(job.at("state").get<std::string>())) waiting=true;
                for (const auto& dec:p.at("decisions")) if (dec.at("state")=="open" && dec.value("jobId","")==job.at("jobId").get<std::string>()) waiting=true;
            }
            const auto blocked=p.value("backlog",Json::object()).value("blocked",Json::object());
            const auto receipt=row.at("id").get<std::string>()+"@"+row.value("resultVersion","");
            if (blocked.contains(receipt)) {const auto dec=blocked.at(receipt).value("decisionId","");if(p.at("decisions").contains(dec) && p.at("decisions").at(dec).at("state")=="open") waiting=true;}
            if (waiting) continue;
            const auto key="handoff:"+receipt;
            candidates[key]={{"actionId",key},{"kind","handoff"},{"sessionId",row.at("id")},{"workItemId",work},{"resultVersion",row.value("resultVersion","")},{"completed",row.value("completed",0LL)}};
        }
    }
    Store s(data);auto& live=s.value["projects"][p.at("projectId").get<std::string>()];auto& d=live["domains"][owner];
    Json order=Json::array(),items=Json::array();std::set<std::string> seen;
    for (const auto& key:d.value("actionQueue",Json::array())) if (candidates.contains(key.get<std::string>()) && seen.insert(key.get<std::string>()).second) order.push_back(key);
    std::vector<std::string> added;for(const auto& [key,row]:candidates) if (!seen.contains(key)) added.push_back(key);
    const auto arrival=now();
    std::ranges::sort(added,[&](const std::string& a,const std::string& b){const auto at=candidates.at(a).value("completed",arrival),bt=candidates.at(b).value("completed",arrival);return at==bt?a<b:at<bt;});
    for (const auto& key:added) order.push_back(key);
    for (const auto& key:order) items.push_back(candidates.at(key.get<std::string>()));
    if (d.value("actionQueue",Json::array())!=order) {d["actionQueue"]=order;s.save();}
    return {{"items",items},{"count",items.size()},{"empty",items.empty()},{"head",items.empty()?Json():items.front()},{"policy","fifo"}};
}
Json domain_action_contract() {
    return {{"operation","cluster_update_work_item"},{"actions",Json::array({"record_review","add_issue","resolve_issue","accept","integrate","reopen"})},
        {"example",{{"tool","team_domain_action"},{"arguments",{{"projectId","<current projectId>"},{"domainId","<current domainId>"},{"token","<current token>"},
            {"operation","cluster_update_work_item"},{"arguments",{{"workItemId","<owned workItemId>"},{"expectedRevision",0},{"requestId","<stable operation ID>"},{"action","record_review"},
                {"details",{{"sessionId","<reviewer sessionId>"},{"resultVersion","<exact resultVersion>"},{"candidateSha","<full candidate SHA>"},{"verdict","blocked"},{"notes","<verified conclusion>"}}}}}}}}},
        {"rule","Replace placeholders with real context; expectedRevision comes from the current ledger. action belongs inside inner arguments, never in operation. Do not probe invented operation names. Read team_get_contract once; if still blocked escalate with jobId and the exact error."}};
}
Json lead_tool_contract() {
    Json selected=Json::array();
    for (const auto& tool:team_tools()) {
        const auto& fields=tool.at("inputSchema").at("properties");
        if (fields.contains("domainId") || tool.at("name")=="team_get_prompt") selected.push_back(tool);
    }
    return {{"version",7},{"tools",selected},{"domainAction",domain_action_contract()}};
}
Json job_view(const Json& j) {
    Json out;for (const auto* k:{"jobId","domainId","title","role","state","sessionId","workItemId","baseSha","workspace","resultVersion","sourceJobId","error","accepted"})
        if (j.contains(k)) out[k]=j.at(k);
    return out;
}
void incident(Json& p,Json& d,const std::string& key,const std::string& reason) {
    if (p.at("decisions").contains(key)) return;
    p["decisions"][key]={{"decisionId",key},{"domainId",d.at("domainId")},{"state","open"},{"revision",0},
        {"question","团队执行需要处理"},{"reason",reason},{"recommendation","修复连接或明确下一步后恢复负责人"},
        {"alternatives","暂停项目并保留所有产物"},{"impact","当前领域无法继续"},{"blockedScope",d.at("name")}};
    d["state"]="decision";event(p,"incident",p["decisions"][key]);
}
Json new_job(const Json& p,const Json& d,const std::string& role,const std::string& title,const std::string& task,const std::string& base) {
    const auto key=id();
    return {{"jobId",key},{"domainId",d.at("domainId")},{"role",role},{"title",title},{"task",task},
        {"baseSha",base},{"state","queued"},{"created",now()},{"attempts",0},{"requestId","team-"+key},
        {"charterVersion",p.at("charterVersion")}};
}
std::string cursor(const Json& p) {return p.at("projectId").get<std::string>()+":"+std::to_string(p.at("sequence").get<long long>());}
long long parse_cursor(const Json& p,const std::string& value) {
    const auto prefix=p.at("projectId").get<std::string>()+":";
    check(value.starts_with(prefix),"游标不属于此项目");
    long long number=-1;const auto tail=value.substr(prefix.size());
    const auto [end,error]=std::from_chars(tail.data(),tail.data()+tail.size(),number);
    check(error==std::errc{} && end==tail.data()+tail.size() && number>=0 && number<=p.at("sequence").get<long long>(),"决策游标无效");
    return number;
}
}

std::string team_prompt(std::string_view role) {
    if (role=="principal") return principal_prompt;
    if (role=="lead") return lead_prompt;
    return worker_prompt;
}
bool team_managed_queue(const fs::path& data,std::string_view queue) {
    Store store(data);for (const auto& p:store.value.at("projects")) if (p.value("queueId","")==queue) return true;
    return false;
}
void check_team_reservation(const fs::path& data,const Json& sessions,const Json& input) {
    if (!input.contains("teamProjectId")) return;
    Store store(data);const auto& settings=store.value.at("settings");
    const auto& project=store.value.at("projects").at(input.at("teamProjectId").get<std::string>());
    const auto& job=project.at("jobs").at(input.at("teamJobId").get<std::string>());
    if (project.at("status")!="running" || job.value("cancelRequested",false) ||
        input.value("teamCharterVersion",0)!=project.at("charterVersion").get<int>())
        throw DispatchError("项目授权或执行状态已变化，停止旧任务派发",{{"reason","team_stale_authorization"},{"retryable",false}});
    const auto role=input.at("role").get<std::string>();
    std::set<std::string> historical;
    for (const auto& p:store.value.at("projects")) for (const auto& j:p.at("jobs")) if (j.value("historical",false) && j.contains("sessionId")) historical.insert(j.at("sessionId").get<std::string>());
    int all=0,developers=0,reviewers=0;
    for (const auto& s:sessions) if ((s.contains("teamProjectId") || historical.contains(s.value("id",""))) && !terminal(s.value("status",""))) {
        ++all;const auto r=s.value("role","");if (r=="developer" || r=="repair") ++developers;if (r=="reviewer") ++reviewers;
    }
    if (all>=settings.at("totalConcurrency").get<int>() ||
        ((role=="developer" || role=="repair") && developers>=settings.at("developerConcurrency").get<int>()) ||
        (role=="reviewer" && reviewers>=settings.at("reviewerConcurrency").get<int>()))
        throw DispatchError("团队并发已满，任务保持排队",{{"reason","team_capacity"},{"retryable",true},{"retryAfterMs",2000}});
}

Json team_invoke(const Cluster& cluster,const fs::path& data,std::string_view tool,const Json& in) {
    check(in.is_object(),"参数须为对象");
    if (tool=="team_get_prompt") {const auto role=in.value("role","principal");return {{"version",role=="worker"?2:7},{"prompt",team_prompt(role)}};}
    if (tool=="team_get_settings") { Store s(data);return s.value.at("settings"); }
    if (tool=="team_update_settings") {
        const auto pools=cluster.invoke("cluster_get_settings").at("pools");
        Store s(data);auto& settings=s.value["settings"];
        check(in.at("revision")==settings.at("revision"),"团队配置已变化，请刷新后保存");
        auto next=settings;
        for (const auto* k:{"leadPool","developerPool","reviewerPool","integratorPool"}) {
            const auto key=text(in,k,100);check(pools.contains(key) && pools.at(key).value("enabled",false),"所选角色模型不存在或未启用");next[k]=key;
        }
        for (const auto* k:{"totalConcurrency","developerConcurrency","reviewerConcurrency"}) {
            check(in.contains(k) && in.at(k).is_number_integer(),"并发须为整数");const auto n=in.at(k).get<int>();check(n>=1 && n<=64,"团队并发范围为 1–64");next[k]=n;
        }
        check(next.at("developerConcurrency")<=next.at("totalConcurrency") && next.at("reviewerConcurrency")<=next.at("totalConcurrency"),"角色并发不能超过总并发");
        next["revision"]=settings.at("revision").get<int>()+1;settings=next;s.save();return next;
    }
    if (tool=="team_list_projects") {
        Store s(data);Json rows=Json::array();for (const auto& p:s.value.at("projects")) rows.push_back(project_summary(p));return {{"projects",rows}};
    }
    if (tool=="team_get_queue" || tool=="team_desktop_queue") {
        const auto queue=text(in,"queueId",100);cluster.for_queue(queue);
        Store s(data);Json bound=nullptr;
        for (const auto& p:s.value.at("projects")) if (p.value("queueId","")==queue) {bound=project_summary(p);break;}
        auto& modes=s.value["queueModes"];if (modes.is_null()) modes=Json::object();
        if (in.contains("mode")) {
            check(tool=="team_desktop_queue","队列模式只能从桌面修改");
            const auto mode=text(in,"mode",20);check(mode=="direct" || mode=="team","未知队列执行方式");
            check(bound.is_null() || mode=="team","此队列已有团队运行记录，不能切回直接管理");modes[queue]=mode;s.save();
        }
        return {{"queueId",queue},{"mode",bound.is_null()?modes.value(queue,"direct"):"team"},{"project",bound},
            {"principalPromptVersion",4},{"historyTakeoverSupported",true},
            {"nextAction",bound.is_null()?"task_authorized_then_team_start_project_same_queue":"resume_bound_project"},
            {"guidance","请使用 team_get_prompt({role:principal}) 的 v7 规程。负责人通过 team_next_action 机械循环处理统一FIFO；结果排队，不抢占，处理本条后继续下一条。负责人持续接续，不索要第二条调度指令。"}};
    }
    if (tool=="team_start_project") {
        const auto request=text(in,"requestId",100),title=text(in,"title",200);
        valid_charter(in.at("charter"));
        const auto work=path_from_utf8(text(in,"workspace",4000));check(work.is_absolute() && fs::is_directory(work),"项目工作区须为现有绝对目录");
        const auto workspace=fs::canonical(work);
        Json p;
        {
            Store s(data);
            if (s.value.at("requests").contains(request)) {
                const auto& old=s.value.at("requests").at(request);check(old.at("input")==in,"requestId 已用于其他项目参数");return project_summary(s.value.at("projects").at(old.at("projectId").get<std::string>()));
            }
        }
        const auto queue=in.value("queueId",std::string());
        Json history={{"counts",Json::object()},{"items",Json::object()}};
        if (!queue.empty()) {
            const auto scoped=cluster.for_queue(queue);int offset=0;
            for (;;) {
                const auto ledger=scoped.invoke("cluster_list_work_items",{{"limit",100},{"offset",offset},{"remainingOnly",true}});
                history["counts"]=ledger.at("counts");
                for (const auto& item:ledger.at("items")) history["items"][item.at("workItemId").get<std::string>()]={{"title",item.at("title")},{"state",item.at("state")},{"domainId",""}};
                if (ledger.at("nextOffset").is_null()) break;offset=ledger.at("nextOffset").get<int>();
            }
        }
        check(git(workspace,{L"status",L"--porcelain"}).empty(),"项目工作树存在未提交改动；请先明确整理基线，不能把用户改动隐式丢在子工作树之外");
        const auto base=git(workspace,{L"rev-parse",L"--verify",L"HEAD"});check(sha(base),"项目必须有有效 Git HEAD");
        const auto ds=in.at("domains");check(ds.is_array() && !ds.empty() && ds.size()<=8,"须提供 1–8 个领域");
        std::set<std::string> names,seen;Json domains=Json::object();std::string first_domain;
        for (const auto& entry:ds) check(names.insert(text(entry,"name",100)).second,"领域名称不可重复");
        // Require topological order: it makes dependency mistakes explicit for small models.
        for (const auto& entry:ds) {
            const auto name=text(entry,"name",100),key=id();const auto deps=entry.value("dependencies",Json::array());
            if (first_domain.empty()) first_domain=key;
            check(deps.is_array() && deps.size()<=8,"dependencies 须为领域名称数组");
            for (const auto& dep:deps) check(dep.is_string() && seen.contains(dep.get<std::string>()),"依赖必须是排在当前领域之前的领域，禁止循环依赖");
            domains[key]={{"domainId",key},{"name",name},{"mission",text(entry,"mission",6000)},{"acceptance",text(entry,"acceptance",6000)},
                {"dependencies",deps},{"token",id()},{"state","pending"},{"wake",1},{"checkpoint",Json::object()}};seen.insert(name);
        }
        const auto settings=cluster.invoke("cluster_get_settings");
        {
            Store s(data);
            // Serialize creation against retries after the Git checks.
            if (s.value.at("requests").contains(request)) {
                const auto& old=s.value.at("requests").at(request);check(old.at("input")==in,"requestId 参数不一致");return project_summary(s.value.at("projects").at(old.at("projectId").get<std::string>()));
            }
            for (const auto* k:{"leadPool","developerPool","reviewerPool","integratorPool"}) {
                const auto pool=s.value.at("settings").at(k).get<std::string>();
                check(!pool.empty() && settings.at("pools").contains(pool) && settings.at("pools").at(pool).value("enabled",false),"请在设置 → 团队执行中配置四个角色的可用模型");
            }
            if (!queue.empty()) for (const auto& existing:s.value.at("projects")) check(existing.value("queueId","")!=queue,"此队列已经绑定团队，请续接原项目");
            const auto key=id();p={{"projectId",key},{"title",title},{"workspace",utf8(workspace)},{"baseSha",base},{"status","starting"},
                {"charter",in.at("charter")},{"charterVersion",1},{"config",s.value.at("settings")},{"domains",domains},
                {"jobs",Json::object()},{"decisions",Json::object()},{"events",Json::array()},{"operations",Json::object()},{"sequence",0},{"updated",now()}};
            if (!queue.empty()) {p["queueId"]=queue;p["status"]="running";}
            p["history"]=history;
            p["backlogOwnerDomainId"]=first_domain;
            s.value["projects"][key]=p;s.value["requests"][request]={{"projectId",key},{"input",in}};s.save();
        }
        // Queue creation is recovered by advance_teams after interruption.
        return project_summary(p);
    }
    if (tool=="team_get_project") {
        Store s(data);const auto& p=project(s,in);auto out=project_summary(p);
        if (in.value("includeCharter",false)) out["charter"]=p.at("charter");return out;
    }
    if (tool=="team_backlog_status" || tool=="team_next_handoff" || tool=="team_priority_results") {
        Json pcopy;std::string owner;
        {Store s(data);auto& p=project(s,in);auto& d=domain(p,in);pcopy=p;owner=backlog_owner(p);
            if (tool=="team_next_handoff") {running_project(p);check(d.at("domainId")==owner,"队列交接由指定值守负责人处理，其他领域继续实施任务");}}
        const auto scoped=cluster.for_queue(pcopy.at("queueId").get<std::string>());
        const auto urgent=priority_results(cluster,pcopy,text(in,"domainId",100));
        if (tool=="team_priority_results") return {{"items",urgent},{"count",urgent.size()},{"policyVersion",7}};
        if (tool=="team_backlog_status") {
            auto status=backlog_snapshot(cluster,pcopy);status["ownerDomainId"]=owner;
            status["processed"]=pcopy.value("backlog",Json::object()).value("processed",0);
            status["priorityResults"]=urgent;status["priorityCount"]=urgent.size();
            return status;
        }
        const auto handoff_call=[&](std::string_view name,const Json& arguments) {
            try {return scoped.invoke(name,arguments);}
            catch (const std::exception& e) {
                Store s(data);auto& p=project(s,in);auto& b=p["backlog"];if (b.is_null()) b=Json::object();
                b["retryAt"]=now()+30000;b["lastError"]=std::string(e.what());s.save();throw;
            }
        };
        Json args=Json::object();const auto outcome=in.value("outcome","");std::string operation;
        bool replay=false;
        if (!outcome.empty()) {
            operation=owner+":handoff:"+text(in,"requestId",100);
            if (pcopy.contains("backlog") && pcopy.at("backlog").contains("operations") && pcopy.at("backlog").at("operations").contains(operation)) {
                const auto& saved=pcopy.at("backlog").at("operations").at(operation);check(saved.at("input")==in,"交接 requestId 参数不一致");
                if (saved.contains("result")) return saved.at("result");args=saved.at("arguments");replay=true;
            }
        }
        if (!replay) {
            const auto backlog=pcopy.value("backlog",Json::object());
            if (backlog.contains("claim")) args["claimToken"]=backlog.at("claim").at("claimToken");
            const auto resumed=handoff_call("cluster_resume_handoff",Json::object());
            if (resumed.contains("claimToken")) args["claimToken"]=resumed.at("claimToken");
            else args.erase("claimToken");
            if (!outcome.empty()) {
                check(args.contains("claimToken"),"请先领取本条交接，再登记处置");
                const auto claim=handoff_call("cluster_next_handoff",args);check(claim.contains("session"),"领取已变化，请重新领取");
                check(claim.at("claimToken")==text(in,"claimToken",100),"处置必须绑定返回的领取凭据；当前交接已变化，不能用上一条证据确认下一条");
                const auto work=claim.value("workItemId",claim.at("session").value("workItemId",""));
                const auto notes=text(in,"notes",1000);
                check(outcome=="confirm" || outcome=="linked" || outcome=="blocked","outcome 须为 confirm/linked/blocked");
                if (outcome=="confirm") {
                    check(!work.empty(),"交接尚未关联总账，不能直接确认");
                    const auto item=scoped.invoke("cluster_get_work_item",{{"workItemId",work}});
                    check(item.at("state")=="integrated" || item.at("state")=="accepted" || item.at("state")=="cancelled","工作项未验收/集成/取消；应先核实、返修或关联在途任务，不能直接确认");
                    const auto evidence=in.value("evidence",Json::array());check(evidence.is_array() && !evidence.empty() && evidence.size()<=8 && evidence.dump().size()<=4000,"确认历史交接须附 1–8 项核验依据引用");
                    for (const auto& e:evidence) check(e.is_string() && !e.get<std::string>().empty(),"证据引用须为非空文本");
                } else if (outcome=="linked") {
                    const auto key=text(in,"jobId",100);check(pcopy.at("jobs").contains(key),"关联任务不存在");const auto& job=pcopy.at("jobs").at(key);
                    check(job.at("role")!="lead" && !terminal(job.at("state").get<std::string>()) && !work.empty() && job.value("workItemId","")==work,"必须关联同一工作项的真实在途任务，不能用无关任务跳过缺陷");
                } else {
                    const auto decision=text(in,"decisionId",100);check(pcopy.at("decisions").contains(decision) && pcopy.at("decisions").at(decision).at("state")=="open","必须先登记具体阻塞决策");
                }
                args["outcome"]=outcome+": "+notes;args["claimNext"]=true;
                Store s(data);auto& p=project(s,in);domain(p,in);running_project(p);auto& b=p["backlog"];if (b.is_null()) b=Json::object();
                b["operations"][operation]={{"input",in},{"arguments",args}};s.save();
            }
        }
        auto result=handoff_call("cluster_next_handoff",args);
        {
            Store s(data);auto& p=project(s,in);domain(p,in);auto& b=p["backlog"];if (b.is_null()) b=Json::object();
            if (result.contains("session") && result.contains("claimToken")) {
                b["claim"]={{"claimToken",result.at("claimToken")},{"sessionId",result.at("session").at("id")},{"workItemId",result.value("workItemId","")},{"resultVersion",result.value("resultVersion","")}};
                b.erase("retryAt");
            } else if (result.value("empty",false) || result.value("claimReleased",false) || result.value("completed",false) || result.value("changed",false)) b.erase("claim");
            if (!outcome.empty()) {b["operations"][operation]["result"]=result;if (result.value("completed",false)) {b["processed"]=b.value("processed",0)+1;b["lastProgressAt"]=now();}}
            if (result.value("empty",false) || result.value("busy",false)) b["retryAt"]=now()+30000;
            b["lastScanAt"]=now();s.save();
        }
        return result;
    }
    if (tool=="team_wait_decisions") {
        const auto timeout=in.value("timeoutMs",25000);check(timeout>=0 && timeout<=30000,"等待范围为 0–30000 毫秒");
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(timeout);
        for (;;) {
            Json out;bool ready=false;
            {
                Store s(data);const auto& p=project(s,in);out={{"projectId",p.at("projectId")},{"status",p.at("status")},{"cursor",cursor(p)},{"events",Json::array()}};
                const auto after=in.contains("afterCursor")?parse_cursor(p,text(in,"afterCursor",140)):-1LL;
                const auto oldest=p.at("events").empty()?p.at("sequence").get<long long>():p.at("events").front().at("sequence").get<long long>();
                out["resetRequired"]=after>=0 && after<oldest-1;
                if (after<0 || out.at("resetRequired")==true) {
                    for (const auto& d:p.at("decisions")) if (d.at("state")=="open") out["events"].push_back({{"kind","decision"},{"detail",d}});
                    if (p.at("status")!="running" && p.at("status")!="starting") out["events"].push_back({{"kind","project.status"},{"detail",{{"status",p.at("status")}}}});
                } else {
                    auto last=after;
                    for (const auto& e:p.at("events")) if (e.at("sequence").get<long long>()>after && out["events"].size()<8) {out["events"].push_back(e);last=e.at("sequence").get<long long>();}
                    out["hasMore"]=last<p.at("sequence").get<long long>();
                    if (!out.at("events").empty()) out["cursor"]=p.at("projectId").get<std::string>()+":"+std::to_string(last);
                }
                ready=!out.at("events").empty();out["ready"]=ready;
            }
            if (ready || std::chrono::steady_clock::now()>=deadline) {out["timedOut"]=!ready;return out;}
            Sleep(100);
        }
    }
    if (tool=="team_resolve_decision" || tool=="team_update_charter" || tool=="team_control_project") {
        Store s(data);auto& p=project(s,in);const auto request=text(in,"requestId",100);
        auto& operations=p["operations"];
        if (operations.contains(request)) {check(operations.at(request).at("input")==in,"requestId 已用于其他操作参数");return operations.at(request).at("result");}
        if (tool=="team_resolve_decision") {
            const auto key=text(in,"decisionId",100);check(p.at("decisions").contains(key),"决策不存在");auto& d=p["decisions"][key];
            check(d.at("state")=="open" && d.at("revision")==in.at("expectedRevision"),"决策已处理或版本已变化");
            d["resolution"]=text(in,"resolution",8000);d["state"]="resolved";d["revision"]=d.at("revision").get<int>()+1;
            auto& owner=p["domains"][d.at("domainId").get<std::string>()];owner["wake"]=owner.at("wake").get<int>()+1;owner["state"]="working";
            if (d.at("domainId")==backlog_owner(p) && p.contains("backlog")) p["backlog"]["suspendedOnStall"]=false;
        } else if (tool=="team_update_charter") {
            check(p.at("status")!="cancelled","已取消项目不能更新授权");
            check(p.at("charterVersion")==in.at("expectedVersion"),"授权书版本已变化");valid_charter(in.at("charter"));text(in,"changeSummary",4000);
            p["charter"]=in.at("charter");p["charterVersion"]=p.at("charterVersion").get<int>()+1;
            for (auto& job:p["jobs"]) if (job.at("state")=="queued" && !job.contains("launch") && !job.contains("sessionId")) {job["state"]="cancelled";job["error"]="授权已更新；负责人须按新版本重新评估任务";}
            for (auto& d:p["domains"]) {d["wake"]=d.at("wake").get<int>()+1;if (d.at("state")=="completed") d["state"]="working";}
            if (p.at("status")=="completed") p["status"]="running";
            event(p,"charter.changed",{{"version",p.at("charterVersion")},{"summary",in.at("changeSummary")}});
        } else {
            const auto action=text(in,"action",32);check(action=="pause" || action=="resume" || action=="cancel","action 须为 pause/resume/cancel");
            check(p.at("status")!="cancelled","已取消项目不可恢复");
            if (action=="resume") check(p.at("status")=="paused","只能恢复已暂停项目");
            p["status"]=action=="pause"?"paused":action=="cancel"?"cancelled":"running";
            if (action=="cancel") for (auto& j:p["jobs"]) if (j.at("state")=="queued") j["state"]="cancelled";
            if (action=="resume") for (auto& d:p["domains"]) if (d.at("state")!="completed") d["wake"]=d.at("wake").get<int>()+1;
            event(p,"project.status",{{"status",p.at("status")},{"note","暂停停止新派发；已运行任务由后台请求取消并核对"}});
        }
        auto result=project_summary(p);operations[request]={{"input",in},{"result",result}};s.save();return result;
    }
    if (tool=="team_domain_history") {
        std::string queue;Json ownership;
        {Store s(data);auto& p=project(s,in);domain(p,in);queue=p.at("queueId").get<std::string>();ownership=p.value("history",Json::object()).value("items",Json::object());
            for (const auto& job:p.at("jobs")) if (job.contains("workItemId") && !ownership.contains(job.at("workItemId").get<std::string>())) ownership[job.at("workItemId").get<std::string>()]={{"domainId",job.at("domainId")}};
        }
        const auto offset=in.value("offset",0),limit=in.value("limit",32);check(offset>=0 && limit>=1 && limit<=64,"历史分页范围为 1–64");
        auto page=cluster.for_queue(queue).invoke("cluster_list_work_items",{{"offset",offset},{"limit",limit},{"remainingOnly",!in.value("includeClosed",false)}});
        for (auto& item:page["items"]) {
            const auto key=item.at("workItemId").get<std::string>();item["ownerDomainId"]=ownership.contains(key)?ownership.at(key).value("domainId",""):"";
        }
        return page;
    }
    if (tool=="team_claim_work_item") {
        const auto key=text(in,"workItemId",100),request=text(in,"requestId",100);std::string queue,owner;
        const bool deferred=in.value("disposition","adopt")=="defer";
        check(in.value("disposition","adopt")=="adopt" || deferred,"disposition 须为 adopt/defer");
        const auto reason=deferred?text(in,"reason",2000):std::string();
        {
            Store s(data);auto& p=project(s,in);auto& d=domain(p,in);running_project(p);owner=d.at("domainId").get<std::string>();queue=p.at("queueId").get<std::string>();
            for (const auto& job:p.at("jobs")) if (!job.value("historical",false) && job.value("workItemId","")==key) {
                check(job.at("domainId")==owner,"该工作项已经由其他领域管理");check(!deferred,"团队已派发的实施项不能改为跳过");
                return {{"workItemId",key},{"domainId",owner},{"alreadyManaged",true},{"importedSessionCount",0},{"requestId",request}};
            }
            if (p.contains("history") && p.at("history").at("items").contains(key)) {
                const auto assigned=p.at("history").at("items").at(key).value("domainId","");
                check(assigned.empty() || assigned==owner,"该历史工作项已由另一个领域负责人接管");
                if (!assigned.empty()) check(p.at("history").at("items").at(key).value("deferred",false)==deferred,"不能将已接管工作改为跳过，须完成或升级给主代理");
            }
        }
        const auto scoped=cluster.for_queue(queue);const auto item=scoped.invoke("cluster_get_work_item",{{"workItemId",key}});
        check(item.at("state")!="integrated" && item.at("state")!="cancelled" && item.at("state")!="superseded","已集成、已取消或归并的工作项不自动重新实施");
        Json imported=Json::array();
        for (const auto& [sid,row]:item.at("sessions").items()) {
            if (deferred) break;
            const auto role=row.value("role","developer");if (role=="lead") continue;
            Json job={{"jobId","history-"+sha256(key+"\n"+sid)},{"domainId",owner},{"title",item.at("title")},{"role",role},{"state",row.at("status")},
                {"sessionId",sid},{"workItemId",key},{"historical",true},{"accepted",item.at("state")=="accepted"},
                {"baseSha",row.value("baseSha",Json())},{"created",row.value("updated",0LL)},{"attempts",0}};
            job["observedTerminal"]=terminal(row.at("status").get<std::string>()) && !row.value("stale",false);
            for (const auto* field:{"workspace","resultVersion"}) if (row.contains(field)) job[field]=row.at(field);
            imported.push_back(std::move(job));
        }
        Store s(data);auto& p=project(s,in);auto& d=domain(p,in);running_project(p);
        auto& history=p["history"];if (history.is_null()) history={{"counts",Json::object()},{"items",Json::object()}};
        auto& items=history["items"];if (items.contains(key)) {
            const auto assigned=items.at(key).value("domainId","");check(assigned.empty() || assigned==owner,"该历史工作项已被其他领域接管");
            if (!assigned.empty()) check(items.at(key).value("deferred",false)==deferred,"不能改变已登记的历史处置");
        }
        items[key]={{"title",item.at("title")},{"state",item.at("state")},{"domainId",owner},{"deferred",deferred},{"reason",reason}};
        if (!deferred) {
            if (!d.contains("workItemIds")) d["workItemIds"]=Json::array();
            if (std::ranges::find(d.at("workItemIds"),Json(key))==d.at("workItemIds").end()) d["workItemIds"].push_back(key);
        }
        for (auto& job:imported) {const auto jid=job.at("jobId").get<std::string>();if (!p.at("jobs").contains(jid)) p["jobs"][jid]=std::move(job);}
        s.save();return {{"workItemId",key},{"domainId",owner},{"importedSessionCount",imported.size()},{"state",item.at("state")},{"requestId",request},{"deferred",deferred}};
    }
    if (tool=="team_get_contract") {Store s(data);auto& p=project(s,in);domain(p,in);return lead_tool_contract();}
    if (tool=="team_next_action") {
        Json copy;{Store s(data);auto& p=project(s,in);domain(p,in);running_project(p);copy=p;}
        return coordinator_queue(cluster,data,copy,text(in,"domainId",100));
    }
    if (tool=="team_domain_context" || tool=="team_domain_jobs") {
        Json urgent=Json::array(),worklist=Json::array(),queue;
        if (tool=="team_domain_context") {
            Json copy;{Store s(data);auto& p=project(s,in);domain(p,in);copy=p;}
            urgent=priority_results(cluster,copy,text(in,"domainId",100));
            worklist=domain_worklist(cluster,copy,text(in,"domainId",100));
            queue=coordinator_queue(cluster,data,copy,text(in,"domainId",100));
        }
        Store s(data);auto& p=project(s,in);auto& d=domain(p,in);
        Json jobs=Json::array();
        for (const auto& j:p.at("jobs")) if (j.at("domainId")==d.at("domainId") && j.at("role")!="lead" && (in.value("includeAccepted",false) || !j.value("accepted",false))) jobs.push_back(job_view(j));
        const auto offset=in.value("offset",0),limit=in.value("limit",32);check(offset>=0 && limit>=1 && limit<=64,"任务分页范围为 1–64，offset 须非负");
        Json page=Json::array();const auto end=std::min(jobs.size(),static_cast<std::size_t>(offset)+limit);
        for (auto i=static_cast<std::size_t>(offset);i<end;++i) page.push_back(jobs.at(i));
        if (tool=="team_domain_jobs") return {{"jobs",page},{"total",jobs.size()},{"nextOffset",end<jobs.size()?Json(end):Json()}};
        Json out={{"projectId",p.at("projectId")},{"queueId",p.value("queueId","")},{"status",p.at("status")},{"charter",p.at("charter")},
            {"charterVersion",p.at("charterVersion")},{"workspace",p.at("workspace")},{"baseSha",p.at("baseSha")},{"domain",d},{"jobs",Json::array()},{"decisions",Json::array()},{"dependencies",Json::array()}};
        out["domain"].erase("token");
        out["domainActionContract"]=domain_action_contract();out["toolContractTool"]="team_get_contract";out["workQueue"]=queue;out["readyWorkItems"]=worklist;out["priorityResults"]=urgent;out["rolePromptVersion"]=7;
        out["dispatchPolicy"]="统一FIFO：team_next_action取队首，处理/派后继后取下一项；子任务结果排队，不抢占当前项。priorityResults为兼容字段，不表示抢占优先级。";
        out["backlog"]={{"ownerDomainId",backlog_owner(p)},{"isOwner",d.at("domainId")==backlog_owner(p)},
            {"pending",p.value("backlog",Json::object()).value("pending",0)},{"actionable",p.value("backlog",Json::object()).value("actionable",0)},
            {"waiting",p.value("backlog",Json::object()).value("waiting",0)},{"processed",p.value("backlog",Json::object()).value("processed",0)}};
        out["historyCounts"]=p.value("history",Json::object()).value("counts",Json::object());
        out["unclaimedHistory"]=Json::array();int unclaimed=0;
        if (p.contains("history")) for (const auto& [key,item]:p.at("history").at("items").items()) if (item.value("domainId","").empty()) {
            ++unclaimed;if (out["unclaimedHistory"].size()<32) out["unclaimedHistory"].push_back({{"workItemId",key},{"title",item.at("title")},{"state",item.at("state")}});
        }
        out["unclaimedHistoryCount"]=unclaimed;
        out["jobs"]=page;out["totalUnacceptedJobs"]=jobs.size();out["nextJobOffset"]=end<jobs.size()?Json(end):Json();
        Json resolved=Json::array();
        for (const auto& dec:p.at("decisions")) if (dec.at("domainId")==d.at("domainId")) {
            if (dec.at("state")=="open") out["decisions"].push_back(dec);else resolved.push_back(dec);
        }
        if (resolved.size()>8) resolved.erase(resolved.begin(),resolved.end()-8);
        for (const auto& dec:resolved) out["decisions"].push_back(dec);
        for (const auto& other:p.at("domains")) for (const auto& dep:d.at("dependencies")) if (other.at("name")==dep) {
            Json row=other;row.erase("token");row.erase("checkpoint");out["dependencies"].push_back(std::move(row));
        }
        return out;
    }
    if (tool=="team_delegate" || tool=="team_checkpoint" || tool=="team_escalate" || tool=="team_finish_domain" || tool=="team_cancel_job") {
        {
            Store s(data);auto& p=project(s,in);auto& d=domain(p,in);
            const auto key=d.at("domainId").get<std::string>()+":"+text(in,"requestId",100);
            if (p.at("operations").contains(key)) {
                const auto& old=p.at("operations").at(key);check(old.at("input")==in,"requestId 参数不一致");return old.at("result");
            }
        }
        Json reviewed;
        if (tool=="team_delegate" && in.value("role","")=="integrator") {
            check(!in.contains("workItemId") && !in.contains("sourceJobId"),"最终集成须创建新的集成工作项，不得把新会话挂回已验收源项；用 sourceWorkItemIds 保留来源");
            const auto sources=in.value("sourceWorkItemIds",Json::array());
            check(sources.is_array() && !sources.empty() && sources.size()<=32,"最终 integrator 必须提供 1–32 个 sourceWorkItemIds，不能丢失待集成来源");
            std::string queue;
            {Store s(data);auto& p=project(s,in);auto& d=domain(p,in);queue=p.at("queueId").get<std::string>();
                for (const auto& key:sources) {
                    check(key.is_string(),"集成来源须为工作项 ID");bool own=false;
                    if (d.contains("workItemIds")) own=std::ranges::find(d.at("workItemIds"),key)!=d.at("workItemIds").end();
                    for (const auto& j:p.at("jobs")) if (j.at("domainId")==d.at("domainId") && j.value("workItemId","")==key.get<std::string>()) own=true;
                    check(own,"集成来源必须属于本领域");
                }
            }
            for (const auto& key:sources) {const auto item=cluster.for_queue(queue).invoke("cluster_get_work_item",{{"workItemId",key}});check(item.at("state")=="accepted","源工作项须先独立审查并验收，再派最终集成");}
        }
        if (tool=="team_delegate" && in.value("role","")=="reviewer") {
            Json source;std::string queue;
            {Store s(data);auto& p=project(s,in);auto& d=domain(p,in);source=authorize_job(p,d,text(in,"sourceJobId",100));queue=p.at("queueId").get<std::string>();}
            check(source.at("role")=="developer" || source.at("role")=="repair" || source.at("role")=="integrator","reviewer 必须审查实施任务");
            check(source.at("state")=="succeeded" && source.contains("resultVersion"),"被审任务尚未成功结束并同步结果版本");
            reviewed=cluster.for_queue(queue).invoke("cluster_get_session_result",{{"sessionId",source.at("sessionId")},{"resultVersion",source.at("resultVersion")},{"limit",4}});
            check(reviewed.at("delivery").value("candidateSha",Json()).is_string() && reviewed.at("delivery").at("candidateSha")==in.value("baseSha",Json()),"审查基线必须等于原任务结构化交付中的精确候选 SHA");
        }
        // Completion checks read the ledger before acquiring the team lock.
        Json ledger;
        Json backlog_guard;
        Json priority_guard;
        Json worklist_guard;
        if (tool=="team_checkpoint" || tool=="team_finish_domain") {
            Json pcopy;bool owner=false;
            {Store s(data);auto& p=project(s,in);auto& d=domain(p,in);owner=d.at("domainId")==backlog_owner(p);pcopy=p;}
            if (owner) backlog_guard=backlog_snapshot(cluster,pcopy);
            priority_guard=priority_results(cluster,pcopy,text(in,"domainId",100));
            worklist_guard=domain_worklist(cluster,pcopy,text(in,"domainId",100));
        }
        if (tool=="team_finish_domain") {
            std::string queue;
            {Store s(data);auto& p=project(s,in);domain(p,in);queue=p.at("queueId").get<std::string>();}
            ledger={{"items",Json::array()}};int offset=0;
            for (;;) {
                const auto page=cluster.for_queue(queue).invoke("cluster_list_work_items",{{"limit",100},{"offset",offset}});
                for (const auto& item:page.at("items")) ledger["items"].push_back(item);
                if (page.at("nextOffset").is_null()) break;offset=page.at("nextOffset").get<int>();
            }
        }
        Store s(data);auto& p=project(s,in);auto& d=domain(p,in);running_project(p);
        const auto request=d.at("domainId").get<std::string>()+":"+text(in,"requestId",100);auto& ops=p["operations"];
        if (ops.contains(request)) {check(ops.at(request).at("input")==in,"requestId 参数不一致");return ops.at(request).at("result");}
        check(d.at("state")!="completed","领域已完成");Json result;
        if (tool=="team_delegate") {
            check(in.contains("charterVersion") && in.at("charterVersion")==p.at("charterVersion"),"授权书已变化或缺少 charterVersion，请重新 team_domain_context 后按当前版本派发");
            const auto role=text(in,"role",32);check(role=="developer" || role=="reviewer" || role=="repair" || role=="integrator","未知执行角色");
            check(p.at("jobs").size()<512,"项目最多 512 个任务，请分阶段验收并启动下一项目");
            auto base=in.value("baseSha",p.at("baseSha").get<std::string>());check(sha(base),"baseSha 必须为完整提交 SHA");
            auto job=new_job(p,d,role,text(in,"title",200),text(in,"task",16000),base);
            if (role=="integrator") {job["sourceWorkItemIds"]=in.at("sourceWorkItemIds");job["task"]=job.at("task").get<std::string>()+"\n已验收的源工作项："+in.at("sourceWorkItemIds").dump()+"。在隔离工作树整合与累计验证，返回新的集成候选供独立审查；不自动合入主工作树。";}
            if (in.contains("sourceJobId")) {
                const auto source=text(in,"sourceJobId",100);const auto& original=authorize_job(p,d,source);
                check(original.at("role")!="lead" && terminal(original.at("state").get<std::string>()),"被审查/返修任务必须已经结束");
                check(original.contains("workItemId"),"原任务没有工作项");job["sourceJobId"]=source;job["workItemId"]=original.at("workItemId");
                if (!reviewed.is_null()) {
                    check(original.at("resultVersion")==reviewed.at("resultVersion"),"原结果在准备审查期间已变化，请重新核对");
                    job["sourceVersion"]=original.at("resultVersion");
                }
            } else if (in.contains("workItemId")) {
                const auto key=text(in,"workItemId",100);check(role!="reviewer","reviewer 必须绑定 sourceJobId 的精确候选");
                check(d.contains("workItemIds") && std::ranges::find(d.at("workItemIds"),Json(key))!=d.at("workItemIds").end(),"请先接管该工作项再派发续接任务");job["workItemId"]=key;
            } else check(role!="reviewer" && role!="repair","repair 需提供已接管 workItemId 或 sourceJobId；reviewer 必须提供 sourceJobId");
            p["jobs"][job.at("jobId").get<std::string>()]=job;result=job_view(job);
        } else if (tool=="team_cancel_job") {
            auto& job=authorize_job(p,d,text(in,"jobId",100));check(job.at("role")!="lead","不能通过此入口取消负责人自身");
            const auto reason=text(in,"reason",2000);job["cancelRequested"]=true;job["cancelReason"]=reason;
            if (job.at("state")=="queued" && !job.contains("launch") && !job.contains("sessionId")) job["state"]="cancelled";
            result={{"jobId",job.at("jobId")},{"state",job.at("state")},{"cancelRequested",true},{"note","活动会话由后台取消并核对终态；不推断取消成功"}};
        } else if (tool=="team_checkpoint") {
            const auto state=text(in,"state",32);check(state=="working" || state=="waiting" || state=="decision" || state=="continue","checkpoint state 无效");
            check(in.at("checkpoint").is_object() && in.at("checkpoint").dump().size()<=16000,"checkpoint 必须是小于 16000 字节的对象");
            if (state=="waiting" || state=="decision") check(worklist_guard.empty(),"仍有未接管或本领域无在途后继的工作项，不能等待整批结束；读取 team_domain_context.readyWorkItems，先派下一项或绑定具体阻塞");
            const bool retry_wait=!backlog_guard.is_null() && backlog_guard.at("pending").get<int>()>0 && p.value("backlog",Json::object()).value("retryAt",0LL)>now();
            if (state=="waiting" || state=="decision") check(priority_guard.is_null() || priority_guard.empty(),"还有结果/验收/集成阶段在队列中，不能全局等待；请继续 team_next_action 循环");
            if (state=="waiting" || state=="decision") check(backlog_guard.is_null() || backlog_guard.at("actionable")==0 || retry_wait,"队列还有可处理积压，不能因一条返修/决策等待而退出；请继续处理或 checkpoint state=continue 自动接续");
            if (state=="waiting") check(retry_wait || std::ranges::any_of(p.at("jobs"),[&](const Json& j){return j.at("domainId")==d.at("domainId") && j.at("role")!="lead" && !terminal(j.at("state").get<std::string>());}),"没有等待中的执行任务，请继续处理积压、升级或完成领域");
            if (state=="decision") check(std::ranges::any_of(p.at("decisions"),[&](const Json& dec){return dec.at("domainId")==d.at("domainId") && dec.at("state")=="open";}),"尚无待决策，请先 team_escalate");
            d["checkpoint"]=in.at("checkpoint");d["checkpointVersion"]=p.at("charterVersion");d["state"]=state=="continue"?"working":state;
            if (state=="continue") d["wake"]=d.at("wake").get<int>()+1;
            else if (state=="waiting" || state=="decision") {
                // This validated checkpoint acknowledges updates already consumed
                // by the running coordinator, so they must not spawn an empty
                // successor. A later child result still increments wake.
                for (auto& job:p["jobs"]) if (job.at("role")=="lead" && job.at("domainId")==d.at("domainId") && !terminal(job.at("state").get<std::string>())) job["wake"]=d.at("wake");
            }
            result={{"saved",true},{"state",state},{"willResume",state=="continue"}};
        } else if (tool=="team_escalate") {
            Json decision={{"decisionId",id()},{"domainId",d.at("domainId")},{"state","open"},{"revision",0}};
            if (in.contains("jobId")) {const auto key=text(in,"jobId",100);authorize_job(p,d,key);decision["jobId"]=key;}
            for (const auto* k:{"question","reason","recommendation","alternatives","impact","blockedScope"}) decision[k]=text(in,k,2000);
            // Dedupe an unresolved question even when a small model changes requestId.
            for (const auto& old:p.at("decisions")) if (old.at("domainId")==d.at("domainId") && old.at("state")=="open" && old.at("question")==decision.at("question")) {return old;}
            p["decisions"][decision.at("decisionId").get<std::string>()]=decision;d["state"]="working";event(p,"decision",decision);result=decision;
        } else {
            check(priority_guard.is_null() || priority_guard.empty(),"本领域仍有待处理结果/待集成事项，不能宣布完成");
            check(!std::ranges::any_of(p.at("decisions"),[&](const Json& dec){return dec.at("domainId")==d.at("domainId") && dec.at("state")=="open";}),"领域仍有待决策");
            check(backlog_guard.is_null() || backlog_guard.at("pending")==0,"队列还有未处置交接，不能只做完一个工作项就结束负责人职责");
            std::set<std::string> items;
            if (d.contains("workItemIds")) for (const auto& key:d.at("workItemIds")) items.insert(key.get<std::string>());
            for (const auto& j:p.at("jobs")) if (j.at("domainId")==d.at("domainId") && j.at("role")!="lead") {
                check(terminal(j.at("state").get<std::string>()),"领域仍有未结束任务");
                if (j.contains("workItemId") && j.at("state")!="cancelled") items.insert(j.at("workItemId").get<std::string>());
            }
            check(!items.empty() || (!backlog_guard.is_null() && p.value("backlog",Json::object()).value("processed",0)>0),"领域没有可验收工作项或已核对的交接成果");
            const bool last=std::ranges::all_of(p.at("domains"),[&](const Json& other){return other.at("domainId")==d.at("domainId") || other.at("state")=="completed";});
            if (last && p.contains("history")) for (const auto& item:p.at("history").at("items")) check(!item.value("domainId","").empty(),"历史剩余工作尚未分配负责人，不能宣布整个团队完成");
            for (const auto& key:items) {
                bool accepted=false;for (const auto& item:ledger.at("items")) if (item.at("workItemId")==key) accepted=item.at("state")=="accepted" || item.at("state")=="integrated";
                check(accepted,"所有实施工作项必须独立审查并验收，不能仅凭完成声明结束领域");
            }
            check(d.value("checkpointVersion",0)==p.at("charterVersion").get<int>(),"请先读取当前授权并保存对应版本的检查点，再提交领域成果");
            d["summary"]=text(in,"summary",4000);check(in.at("evidence").is_array() && !in.at("evidence").empty() && in.at("evidence").size()<=32,"evidence 须为 1–32 项的引用数组");
            check(in.at("acceptanceResults").is_array() && !in.at("acceptanceResults").empty() && in.at("acceptanceResults").dump().size()<8000,"须逐项提供验收结果");
            for (const auto& item:in.at("acceptanceResults")) {
                text(item,"criterion",2000);text(item,"evidence",2000);check(item.value("outcome","")=="passed","每条验收条件必须明确 passed 并附证据；存在缺口时不能完成领域");
            }
            d["evidence"]=in.at("evidence");d["acceptanceResults"]=in.at("acceptanceResults");d["state"]="completed";
            event(p,"domain.completed",{{"domain",d.at("name")},{"summary",d.at("summary")},{"evidence",d.at("evidence")}});
            if (std::ranges::all_of(p.at("domains"),[](const Json& x){return x.at("state")=="completed";})) {p["status"]="completed";event(p,"project.completed",{{"status","completed"},{"note","团队成果已就绪，仍需主代理最终验收"}});}
            result={{"completed",true}};
        }
        ops[request]={{"input",in},{"result",result}};s.save();return result;
    }
    if (tool=="team_domain_read" || tool=="team_domain_action") {
        Json args=in.value("arguments",Json::object());const auto operation=text(in,"operation",100);
        std::string queue;Json pcopy,dcopy;
        {
            Store s(data);auto& p=project(s,in);auto& d=domain(p,in);queue=p.at("queueId").get<std::string>();pcopy=p;dcopy=d;
            if (tool=="team_domain_action") running_project(p);
            if (args.contains("sessionId")) {
                bool own=false;for (const auto& j:p.at("jobs")) if (j.at("domainId")==d.at("domainId") && j.value("sessionId","")==args.at("sessionId").get<std::string>()) own=true;
                if (tool=="team_domain_read" && d.at("domainId")==backlog_owner(p) && p.contains("backlog") && p.at("backlog").contains("claim")) own=own || p.at("backlog").at("claim").at("sessionId")==args.at("sessionId");
                check(own,"sessionId 不属于当前领域");
            }
            if (args.contains("workItemId")) {
                bool own=false;for (const auto& j:p.at("jobs")) if (j.at("domainId")==d.at("domainId") && j.value("workItemId","")==args.at("workItemId").get<std::string>()) own=true;
                if (d.contains("workItemIds") && std::ranges::find(d.at("workItemIds"),args.at("workItemId"))!=d.at("workItemIds").end()) own=true;
                if (tool=="team_domain_read" && d.at("domainId")==backlog_owner(p) && p.contains("backlog") && p.at("backlog").contains("claim")) own=own || p.at("backlog").at("claim").at("workItemId")==args.at("workItemId");
                check(own,"workItemId 不属于当前领域");
            }
        }
        args.erase("queueId");
        if (tool=="team_domain_read") {
            check(operation=="cluster_get_session_status" || operation=="cluster_get_session_result" || operation=="cluster_get_work_item" || operation=="cluster_get_evidence_manifest","不支持该领域读取操作");
            if (operation=="cluster_get_session_result") args["limit"]=std::min(args.value("limit",4000),8000);
            if (operation=="cluster_get_evidence_manifest") {
                // Manifest IDs are validated against their queue by the evidence store.
                args["limit"]=std::min(args.value("limit",20),32);
            }
        } else {
            if (operation!="cluster_update_work_item" && operation!="cluster_cancel_session") throw DispatchError("operation只允许cluster_update_work_item或cluster_cancel_session；record_review等属于内层arguments.action。不要猜操作名，请按返回示例修正。",{{"reason","invalid_domain_operation"},{"allowedOperations",Json::array({"cluster_update_work_item","cluster_cancel_session"})},{"contract",domain_action_contract()}});
            if (operation=="cluster_update_work_item" && args.value("action","")=="record_review") {
                const auto& details=args.at("details");bool matches=false;
                for (const auto& j:pcopy.at("jobs")) if (j.at("domainId")==dcopy.at("domainId") && j.at("role")=="reviewer" &&
                    j.value("sessionId","")==details.value("sessionId","") && j.at("baseSha")==details.at("candidateSha") && j.value("workItemId","")==args.at("workItemId").get<std::string>()) matches=true;
                check(matches,"审查记录必须匹配本领域独立 reviewer 的精确候选 SHA 和工作项");
            }
            if (operation=="cluster_update_work_item") check(args.value("action","")!="attach_session","领域不能关联其他会话，请使用 sourceJobId 派发");
        }
        const auto scoped=cluster.for_queue(queue);
        if (operation=="cluster_update_work_item" && args.value("action","")=="record_review" && args.at("details").value("verdict","")=="passed") {
            const auto& detail=args.at("details");
            const auto report=scoped.invoke("cluster_get_session_result",{{"sessionId",detail.at("sessionId")},{"resultVersion",detail.at("resultVersion")},{"limit",4}});
            check(report.at("delivery").value("status","")=="passed" && report.at("delivery").value("candidateSha",Json())==detail.at("candidateSha"),"reviewer 的结构化结论不是该 SHA 的 passed，不能登记通过");
            const auto paths=report.at("delivery").value("evidencePaths",Json::array());
            if (!paths.empty()) check(report.contains("evidence") && report.at("evidence").value("unavailableFiles",0)==0 && report.at("evidence").value("state","")!="unavailable","审查声明的证据尚未采集或存在目录外/缺失文件，不能直接登记 passed；须补齐或由主代理显式核验采集");
        }
        auto result=scoped.invoke(operation,args);
        if (operation=="cluster_update_work_item" && (args.value("action","")=="accept" || args.value("action","")=="integrate"))
            for (const auto& j:pcopy.at("jobs")) if (j.value("workItemId","")==args.at("workItemId").get<std::string>() && j.contains("sessionId") && j.contains("resultVersion") && terminal(j.at("state").get<std::string>()))
                try {scoped.invoke("cluster_mark_receipt",{{"sessionId",j.at("sessionId")},{"resultVersion",j.at("resultVersion")},{"processed",true}});} catch (const std::exception&) {}
        if (operation=="cluster_update_work_item") {
            const auto action=args.value("action","");
            if (action=="accept" || action=="integrate" || action=="reopen" || action=="add_issue") {
                Store s(data);auto& p=project(s,in);
                for (auto& j:p["jobs"]) if (j.value("workItemId","")==args.at("workItemId").get<std::string>()) j["accepted"]=action=="accept" || action=="integrate";
                s.save();
            }
        }
        return result;
    }
    throw std::runtime_error("未知团队工具");
}

void advance_teams(const Cluster& cluster,const fs::path& data,std::stop_token stop) {
    // Only one runtime across desktop/stdio/HTTP processes prepares a job at a time.
    // File sharing denies a second runtime without blocking ordinary team requests.
    HANDLE runtime=CreateFileW((data/L"team-runtime.lock").c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if (runtime==INVALID_HANDLE_VALUE) return;
    struct Close {HANDLE h;~Close(){CloseHandle(h);}} close{runtime};
    Json projects;
    {Store s(data);projects=s.value.at("projects");}
    for (const auto& original:projects) {
        if (stop.stop_requested()) return;
        const auto pid=original.at("projectId").get<std::string>();
        RuntimeHealth health(data,pid,original.value("runtimeErrors",Json::object()));
        if (original.at("status")=="starting") {
            const bool recovered=health.run("create_queue","",[&] {
                const auto q=cluster.invoke("cluster_create_queue",{{"name",original.at("title")},{"requestId","team-queue-"+pid},{"workspace",original.at("workspace")},{"workKey","team:"+pid}});
                Store s(data);auto& p=s.value["projects"][pid];p["queueId"]=q.at("queueId");if (p.at("status")=="starting") p["status"]="running";s.save();
            });
            if (!recovered) continue;
        }
        Json p;
        {Store s(data);p=s.value.at("projects").at(pid);}
        if (!p.contains("queueId")) continue;
        const auto scoped=cluster.for_queue(p.at("queueId").get<std::string>());
        // Synchronize only this queue, using native code rather than model polling.
        health.run("refresh_sessions","",[&] {scoped.invoke("cluster_refresh_sessions",{{"limit",20}});});
        for (const auto& old:p.at("jobs")) {
            if (!old.contains("sessionId") || old.value("observedTerminal",false)) continue;
            if (old.at("role")=="lead" && !terminal(old.at("state").get<std::string>()) &&
                (old.value("notifiedPolicyVersion",old.value("schedulerPolicyVersion",0))<7 || old.value("notifiedCharterVersion",old.value("charterVersion",0))<p.at("charterVersion").get<int>())) {
                health.run("notify_policy",old.at("jobId"),[&] {
                    const auto version=p.at("charterVersion").get<int>();
                    const auto notice=scoped.invoke("cluster_send_message",{{"sessionId",old.at("sessionId")},
                        {"requestId","team-policy7-c"+std::to_string(version)+"-"+old.at("jobId").get<std::string>()},{"mode","steer"},
                        {"message","调度规程已升级 v7，当前授权书版本 "+std::to_string(version)+"。立即重新读取 team_get_prompt(role=lead) 与 team_domain_context/team_next_action。用户已澄清循环模型：你只需 team_next_action 取统一FIFO队首，处理本条/派后继后继续下一项。子任务结果入队尾等待，不抢占、不要求完成后立刻接上。旧“新结果优先/每两条历史抢先查结果”规程失效。读取v7规程和team_get_contract。team_domain_action外层operation只能cluster_update_work_item/cluster_cancel_session，record_review放内层arguments.action；禁止猜finish/complete等操作名，错误后只核对契约一次，再失败绑定jobId升级。工作者仍单项执行。已在途不重复派发，审查返修测试领域集成均由循环推进，最终交主代理终审。"}});
                    if (notice.value("accepted",false)) {
                        Store s(data);auto& saved=s.value["projects"][pid]["jobs"][old.at("jobId").get<std::string>()];
                        saved["notifiedPolicyVersion"]=7;saved["notifiedCharterVersion"]=version;s.save();
                    }
                });
            }
            health.run("observe_session",old.at("sessionId"),[&] {
                auto row=scoped.invoke("cluster_get_session_status",{{"sessionId",old.at("sessionId")}});
                // Pause and cancel request cancellation, but never infer it succeeded.
                if ((p.at("status")=="paused" || p.at("status")=="cancelled" || old.value("cancelRequested",false)) && !terminal(row.value("status",""))) {
                    health.run("cancel_session",old.at("sessionId"),[&] {scoped.invoke("cluster_cancel_session",{{"sessionId",old.at("sessionId")}});});
                    row=scoped.invoke("cluster_get_session_status",{{"sessionId",old.at("sessionId")}});
                }
                if (row.value("stale",false)) throw std::runtime_error("会话实时状态尚未确认，将继续重试");
                if (old.at("role")=="lead" && terminal(row.value("status","")) && row.contains("resultVersion"))
                    if (!health.run("mark_receipt",old.at("sessionId"),[&] {scoped.invoke("cluster_mark_receipt",{{"sessionId",old.at("sessionId")},{"resultVersion",row.at("resultVersion")},{"processed",true}});})) return;
                if (terminal(row.value("status",""))) {
                    health.resolve("cancel_session",old.at("sessionId"));
                    health.resolve("notify_policy",old.at("jobId"));
                }
                Store s(data);auto& current=s.value["projects"][pid];auto& job=current["jobs"][old.at("jobId").get<std::string>()];
                job["state"]=row.at("status");if (row.contains("resultVersion")) job["resultVersion"]=row.at("resultVersion");
                auto& d=current["domains"][job.at("domainId").get<std::string>()];
                if (terminal(job.at("state").get<std::string>())) {
                    job["observedTerminal"]=true;
                    if (job.at("role")!="lead") d["wake"]=d.at("wake").get<int>()+1;
                    else if (current.at("status")=="running" && d.at("state")!="completed" && d.at("state")!="decision" && d.at("state")!="waiting") {
                        const bool backlog_duty=d.at("domainId")==backlog_owner(current) && current.value("backlog",Json::object()).value("actionable",0)>0;
                        if (backlog_duty) {
                            auto& b=current["backlog"];
                            const bool stalled=job.value("backlogProcessed",-1)==b.value("processed",0) && job.value("backlogSignature","")==b.value("signature","");
                            d["noProgressTurns"]=stalled?d.value("noProgressTurns",0)+1:0;
                            if (d.at("noProgressTurns").get<int>()>=3) {
                                b["suspendedOnStall"]=true;
                                incident(current,d,"backlog-stall-"+job.at("jobId").get<std::string>(),"负责人连续三次接续未推进积压。请主代理修正具体处理步骤后恢复，不能把调度抛给用户。");
                            } else if (d.at("wake").get<int>()<=job.value("wake",0)) d["wake"]=job.value("wake",0)+1;
                        } else if (d.at("wake").get<int>()<=job.value("wake",0))
                            incident(current,d,"lead-ended-"+job.at("jobId").get<std::string>(),"负责人结束时没有保存等待条件、升级或完成领域。请明确恢复方向。");
                    }
                }
                s.save();
            });
        }
        Json fresh_backlog;
        Json fresh_priority=Json::array();
        bool priority_refreshed=false;
        {Store s(data);p=s.value.at("projects").at(pid);}
        if (p.at("status")=="running") {
            health.run("backlog_snapshot","",[&] {fresh_backlog=backlog_snapshot(cluster,p);});
            priority_refreshed=health.run("priority_results","",[&] {fresh_priority=priority_results(cluster,p);});
        }
        std::map<std::string,Json> fresh_worklists;
        if (p.at("status")=="running") for (const auto& d:p.at("domains")) if (d.at("state")!="completed") {
            health.run("domain_worklist",d.at("domainId"),[&] {fresh_worklists[d.at("domainId").get<std::string>()]=domain_worklist(cluster,p,d.at("domainId").get<std::string>());coordinator_queue(cluster,data,p,d.at("domainId").get<std::string>());});
        }
        {
            Store s(data);auto& current=s.value["projects"][pid];
            if (current.at("status")!="running") continue;
            if (priority_refreshed) current["priorityResults"]=fresh_priority;
            for (const auto& [owner,items]:fresh_worklists) if (!items.empty()) {
                auto& d=current["domains"][owner];
                const bool active=std::ranges::any_of(current.at("jobs"),[&](const Json& j){return j.at("role")=="lead" && j.at("domainId")==owner && !terminal(j.at("state").get<std::string>());});
                if (!active && !current.value("backlog",Json::object()).value("suspendedOnStall",false)) {d["state"]="working";d["wake"]=d.at("wake").get<int>()+1;d["resumeReason"]="有独立待办可推进，不等待另一条任务链";}
            }
            for (const auto& ready:fresh_priority) {
                auto& d=current["domains"][ready.at("domainId").get<std::string>()];
                const bool active=std::ranges::any_of(current.at("jobs"),[&](const Json& j){return j.at("role")=="lead" && j.at("domainId")==d.at("domainId") && !terminal(j.at("state").get<std::string>());});
                if (!active) {d["state"]="working";d["wake"]=d.at("wake").get<int>()+1;d["resumeReason"]="待办队列非空，继续循环处理队首，不抢占当前项";}
            }
            if (!fresh_backlog.is_null()) {
                auto& b=current["backlog"];if (b.is_null()) b=Json::object();
                for (const auto& [key,value]:fresh_backlog.items()) b[key]=value;
                auto& duty=current["domains"][backlog_owner(current)];
                const bool active_lead=std::ranges::any_of(current.at("jobs"),[&](const Json& j){return j.at("role")=="lead" && j.at("domainId")==duty.at("domainId") && !terminal(j.at("state").get<std::string>());});
                if (b.at("actionable").get<int>()>0 && b.value("retryAt",0LL)<=now() && !b.value("suspendedOnStall",false) && !active_lead) {
                    duty["state"]="working";duty["wake"]=duty.at("wake").get<int>()+1;
                    duty["resumeReason"]="继续核对队列积压；单项返修等待不结束负责人岗位";
                }
            }
            for (auto& d:current["domains"]) {
                if (d.at("state")=="completed" || d.at("state")=="decision") continue;
                bool dependencies=true,lead_active=false;int last_wake=0;
                for (const auto& name:d.at("dependencies")) for (const auto& other:current.at("domains")) if (other.at("name")==name && other.at("state")!="completed") dependencies=false;
                for (const auto& job:current.at("jobs")) if (job.at("domainId")==d.at("domainId") && job.at("role")=="lead") {
                    lead_active=lead_active || !terminal(job.at("state").get<std::string>());last_wake=std::max(last_wake,job.value("wake",0));
                }
                if (!dependencies || lead_active || d.at("wake").get<int>()<=last_wake) continue;
                auto job=new_job(current,d,"lead","领域负责人 · "+d.at("name").get<std::string>(),"读取当前上下文并按负责人规程执行。",current.at("baseSha").get<std::string>());
                if (d.at("domainId")==backlog_owner(current)) {
                    job["task"]="先读 team_domain_context，然后 team_next_action 取FIFO队首。只处理这一项并派必要后继，再取下一项；子任务完成的结果排队等待，不抢占、不轮询。循环至队列为空才等待；上下文将满 checkpoint continue 接续。按规程完成独立审查、返修复审、测试、领域集成并提交主代理终审，不重做已关闭项凑并发。";
                    job["backlogProcessed"]=current.value("backlog",Json::object()).value("processed",0);
                    job["backlogSignature"]=current.value("backlog",Json::object()).value("signature","");
                }
                job["schedulerPolicyVersion"]=7;
                job["wake"]=d.at("wake");current["jobs"][job.at("jobId").get<std::string>()]=job;d["state"]="working";
            }
            s.save();p=current;
        }
    }
    Json cursors;
    {Store s(data);projects=s.value.at("projects");cursors=s.value.value("scheduler",Json::object()).value("lastProjects",Json::object());}
    const auto plan=team_dispatch_plan(projects,cursors,now());
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(20);
    for (const auto& ticket:plan) {
        if (stop.stop_requested() || std::chrono::steady_clock::now()>=deadline) return;
        const auto pid=ticket.at("projectId").get<std::string>(),jid=ticket.at("jobId").get<std::string>();
        Json current,d,job;
        std::string previous_project;
        {Store s(data);current=s.value.at("projects").at(pid);if (current.at("status")!="running") continue;
            job=current.at("jobs").at(jid);
            if ((job.at("state")!="queued" && job.at("state")!="starting") || job.value("retryAt",0LL)>now()) continue;
            d=current.at("domains").at(job.at("domainId").get<std::string>());
            // Advance before dispatch: interruption cannot reset a busy project
            // to the front. Session requestId recovers a partially linked launch.
            auto& rotation=s.value["scheduler"]["lastProjects"];
            if (rotation.is_null()) rotation=Json::object();
            previous_project=rotation.value(ticket.at("priority").get<std::string>(),std::string());
            rotation[ticket.at("priority").get<std::string>()]=pid;s.save();}
        const auto scoped=cluster.for_queue(current.at("queueId").get<std::string>());
        try {
            const auto role=job.at("role").get<std::string>();
            auto workspace=data/L"team-workspaces"/path_from_utf8(pid)/path_from_utf8(jid);
            fs::create_directories(workspace.parent_path());
            if (role=="lead") fs::create_directories(workspace);
            else {
                const auto root=path_from_utf8(current.at("workspace").get<std::string>());
                if (!fs::exists(workspace/L".git")) git(root,{L"worktree",L"add",L"--detach",workspace.native(),widen(job.at("baseSha").get<std::string>())});
                if (!job.contains("launch")) check(git(workspace,{L"rev-parse",L"HEAD"})==job.at("baseSha").get<std::string>(),"已有任务工作树 HEAD 与派发基线不一致");
            }
            Json args;
            // A built-in local bridge means DSH need not have an MCP plugin
            // configured. The lead only needs its ordinary terminal tool.
            if (role=="lead") {
                std::wstring executable(32768,L'\0');const auto length=GetModuleFileNameW(nullptr,executable.data(),static_cast<DWORD>(executable.size()));
                check(length>0 && length<executable.size(),"无法定位本机工具桥接程序");executable.resize(length);
                const auto ps_literal=[](const std::wstring& value) {std::wstring s=L"'";for (const auto ch:value) {s+=ch;if (ch==L'\'') s+=ch;}return s+L"'";};
                const auto helper=L"param([Parameter(Mandatory=$true)][string]$RequestPath)\n$ErrorActionPreference='Stop'\n$env:FATFISHBOX_DATA_DIR="+ps_literal(data.native())+
                    L"\n$responsePath=Join-Path $PSScriptRoot ([guid]::NewGuid().ToString()+'.response.json')\n$taskProcess=Start-Process -FilePath "+ps_literal(executable)+
                    L" -ArgumentList @('--team-call', ('\"'+(Resolve-Path -LiteralPath $RequestPath).Path+'\"'), ('\"'+$responsePath+'\"')) -WindowStyle Hidden -Wait -PassThru\nGet-Content -LiteralPath $responsePath -Raw -Encoding UTF8\nif ($taskProcess.ExitCode -ne 0) { exit $taskProcess.ExitCode }\n";
                std::ofstream catalog(workspace/L"team-tools.json",std::ios::binary);catalog<<lead_tool_contract().dump(2);catalog.close();check(bool(catalog),"无法保存负责人工具契约");
                std::ofstream bridge(workspace/L"team-call.ps1",std::ios::binary);bridge<<"\xEF\xBB\xBF"<<narrow(helper);bridge.close();check(bool(bridge),"无法保存工具桥接脚本");
            }
            {
                Store s(data);auto& live=s.value["projects"][pid];if (live.at("status")!="running") continue;
                auto& saved=live["jobs"][jid];
                if (saved.at("state")=="cancelled") continue;
                if (!saved.contains("launch")) {
                    const auto work=role=="lead"?"":job.value("workItemId","");
                    std::string prompt=team_prompt(role)+"\n\n当前任务\n"+job.at("task").get<std::string>();
                    if (role=="lead") prompt+="\n\n凭据（仅供工具调用）："+Json{{"projectId",pid},{"domainId",d.at("domainId")},{"token",d.at("token")}}.dump()+
                        "\n若所需 fatfishbox MCP 工具缺失、目录过旧或无法调用，不要停止或要求用户安装插件/重连。先读取当前工作区 team-tools.json 获取准确工具schema和嵌套调用示例，不要猜接口名。当前工作区自带 team-call.ps1：先将 {\"tool\":\"team_domain_context\",\"arguments\":{上述三个凭据字段}} 保存为 UTF-8 JSON 文件 context.request.json，运行 powershell -NoProfile -ExecutionPolicy Bypass -File ./team-call.ps1 -RequestPath ./context.request.json。输出 {ok,result}；ok=false 是失败。后续所有 team_* 调用都可以同样保存 JSON 再调用。脚本由肥鱼重工生成，直接连接同一数据目录，不需要 HTTP 或其他配置。不要把伪代码“上述三个凭据字段”写成实际 JSON，要填入真实字段值。";
                    else prompt+="\n\n项目授权书版本 "+std::to_string(live.at("charterVersion").get<int>())+"\n"+live.at("charter").dump()+"\n领域验收："+d.at("acceptance").get<std::string>();
                    args={{"requestId",job.at("requestId")},{"title",job.at("title")},{"batch",pid+"-"+role},{"role",role},{"workspace",utf8(workspace)},
                        {"task",prompt},{"baseSha",job.at("baseSha")},{"poolId",role_pool(live.at("config"),role)},
                        {"teamProjectId",pid},{"teamDomainId",d.at("domainId")},{"teamJobId",jid},{"teamCharterVersion",live.at("charterVersion")}};
                    if (!work.empty()) args["workItemId"]=work;
                    saved["launch"]=args;saved["workspace"]=utf8(workspace);saved["state"]="starting";s.save();
                } else args=saved.at("launch");
            }
            const auto launched=scoped.invoke("cluster_start_session",args);
            Store s(data);auto& saved=s.value["projects"][pid]["jobs"][jid];
            saved["sessionId"]=launched.at("id");saved["workItemId"]=launched.at("workItemId");saved["state"]=launched.at("status");saved.erase("error");s.save();
        } catch (const DispatchError& e) {
            Store s(data);auto& live=s.value["projects"][pid];auto& saved=live["jobs"][jid];
            const auto reason=e.details.value("reason","");
            // A full-capacity probe did not receive a dispatch opportunity.
            // Preserve the last served project, otherwise repeated failed
            // probes can put the same busy project first on every tick.
            if (reason=="team_capacity") s.value["scheduler"]["lastProjects"][ticket.at("priority").get<std::string>()]=previous_project;
            saved["error"]=std::string(e.what());saved["retryAt"]=now()+(reason=="team_capacity"?0:5000);
            if (reason=="team_stale_authorization") {
                saved["state"]="cancelled";auto& owner=live["domains"][saved.at("domainId").get<std::string>()];
                owner["wake"]=owner.at("wake").get<int>()+1;s.save();continue;
            }
            if (reason!="team_capacity" && reason!="no_capacity" && reason!="dsh_recovering") {
                const int attempts=saved.value("attempts",0)+1;saved["attempts"]=attempts;
                if (attempts>=3) incident(live,live["domains"][saved.at("domainId").get<std::string>()],"dispatch-"+jid,std::string(e.what()));
            }
            s.save();
        } catch (const std::exception& e) {
            Store s(data);auto& live=s.value["projects"][pid];auto& saved=live["jobs"][jid];
            saved["error"]=std::string(e.what());saved["retryAt"]=now()+10000;const int attempts=saved.value("attempts",0)+1;saved["attempts"]=attempts;
            if (attempts>=3) incident(live,live["domains"][saved.at("domainId").get<std::string>()],"dispatch-"+jid,std::string(e.what()));s.save();
        }
    }
}

Json team_tools() {
    const Json text{{"type","string"}},integer{{"type","integer"}},object{{"type","object"}},array{{"type","array"},{"items",text}};
    Json tools=Json::array();
    const auto add=[&](const char* name,const char* description,Json fields,Json required) {
        tools.push_back({{"name",name},{"description",description},{"inputSchema",{{"type","object"},{"properties",fields},{"required",required},{"additionalProperties",false}}}});
    };
    add("team_get_settings","读取已保存的团队角色模型和总/角色并发。",Json::object(),Json::array());
    add("team_update_settings","保存团队方案；revision 防止覆盖，四个模型须已启用。",{{"revision",integer},{"leadPool",text},{"developerPool",text},{"reviewerPool",text},{"integratorPool",text},{"totalConcurrency",integer},{"developerConcurrency",integer},{"reviewerConcurrency",integer}},
        {"revision","leadPool","developerPool","reviewerPool","integratorPool","totalConcurrency","developerConcurrency","reviewerConcurrency"});
    add("team_get_prompt","读取详细角色规程；主代理先读取 principal。",{{"role",{{"type","string"},{"enum",Json::array({"principal","lead","worker"})}}}},{"role"});
    const Json charter{{"type","object"},{"properties",{{"objective",text},{"acceptance",text},{"constraints",text},{"architecture",text},{"authority",text},{"escalation",text}}},
        {"required",Json::array({"objective","acceptance","constraints","architecture","authority","escalation"})},{"additionalProperties",false}};
    const Json domain_spec{{"type","object"},{"properties",{{"name",text},{"mission",text},{"acceptance",text},{"dependencies",array}}},
        {"required",Json::array({"name","mission","acceptance","dependencies"})},{"additionalProperties",false}};
    add("team_start_project","主代理一次提交目标、授权和领域；绑定现有队列（含历史任务）或自动建队列，后台启动负责人管理开发审查。重试复用原 requestId。",{{"requestId",text},{"queueId",text},{"title",text},{"workspace",text},{"charter",charter},{"domains",{{"type","array"},{"items",domain_spec},{"minItems",1},{"maxItems",8}}}},
        {"requestId","title","workspace","charter","domains"});
    add("team_list_projects","桌面与续接用团队项目摘要；不要周期性读取全部历史。",Json::object(),Json::array());
    add("team_get_queue","按当前队列查执行方式与绑定的团队运行摘要；已有团队时续接，不再创建项目。",{{"queueId",text}},{"queueId"});
    add("team_get_project","读取单项目阶段成果与待决策；不返回执行日志。",{{"projectId",text},{"includeCharter",{{"type","boolean"}}}},{"projectId"});
    add("team_wait_decisions","等待关键决策、异常和里程碑；普通运行状态不唤醒主代理。宿主无主动唤醒时使用有界等待。",{{"projectId",text},{"afterCursor",text},{"timeoutMs",{{"type","integer"},{"minimum",0},{"maximum",30000},{"default",25000}}}},{"projectId"});
    add("team_resolve_decision","主代理回复一张决策卡；自动保存并恢复领域负责人。",{{"projectId",text},{"decisionId",text},{"expectedRevision",integer},{"requestId",text},{"resolution",text}},{"projectId","decisionId","expectedRevision","requestId","resolution"});
    add("team_update_charter","更新完整授权书；版本化保存，通知负责人重新核对受影响任务。",{{"projectId",text},{"expectedVersion",integer},{"requestId",text},{"charter",charter},{"changeSummary",text}},{"projectId","expectedVersion","requestId","charter","changeSummary"});
    add("team_control_project","暂停/恢复/取消项目；暂停和取消停止新派发并请求取消活动任务，保留产物。",{{"projectId",text},{"requestId",text},{"action",{{"type","string"},{"enum",Json::array({"pause","resume","cancel"})}}}},{"projectId","requestId","action"});
    const Json auth{{"projectId",text},{"domainId",text},{"token",text},{"charterVersion",integer}};
    const auto scoped=[&](const char* name,const char* desc,Json fields,Json required) {
        fields.update(auth);for (const auto* k:{"projectId","domainId","token"}) required.push_back(k);add(name,desc,fields,required);
    };
    scoped("team_domain_context","负责人首次和每次接续必读：授权、检查点、依赖成果、任务摘要和决策。",Json::object(),Json::array());
    scoped("team_domain_jobs","分页查询本领域任务。默认仅未验收项；历史按需 includeAccepted=true。",{{"offset",integer},{"limit",integer},{"includeAccepted",{{"type","boolean"}}}},Json::array());
    scoped("team_backlog_status","负责人读取队列积压数量、可处理/在途等待分类；持续工作以队列为准，不以单项任务完成为准。",Json::object(),Json::array());
    scoped("team_get_contract","读取负责人准确工具schema和完整调用模板；CLI桥接启动前先读本工作区team-tools.json，禁止猜operation。",Json::object(),Json::array());
    scoped("team_next_action","负责人机械循环入口：返回持久FIFO队首。处理本条/派后继后再调用；新结果只排队，不抢占。",Json::object(),Json::array());
    scoped("team_priority_results","兼容结果列表；不代表优先级。负责人统一调用team_next_action按FIFO处理。",Json::object(),Json::array());
    scoped("team_next_handoff","值守负责人领取/恢复一条交接；处置须返回的 claimToken、稳定 requestId、notes 和 outcome=confirm/linked/blocked，自动取下一条。confirm 须证据和已验收总账；linked 须同工作项在途 jobId。",{{"requestId",text},{"claimToken",text},{"outcome",{{"type","string"},{"enum",Json::array({"confirm","linked","blocked"})}}},{"notes",text},{"evidence",array},{"jobId",text},{"decisionId",text}},Json::array());
    scoped("team_domain_history","负责人分页核对原队列总账；默认只看未完成项，includeClosed=true 才包含已集成和已取消历史。",{{"offset",integer},{"limit",integer},{"includeClosed",{{"type","boolean"}}}},Json::array());
    scoped("team_claim_work_item","负责人接管原队列未完成项及会话，保持原 ID/缺陷/审查/回执，防止重复接管。范围外项可 disposition=defer 并写 reason，保留原状态。",{{"workItemId",text},{"requestId",text},{"disposition",{{"type","string"},{"enum",Json::array({"adopt","defer"})}}},{"reason",text}},{"workItemId","requestId"});
    scoped("team_delegate","负责人派发开发/审查/返修/集成。历史续接先 claim；审查必须 sourceJobId；最终 integrator 必须 sourceWorkItemIds（已验收来源），新建集成工作项保留来源关联。",{{"requestId",text},{"title",text},{"role",{{"type","string"},{"enum",Json::array({"developer","reviewer","repair","integrator"})}}},{"task",text},{"baseSha",text},{"sourceJobId",text},{"workItemId",text},{"sourceWorkItemIds",array}},
        {"requestId","title","role","task","charterVersion"});
    scoped("team_cancel_job","负责人取消本领域排队或活动执行任务；后台核对真实终态，保留提交与证据。",{{"jobId",text},{"requestId",text},{"reason",text}},{"jobId","requestId","reason"});
    scoped("team_checkpoint","保存可恢复检查点。还有积压用 state=continue 自动接续；全部在途等待才用 waiting。会话结束不等于负责人岗位结束。",{{"requestId",text},{"state",{{"type","string"},{"enum",Json::array({"working","waiting","decision","continue"})}}},{"checkpoint",object}},{"requestId","state","checkpoint"});
    scoped("team_escalate","负责人提交关键决策卡；处理某个完成结果受阻时附 jobId，其他结果和历史可继续。日常调度不得升级。",{{"requestId",text},{"question",text},{"reason",text},{"recommendation",text},{"alternatives",text},{"impact",text},{"blockedScope",text},{"jobId",text}},
        {"requestId","question","reason","recommendation","alternatives","impact","blockedScope"});
    scoped("team_finish_domain","提交领域成果；必须无活动执行任务、无待决策且工作项已独立审查验收。",{{"requestId",text},{"summary",text},{"evidence",array},{"acceptanceResults",{{"type","array"},{"items",object}}}},
        {"requestId","summary","evidence","acceptanceResults"});
    scoped("team_domain_read","按需读取本领域 session/工作项/证据；operation 使用 cluster_get_*，日志默认 4000 字节。",{{"operation",text},{"arguments",object}},{"operation","arguments"});
    scoped("team_domain_action","负责人更新本领域总账或取消 session；operation为cluster_update_work_item/cluster_cancel_session，record_review等放内层arguments.action。",{{"operation",{{"type","string"},{"enum",Json::array({"cluster_update_work_item","cluster_cancel_session"})}}},{"arguments",object}},{"operation","arguments"});
    return tools;
}
}
