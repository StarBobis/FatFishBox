#include "team.h"
#include "team_scheduler.h"
#include "team_startup.h"
#include "task_plan.h"
#include "diagnostics.h"
#include "dsh.h"
#include "state_file.h"
#include "team_prompts.h"
#include "handoff_artifacts.h"
#include "evidence.h"
#include <windows.h>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <fstream>
#include <iterator>
#include <ranges>
#include <set>
#include <thread>
#include <vector>

namespace fatfish {
namespace {
void check(bool yes,const char* message) { if (!yes) throw std::runtime_error(message); }
std::string text(const Json& j,const char* key,std::size_t max=8000) {
    if (!j.contains(key) || !j.at(key).is_string()) throw std::runtime_error(std::string("缺少必需文本字段：")+key);
    const auto s=j.at(key).get<std::string>();
    check(!s.empty() && s.size()<=max && s.find('\0')==s.npos && s.find_first_not_of(" \r\n\t")!=s.npos,(std::string("文本为空或超出长度限制：")+key).c_str());
    return s;
}
bool terminal(const std::string& s) { return s=="succeeded" || s=="failed" || s=="cancelled" || s=="interrupted"; }
bool sha(const std::string& s) { return (s.size()==40 || s.size()==64) && s.find_first_not_of("0123456789abcdefABCDEF")==s.npos; }
Json defaults() {
    return {{"revision",0},{"totalConcurrency",20}};
}
void event(Json&,const std::string&,const Json&);
class Store {
public:
    explicit Store(const fs::path& data):file(data/L"teams.json"),lock(data/L"teams.lock") {
        value=fs::exists(file)?read_json(file,32*1024*1024):Json{{"settings",defaults()},{"projects",Json::object()},{"requests",Json::object()}};
        auto& settings=value["settings"];settings={{"revision",settings.value("revision",0)},{"totalConcurrency",settings.value("totalConcurrency",20)}};
        bool migrated=false;
        for(auto& p:value["projects"]) for(auto& d:p["decisions"]) if(d.value("state","")=="open" && (d.contains("evidenceSessionId") || d.contains("blockedSessionId"))) {
            d["state"]="resolved";d["revision"]=d.value("revision",0)+1;d["resolution"]="Formal evidence/review gates were removed under policy v13; resume normal work and manager judgement.";
            event(p,"evidence.policy_relaxed",{{"decisionId",d.at("decisionId")}});migrated=true;
        }
        for (auto& p:value["projects"]) if (!p.contains("closedAt") &&
            (p.at("status")=="cancelled" || p.at("status")=="completed")) {
            p["closedAt"]=p.value("updated",0LL);
            migrated=true;
        }
        if(migrated) save();
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
std::string canonical_queue(std::string queue) {
    for (auto& ch:queue) if (ch>='a' && ch<='f') ch=char(ch-'a'+'A');
    return queue;
}
bool archived_project(const Json& p) {
    return p.at("status")=="cancelled" || p.at("status")=="completed";
}
bool has_unsettled_jobs(const Json& p) {
    return std::ranges::any_of(p.at("jobs"),[](const Json& j) {
        return !terminal(j.at("state").get<std::string>()) ||
            (j.contains("sessionId") && !j.value("observedTerminal",false));
    });
}
bool holds_queue(const Json& p) {
    return !archived_project(p) || has_unsettled_jobs(p);
}
Json project_actions(const Json& p) {
    const auto status=p.at("status").get<std::string>();
    if (status=="cancelled") return Json::array({"team_get_queue"});
    if (status=="completed") return Json::array({"team_record_merge","team_update_charter"});
    if (status=="draft") return Json::array({"team_update_charter","team_activate_project","cancel"});
    if (status=="paused") {
        auto actions=Json::array({"team_update_charter","cancel"});
        if (!has_unsettled_jobs(p)) actions.push_back("resume");
        return actions;
    }
    if (status=="finishing") return Json::array({"team_wait_decisions","cancel"});
    return Json::array({"team_update_charter","team_wait_decisions","pause","cancel"});
}
void team_error(const std::string& code,const std::string& message,Json details,Json actions) {
    throw DispatchError(message,{{"code",code},{"reason",code},{"message",message},
        {"details",std::move(details)},{"allowedActions",std::move(actions)}});
}
void queue_conflict(const Json& p) {
    const bool draining=archived_project(p);
    team_error(draining?"queue_draining":"queue_bound",
        draining?"旧项目的会话正在停止，确认退出后可接管队列":"队列已有项目，请按当前状态继续或更新计划",
        {{"queueId",p.at("queueId")},{"boundProjectId",p.at("projectId")},{"projectStatus",p.at("status")}},
        draining?Json::array({"team_get_queue","team_wait_decisions"}):project_actions(p));
}
const Json* latest_archive(const Json& projects,const std::string& queue) {
    const Json* latest=nullptr;
    for (const auto& p:projects) if (canonical_queue(p.value("queueId",""))==queue && archived_project(p) &&
        (!latest || p.value("closedAt",p.value("updated",0LL))>latest->value("closedAt",latest->value("updated",0LL)))) latest=&p;
    return latest;
}
Json project_summary(const Json& p) {
    Json result;
    for (const auto* k:{"projectId","queueId","title","workspace","baseSha","status","charterVersion","updated","sequence"})
        if (p.contains(k)) result[k]=p.at(k);
    const bool archived=archived_project(p);
    result["archived"]=archived;
    result["allowedActions"]=project_actions(p);
    result["draining"]=archived && has_unsettled_jobs(p);
    if (archived) result["closedAt"]=p.value("closedAt",p.value("updated",0LL));
    if (p.contains("replacesProjectId")) result["replacesProjectId"]=p.at("replacesProjectId");
    result["managerStartup"]=team_manager_startup(p,archived?result.at("closedAt").get<std::int64_t>():now());
    if (p.at("status")=="running" && result["managerStartup"].at("pending")==true) result["status"]="starting";
    result["pollIntervalMs"]=300000;result["tasks"]=p.value("tasks",Json::array());
    result["domains"]=Json::array();
    int complete=0,pending=0,running=0;
    for (const auto& d:p.at("domains")) {
        Json row;for (const auto* k:{"domainId","name","mission","state","summary","evidence","acceptanceResults","lastError"})
            if (d.contains(k)) row[k]=d.at(k);
        std::int64_t latest=-1;
        for (const auto& job:p.at("jobs")) if (job.at("role")=="lead" && job.at("domainId")==d.at("domainId") && job.value("created",0LL)>=latest) {
            latest=job.value("created",0LL);row["leadState"]=job.at("state");row["leadSessionId"]=job.value("sessionId","");if (!archived && job.contains("goal")) row["goal"]=job.at("goal");
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
    result["verifications"]=p.value("verifications",Json::object());
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
Json run_candidate_test(const fs::path& workspace,const fs::path& log_path,const Json& in) {
    const auto executable=path_from_utf8(text(in,"executable",4000));
    const auto arguments=in.value("arguments",Json::array());
    check(arguments.is_array() && arguments.size()<=128,"arguments须为不超过128项的数组");
    std::wstring command=quote(executable.native());
    for (const auto& arg:arguments) {check(arg.is_string(),"测试参数须为字符串");command+=L" "+quote(widen(arg.get<std::string>()));}
    const auto timeout=in.value("timeoutMs",60000);check(timeout>=1 && timeout<=600000,"测试超时范围1–600000毫秒");
    SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};
    HANDLE log=CreateFileW(log_path.c_str(),GENERIC_WRITE,FILE_SHARE_READ,&security,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    check(log!=INVALID_HANDLE_VALUE,"无法创建测试日志");
    HANDLE input=CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&security,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    HANDLE job=CreateJobObjectW(nullptr,nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    const bool bounded=job && SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limits,sizeof(limits));
    STARTUPINFOW start{};start.cb=sizeof(start);start.dwFlags=STARTF_USESTDHANDLES;start.hStdInput=input;start.hStdOutput=log;start.hStdError=log;
    PROCESS_INFORMATION process{};
    const bool created=bounded && CreateProcessW(nullptr,command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW|CREATE_SUSPENDED,nullptr,workspace.c_str(),&start,&process);
    DWORD code=1;bool timed_out=false;
    if (created) {
        if (AssignProcessToJobObject(job,process.hProcess)) {
            ResumeThread(process.hThread);timed_out=WaitForSingleObject(process.hProcess,static_cast<DWORD>(timeout))!=WAIT_OBJECT_0;
            if (timed_out) {TerminateJobObject(job,1);WaitForSingleObject(process.hProcess,5000);}
            GetExitCodeProcess(process.hProcess,&code);
        } else {TerminateProcess(process.hProcess,1);WaitForSingleObject(process.hProcess,5000);}
        CloseHandle(process.hThread);CloseHandle(process.hProcess);
    }
    CloseHandle(log);if (input!=INVALID_HANDLE_VALUE) CloseHandle(input);if (job) CloseHandle(job);
    return {{"executable",utf8(executable)},{"arguments",arguments},{"exitCode",code},{"timedOut",timed_out},{"started",created},{"logPath",utf8(log_path)}};
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
        if (job.at("role")=="lead" || job.value("historical",false) || job.value("retired",false) || !job.contains("workItemId")) continue;
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
        if (state=="accepted" || state=="integrated" || state=="cancelled" || state=="superseded") continue;
        bool pending=false;
        for (const auto& other:p.at("jobs")) if (other.at("role")!="lead" && !terminal(other.at("state").get<std::string>())) {
            if (other.value("workItemId","")==work) pending=true;
            for (const auto& source:other.value("sourceWorkItemIds",Json::array())) if (source==work) pending=true;
        }
        for (const auto& decision:p.at("decisions")) if (decision.at("state")=="open" && decision.value("jobId","")==job.at("jobId").get<std::string>()) pending=true;
        if (pending) continue;
        std::string reason;
        if (job.at("role")=="reviewer") {
            Json review;
            for (const auto& r:item.at("reviews")) if (r.at("sessionId")==job.at("sessionId") && r.at("resultVersion")==job.at("resultVersion")) review=r;
            if (review.is_null()) reason="review_result_unhandled";
            else if (review.at("verdict")!="passed") reason="review_requires_repair";
            else reason="verify_issues_and_accept";
        } else reason=job.at("state")=="succeeded"?"candidate_needs_review":"execution_failed";
        results.push_back({{"jobId",job.at("jobId")},{"domainId",job.at("domainId")},{"workItemId",work},{"role",job.at("role")},
            {"sessionId",job.value("sessionId","")},{"resultVersion",job.at("resultVersion")},{"reason",reason},{"created",job.value("created",0LL)}});
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
        if(std::ranges::any_of(p.at("decisions"),[&](const Json& dec){return dec.at("state")=="open" && dec.value("blockedWorkItemId","")==key;})) continue;
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
    for(const auto& dec:p.at("decisions")) if(dec.at("state")=="open" && dec.contains("actionId")) {
        for(auto it=candidates.begin();it!=candidates.end();) {if(it->first==dec.at("actionId").get<std::string>() || (!dec.value("blockedWorkItemId","").empty() && it->second.value("workItemId","")==dec.at("blockedWorkItemId").get<std::string>())) it=candidates.erase(it);else ++it;}
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
    const Json templates={
        {"add_issue",{{"message","<reproduction, expected/actual behavior and impact>"},{"acceptance","<repair verification criteria>"}}},
        {"resolve_issue",{{"issueId","<actual issueId>"},{"notes","<verified resolution>"}}},
        {"record_review",{{"sessionId","<reviewer sessionId>"},{"resultVersion","<exact resultVersion>"},{"candidateSha","<full reviewed SHA>"},{"verdict","blocked"},{"notes","<actual conclusion and attachment limitations>"}}},
        {"accept",{{"candidateSha","<full SHA>"},{"notes","<manager confirmation>"}}},
        {"reopen",{{"notes","<reason>"}}},{"block",{{"notes","<reason>"}}},{"cancel",{{"notes","<reason; sessions must be terminal>"}}}};
    Json examples=Json::object(),required=Json::object(),actions=Json::array();
    for(const auto& [action,details]:templates.items()) {actions.push_back(action);required[action]=Json::array();for(const auto& [key,v]:details.items()) required[action].push_back(key);
        examples[action]={{"operation","cluster_update_work_item"},{"arguments",{{"workItemId","<owned workItemId>"},{"expectedRevision",0},{"requestId","<stable ID>"},{"action",action},{"details",details}}}};}
    return {{"operation","cluster_update_work_item"},{"actions",actions},{"requiredDetails",required},{"detailsTemplates",templates},{"examples",examples},{"example",{{"tool","team_domain_action"},{"arguments",{{"projectId","<projectId>"},{"domainId","<domainId>"},{"token","<token>"},{"operation","cluster_update_work_item"},{"arguments",examples.at("record_review").at("arguments")}}}}},
        {"optionalDetails",{{"add_issue",Json::array({"candidateSha"})}}},
        {"completionPolicy","Manager judgement after ordinary self-tests and independent review; no verificationId, manifest, capture or formal proof is required."},
        {"rule","Use actual IDs and latest expectedRevision. action belongs in inner arguments. Fix schema errors from these templates. For genuinely blocked work, team_escalate with actionId/jobId, then continue other tasks or MCP-wait."}};
}
void validate_domain_update(const Json& args) {
    const auto contract=domain_action_contract();const auto action=args.value("action","");Json missing=Json::array();
    for(const auto* key:{"workItemId","requestId","action"}) if(!args.contains(key) || !args.at(key).is_string() || args.at(key)=="") missing.push_back(key);
    if(!args.contains("expectedRevision") || !args.at("expectedRevision").is_number_integer()) missing.push_back("expectedRevision");
    if(!contract.at("requiredDetails").contains(action)) throw DispatchError("Unknown ledger action: "+action,{{"reason","invalid_domain_action"},{"actualAction",action},{"contract",contract}});
    const auto details=args.value("details",Json::object());
    for(const auto& key:contract.at("requiredDetails").at(action)) if(!details.is_object() || !details.contains(key.get<std::string>()) || !details.at(key.get<std::string>()).is_string() || details.at(key.get<std::string>())=="") missing.push_back("details."+key.get<std::string>());
    if(!missing.empty()) throw DispatchError("Ledger action "+action+" missing required fields: "+missing.dump(),{{"reason","invalid_action_arguments"},{"action",action},{"missingFields",missing},{"example",contract.at("examples").at(action)}});
}
Json lead_tool_contract() {
    Json selected=Json::array();
    for (const auto& tool:team_tools()) {
        const auto& fields=tool.at("inputSchema").at("properties");
        if (tool.at("name")!="team_capture_evidence" && (fields.contains("domainId") || tool.at("name")=="team_get_prompt")) selected.push_back(tool);
    }
    return {{"version",13},{"tools",selected},{"domainAction",domain_action_contract()}};
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
void prepare_manager(Json& current) {
    if (current.at("status")!="running" || !current.contains("queueId")) return;
    for (auto& d:current["domains"]) {
        if (d.at("state")=="completed") continue;
        bool exists=false;
        for (const auto& job:current.at("jobs")) if (job.at("domainId")==d.at("domainId") && job.at("role")=="lead" && (!terminal(job.at("state").get<std::string>()) || (job.value("charterVersion",0)==current.at("charterVersion").get<int>() && !job.value("restartOnResume",false)))) exists=true;
        if (exists) continue;
        auto job=new_job(current,d,"lead","项目经理 · "+d.at("name").get<std::string>(),
            "Read team_domain_context and the complete plan. Queue all ready tasks through team_dispatch_ready, then handle team_next_action FIFO. Developers perform relevant self-tests, then independent review. Confirm tasks using judgement; extra manager tests are optional. Do not collect proof or expand work into exhaustive audits. Repeat ready dispatch after each handled result. Wait only through team_wait_actions; never sleep. When all planned items are accepted, team_finish_domain, complete the goal and exit naturally.",current.at("baseSha").get<std::string>());
        job["schedulerPolicyVersion"]=13;job["wake"]=d.at("wake");
        current["jobs"][job.at("jobId").get<std::string>()]=job;d["state"]="working";
    }
}

Json dispatch_readiness(const Cluster& cluster,const Json& p) {
    const auto tasks=p.value("tasks",Json::array());
    const auto error=task_plan_error(tasks,64);
    if (!error.empty()) return {{"readyTasks",Json::array()},{"blockedTasks",Json::array()},{"planNeedsUpdate",true},{"reason",error}};
    const auto scoped=cluster.for_queue(p.at("queueId").get<std::string>());
    Json states=Json::object(),inputs=Json::object();std::map<std::string,Json> ledger;
    for (const auto& task:tasks) {
        const auto name=task.at("name").get<std::string>();
        for (const auto& job:p.at("jobs")) if (job.value("taskName","")==name && job.at("role")=="developer" && !job.value("retired",false) && job.at("state")!="cancelled") {
            states[name]="assigned";
            if (job.contains("workItemId")) {
                const auto key=job.at("workItemId").get<std::string>();
                if (!ledger.contains(key)) ledger[key]=scoped.invoke("cluster_get_work_item",{{"workItemId",key}});
                const auto& item=ledger.at(key);states[name]=item.at("state");
                if (item.at("state")=="accepted" || item.at("state")=="integrated")
                    inputs[name]={{"taskName",name},{"workItemId",key},{"candidateSha",item.at("acceptance").at("candidateSha")}};
            }
        }
    }
    auto result=task_plan_readiness(tasks,states);result["dependencyCandidates"]=inputs;return result;
}
std::string planned_task_text(const Json& task,const Json& inputs) {
    Json dependencies=Json::array();for (const auto& name:task.at("dependsOn")) dependencies.push_back(inputs.at(name.get<std::string>()));
    return "Complete task contract prepared by the principal (do not expand scope or re-split):\n"+task.dump(2)+
        "\nDependencies accepted after independent review and manager testing:"+dependencies.dump(2)+
        "\nWork only in the assigned isolated branch. Merge accepted dependency candidates into this branch when required; never modify main. Report concrete blocked conditions for dependency conflicts or unavailable inputs instead of guessing. Obey files boundaries, run the planned builds/tests, and return the candidate and a concise summary. Never expand work into bulk evidence collection.";
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
    if (role!="principal" && role!="lead" && role!="worker") role="worker";
    // 优先读安装目录旁的 prompts/<role>.md，便于不重新编译就改提示词；
    // 文件缺失或为空时退回编译内置的副本。
    std::wstring executable(32768,L'\0');
    const auto length=GetModuleFileNameW(nullptr,executable.data(),static_cast<DWORD>(executable.size()));
    if (length>0 && length<executable.size()) {
        executable.resize(length);
        const std::wstring name(role.begin(),role.end());
        const auto file=fs::path(executable).parent_path()/L"prompts"/(name+L".md");
        std::ifstream external(file,std::ios::binary);
        if (external) {
            std::string text((std::istreambuf_iterator<char>(external)),std::istreambuf_iterator<char>());
            if (!text.empty()) return text;
        }
    }
    if (role=="principal") return principal_prompt;
    if (role=="lead") return lead_prompt;
    return worker_prompt;
}
bool team_managed_queue(const fs::path& data,std::string_view queue) {
    Store store(data);for (const auto& p:store.value.at("projects")) if (canonical_queue(p.value("queueId",""))==canonical_queue(std::string(queue)) && holds_queue(p)) return true;
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
    const auto all=std::ranges::count_if(sessions,[](const Json& row){return !terminal(row.value("status",""));});
    if (all>=settings.at("totalConcurrency").get<int>())
        throw DispatchError("最大并发已满，任务保持排队",{{"reason","team_capacity"},{"retryable",true},{"retryAfterMs",2000}});
}

Json team_invoke(const Cluster& cluster,const fs::path& data,std::string_view tool,const Json& in) {
    check(in.is_object(),"参数须为对象");
    if (tool=="team_start_project" || tool=="team_activate_project" || tool=="team_wait_decisions" ||
        tool=="team_control_project" || tool=="team_update_charter") {
        static const auto catalog=team_tools();
        const auto entry=std::ranges::find_if(catalog,[&](const Json& item) {return item.at("name").get<std::string>()==tool;});
        const auto& schema=entry->at("inputSchema");
        const auto& properties=schema.at("properties");
        for (const auto& key:schema.at("required"))
            if (!in.contains(key.get<std::string>()))
                team_error("invalid_arguments","缺少必需字段",{{"field",key}},Json::array({"team_get_prompt"}));
        for (const auto& [key,value]:in.items()) {
            if (!properties.contains(key))
                team_error("invalid_arguments","工具不接受该字段",{{"field",key}},Json::array({"team_get_prompt"}));
            const auto type=properties.at(key).at("type");
            const bool valid=(type=="string" && value.is_string()) || (type=="integer" && value.is_number_integer()) ||
                (type=="boolean" && value.is_boolean()) || (type=="object" && value.is_object()) ||
                (type=="array" && value.is_array());
            if (!valid) team_error("invalid_arguments","字段类型不正确",{{"field",key},{"expectedType",type}},Json::array({"team_get_prompt"}));
        }
    }
    if (tool=="team_get_prompt") {const auto role=in.value("role","principal");auto out=Json{{"version",role=="principal"?16:13},{"prompt",team_prompt(role)}};
        if (role=="principal") {
            out["tools"]=Json::object();
            for (const auto& entry:team_tools()) {
                const auto name=entry.at("name").get<std::string>();
                if (name=="team_start_project" || name=="team_activate_project" || name=="team_wait_decisions" || name=="team_control_project")
                    out["tools"][name]=entry.at("inputSchema");
            }
        }
        return out;}
    if (tool=="team_get_settings") { Store s(data);return s.value.at("settings"); }
    if (tool=="team_update_settings") {
        Store s(data);auto& settings=s.value["settings"];
        check(in.at("revision")==settings.at("revision"),"配置已变化，请刷新后保存");
        check(in.contains("totalConcurrency") && in.at("totalConcurrency").is_number_integer(),"并发须为整数");
        const auto count=in.at("totalConcurrency").get<int>();check(count>=2 && count<=64,"最大并发须为2–64（含项目经理）");
        auto next=settings;next["totalConcurrency"]=count;
        next["revision"]=settings.at("revision").get<int>()+1;settings=next;s.save();return next;
    }
    if (tool=="team_list_projects") {
        Store s(data);Json rows=Json::array();
        for (const auto& p:s.value.at("projects"))
            if (!archived_project(p) || in.value("includeArchived",false)) rows.push_back(project_summary(p));
        return {{"projects",rows}};
    }
    if (tool=="team_get_queue" || tool=="team_desktop_queue") {
        const auto queue=canonical_queue(text(in,"queueId",100));cluster.for_queue(queue);
        Store s(data);Json bound=nullptr,draining=nullptr,history=nullptr;
        for (const auto& p:s.value.at("projects")) if (canonical_queue(p.value("queueId",""))==queue && holds_queue(p)) {
            if (archived_project(p)) draining=project_summary(p);
            else bound=project_summary(p);
        }
        if (const auto* p=latest_archive(s.value.at("projects"),queue))
            history={{"projectId",p->at("projectId")},{"status",p->at("status")},{"closedAt",p->value("closedAt",p->value("updated",0LL))}};
        std::string next="start_project_draft";
        Json actions=Json::array({"team_start_project"});
        if (!draining.is_null()) {next="wait_for_project_shutdown";actions=Json::array({"team_get_queue","team_wait_decisions"});}
        else if (!bound.is_null()) {
            const auto status=bound.at("status").get<std::string>();
            if (status=="draft") next="review_and_activate_plan";
            else if (status=="paused") next="review_paused_project";
            else if (bound.at("managerStartup").value("needsAttention",false)) next="inspect_manager_status";
            else next="monitor_project";
            actions=bound.at("allowedActions");
        } else if (!history.is_null()) next="take_over_queue_history";
        return {{"queueId",queue},{"mode","team"},{"project",bound},{"drainingProject",draining},{"previousProject",history},
            {"principalPromptVersion",15},{"historyTakeoverSupported",true},{"nextAction",next},{"allowedActions",actions}};
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
        const auto queue=canonical_queue(in.value("queueId",std::string()));
        const auto replace=in.value("replaceProjectId",std::string());
        if (!replace.empty() && queue.empty())
            team_error("invalid_arguments","replaceProjectId requires queueId",{{"field","queueId"}},Json::array({"team_get_queue"}));
        if (!queue.empty()) {
            Store s(data);
            for (const auto& existing:s.value.at("projects"))
                if (canonical_queue(existing.value("queueId",""))==queue && holds_queue(existing)) {
                    if (const auto& requests=s.value.at("requests");requests.contains(request) && requests.at(request).at("input")==in)
                        return project_summary(s.value.at("projects").at(requests.at(request).at("projectId").get<std::string>()));
                    queue_conflict(existing);
                }
        }
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
        const auto ds=in.at("domains");check(ds.is_array() && !ds.empty() && ds.size()==1,"只允许一个项目经理定义");
        std::set<std::string> names,seen;Json domains=Json::object();std::string first_domain;
        for (const auto& entry:ds) check(names.insert(text(entry,"name",100)).second,"领域名称不可重复");
        // Require topological order: it makes dependency mistakes explicit for small models.
        for (const auto& entry:ds) {
            const auto name=text(entry,"name",100),key=id();const auto deps=entry.value("dependencies",Json::array());
            if (first_domain.empty()) first_domain=key;
            check(deps.is_array() && deps.empty(),"单项目经理不使用领域依赖");
            for (const auto& dep:deps) check(dep.is_string() && seen.contains(dep.get<std::string>()),"依赖必须是排在当前领域之前的领域，禁止循环依赖");
            domains[key]={{"domainId",key},{"name",name},{"mission",text(entry,"mission",6000)},{"acceptance",text(entry,"acceptance",6000)},
                {"dependencies",deps},{"token",id()},{"state","pending"},{"wake",1},{"checkpoint",Json::object()}};seen.insert(name);
        }
        {
            Store s(data);
            // Serialize creation against retries after the Git checks.
            if (s.value.at("requests").contains(request)) {
                const auto& old=s.value.at("requests").at(request);check(old.at("input")==in,"requestId 参数不一致");return project_summary(s.value.at("projects").at(old.at("projectId").get<std::string>()));
            }
            const auto& projects=s.value.at("projects");
            if (!queue.empty()) for (const auto& existing:projects)
                if (canonical_queue(existing.value("queueId",""))==queue && holds_queue(existing)) queue_conflict(existing);
            const auto* previous=queue.empty()?nullptr:latest_archive(projects,queue);
            if (!replace.empty() && (!previous || previous->at("projectId")!=replace))
                team_error("version_conflict","队列上的上一项目已变化，请重新读取队列",
                    {{"queueId",queue},{"expectedProjectId",replace},{"actualProjectId",previous?previous->at("projectId"):Json()}},
                    Json::array({"team_get_queue"}));
            const auto key=id();p={{"projectId",key},{"title",title},{"workspace",utf8(workspace)},{"baseSha",base},{"status","draft"},
                {"charter",in.at("charter")},{"charterVersion",1},{"config",s.value.at("settings")},{"domains",domains},
                {"jobs",Json::object()},{"decisions",Json::object()},{"events",Json::array()},{"operations",Json::object()},{"sequence",0},{"updated",now()}};
            if (!queue.empty()) p["queueId"]=queue;
            if (previous) p["replacesProjectId"]=previous->at("projectId");
            check(s.value.at("settings").at("totalConcurrency").get<int>()>=2,"单goal经理需要至少2个总并发名额");
            const auto tasks=in.value("tasks",Json::array());
            const auto plan_error=task_plan_error(tasks,static_cast<std::size_t>(s.value.at("settings").at("totalConcurrency").get<int>()-1));
            if (!plan_error.empty()) team_error("invalid_plan",plan_error,Json::object(),Json::array({"team_start_project"}));
            std::set<std::string> task_names;for (const auto& task:tasks) {check(task_names.insert(text(task,"name",200)).second,"计划任务名称须唯一");text(task,"steps",12000);text(task,"acceptance",6000);}
            p["tasks"]=tasks;p["protocolVersion"]=14;p["pollIntervalMs"]=300000;
            p["history"]=history;
            p["backlogOwnerDomainId"]=first_domain;
            s.value["projects"][key]=p;s.value["requests"][request]={{"projectId",key},{"input",in}};s.save();
        }
        return project_summary(p);
    }
    if (tool=="team_activate_project") {
        const auto request=text(in,"requestId",100);
        const auto replay=[&](const Json& p)->Json {
            const auto& ops=p.at("operations");
            if (!ops.contains(request)) return nullptr;
            const auto& saved=ops.at(request);
            if (saved.value("tool","")!="team_activate_project" || saved.at("input")!=in)
                team_error("request_conflict","requestId 已用于其他操作",{{"requestId",request}},Json::array());
            return project_summary(p);
        };
        const auto validate=[&](const Json& p) {
            if (p.at("status")!="draft")
                team_error(archived_project(p)?"project_terminal":"invalid_project_state","只能启动已完成规划的草稿",
                    {{"projectId",p.at("projectId")},{"projectStatus",p.at("status")}},project_actions(p));
            if (p.at("charterVersion")!=in.at("expectedVersion"))
                team_error("version_conflict","确认的计划版本已变化",{{"expectedVersion",in.at("expectedVersion")},{"actualVersion",p.at("charterVersion")}},
                    Json::array({"team_get_project"}));
        };
        Json draft;
        {
            Store s(data);draft=project(s,in);
            if (auto prior=replay(draft);!prior.is_null()) return prior;
            validate(draft);
        }
        const auto workspace=path_from_utf8(draft.at("workspace").get<std::string>());
        if (!git(workspace,{L"status",L"--porcelain"}).empty() ||
            git(workspace,{L"rev-parse",L"--verify",L"HEAD"})!=draft.at("baseSha").get<std::string>())
            team_error("baseline_changed","工作区已改变，请刷新草稿基线并核对完整计划",
                {{"projectId",draft.at("projectId")},{"baseSha",draft.at("baseSha")}},Json::array({"team_update_charter"}));
        const auto settings=cluster.invoke("cluster_get_settings");
        if (std::ranges::none_of(settings.at("pools"),[](const Json& pool){return pool.value("enabled",false);}))
            team_error("no_enabled_models","请在账号与模型池启用至少一个模型",Json::object(),Json::array({"team_activate_project"}));
        Store s(data);auto& p=project(s,in);
        if (auto prior=replay(p);!prior.is_null()) return prior;
        validate(p);
        const auto error=task_plan_error(p.at("tasks"),static_cast<std::size_t>(s.value.at("settings").at("totalConcurrency").get<int>()-1));
        if (!error.empty()) team_error("invalid_plan",error,Json::object(),Json::array({"team_update_charter"}));
        p["status"]=p.contains("queueId")?"running":"starting";
        prepare_manager(p);
        event(p,"project.activated",{{"charterVersion",p.at("charterVersion")}});
        p["operations"][request]={{"tool","team_activate_project"},{"input",in}};
        s.save();
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
        const auto timeout=in.value("timeoutMs",300000);check(timeout>=0 && timeout<=300000,"等待范围为 0–300000 毫秒");
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(timeout);
        for (;;) {
            Json out;bool ready=false;
            {
                Store s(data);const auto& p=project(s,in);out={{"projectId",p.at("projectId")},{"status",p.at("status")},{"cursor",cursor(p)},{"events",Json::array()}};
                const auto after=in.contains("afterCursor") && in.at("afterCursor")!=""?parse_cursor(p,text(in,"afterCursor",140)):-1LL;
                const auto oldest=p.at("events").empty()?p.at("sequence").get<long long>():p.at("events").front().at("sequence").get<long long>();
                out["resetRequired"]=after>=0 && after<oldest-1;
                if (after<0 || out.at("resetRequired")==true) {
                    for (const auto& d:p.at("decisions")) if (d.at("state")=="open") out["events"].push_back({{"kind","decision"},{"detail",d}});
                } else {
                    auto last=after;
                    for (const auto& e:p.at("events")) {
                        const auto seq=e.at("sequence").get<long long>();
                        if (seq<=after) continue;
                        const auto& kind=e.at("kind");
                        if (kind=="decision" || kind=="incident") {
                            const auto& decisions=p.at("decisions");
                            const auto decision=decisions.find(e.at("detail").at("decisionId").get<std::string>());
                            if (decision!=decisions.end() && decision->at("state")=="open") {
                                out["events"].push_back(e);
                                if (out["events"].size()==8) {last=seq;break;}
                            }
                        }
                        last=seq;
                    }
                    out["hasMore"]=last<p.at("sequence").get<long long>();
                    if (!out.at("events").empty()) out["cursor"]=p.at("projectId").get<std::string>()+":"+std::to_string(last);
                }
                const bool archived=archived_project(p);
                const auto closedAt=p.value("closedAt",p.value("updated",0LL));
                const bool draining=archived && has_unsettled_jobs(p);
                out["archived"]=archived;
                out["draining"]=draining;
                if (archived) out["closedAt"]=closedAt;
                out["allowedActions"]=project_actions(p);
                out["managerStartup"]=team_manager_startup(p,archived?closedAt:now());
                out["runtimeErrors"]=p.value("runtimeErrors",Json::object());
                if (p.at("status")=="running" && out["managerStartup"].at("pending")==true) out["status"]="starting";
                const bool attention=out["managerStartup"].value("needsAttention",false) || !out["runtimeErrors"].empty();
                out["needsAttention"]=attention;
                ready=!out.at("events").empty() || attention || (archived && !draining) || p.at("status")=="draft" || p.at("status")=="paused";out["ready"]=ready;
            }
            if (ready || std::chrono::steady_clock::now()>=deadline) {out["timedOut"]=!ready;return out;}
            Sleep(100);
        }
    }
    if (tool=="team_resolve_decision" || tool=="team_update_charter" || tool=="team_control_project") {
        const auto request=text(in,"requestId",100);
        std::string refreshed_base;
        if (tool=="team_update_charter") {
            Json copy;
            {Store s(data);copy=project(s,in);}
            if (!copy.at("operations").contains(request) && (copy.at("status")=="draft" || copy.at("status")=="completed")) {
                const auto workspace=path_from_utf8(copy.at("workspace").get<std::string>());
                check(git(workspace,{L"status",L"--porcelain"}).empty(),"规划基线必须是干净工作树");
                refreshed_base=git(workspace,{L"rev-parse",L"--verify",L"HEAD"});
                check(sha(refreshed_base),"项目必须有有效 Git HEAD");
            }
        }
        Store s(data);auto& p=project(s,in);
        auto& operations=p["operations"];
        if (operations.contains(request)) {
            const auto& saved=operations.at(request);
            if (saved.at("input")!=in || !saved.contains("result") || saved.value("tool",std::string(tool))!=tool)
                team_error("request_conflict","requestId 已用于其他操作",{{"requestId",request}},Json::array());
            return saved.at("result");
        }
        const auto reject_terminal=[&] {
            team_error("project_terminal","项目已归档，请根据队列状态创建新计划或完成合并",
                {{"projectId",p.at("projectId")},{"projectStatus",p.at("status")},{"queueId",p.value("queueId","")}},project_actions(p));
        };
        if (tool=="team_resolve_decision") {
            if (archived_project(p)) reject_terminal();
            const auto key=text(in,"decisionId",100);check(p.at("decisions").contains(key),"决策不存在");auto& d=p["decisions"][key];
            if (d.at("state")!="open" || d.at("revision")!=in.at("expectedRevision"))
                team_error("version_conflict","决策已处理或版本已变化",{{"decisionId",key},{"actualRevision",d.at("revision")},{"state",d.at("state")}},Json::array({"team_get_project"}));
            d["resolution"]=text(in,"resolution",8000);d["state"]="resolved";d["revision"]=d.at("revision").get<int>()+1;
            auto& owner=p["domains"][d.at("domainId").get<std::string>()];owner["wake"]=owner.at("wake").get<int>()+1;owner["state"]="working";
            if (d.at("domainId")==backlog_owner(p) && p.contains("backlog")) p["backlog"]["suspendedOnStall"]=false;
        } else if (tool=="team_update_charter") {
            if (p.at("status")=="cancelled") reject_terminal();
            if (p.at("status")=="finishing")
                team_error("invalid_project_state","等经理退出后再更新计划",{{"projectStatus",p.at("status")}},project_actions(p));
            if (p.at("charterVersion")!=in.at("expectedVersion"))
                team_error("version_conflict","授权书版本已变化",{{"expectedVersion",in.at("expectedVersion")},{"actualVersion",p.at("charterVersion")}},Json::array({"team_get_project"}));
            if (p.at("status")=="completed") for (const auto& other:s.value.at("projects"))
                if (other.at("projectId")!=p.at("projectId") && canonical_queue(other.value("queueId",""))==canonical_queue(p.value("queueId","")) && holds_queue(other)) queue_conflict(other);
            if ((p.at("status")=="draft" || p.at("status")=="completed") && refreshed_base.empty())
                team_error("version_conflict","项目状态变化，请重新读取",{{"projectStatus",p.at("status")}},Json::array({"team_get_project"}));
            valid_charter(in.at("charter"));text(in,"changeSummary",4000);
            if (in.contains("tasks")) {
                const auto error=task_plan_error(in.at("tasks"),static_cast<std::size_t>(s.value.at("settings").at("totalConcurrency").get<int>()-1));
                if (!error.empty()) team_error("invalid_plan",error,Json::object(),Json::array({"team_update_charter"}));
                p["tasks"]=in.at("tasks");
            }
            p["charter"]=in.at("charter");p["charterVersion"]=p.at("charterVersion").get<int>()+1;
            for (auto& job:p["jobs"]) if (job.at("state")=="queued" && !job.contains("launch") && !job.contains("sessionId")) {job["state"]="cancelled";job["error"]="授权已更新；负责人须按新版本重新评估任务";}
            for (auto& d:p["domains"]) {d["wake"]=d.at("wake").get<int>()+1;if (d.at("state")=="completed") d["state"]="working";}
            if (!refreshed_base.empty() && (p.at("status")=="draft" || p.at("status")=="completed")) {
                p["status"]="draft";
                p.erase("closedAt");
                p["baseSha"]=refreshed_base;
            }
            event(p,"charter.changed",{{"version",p.at("charterVersion")},{"summary",in.at("changeSummary")}});
        } else {
            const auto action=text(in,"action",32);check(action=="pause" || action=="resume" || action=="cancel","action 须为 pause/resume/cancel");
            if (archived_project(p)) reject_terminal();
            if (action=="cancel") {
                const auto impact=Json{{"projectId",p.at("projectId")},{"projectStatus",p.at("status")},
                    {"queueId",p.value("queueId","")},{"terminal",true},{"willDrain",has_unsettled_jobs(p)},
                    {"impact","Cancel archives the project permanently and releases the queue after active jobs stop. Work and history are retained."}};
                if (in.value("dryRun",false)) return impact;
                if (!in.value("confirm",false))
                    team_error("confirmation_required","取消是终态操作，请先核对影响，然后传 confirm:true",impact,Json::array({"cancel","pause"}));
            } else if ((action=="pause" && p.at("status")!="running" && p.at("status")!="starting") ||
                       (action=="resume" && p.at("status")!="paused")) {
                team_error("invalid_project_state","项目当前状态不允许该操作",{{"projectStatus",p.at("status")}},project_actions(p));
            }
            if (action=="resume" && has_unsettled_jobs(p))
                team_error("project_draining","请等待已请求停止的会话确认退出",{{"projectId",p.at("projectId")}},project_actions(p));
            if (action=="pause" || action=="cancel") {
                for (auto& j:p["jobs"]) {
                    if (!j.contains("launch") && !j.contains("sessionId") && !terminal(j.at("state").get<std::string>())) j["state"]="cancelled";
                    if (action=="pause" && j.at("role")=="lead") j["restartOnResume"]=true;
                }
            }
            p["status"]=action=="pause"?"paused":action=="cancel"?"cancelled":p.contains("queueId")?"running":"starting";
            if (action=="cancel") p["closedAt"]=now();
            if (action=="resume") {
                for (auto& d:p["domains"]) if (d.at("state")!="completed") d["wake"]=d.at("wake").get<int>()+1;
                prepare_manager(p);
            }
            event(p,"project.status",{{"status",p.at("status")}});
        }
        auto result=project_summary(p);operations[request]={{"tool",tool},{"input",in},{"result",result}};s.save();return result;
    }
    if (tool=="team_dispatch_ready") {
        Json copy;const auto request=text(in,"requestId",100);
        {Store s(data);copy=project(s,in);domain(copy,in);running_project(copy);
            const auto key=text(in,"domainId",100)+":dispatch-ready:"+request;
            if (copy.at("operations").contains(key)) {const auto& old=copy.at("operations").at(key);check(old.at("input")==in,"requestId参数不一致");return old.at("result");}}
        const auto readiness=dispatch_readiness(cluster,copy);
        check(!readiness.at("planNeedsUpdate").get<bool>(),"主代理须先提交包含files/dependsOn/startCondition/tests的完整任务计划；项目经理不能自行猜拆分");
        Store s(data);auto& p=project(s,in);auto& d=domain(p,in);running_project(p);
        check(in.value("charterVersion",0)==p.at("charterVersion").get<int>() && copy.at("charterVersion")==p.at("charterVersion"),"授权已更新，请重读完整计划");
        const auto key=d.at("domainId").get<std::string>()+":dispatch-ready:"+request;
        if (p.at("operations").contains(key)) {const auto& old=p.at("operations").at(key);check(old.at("input")==in,"requestId参数不一致");return old.at("result");}
        Json queued=Json::array();
        for (const auto& name:readiness.at("readyTasks")) {
            bool exists=false;for (const auto& j:p.at("jobs")) if (j.at("role")=="developer" && !j.value("retired",false) && j.value("taskName","")==name.get<std::string>() && j.at("state")!="cancelled") exists=true;
            if (exists) continue;
            check(p.at("jobs").size()<512,"项目任务数达到上限");
            for (const auto& task:p.at("tasks")) if (task.at("name")==name) {
                auto base=p.at("baseSha").get<std::string>();
                if (!task.at("dependsOn").empty()) base=readiness.at("dependencyCandidates").at(task.at("dependsOn").front().get<std::string>()).at("candidateSha").get<std::string>();
                auto job=new_job(p,d,"developer",name.get<std::string>(),planned_task_text(task,readiness.at("dependencyCandidates")),base);
                job["taskName"]=name;job["planContract"]=task;p["jobs"][job.at("jobId").get<std::string>()]=job;queued.push_back(job_view(job));
            }
        }
        auto result=readiness;result["queued"]=queued;result["queuedCount"]=queued.size();
        p["operations"][key]={{"input",in},{"result",result}};s.save();return result;
    }
    if (tool=="team_verify_candidate") {
        const auto key=text(in,"workItemId",100),candidate=text(in,"candidateSha",64),request=text(in,"requestId",100);
        check(sha(candidate),"候选须为完整SHA");Json pcopy,dcopy;
        {Store s(data);pcopy=project(s,in);dcopy=domain(pcopy,in);running_project(pcopy);
            const auto op=dcopy.at("domainId").get<std::string>()+":verify:"+request;
            if (pcopy.at("operations").contains(op)) {const auto& prior=pcopy.at("operations").at(op);check(prior.at("input")==in,"requestId参数不一致");return prior.at("result");}}
        std::string workspace;std::int64_t newest=-1;
        for (const auto& j:pcopy.at("jobs")) if (j.at("domainId")==dcopy.at("domainId") && j.value("workItemId","")==key &&
            (j.at("role")=="developer" || j.at("role")=="repair") && j.at("state")=="succeeded" && j.contains("workspace") && j.value("created",0LL)>=newest) {workspace=j.at("workspace").get<std::string>();newest=j.value("created",0LL);}
        check(!workspace.empty(),"找不到本经理所属的已完成开发候选工作树");
        const auto work=path_from_utf8(workspace);
        fs::create_directories(data/L"test-results");const auto verification=id();
        const ExclusiveStateLock verify_lock(data/L"test-results"/path_from_utf8(sha256(workspace)+".lock"));
        auto result=run_candidate_test(work,data/L"test-results"/path_from_utf8(verification+".log"),in);
        result["candidateSha"]=candidate;result["workItemId"]=key;result["workspace"]=workspace;result["verificationId"]=verification;
        result["sourceUnchanged"]=nullptr;
        result["passed"]=result.at("started")==true && result.at("exitCode")==0 && result.at("timedOut")==false;
        Store s(data);auto& p=project(s,in);domain(p,in);running_project(p);
        p["verifications"][verification]=result;p["operations"][dcopy.at("domainId").get<std::string>()+":verify:"+request]={{"input",in},{"result",result}};s.save();return result;
    }
    if (tool=="team_record_merge") {
        const auto request=text(in,"requestId",100),op="merge:"+request;
        Json pcopy;{Store s(data);pcopy=project(s,in);
            if (pcopy.at("operations").contains(op)) {
                const auto& saved=pcopy.at("operations").at(op);check(saved.at("input")==in,"requestId参数不一致");
                if (saved.contains("result")) return saved.at("result");
            }
            check(pcopy.at("status")=="completed","项目经理完成后主代理才能登记main合并");}
        const auto scoped=cluster.for_queue(pcopy.at("queueId").get<std::string>());
        if (pcopy.at("operations").contains(op)) {
            auto result=scoped.invoke("cluster_update_work_item",pcopy.at("operations").at(op).at("arguments"));
            Store s(data);project(s,in)["operations"][op]["result"]=result;s.save();return result;
        }
        const auto key=text(in,"workItemId",100),merge=text(in,"integrationSha",64);check(sha(merge),"集成提交须为完整SHA");
        const auto branch=text(in,"targetBranch",200);check(branch=="main","最终合入目标须为main");
        const auto root=path_from_utf8(pcopy.at("workspace").get<std::string>());
        check(git(root,{L"rev-parse",L"refs/heads/main"})==merge,"main实际提交与登记不一致");
        const auto item=scoped.invoke("cluster_get_work_item",{{"workItemId",key}});
        check(item.at("state")=="accepted" || item.at("state")=="integrated","只合入经理验收通过的候选");
        bool owns=false;for (const auto& j:pcopy.at("jobs")) if (j.value("workItemId","")==key) owns=true;check(owns,"工作项不属于本项目");
        const auto candidate=item.value("acceptance",Json::object()).value("candidateSha",std::string());check(sha(candidate),"缺少已验收候选SHA");
        git(root,{L"merge-base",L"--is-ancestor",widen(candidate),widen(merge)});
        const Json args={{"workItemId",key},{"expectedRevision",item.at("revision")},{"requestId",text(in,"requestId",100)},
            {"action","integrate"},{"details",{{"integrationSha",merge},{"targetBranch",branch},{"notes",text(in,"notes",4000)}}}};
        {Store s(data);project(s,in)["operations"][op]={{"input",in},{"arguments",args}};s.save();}
        auto result=scoped.invoke("cluster_update_work_item",args);
        {Store s(data);project(s,in)["operations"][op]["result"]=result;s.save();}
        return result;
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
                check(job.at("domainId")==owner,"该工作项已经由其他领域管理");check(!deferred,"团队已派发的实施项不能用defer跳过；旧轮作废请team_retire_work_item，所有会话须先真实结束");
                return {{"workItemId",key},{"domainId",owner},{"alreadyManaged",true},{"importedSessionCount",0},{"requestId",request}};
            }
            if (p.contains("history") && p.at("history").at("items").contains(key)) {
                const auto assigned=p.at("history").at("items").at(key).value("domainId","");
                check(assigned.empty() || assigned==owner,"该历史工作项已由另一个领域负责人接管");
                if (!assigned.empty()) check(p.at("history").at("items").at(key).value("deferred",false)==deferred,"已接管旧工作不能用defer跳过；作废请team_retire_work_item，保留总账与证据");
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
    if (tool=="team_retire_work_item") {
        Json copy;const bool manager=in.contains("domainId") || in.contains("token");
        const auto work=text(in,"workItemId",100),request=text(in,"requestId",100),reason=text(in,"reason",4000);
        {Store s(data);auto& p=project(s,in);if(manager) domain(p,in);const auto op="retire:"+request;if(p.at("operations").contains(op)) {const auto& saved=p.at("operations").at(op);check(saved.at("input")==in,"requestId已用于其他作废参数");return saved.at("result");}copy=p;check(in.at("charterVersion")==p.at("charterVersion"),"授权版本已变化，请重读计划");}
        const auto scoped=cluster.for_queue(copy.at("queueId").get<std::string>());
        auto item=scoped.invoke("cluster_get_work_item",{{"workItemId",work}});
        check(item.at("state")!="accepted" && item.at("state")!="integrated","已验收或集成的成果不能作为废项关闭");
        if (manager) {
            bool old=false;
            if (copy.contains("history") && copy.at("history").at("items").contains(work)) old=true;
            for (const auto& job:copy.at("jobs")) if (job.value("workItemId","")==work) {
                check(job.at("domainId")==in.at("domainId"),"工作项不属于当前经理");
                if (job.at("role")=="developer") check(job.value("historical",false) || job.value("charterVersion",0)<copy.at("charterVersion").get<int>(),"当前轮开发工作项须由主代理显式作废");
                old=old || job.value("historical",false) || (job.at("role")=="developer" && job.value("charterVersion",0)<copy.at("charterVersion").get<int>());
            }
            check(old,"仅能关闭已接管的历史或旧轮事项");
        }
        for (const auto& job:copy.at("jobs")) if (job.value("workItemId","")==work) check(terminal(job.at("state").get<std::string>()),"仍有活动任务，不得作废；先team_cancel_job并等待真实结束");
        if (item.at("state")!="cancelled") scoped.invoke("cluster_update_work_item",{{"workItemId",work},{"expectedRevision",item.at("revision")},{"requestId","retire-"+request},{"action","cancel"},{"details",{{"notes",reason}}}});
        {Store s(data);auto& p=project(s,in);p["retirements"][work]={{"reason",reason},{"charterVersion",in.at("charterVersion")},{"requestId",request}};
            for (auto& job:p["jobs"]) if (job.value("workItemId","")==work) job["retired"]=true;
            if (p.contains("history") && p["history"]["items"].contains(work)) {auto& h=p["history"]["items"][work];h["state"]="cancelled";h["deferred"]=true;h["reason"]=reason;if(h.value("domainId","").empty()) h["domainId"]=backlog_owner(p);}
            for(auto& decision:p["decisions"]) if (decision.at("state")=="open" && decision.value("workItemId","")==work) {decision["state"]="resolved";decision["resolution"]="work_item_retired";}
            event(p,"work.retired",{{"workItemId",work},{"reason",reason}});s.save();}
        item=scoped.invoke("cluster_get_work_item",{{"workItemId",work}});
        Json pending=Json::array();
        for (const auto& [sid,row]:item.at("sessions").items()) if (terminal(row.value("status","")) && !row.value("resultVersion","").empty()) {
            try {scoped.invoke("cluster_mark_receipt",{{"sessionId",sid},{"resultVersion",row.at("resultVersion")},{"processed",true}});} catch(const std::exception& e) {pending.push_back({{"sessionId",sid},{"error",e.what()}});}
        }
        try {
            const auto claim=scoped.invoke("cluster_resume_handoff",Json::object());
            if(claim.contains("session") && claim.contains("claimToken") && item.at("sessions").contains(claim.at("session").at("id").get<std::string>())) {
                scoped.invoke("cluster_next_handoff",{{"claimToken",claim.at("claimToken")},{"outcome","retired: "+reason},{"claimNext",false}});
            }
        } catch(const std::exception& e) {pending.push_back({{"operation","confirm_retired_claim"},{"error",e.what()}});}
        if(pending.empty()) {Store s(data);auto& p=project(s,in);if(p.contains("backlog") && p["backlog"].contains("claim") && p["backlog"]["claim"].value("workItemId","")==work) {p["backlog"].erase("claim");p["backlog"]["processed"]=p["backlog"].value("processed",0)+1;s.save();}}
        Json result={{"workItemId",work},{"state","cancelled"},{"retired",true},{"pendingReceipts",pending},{"nextAction",pending.empty()?"continue_queue":"team_next_handoff outcome=confirm; ledger is cancelled"}};
        {Store s(data);auto& p=project(s,in);p["operations"]["retire:"+request]={{"input",in},{"result",result}};s.save();}return result;
    }
    if (tool=="team_capture_evidence") return {{"collectionDisabled",true},{"nextAction","Continue normal work; do not collect proof, create manifests or run exhaustive audits."}};
    if (tool=="team_get_contract") {Store s(data);auto& p=project(s,in);domain(p,in);return lead_tool_contract();}
    if (tool=="team_wait_actions") {
        const auto timeout=in.value("timeoutMs",300000);check(timeout>=0 && timeout<=300000,"等待范围为 0–300000 毫秒");
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(timeout);
        for (;;) {
            Json copy;{Store s(data);auto& p=project(s,in);domain(p,in);copy=p;}
            Json out={{"projectId",copy.at("projectId")},{"status",copy.at("status")},{"cursor",cursor(copy)}};
            bool ready=copy.at("status")!="running";
            out["reason"]=ready?"project_status":"timeout";
            if (!ready) {
                auto queue=coordinator_queue(cluster,data,copy,text(in,"domainId",100));
                auto plan=dispatch_readiness(cluster,copy);
                ready=!queue.value("empty",true) || !plan.at("readyTasks").empty() || plan.value("planNeedsUpdate",false);
                bool active=false;for (const auto& j:copy.at("jobs")) if (j.at("role")!="lead" && !terminal(j.at("state").get<std::string>())) active=true;
                if (!ready && !active && plan.at("blockedTasks").empty() && !std::ranges::any_of(copy.at("decisions"),[](const Json& d){return d.at("state")=="open";})) {ready=true;out["reason"]="completion_check";}
                else if (ready) out["reason"]="action_ready";
                out["workQueue"]=std::move(queue);out["dispatchPlan"]=std::move(plan);
                if (in.contains("charterVersion") && in.at("charterVersion")!=copy.at("charterVersion")) {ready=true;out["reason"]="charter_changed";}
            }
            out["blockedDecisions"]=Json::array();for(const auto& d:copy.at("decisions")) if(d.at("state")=="open" && out["blockedDecisions"].size()<8) out["blockedDecisions"].push_back({{"decisionId",d.at("decisionId")},{"jobId",d.value("jobId","")},{"actionId",d.value("actionId","")}});
            out["ready"]=ready;out["timedOut"]=!ready;if(!ready && !out["blockedDecisions"].empty()) out["reason"]="blocked_decisions";
            if (ready || std::chrono::steady_clock::now()>=deadline) return out;
            // Only the MCP server waits; never retain Store's lock while sleeping.
            Sleep(100);
        }
    }
    if (tool=="team_next_action") {
        Json copy;{Store s(data);auto& p=project(s,in);domain(p,in);running_project(p);copy=p;}
        auto result=coordinator_queue(cluster,data,copy,text(in,"domainId",100));result["dispatchPlan"]=dispatch_readiness(cluster,copy);return result;
    }
    if (tool=="team_domain_context" || tool=="team_domain_jobs") {
        Json urgent=Json::array(),worklist=Json::array(),queue,dispatch_plan;
        if (tool=="team_domain_context") {
            Json copy;{Store s(data);auto& p=project(s,in);domain(p,in);copy=p;}
            urgent=priority_results(cluster,copy,text(in,"domainId",100));
            worklist=domain_worklist(cluster,copy,text(in,"domainId",100));
            queue=coordinator_queue(cluster,data,copy,text(in,"domainId",100));dispatch_plan=dispatch_readiness(cluster,copy);
        }
        Store s(data);auto& p=project(s,in);auto& d=domain(p,in);
        Json jobs=Json::array();
        for (const auto& j:p.at("jobs")) if (j.at("domainId")==d.at("domainId") && j.at("role")!="lead" && (in.value("includeAccepted",false) || !j.value("accepted",false))) jobs.push_back(job_view(j));
        const auto offset=in.value("offset",0),limit=in.value("limit",32);check(offset>=0 && limit>=1 && limit<=64,"任务分页范围为 1–64，offset 须非负");
        Json page=Json::array();const auto end=std::min(jobs.size(),static_cast<std::size_t>(offset)+limit);
        for (auto i=static_cast<std::size_t>(offset);i<end;++i) page.push_back(jobs.at(i));
        if (tool=="team_domain_jobs") return {{"jobs",page},{"total",jobs.size()},{"nextOffset",end<jobs.size()?Json(end):Json()}};
        Json out={{"projectId",p.at("projectId")},{"queueId",p.value("queueId","")},{"status",p.at("status")},{"charter",p.at("charter")},
            {"charterVersion",p.at("charterVersion")},{"tasks",p.value("tasks",Json::array())},{"workspace",p.at("workspace")},{"baseSha",p.at("baseSha")},{"domain",d},{"jobs",Json::array()},{"decisions",Json::array()},{"dependencies",Json::array()}};
        out["domain"].erase("token");
        out["domainActionContract"]=domain_action_contract();out["toolContractTool"]="team_get_contract";out["workQueue"]=queue;out["dispatchPlan"]=dispatch_plan;out["readyWorkItems"]=worklist;out["priorityResults"]=urgent;out["rolePromptVersion"]=13;
        out["dispatchPolicy"]="FIFO: take the team_next_action head, handle it or dispatch its successor, then take the next. Child results join the tail without preemption. priorityResults is a compatibility field, not a priority policy.";
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
        Json reviewed,plan_guard,action_guard;
        if(tool=="team_escalate" && in.contains("actionId")) {Json copy;{Store store(data);copy=project(store,in);domain(copy,in);}const auto queue=coordinator_queue(cluster,data,copy,text(in,"domainId",100));for(const auto& action:queue.at("items")) if(action.at("actionId")==in.at("actionId")) action_guard=action;check(!action_guard.is_null(),"actionId is not currently pending; re-read team_next_action");}
        if (tool=="team_delegate" && in.value("role","")=="developer") {
            Json copy;{Store s(data);copy=project(s,in);domain(copy,in);}
            plan_guard=dispatch_readiness(cluster,copy);
        }
        if (tool=="team_delegate" && in.value("role","")=="reviewer") {
            Json source;std::string queue;
            {Store s(data);auto& p=project(s,in);auto& d=domain(p,in);source=authorize_job(p,d,text(in,"sourceJobId",100));queue=p.at("queueId").get<std::string>();}
            check(source.at("role")=="developer" || source.at("role")=="repair" || source.at("role")=="integrator","reviewer 必须审查实施任务");
            check(terminal(source.at("state").get<std::string>()),"Source worker is still running");
        }
        // Completion checks read the ledger before acquiring the team lock.
        Json ledger;
        Json backlog_guard;
        Json priority_guard;
        Json worklist_guard,dispatch_guard;
        if (tool=="team_checkpoint" || tool=="team_finish_domain") {
            Json pcopy;bool owner=false;
            {Store s(data);auto& p=project(s,in);auto& d=domain(p,in);owner=d.at("domainId")==backlog_owner(p);pcopy=p;}
            if (owner) backlog_guard=backlog_snapshot(cluster,pcopy);
            priority_guard=priority_results(cluster,pcopy,text(in,"domainId",100));
            worklist_guard=domain_worklist(cluster,pcopy,text(in,"domainId",100));dispatch_guard=dispatch_readiness(cluster,pcopy);
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
        Store s(data);auto& p=project(s,in);auto& d=domain(p,in);
        const auto request=d.at("domainId").get<std::string>()+":"+text(in,"requestId",100);auto& ops=p["operations"];
        if (ops.contains(request)) {check(ops.at(request).at("input")==in,"requestId 参数不一致");return ops.at(request).at("result");}
        running_project(p);check(d.at("state")!="completed","项目经理已完成");Json result;
        if (tool=="team_delegate") {
            check(in.contains("charterVersion") && in.at("charterVersion")==p.at("charterVersion"),"授权书已变化或缺少 charterVersion，请重新 team_domain_context 后按当前版本派发");
            const auto role=text(in,"role",32);check(role=="developer" || role=="reviewer" || role=="repair","未知执行角色");
            check(p.at("jobs").size()<512,"项目最多 512 个任务，请分阶段验收并启动下一项目");
            auto base=in.value("baseSha",p.at("baseSha").get<std::string>());check(sha(base),"baseSha 必须为完整提交 SHA");
            auto job=new_job(p,d,role,text(in,"title",200),text(in,"task",16000),base);
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
            if (in.contains("sourceJobId")) {
                const auto& source=authorize_job(p,d,text(in,"sourceJobId",100));
                if (source.contains("taskName")) job["taskName"]=source.at("taskName");
            } else if (in.contains("workItemId")) {
                for (const auto& other:p.at("jobs")) if (other.value("workItemId","")==in.at("workItemId").get<std::string>() && other.contains("taskName")) job["taskName"]=other.at("taskName");
            }
            if (role=="developer" && !p.value("tasks",Json::array()).empty()) {
                const auto name=text(in,"taskName",200);bool planned=false;
                for (const auto& task:p.at("tasks")) if (task.at("name")==name) planned=true;
                check(planned,"开发任务须绑定计划中的taskName");
                check(!plan_guard.value("planNeedsUpdate",true),"主代理须补齐结构化任务计划，经理不可自行拆分");
                check(std::ranges::find(plan_guard.at("readyTasks"),Json(name))!=plan_guard.at("readyTasks").end(),"该任务已有派发或依赖尚未验收，不可提前启动");
                for (const auto& task:p.at("tasks")) if (task.at("name")==name) {job["task"]=job.at("task").get<std::string>()+"\n"+planned_task_text(task,plan_guard.at("dependencyCandidates"));job["planContract"]=task;}
                for (const auto& other:p.at("jobs")) if (other.value("taskName","")==name && other.at("role")=="developer" && !other.value("retired",false) && other.at("state")!="cancelled")
                    check(false,"计划项已有开发任务；修复沿用sourceJobId或workItemId，不重复创建");
                job["taskName"]=name;
            }
            p["jobs"][job.at("jobId").get<std::string>()]=job;result=job_view(job);
        } else if (tool=="team_cancel_job") {
            auto& job=authorize_job(p,d,text(in,"jobId",100));check(job.at("role")!="lead","不能通过此入口取消负责人自身");
            const auto reason=text(in,"reason",2000);job["cancelRequested"]=true;job["cancelReason"]=reason;
            if (job.at("state")=="queued" && !job.contains("launch") && !job.contains("sessionId")) job["state"]="cancelled";
            result={{"jobId",job.at("jobId")},{"state",job.at("state")},{"cancelRequested",true},{"note","活动会话由后台取消并核对终态；不推断取消成功"}};
        } else if (tool=="team_checkpoint") {
            const auto state=text(in,"state",32);check(state=="working" || state=="waiting" || state=="decision" || state=="continue","checkpoint state 无效");
            check(in.at("checkpoint").is_object() && in.at("checkpoint").dump().size()<=16000,"checkpoint 必须是小于 16000 字节的对象");
            if (state=="waiting") check(!dispatch_guard.value("planNeedsUpdate",true) && dispatch_guard.at("readyTasks").empty(),"仍有可直接派发的独立计划项或计划字段缺失；先team_dispatch_ready或请求主代理补齐计划，不能只派一项就等待");
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
            result={{"saved",true},{"state",state},{"willResume",false},{"sameGoalSession",true}};
        } else if (tool=="team_escalate") {
            Json decision={{"decisionId",id()},{"domainId",d.at("domainId")},{"state","open"},{"revision",0}};
            if (in.contains("jobId")) {const auto key=text(in,"jobId",100);authorize_job(p,d,key);decision["jobId"]=key;}
            if(!action_guard.is_null()) {decision["actionId"]=action_guard.at("actionId");if(action_guard.contains("jobId")) decision["jobId"]=action_guard.at("jobId");if(action_guard.contains("workItemId")) decision["blockedWorkItemId"]=action_guard.at("workItemId");}
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
            for (const auto& j:p.at("jobs")) if (j.at("domainId")==d.at("domainId") && j.at("role")!="lead" && !j.value("retired",false)) {
                check(terminal(j.at("state").get<std::string>()),"领域仍有未结束任务");
                if (j.contains("workItemId") && j.at("state")!="cancelled") items.insert(j.at("workItemId").get<std::string>());
            }
            check(!items.empty() || (!backlog_guard.is_null() && p.value("backlog",Json::object()).value("processed",0)>0),"领域没有可验收工作项或已核对的交接成果");
            const bool last=std::ranges::all_of(p.at("domains"),[&](const Json& other){return other.at("domainId")==d.at("domainId") || other.at("state")=="completed";});
            if (last && p.contains("history")) for (const auto& item:p.at("history").at("items")) check(!item.value("domainId","").empty(),"历史剩余工作尚未分配负责人，不能宣布整个团队完成");
            for (const auto& key:items) {
                bool accepted=false;for (const auto& item:ledger.at("items")) if (item.at("workItemId")==key) accepted=item.at("state")=="accepted" || item.at("state")=="integrated" || (item.at("state")=="cancelled" && p.value("retirements",Json::object()).contains(key));
                check(accepted,"所有实施工作项必须独立审查并验收，不能仅凭完成声明结束领域");
            }
            for (const auto& task:p.value("tasks",Json::array())) {
                bool covered=false;
                for (const auto& j:p.at("jobs")) if (j.value("taskName","")==task.at("name").get<std::string>() && j.contains("workItemId"))
                    for (const auto& item:ledger.at("items")) if (item.at("workItemId")==j.at("workItemId") && (item.at("state")=="accepted" || item.at("state")=="integrated")) covered=true;
                check(covered,"完整计划仍有未派发或未验收的开发事项，不能完成");
            }
            d["summary"]=text(in,"summary",4000);d["state"]="completed";
            if(in.contains("evidence")) d["evidence"]=in.at("evidence");
            if(in.contains("acceptanceResults")) d["acceptanceResults"]=in.at("acceptanceResults");
            event(p,"domain.completed",{{"domain",d.at("name")},{"summary",d.at("summary")}});
            if (std::ranges::all_of(p.at("domains"),[](const Json& x){return x.at("state")=="completed";})) {p["status"]="finishing";event(p,"manager.verified",{{"status","finishing"},{"note","所有候选验收通过，等待项目经理完成goal并退出"}});}
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
        if(tool=="team_domain_action" && operation=="cluster_update_work_item") validate_domain_update(args);
        if (tool=="team_domain_read") {
            check(operation=="cluster_get_session_status" || operation=="cluster_get_session_result" || operation=="cluster_get_work_item" || operation=="cluster_get_evidence_manifest","不支持该领域读取操作");
            if (operation=="cluster_get_session_result") args["limit"]=std::min(args.value("limit",4000),8000);
            if (operation=="cluster_get_evidence_manifest") {
                // Manifest IDs are validated against their queue by the evidence store.
                args["limit"]=std::min(args.value("limit",20),32);
            }
        } else {
            if (operation!="cluster_update_work_item" && operation!="cluster_cancel_session" && operation!="cluster_capture_evidence") throw DispatchError("operation只允许cluster_update_work_item、cluster_cancel_session或cluster_capture_evidence；record_review等属于内层arguments.action。不要猜操作名，请按返回示例修正。",{{"reason","invalid_domain_operation"},{"allowedOperations",Json::array({"cluster_update_work_item","cluster_cancel_session","cluster_capture_evidence"})},{"contract",domain_action_contract()}});
            if (operation=="cluster_update_work_item") check(args.value("action","")!="attach_session","领域不能关联其他会话，请使用 sourceJobId 派发");
        }
        if (tool=="team_domain_action" && operation=="cluster_update_work_item") check(args.value("action","")!="integrate","项目经理只验收候选；main由主代理完成后调用team_record_merge登记");
        const auto scoped=cluster.for_queue(queue);
        std::string reviewOperation;
        if(operation=="cluster_update_work_item" && args.value("action","")=="record_review") {
            reviewOperation="review:"+args.at("workItemId").get<std::string>()+":"+text(args,"requestId",100);
            Store store(data);auto& p=project(store,in);auto& ops=p["operations"];
            if(ops.contains(reviewOperation)) {const auto& saved=ops.at(reviewOperation);check(saved.at("input")==in,"requestId already used with different review arguments");args=saved.at("arguments");if(saved.contains("result")) return saved.at("result");}
            else {ops[reviewOperation]={{"input",in},{"arguments",args}};store.save();}
        }
        auto result=scoped.invoke(operation,args);

        if(operation=="cluster_update_work_item" && args.value("action","")=="record_review") {
            Store s(data);auto& p=project(s,in);const auto& details=args.at("details");bool changed=false;
            for(auto& dec:p["decisions"]) if(dec.at("state")=="open" && (dec.contains("evidenceSessionId") || dec.contains("blockedSessionId")) && dec.value("workItemId","")==args.at("workItemId").get<std::string>() && (details.value("verdict","")=="passed" || dec.value("blockedSessionId",dec.value("evidenceSessionId",std::string()))==details.at("sessionId").get<std::string>())) {dec["state"]="resolved";dec["resolution"]="review_conclusion_updated";dec["revision"]=dec.at("revision").get<int>()+1;changed=true;}
            if(!reviewOperation.empty()) {p["operations"][reviewOperation]["result"]=result;changed=true;}
            if(changed) s.save();
        }
        if(operation=="cluster_capture_evidence" && result.value("unavailableFiles",1)==0) {Store s(data);auto& p=project(s,in);for(auto& dec:p["decisions"]) if(dec.at("state")=="open" && dec.value("evidenceSessionId","")==args.value("sessionId","") && dec.value("evidenceResultVersion","")==args.value("resultVersion","")) {dec["state"]="resolved";dec["resolution"]="evidence_recaptured";}event(p,"evidence.captured",{{"sessionId",args.at("sessionId")}});s.save();}
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
    {
        Store s(data);
        bool changed=false;
        for (auto& p:s.value["projects"]) {
            const auto count=p.at("jobs").size();
            prepare_manager(p);
            changed=changed || p.at("jobs").size()!=count;
        }
        if (changed) s.save();
        projects=std::move(s.value["projects"]);
    }
    const auto plan=team_dispatch_plan(projects,Json::object(),now());
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(20);
    for (const auto& ticket:plan) {
        if (stop.stop_requested()) return;
        if (std::chrono::steady_clock::now()>=deadline) break;
        const auto pid=ticket.at("projectId").get<std::string>(),jid=ticket.at("jobId").get<std::string>();
        Json current,d,job;
        {Store s(data);current=s.value.at("projects").at(pid);if (current.at("status")!="running") continue;
            job=current.at("jobs").at(jid);
            if ((job.at("state")!="queued" && job.at("state")!="starting") || job.value("retryAt",0LL)>now()) continue;
            d=current.at("domains").at(job.at("domainId").get<std::string>());
            }
        const auto scoped=cluster.for_queue(current.at("queueId").get<std::string>());
        try {
            const auto role=job.at("role").get<std::string>();
            auto workspace=data/L"team-workspaces"/path_from_utf8(pid)/path_from_utf8(jid);
            fs::create_directories(workspace.parent_path());
            if (role=="lead") fs::create_directories(workspace);
            else {
                const auto root=path_from_utf8(current.at("workspace").get<std::string>());
                if (!fs::exists(workspace/L".git")) git(root,{L"worktree",L"add",L"-b",L"codex/task-"+widen(jid),workspace.native(),widen(job.at("baseSha").get<std::string>())});
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
                    std::string prompt=team_prompt(role)+"\n\nCurrent assignment\n"+job.at("task").get<std::string>();
                    if (role=="lead") prompt+="\n\nCredentials (tool calls only): "+Json{{"projectId",pid},{"domainId",d.at("domainId")},{"token",d.at("token")}}.dump()+
                        "\nIf required FatFishBox MCP tools are missing or stale, do not stop or ask the user to install/reconnect. Read team-tools.json and refresh team_get_contract for exact schemas; never invent interface names. Use the generated team-call.ps1 bridge: save a UTF-8 JSON request {tool:team_domain_context,arguments:{projectId,domainId,token}} with the actual three credential values, then run powershell -NoProfile -ExecutionPolicy Bypass -File ./team-call.ps1 -RequestPath ./context.request.json. The output is {ok,result}; ok=false means failure. All team_* tools support this bridge to the same local data directory without HTTP configuration. Replace placeholders with actual JSON values.";
                    else prompt+="\n\nProject charter version "+std::to_string(live.at("charterVersion").get<int>())+"\n"+live.at("charter").dump()+"\nManager acceptance criteria: "+d.at("acceptance").get<std::string>();
                    args={{"requestId",job.at("requestId")},{"title",job.at("title")},{"batch",pid+"-"+role},{"role",role},{"workspace",utf8(workspace)},
                        {"task",prompt},{"baseSha",job.at("baseSha")},
                        {"teamProjectId",pid},{"teamDomainId",d.at("domainId")},{"teamJobId",jid},{"teamCharterVersion",live.at("charterVersion")}};
                    if (role=="lead") {
                        args["teamGoal"]=live.at("charter").at("objective");
                        args["teamBootstrapContext"]={{"charterVersion",live.at("charterVersion")},{"charter",live.at("charter")},
                            {"tasks",live.value("tasks",Json::array())},{"workspace",live.at("workspace")},{"baseSha",live.at("baseSha")},{"acceptance",d.at("acceptance")}};
                    }
                    if (!work.empty()) args["workItemId"]=work;
                    saved["launch"]=args;saved["workspace"]=utf8(workspace);saved["state"]="starting";s.save();
                } else args=saved.at("launch");
            }
            const auto launched=scoped.invoke("cluster_start_session",args);
            Store s(data);auto& saved=s.value["projects"][pid]["jobs"][jid];
            saved["sessionId"]=launched.at("id");saved["workItemId"]=launched.at("workItemId");saved["state"]=launched.at("status");if (launched.contains("goal")) saved["goal"]=launched.at("goal");saved.erase("error");s.save();
        } catch (const DispatchError& e) {
            Store s(data);auto& live=s.value["projects"][pid];auto& saved=live["jobs"][jid];
            const auto reason=e.details.value("reason","");
            // A full-capacity probe did not receive a dispatch opportunity.
            // Preserve the last served project, otherwise repeated failed
            // probes can put the same busy project first on every tick.
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
    {Store s(data);projects=s.value.at("projects");}
    for (const auto& original:projects) {
        if (stop.stop_requested()) return;
        if ((original.at("status")=="completed" || original.at("status")=="cancelled") &&
            std::ranges::all_of(original.at("jobs"),[](const Json& j) {return terminal(j.at("state").get<std::string>()) && (!j.contains("sessionId") || j.value("observedTerminal",false));})) continue;
        const auto pid=original.at("projectId").get<std::string>();
        RuntimeHealth health(data,pid,original.value("runtimeErrors",Json::object()));
        if (original.at("status")=="starting" && !original.contains("queueId")) {
            const bool recovered=health.run("create_queue","",[&] {
                const auto q=cluster.invoke("cluster_create_queue",{{"name",original.at("title")},{"requestId","team-queue-"+pid},{"workspace",original.at("workspace")},{"workKey","team:"+pid}});
                Store s(data);auto& p=s.value["projects"][pid];p["queueId"]=q.at("queueId");if (p.at("status")=="starting") p["status"]="running";s.save();
            });
            if (!recovered) continue;
        }
        Json p;
        {Store s(data);p=s.value.at("projects").at(pid);}
        if (!p.contains("queueId") || p.at("status")=="draft") continue;
        const auto scoped=cluster.for_queue(p.at("queueId").get<std::string>());
        if (p.at("status")=="paused" || p.at("status")=="cancelled") {
            const auto reservations=scoped.team_session_reservations(pid);
            Store s(data);auto& current=s.value["projects"][pid];
            bool changed=false;
            if (current.at("status")==p.at("status")) for (auto& job:current["jobs"]) {
                if (job.contains("sessionId") || terminal(job.at("state").get<std::string>())) continue;
                const auto key=job.at("jobId").get<std::string>();
                if (reservations.contains(key)) job["sessionId"]=reservations.at(key);
                else job["state"]="cancelled";
                changed=true;
            }
            if (changed) s.save();
            p=current;
        }
        // Synchronize only this queue, using native code rather than model polling.
        health.run("refresh_sessions","",[&] {scoped.invoke("cluster_refresh_sessions",{{"limit",20}});});
        for (const auto& old:p.at("jobs")) {
            if (!old.contains("sessionId") || old.value("observedTerminal",false)) continue;
            if (!terminal(old.at("state").get<std::string>()) &&
                (old.value("notifiedPolicyVersion",old.value("schedulerPolicyVersion",0))<13 || old.value("notifiedCharterVersion",old.value("charterVersion",0))<p.at("charterVersion").get<int>())) {
                health.run("notify_policy",old.at("jobId"),[&] {
                    const auto version=p.at("charterVersion").get<int>();
                    const auto notice=scoped.invoke("cluster_send_message",{{"sessionId",old.at("sessionId")},
                        {"requestId","team-policy13-c"+std::to_string(version)+"-"+old.at("jobId").get<std::string>()},{"mode","steer"},
                        {"message","Policy v13 - user removed formal gates and evidence collection. Stop additional proof-only exhaustive Gallery/page/config/frame capture and screenshot matrices, including proof-only steps in older plans. Do not restart or cancel development sessions. If a proof-only subprocess is still running, stop only that subprocess when safe; preserve actual development/self-tests. Continue the assigned functional work and relevant checks, then return a concise honest summary. No evidence manifests, hashes, attachment collection, verificationId, acceptance proof lists or full audit are required. Managers use ordinary developer self-tests and independent review to decide acceptance; extra manager tests are optional and targeted. Refresh team_get_prompt(role=lead for the manager, role=worker for developers/reviewers/repair workers) and team_get_contract if you are the manager. Stay in the same goal and use MCP waits. Keep user-requested functional checks, but never claim skipped bulk coverage was completed."}});
                    if (notice.value("accepted",false)) {
                        Store s(data);auto& saved=s.value["projects"][pid]["jobs"][old.at("jobId").get<std::string>()];
                        saved["notifiedPolicyVersion"]=13;saved["notifiedCharterVersion"]=version;s.save();
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
                job["state"]=row.at("status");if (row.contains("goal")) job["goal"]=row.at("goal");if (row.contains("resultVersion")) job["resultVersion"]=row.at("resultVersion");
                auto& d=current["domains"][job.at("domainId").get<std::string>()];
                if (terminal(job.at("state").get<std::string>())) {
                    job["observedTerminal"]=true;
                    if (job.at("role")!="lead") d["wake"]=d.at("wake").get<int>()+1;
                    else if (current.at("status")=="running" && d.at("state")!="completed") {
                        incident(current,d,"manager-ended-"+job.at("jobId").get<std::string>(),"项目经理goal结束但计划未验收完成。保留结果，需恢复或更新完整计划；不得自动重建另一项目经理。");
                    }
                }
                if (current.at("status")=="finishing" && std::ranges::all_of(current.at("jobs"),[](const Json& j){return terminal(j.at("state").get<std::string>());})) {
                    const bool natural=std::ranges::all_of(current.at("jobs"),[&](const Json& j){
                        if (j.at("role")!="lead") return true;
                        for (const auto& later:current.at("jobs")) if (later.at("role")=="lead" && later.at("domainId")==j.at("domainId") && later.value("created",0LL)>j.value("created",0LL)) return true;
                        return j.at("state")=="succeeded" && (!j.contains("goal") || j.at("goal").value("phase","")=="complete");
                    });
                    if (natural) {current["status"]="completed";current["closedAt"]=now();event(current,"project.completed",{{"status","completed"},{"note","项目经理已退出，等待主代理逐项验收合并"}});}
                    else incident(current,d,"manager-goal-incomplete-"+job.at("jobId").get<std::string>(),"候选已验收，但项目经理goal未自然完成；不能交给主代理作为成功结果。");
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
            if (!fresh_backlog.is_null()) {
                auto& backlog=current["backlog"];if (backlog.is_null()) backlog=Json::object();
                for (const auto& [key,value]:fresh_backlog.items()) backlog[key]=value;
            }
            prepare_manager(current);
            s.save();p=current;
        }
    }

}

Json team_tools() {
    const Json text{{"type","string"}},integer{{"type","integer"}},object{{"type","object"}},array{{"type","array"},{"items",text}};
    Json tools=Json::array();
    const auto add=[&](const char* name,const char* description,Json fields,Json required) {
        tools.push_back({{"name",name},{"description",description},{"inputSchema",{{"type","object"},{"properties",fields},{"required",required},{"additionalProperties",false}}},{"outputSchema",{{"type","object"}}}});
    };
    add("team_get_settings","Read the saved maximum concurrency (default 20); all roles automatically share enabled account/model pools using global load balancing.",Json::object(),Json::array());
    add("team_update_settings","Save maximum concurrency including the manager, with revision protection. Existing sessions continue unchanged; all roles share enabled models automatically.",{{"revision",integer},{"totalConcurrency",integer}},{"revision","totalConcurrency"});
    add("team_get_prompt","Read the complete role instructions. The principal must read role=principal before starting work.",{{"role",{{"type","string"},{"enum",Json::array({"principal","lead","worker"})}}}},{"role"});
    const Json charter{{"type","object"},{"properties",{{"objective",text},{"acceptance",text},{"constraints",text},{"architecture",text},{"authority",text},{"escalation",text}}},
        {"required",Json::array({"objective","acceptance","constraints","architecture","authority","escalation"})},{"additionalProperties",false}};
    const Json domain_spec{{"type","object"},{"properties",{{"name",text},{"mission",text},{"acceptance",text},{"dependencies",array}}},
        {"required",Json::array({"name","mission","acceptance","dependencies"})},{"additionalProperties",false}};
    const Json task_spec{{"type","object"},{"properties",{{"name",text},{"steps",text},{"acceptance",text},{"files",array},{"dependsOn",array},
        {"startCondition",{{"type","string"},{"description","Executable start condition. Declare all task dependencies separately in dependsOn; independent tasks must not be serialized."}}},{"tests",text}}},
        {"required",Json::array({"name","steps","acceptance","files","dependsOn","startCondition","tests"})},{"additionalProperties",false}};
    add("team_start_project","Save a draft on a new or existing queue. No manager starts until team_activate_project confirms the finished version. After a project closes and its sessions stop, reuse the same queue. Optionally pin the previous archived project with replaceProjectId. Retry with identical arguments and requestId.",{{"requestId",text},{"queueId",text},{"replaceProjectId",text},{"title",text},{"workspace",text},{"charter",charter},{"tasks",{{"type","array"},{"items",task_spec}}},{"domains",{{"type","array"},{"items",domain_spec},{"minItems",1},{"maxItems",1}}}},
        {"requestId","title","workspace","charter","tasks","domains"});
    add("team_activate_project","Start exactly one persistent-goal manager for a saved draft after planning is finished. Pass the reviewed charterVersion as expectedVersion. Retries with the same requestId are safe.",
        {{"projectId",text},{"requestId",text},{"expectedVersion",integer}},{"projectId","requestId","expectedVersion"});
    add("team_list_projects","List active projects and drafts. Set includeArchived=true to include cancelled and completed projects.",
        {{"includeArchived",{{"type","boolean"}}}},Json::array());
    add("team_get_queue","Read the current queue owner, the previous archived project, shutdown progress, and available actions. Reuse the queue after its previous project has stopped.",{{"queueId",text}},{"queueId"});
    add("team_retire_work_item","Formally cancel obsolete work only after every session is terminal; retain audit records and dispose of old handoffs. Managers may retire adopted historical/old-round items. Current-round developer work requires explicit principal authorization.",{{"projectId",text},{"charterVersion",integer},{"workItemId",text},{"requestId",text},{"reason",text},{"domainId",text},{"token",text}},{"projectId","charterVersion","workItemId","requestId","reason"});
    const Json correction{{"type","object"},{"properties",{{"reportedPath",text},{"replacementPath",text},{"reason",text}}},{"required",Json::array({"reportedPath","replacementPath","reason"})},{"additionalProperties",false}};
    add("team_capture_evidence","Optional collection/correction, available to managers with credentials. Select declared evidence or in-worktree files; fix mistaken references with corrections:[{reportedPath,replacementPath,reason}]. Keep original references and real replacement hashes. Attachment problems are warnings. Unrelated external files need principal authorization.",{{"projectId",text},{"domainId",text},{"token",text},{"sessionId",text},{"resultVersion",text},{"candidateSha",text},{"files",array},{"corrections",{{"type","array"},{"items",correction},{"maxItems",128}}},{"reason",text}},{"projectId","sessionId","resultVersion","candidateSha","files","reason"});
    add("team_get_project","Read one project status, deliverables and pending decisions without full execution logs.",{{"projectId",text},{"includeCharter",{{"type","boolean"}}}},{"projectId"});
    add("team_wait_decisions","Wait up to 300000 ms after activation. Omit afterCursor on the first call, then reuse the returned cursor. Open decisions, errors, paused/draft state, completion and finished shutdown return early. On timedOut, silently wait again with the returned cursor.",{{"projectId",text},{"afterCursor",text},{"timeoutMs",{{"type","integer"},{"minimum",0},{"maximum",300000},{"default",300000}}}},{"projectId"});
    add("team_resolve_decision","The principal resolves one decision using its current expectedRevision; the manager is notified to resume affected work.",{{"projectId",text},{"decisionId",text},{"expectedRevision",integer},{"requestId",text},{"resolution",text}},{"projectId","decisionId","expectedRevision","requestId","resolution"});
    add("team_update_charter","Update a draft or atomically apply a complete plan revision to an active project. Provide the entire tasks array when it changes. Updating a completed project reopens a draft if the queue is available. A cancelled project can only be replaced on its queue.",{{"projectId",text},{"expectedVersion",integer},{"requestId",text},{"charter",charter},{"tasks",{{"type","array"},{"items",task_spec}}},{"changeSummary",text}},{"projectId","expectedVersion","requestId","charter","changeSummary"});
    add("team_record_merge","After manager completion, independently accept and actually merge the candidate into main before recording the verified integration commit.",{{"projectId",text},{"requestId",text},{"workItemId",text},{"integrationSha",text},{"targetBranch",text},{"notes",text}},{"projectId","requestId","workItemId","integrationSha","targetBranch","notes"});
    add("team_control_project","Pause stops dispatch and cancels active sessions before resume. Cancel permanently archives the project and releases the queue after sessions stop. Use dryRun=true to preview cancel, then confirm=true to apply it. History and artifacts remain.",{{"projectId",text},{"requestId",text},{"action",{{"type","string"},{"enum",Json::array({"pause","resume","cancel"})}}},{"dryRun",{{"type","boolean"}}},{"confirm",{{"type","boolean"}}}},{"projectId","requestId","action"});
    const Json auth{{"projectId",text},{"domainId",text},{"token",text},{"charterVersion",integer}};
    const auto scoped=[&](const char* name,const char* desc,Json fields,Json required) {
        fields.update(auth);for (const auto* k:{"projectId","domainId","token"}) required.push_back(k);add(name,desc,fields,required);
    };
    scoped("team_domain_context","Required at manager startup and recovery: current charter, checkpoint, dependencies, task contracts, dispatch readiness and decisions.",Json::object(),Json::array());
    scoped("team_domain_jobs","Page manager jobs. Unaccepted jobs only by default; includeAccepted=true includes history when needed.",{{"offset",integer},{"limit",integer},{"includeAccepted",{{"type","boolean"}}}},Json::array());
    scoped("team_backlog_status","Read pending/actionable/in-flight handoff counts. Continue until the whole assigned queue is handled, not just one task.",Json::object(),Json::array());
    scoped("team_get_contract","Read exact manager tool schemas and nested-call examples. Read team-tools.json before CLI calls and refresh this contract instead of guessing operations.",Json::object(),Json::array());
    scoped("team_wait_actions","The manager must wait through this MCP tool for up to 300000 ms. Child completion, actionable FIFO entries, ready plan items or state changes return early. Never sleep.",{{"timeoutMs",{{"type","integer"},{"minimum",0},{"maximum",300000},{"default",300000}}}},Json::array());
    scoped("team_next_action","Read the durable FIFO head. Handle it or dispatch its successor before taking the next item. New results join the tail without preemption.",Json::object(),Json::array());
    scoped("team_priority_results","Legacy result-list compatibility only; it does not grant priority. Use team_next_action FIFO for the manager loop.",Json::object(),Json::array());
    scoped("team_next_handoff","Claim/resume a handoff. Disposition requires the returned claimToken, stable requestId, notes and outcome=confirm/linked/blocked. confirm records manager acknowledgement without proof requirements; linked needs an active job on the same work item.",{{"requestId",text},{"claimToken",text},{"outcome",{{"type","string"},{"enum",Json::array({"confirm","linked","blocked"})}}},{"notes",text},{"evidence",array},{"jobId",text},{"decisionId",text}},Json::array());
    scoped("team_domain_history","Page the original queue ledger. Remaining work by default; includeClosed=true includes integrated and cancelled history.",{{"offset",integer},{"limit",integer},{"includeClosed",{{"type","boolean"}}}},Json::array());
    scoped("team_claim_work_item","Adopt historical work and sessions under their existing IDs, issues, reviews and receipts. Unadopted out-of-scope history may be deferred with a reason. Already adopted obsolete work uses team_retire_work_item instead of fake repairs.",{{"workItemId",text},{"requestId",text},{"disposition",{{"type","string"},{"enum",Json::array({"adopt","defer"})}}},{"reason",text}},{"workItemId","requestId"});
    scoped("team_dispatch_ready","Queue every independent task whose declared dependencies are accepted, including file scope, start conditions, tests and accepted dependency SHAs. Do not duplicate existing assignments.",{{"requestId",text}},{"requestId","charterVersion"});
    scoped("team_delegate","Dispatch developer, reviewer or repair jobs. Use sourceJobId to arrange ordinary independent review. No machine-enforced proof gate is applied. Finish only after all current planned work is accepted.",{{"requestId",text},{"title",text},{"role",{{"type","string"},{"enum",Json::array({"developer","reviewer","repair"})}}},{"task",text},{"baseSha",text},{"sourceJobId",text},{"workItemId",text},{"taskName",text}},
        {"requestId","title","role","task","charterVersion"});
    scoped("team_verify_candidate","Optional targeted test runner. Run only useful checks; verificationId is not required for acceptance and no evidence is captured.",{{"requestId",text},{"workItemId",text},{"candidateSha",text},{"executable",text},{"arguments",array},{"timeoutMs",integer}},{"requestId","workItemId","candidateSha","executable","arguments"});
    scoped("team_cancel_job","Request cancellation of an owned queued or active job. The service confirms actual terminal state and retains commits/evidence.",{{"jobId",text},{"requestId",text},{"reason",text}},{"jobId","requestId","reason"});
    scoped("team_checkpoint","Persist progress in the same goal session. waiting does not end/recreate the manager; continue only records context for the next loop iteration.",{{"requestId",text},{"state",{{"type","string"},{"enum",Json::array({"working","waiting","decision","continue"})}}},{"checkpoint",object}},{"requestId","state","checkpoint"});
    scoped("team_escalate","Create a concrete authorization/failure decision. Attach actionId from team_next_action or jobId to pause a genuinely blocked FIFO item; other work continues and MCP can wait. Correct ordinary schema errors using the contract templates first.",{{"requestId",text},{"question",text},{"reason",text},{"recommendation",text},{"alternatives",text},{"impact",text},{"blockedScope",text},{"jobId",text},{"actionId",text}},
        {"requestId","question","reason","recommendation","alternatives","impact","blockedScope"});
    scoped("team_finish_domain","Finish after manager confirmation of all current planned items and no active work. Only summary and requestId are required; no evidence or acceptance-proof arrays.",{{"requestId",text},{"summary",text}},
        {"requestId","summary"});
    scoped("team_domain_read","Read owned session/work-item details on demand. Logs default to 4000 bytes; no automatic collection is performed.",{{"operation",text},{"arguments",object}},{"operation","arguments"});
    scoped("team_domain_action","Update the owned ledger or cancel a session. operation is cluster_update_work_item/cluster_cancel_session. Put record_review/cancel in inner arguments.action.",{{"operation",{{"type","string"},{"enum",Json::array({"cluster_update_work_item","cluster_cancel_session"})}}},{"arguments",object}},{"operation","arguments"});
    return tools;
}
}
