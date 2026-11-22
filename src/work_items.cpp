#include "work_items.h"
#include "handoff_artifacts.h"
#include "event_journal.h"
#include "state_file.h"
#include "dsh.h"
#include <algorithm>
#include <set>
#include <map>

namespace fatfish {
namespace {
void check(bool value,const char* message) {if (!value) throw std::runtime_error(message);}
std::string text(const Json& input,const char* key,std::size_t max=4000) {
    check(input.contains(key) && input.at(key).is_string(),"总账缺少字符串字段");
    const auto value=input.at(key).get<std::string>();
    check(!value.empty() && value.size()<=max && value.find('\0')==value.npos,"总账字段为空或超过长度限制");
    return value;
}
bool is_sha(const std::string& value) {
    return (value.size()==40 || value.size()==64) && value.find_first_not_of("0123456789abcdefABCDEF")==value.npos;
}
bool active(const Json& row) {
    const auto value=row.value("status","");
    return value!="succeeded" && value!="failed" && value!="cancelled" && value!="interrupted";
}
Json& items(Json& state) {
    if (!state.contains("workItems")) state["workItems"]=Json::object();
    return state.at("workItems");
}
std::string default_key(const std::string& queue,const std::string& root) {return "W-"+sha256(queue+":"+root).substr(0,24);}
Json new_item(const std::string& queue,const std::string& key,const Json& input,const std::string& origin) {
    const auto stamp=now();
    return {{"workItemId",key},{"queueId",queue},{"title",input.value("title",key)},
        {"task",input.value("task","")},{"workspace",input.value("workspace","")},
        {"baseSha",input.value("baseSha",Json())},{"state","open"},{"revision",1},
        {"created",stamp},{"updated",stamp},{"origin",origin},{"sessions",Json::object()},
        {"issues",Json::object()},{"reviews",Json::array()},{"history",Json::array()},
        {"operations",Json::object()}};
}
Json& find_item(Json& state,const std::string& queue,const std::string& key) {
    auto& rows=items(state);
    check(rows.contains(key) && rows.at(key).at("queueId")==queue,"当前队列没有该 workItemId");
    return rows.at(key);
}
const Json& find_item(const Json& state,const std::string& queue,const std::string& key) {
    check(state.contains("workItems") && state.at("workItems").contains(key) &&
        state.at("workItems").at(key).at("queueId")==queue,"当前队列没有该 workItemId");
    return state.at("workItems").at(key);
}
std::string primary_root(const Json& state,const Json& source,const std::string& queue) {
    auto row=source;
    std::set<std::string> seen;
    while (row.contains("repairOf")) {
        const auto parent=row.at("repairOf").value("sessionId","");
        if (parent.empty() || !seen.insert(parent).second) break;
        const auto& native=state.at("sessions");
        if (!native.contains(parent) || native.at(parent).value("queueId",state.value("legacyQueueId",""))!=queue) return parent;
        row=native.at(parent);
        if (row.contains("workItemId")) return row.at("workItemId").get<std::string>();
    }
    return row.at("id").get<std::string>();
}
Json original_spec_source(const Json& state,const Json& source,const std::string& queue) {
    auto row=source;
    std::set<std::string> seen;
    while (row.contains("repairOf")) {
        const auto parent=row.at("repairOf").value("sessionId","");
        if (parent.empty() || !seen.insert(parent).second) return Json{{"title",source.value("title","")}};
        if (!state.at("sessions").contains(parent) ||
            state.at("sessions").at(parent).value("queueId",state.value("legacyQueueId",""))!=queue)
            return Json{{"title",source.value("title","")},{"workspace",source.value("workspace","")}};
        row=state.at("sessions").at(parent);
    }
    return row;
}
Json membership(const Json& row) {
    Json result;
    for (const auto* member:{"id","role","status","resultVersion","workspace","baseSha","processed","stale","updated","checksManifest","evidenceManifest"})
        if (row.contains(member)) result[member]=row.at(member);
    return result;
}
std::size_t open_issues(const Json& item) {
    return std::ranges::count_if(item.at("issues"),[](const Json& value){return value.at("state")=="open";});
}
std::string phase(const Json& item) {
    const auto state=item.at("state").get<std::string>();
    if (state!="open") return state;
    if (open_issues(item)) return "needs_repair";
    bool running=false,review=false,ended=false,failed=false;
    for (const auto& session:item.at("sessions")) {
        running=running || active(session);
        review=review || (session.value("role","")=="reviewer" && !active(session));
        ended=ended || !active(session);
        failed=failed || session.value("status","")=="failed";
    }
    if (running) return "in_progress";
    if (item.at("origin")=="migrated" && !item.at("sessions").empty() &&
        std::ranges::all_of(item.at("sessions"),[](const Json& row){return row.value("processed",false);})) return "needs_reconciliation";
    if (review) return "awaiting_acceptance";
    if (failed) return "needs_attention";
    if (ended) return "awaiting_review";
    return "open";
}
Json summary(const Json& item) {
    Json result;
    for (const auto* member:{"workItemId","queueId","title","state","revision","baseSha","created","updated","origin","supersededBy"})
        if (item.contains(member)) result[member]=item.at(member);
    result["phase"]=phase(item);
    result["reconciliationRequired"]=item.at("origin")=="migrated" && item.at("state")=="open";
    result["sessionCount"]=item.at("sessions").size();
    result["openIssueCount"]=open_issues(item);
    if (item.contains("acceptance")) result["candidateSha"]=item.at("acceptance").at("candidateSha");
    if (item.contains("integration")) result["integrationSha"]=item.at("integration").at("integrationSha");
    return result;
}
void touch(Json& state,Json& item,const std::string& action) {
    item["revision"]=item.at("revision").get<std::int64_t>()+1;
    item["updated"]=now();
    record_event(state,item.at("queueId").get<std::string>(),"work_item."+action,summary(item));
}
Json verification_snapshot(const Json& sessions) {
    Json snapshot=Json::object();
    for (const auto& [key,row]:sessions.items()) {
        snapshot[key]=Json::object();
        for (const auto* member:{"role","status","resultVersion","baseSha"})
            if (row.contains(member)) snapshot[key][member]=row.at(member);
    }
    return snapshot;
}
bool any_active(const Json& item) {
    return std::ranges::any_of(item.at("sessions"),[](const Json& row){return active(row);});
}
}
std::string session_work_item(const Json& state,const Json& row,const std::string& queue) {
    if (row.contains("workItemId")) return row.at("workItemId").get<std::string>();
    const auto key=row.at("id").get<std::string>();
    if (state.contains("workExternalSessions") && state.at("workExternalSessions").contains(key)) {
        const auto& saved=state.at("workExternalSessions").at(key);
        if (saved.at("queueId")==queue) return saved.at("workItemId").get<std::string>();
    }
    const auto root=primary_root(state,row,queue);
    if (state.contains("workItems") && state.at("workItems").contains(root) && state.at("workItems").at(root).at("queueId")==queue)
        return root;
    return default_key(queue,root);
}
void register_work_session(Json& state,Json& row,const Json& input,const std::string& queue) {
    auto& rows=items(state);
    auto key=input.contains("workItemId")?text(input,"workItemId",100):session_work_item(state,row,queue);
    if (input.contains("workItemId")) {
        const auto& current=find_item(state,queue,key);
        check(current.at("state")=="open" || current.at("state")=="blocked","已验收或关闭的事项须先 reopen 再派发");
    }
    if (!rows.contains(key)) rows[key]=new_item(queue,key,input,"dispatch");
    check(rows.at(key).at("queueId")==queue,"workItemId 不能跨队列使用");
    row["workItemId"]=key;
    if (input.contains("baseSha")) {
        const auto base=text(input,"baseSha",64);
        check(is_sha(base),"baseSha 须为完整 Git SHA");
        row["baseSha"]=base;
    } else if (!rows.at(key).at("baseSha").is_null()) row["baseSha"]=rows.at(key).at("baseSha");
}
bool synchronize_work_items(Json& state) {
    const auto before=state.value("workItems",Json::object());
    bool changed=false;
    auto& rows=items(state);
    const auto legacy=state.value("legacyQueueId","");
    if (legacy.empty()) return false;
    for (auto& row:state.at("sessions")) {
        if (row.value("role","")=="lead" && row.contains("teamProjectId")) continue;
        const auto queue=row.value("queueId",legacy);
        if (!row.contains("workItemId")) {
            const auto key=session_work_item(state,row,queue);
            row["workItemId"]=key;
            changed=true;
            if (!rows.contains(key)) {
                Json initial=original_spec_source(state,row,queue);
                if (initial.contains("dispatchOperation")) {
                    const auto& input=initial.at("dispatchOperation").at("input");
                    if (input.contains("task")) initial["task"]=input.at("task");
                }
                rows[key]=new_item(queue,key,initial,"migrated");
            }
        }
    }
    std::map<std::string,Json> memberships;
    for (const auto& [key,item]:rows.items()) memberships[key]=Json::object();
    const auto assign=[&](const std::string& key,const Json& row) {
        if (!rows.contains(key) || rows.at(key).at("queueId")!=row.value("queueId",legacy)) return;
        memberships[key][row.at("id").get<std::string>()]=membership(row);
    };
    for (const auto& row:state.at("sessions")) {
        if (row.value("role","")=="lead" && row.contains("teamProjectId")) continue;
        const auto queue=row.value("queueId",legacy);
        assign(row.at("workItemId").get<std::string>(),row);
        const auto links=row.value("repairLinks",row.contains("repairOf")?Json::array({row.at("repairOf")}):Json::array());
        for (const auto& link:links) {
            const auto parent=link.at("sessionId").get<std::string>();
            if (state.at("sessions").contains(parent)) assign(session_work_item(state,state.at("sessions").at(parent),queue),row);
            else if (state.contains("workExternalSessions") && state.at("workExternalSessions").contains(parent))
                assign(state.at("workExternalSessions").at(parent).at("workItemId").get<std::string>(),row);
        }
    }
    if (state.contains("workExternalSessions")) for (const auto& row:state.at("workExternalSessions"))
        assign(row.at("workItemId").get<std::string>(),row);
    for (auto& [key,item]:rows.items()) if (item.at("sessions")!=memberships.at(key)) {
        item["sessions"]=memberships.at(key);
        if (item.at("state")=="accepted" && item.contains("acceptance") &&
            item.at("acceptance").value("verifiedSessions",Json::object())!=verification_snapshot(item.at("sessions"))) {
            item["history"].push_back({{"action","acceptance_invalidated"},{"time",now()},
                {"notes","会话关联或已验收结果版本发生变化"},{"acceptance",item.at("acceptance")}});
            item["state"]="open";
            item.erase("acceptance");
        }
        touch(state,item,"sessions_changed");
    }
    return changed || before!=rows;
}
void observe_work_sessions(Json& state,const std::string& queue,const Json& external) {
    auto& rows=items(state);
    if (!state.contains("workExternalSessions")) state["workExternalSessions"]=Json::object();
    auto& bound=state.at("workExternalSessions");
    for (const auto& [key,row]:external.items()) {
        if (!bound.contains(key)) {
            const auto work=session_work_item(state,row,queue);
            if (!rows.contains(work)) rows[work]=new_item(queue,work,row,"migrated");
            auto entry=membership(row);
            entry.update({{"queueId",queue},{"workItemId",work},{"role",row.value("role","support")}});
            bound[key]=std::move(entry);
        } else if (bound.at(key).at("queueId")==queue) {
            auto& saved=bound.at(key);
            const auto role=saved.value("role","support");
            const auto work=saved.at("workItemId");
            saved=membership(row);
            saved.update({{"queueId",queue},{"role",role},{"workItemId",work}});
        }
    }
}
void reassign_work_session_queue(Json& state,const std::string& session,const std::string& queue) {
    if (!state.contains("workExternalSessions") || !state.at("workExternalSessions").contains(session)) return;
    auto& row=state.at("workExternalSessions").at(session);
    const auto old=row.at("workItemId").get<std::string>();
    auto& prior=find_item(state,row.at("queueId").get<std::string>(),old);
    check(prior.at("origin")!="explicit" && prior.at("state")=="open" && prior.at("sessions").size()<=1,
        "会话已归入明确跟踪或验收的事项，不能单独更换队列");
    const auto key=default_key(queue,session);
    auto initial=row;
    initial["title"]=prior.at("title");
    items(state)[key]=new_item(queue,key,initial,"migrated");
    row["queueId"]=queue;
    row["workItemId"]=key;
    prior["state"]="superseded";
    prior["supersededBy"]=key;
    touch(state,prior,"moved");
    synchronize_work_items(state);
}
std::string publish_work_item_spec(const fs::path& data,const Json& item) {
    const auto file=data/L"work-items"/path_from_utf8(sha256(item.at("workItemId").get<std::string>())+".json");
    fs::create_directories(file.parent_path());
    Json spec;
    for (const auto* member:{"workItemId","queueId","title","task","baseSha","issues","revision"}) spec[member]=item.at(member);
    spec["metadataOnly"]=true;
    const ExclusiveStateLock lock(file.wstring()+L".lock");
    if (!fs::exists(file) || read_json(file,64*1024*1024)!=spec) write_state_file(file,spec);
    return utf8(file);
}
Json create_work_item(Json& state,const std::string& queue,const Json& input) {
    const auto request=text(input,"requestId",160);
    text(input,"title",400);
    if (input.contains("task")) text(input,"task",64000);
    if (input.contains("baseSha")) check(is_sha(text(input,"baseSha",64)),"baseSha 须为完整 Git SHA");
    auto& rows=items(state);
    for (const auto& item:rows) if (item.at("queueId")==queue && item.value("createRequestId","")==request) {
        check(item.at("createInput")==input,"该事项 requestId 已用于其他参数");
        return summary(item);
    }
    const auto key="W-"+id();
    auto& item=rows[key]=new_item(queue,key,input,"explicit");
    item["createRequestId"]=request;
    item["createInput"]=input;
    record_event(state,queue,"work_item.created",summary(item));
    return summary(item);
}
Json update_work_item(Json& state,const std::string& queue,const Json& input,const Json& observed) {
    const auto key=text(input,"workItemId",100),request=text(input,"requestId",160),action=text(input,"action",40);
    auto& item=find_item(state,queue,key);
    auto& operations=item.at("operations");
    if (operations.contains(request)) {
        check(operations.at(request).at("input")==input,"该更新 requestId 已用于其他参数");
        auto replay=operations.at(request).at("result");
        replay["replayed"]=true;
        return replay;
    }
    check(input.contains("expectedRevision") && input.at("expectedRevision")==item.at("revision"),"事项 revision 已变化，请重新读取后更新");
    const auto details=input.value("details",Json::object());
    check(details.is_object(),"details 须为对象");
    const auto current=item.at("state").get<std::string>();
    std::string previous_work;
    check(current!="superseded","该事项已归并，请使用 supersededBy");
    check((current!="integrated" && current!="cancelled") || action=="reopen","已关闭事项须先 reopen");
    if (action=="add_issue") {
        const auto issue="I-"+sha256(key+":"+request).substr(0,24);
        item["issues"][issue]={{"issueId",issue},{"state","open"},{"message",text(details,"message",4000)},
            {"acceptance",text(details,"acceptance",8000)},{"created",now()}};
        if (details.contains("candidateSha")) {
            check(is_sha(text(details,"candidateSha",64)),"candidateSha 须为完整 Git SHA");
            item["issues"][issue]["candidateSha"]=details.at("candidateSha");
        }
        if (current=="accepted") {item["state"]="open";item.erase("acceptance");}
    } else if (action=="resolve_issue") {
        const auto issue=text(details,"issueId",100);
        check(item.at("issues").contains(issue),"没有该缺陷 ID");
        item["issues"][issue].update({{"state","resolved"},{"resolution",text(details,"notes",4000)},{"resolved",now()}});
    } else if (action=="record_review") {
        const auto session=text(details,"sessionId",256),version=text(details,"resultVersion",100),candidate=text(details,"candidateSha",64);
        const auto verdict=text(details,"verdict",20);
        check(is_sha(candidate),"candidateSha 须为完整 Git SHA");
        check(verdict=="passed" || verdict=="failed" || verdict=="blocked","verdict 须为 passed、failed 或 blocked");
        check(item.at("sessions").contains(session),"审查会话未关联到该事项");
        const auto& row=item.at("sessions").at(session);
        check(row.value("role","")=="reviewer" && !active(row) && !row.value("stale",false) && row.value("resultVersion","")==version,
            "审查会话角色、终态或结果版本不匹配");
        check(verdict!="passed" || row.at("status")=="succeeded","执行失败的审查不能登记为通过");
        auto review=details;
        review["notes"]=text(details,"notes",4000);
        review["recordedAt"]=now();
        review["source"]="main_agent_verified";
        item["reviews"].push_back(review);
        if (verdict!="passed" && current=="accepted") {item["state"]="open";item.erase("acceptance");}
    } else if (action=="accept") {
        const auto candidate=text(details,"candidateSha",64);
        check(is_sha(candidate),"candidateSha 须为完整 Git SHA");
        check(!any_active(item) && !open_issues(item),"尚有活动会话或未解决缺陷，不能验收");
        Json review;
        for (const auto& value:item.at("reviews")) if (value.at("candidateSha")==candidate) review=value;
        check(!review.is_null() && review.at("verdict")=="passed","候选尚无主代理登记通过的独立审查");
        const auto session=review.at("sessionId").get<std::string>();
        check(item.at("sessions").contains(session) && item.at("sessions").at(session).value("resultVersion","")==review.at("resultVersion").get<std::string>(),
            "审查结果版本已变化，须重新核对");
        item["acceptance"]={{"candidateSha",candidate},{"review",review},{"notes",text(details,"notes",4000)},{"acceptedAt",now()},
            {"verifiedSessions",verification_snapshot(item.at("sessions"))}};
        for (const auto* reference:{"checksManifest","evidenceManifest"}) if (details.contains(reference)) item["acceptance"][reference]=details.at(reference);
        item["state"]="accepted";
    } else if (action=="integrate") {
        check(current=="accepted" && item.contains("acceptance") && !any_active(item) && !open_issues(item),"须先完成最终验收");
        const auto commit=text(details,"integrationSha",64);
        check(is_sha(commit),"integrationSha 须为完整 Git SHA");
        item["integration"]={{"integrationSha",commit},{"targetBranch",details.value("targetBranch","main")},
            {"candidateSha",item.at("acceptance").at("candidateSha")},{"notes",text(details,"notes",4000)},
            {"source","main_agent_recorded"},{"integratedAt",now()}};
        for (const auto* reference:{"checksManifest","evidenceManifest"}) if (details.contains(reference)) item["integration"][reference]=details.at(reference);
        item["state"]="integrated";
    } else if (action=="reopen" || action=="block" || action=="cancel") {
        const auto notes=text(details,"notes",4000);
        check(action!="cancel" || !any_active(item),"事项仍有活动会话，请先取消会话再关闭事项");
        item["history"].push_back({{"state",current},{"action",action},{"notes",notes},{"time",now()},
            {"acceptance",item.value("acceptance",Json())},{"integration",item.value("integration",Json())}});
        item["state"]=action=="reopen"?"open":action=="block"?"blocked":"cancelled";
        if (action=="reopen") {item.erase("acceptance");item.erase("integration");}
    } else if (action=="attach_session") {
        const auto session=text(details,"sessionId",256);
        check(current=="open" || current=="blocked","已验收事项须先 reopen 再关联会话");
        if (state.at("sessions").contains(session)) {
            auto& row=state.at("sessions").at(session);
            check(row.value("queueId",state.value("legacyQueueId",""))==queue,"会话不属于当前队列");
            previous_work=row.value("workItemId","");
            if (!previous_work.empty() && previous_work!=key) {
                const auto& previous=find_item(state,queue,previous_work);
                check(previous.at("state")=="open" || previous.at("state")=="blocked","原事项已验收，须先 reopen 再调整归属");
            }
            row["workItemId"]=key;
        } else {
            check(!observed.is_null() && observed.at("id")==session,"外部会话须先读取并核对归属");
            if (state.contains("workExternalSessions") && state.at("workExternalSessions").contains(session))
                previous_work=state.at("workExternalSessions").at(session).at("workItemId").get<std::string>();
            if (!previous_work.empty() && previous_work!=key) {
                const auto& previous=find_item(state,queue,previous_work);
                check(previous.at("state")=="open" || previous.at("state")=="blocked","原事项已验收，须先 reopen 再调整归属");
            }
            auto row=membership(observed);
            const auto role=details.value("role","support");
            check(role=="developer" || role=="reviewer" || role=="repair" || role=="support","会话角色无效");
            row.update({{"workItemId",key},{"queueId",queue},{"role",role}});
            state["workExternalSessions"][session]=row;
        }
    } else throw std::runtime_error("未知事项更新 action");
    synchronize_work_items(state);
    if (!previous_work.empty() && previous_work!=key) {
        auto& previous=find_item(state,queue,previous_work);
        if (previous.at("sessions").empty() && previous.at("origin")!="explicit") {
            previous["state"]="superseded";
            previous["supersededBy"]=key;
            touch(state,previous,"superseded");
        }
    }
    touch(state,item,action);
    auto result=summary(item);
    if (action=="add_issue") result["issueId"]="I-"+sha256(key+":"+request).substr(0,24);
    operations[request]={{"input",input},{"result",result}};
    return result;
}
Json query_work_items(const Json& state,const std::string& queue,const Json& input,bool single) {
    if (single) {
        const auto& item=find_item(state,queue,text(input,"workItemId",100));
        auto result=item;
        result.erase("operations");result.erase("createInput");result.erase("createRequestId");
        if (!input.value("includeTask",false)) result.erase("task");
        result["phase"]=phase(item);
        result["openIssueCount"]=open_issues(item);
        return result;
    }
    const auto offset=input.value("offset",0),limit=input.value("limit",50);
    check(offset>=0 && limit>=1 && limit<=100,"offset 须非负，limit 须为 1 至 100");
    Json counts{{"total",0},{"remaining",0},{"pendingIntegration",0},{"integrated",0},{"cancelled",0},{"migratedNeedsReconciliation",0}};
    Json results=Json::array();
    std::vector<Json> matches;
    if (state.contains("workItems")) for (const auto& item:state.at("workItems")) {
        if (item.at("queueId")!=queue || item.at("state")=="superseded") continue;
        const auto status=item.at("state").get<std::string>();
        counts["total"]=counts.at("total").get<int>()+1;
        if (status!="integrated" && status!="cancelled") counts["remaining"]=counts.at("remaining").get<int>()+1;
        if (status=="accepted") counts["pendingIntegration"]=counts.at("pendingIntegration").get<int>()+1;
        if (status=="integrated" || status=="cancelled") counts[status]=counts.at(status).get<int>()+1;
        if (item.at("origin")=="migrated" && status=="open") counts["migratedNeedsReconciliation"]=counts.at("migratedNeedsReconciliation").get<int>()+1;
        const auto row=summary(item);
        if (input.contains("state") && input.at("state")!=row.at("phase") && input.at("state")!=status) continue;
        if (input.value("remainingOnly",false) && (status=="integrated" || status=="cancelled")) continue;
        matches.push_back(row);
    }
    std::sort(matches.begin(),matches.end(),[](const Json& a,const Json& b) {return a.at("workItemId")<b.at("workItemId");});
    const auto end=std::min(matches.size(),static_cast<std::size_t>(offset)+static_cast<std::size_t>(limit));
    for (auto index=static_cast<std::size_t>(offset);index<end;++index) results.push_back(matches[index]);
    return {{"counts",counts},{"items",results},{"totalMatched",matches.size()},
        {"nextOffset",end<matches.size()?Json(end):Json()},{"source","recorded_work_items"}};
}
}
