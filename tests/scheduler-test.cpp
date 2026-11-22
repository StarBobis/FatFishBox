#include "team_scheduler.h"
#include <iostream>
#include <stdexcept>

using fatfish::Json;
void require(bool yes,const char* message) {if (!yes) throw std::runtime_error(message);}
Json job(const char* id,const char* role,long long created=1) {
    return {{"jobId",id},{"role",role},{"state","queued"},{"created",created}};
}
int main() {
    Json projects={
        {"a",{{"projectId","a"},{"status","running"},{"jobs",{{"a2",job("a2","developer",2)},{"a1",job("a1","developer")},{"ar",job("ar","reviewer")}}}}},
        {"b",{{"projectId","b"},{"status","running"},{"jobs",{{"b1",job("b1","repair")},{"b2",job("b2","developer",2)},{"bl",job("bl","lead")}}}}},
        {"c",{{"projectId","c"},{"status","paused"},{"jobs",{{"cr",job("cr","reviewer")}}}}}
    };
    auto plan=fatfish::team_dispatch_plan(projects,Json::object(),100);
    require(plan.size()==6,"all eligible jobs must be planned");
    require(plan[0]["jobId"]=="ar" && plan[1]["jobId"]=="bl","role priority must apply across projects");
    for (std::size_t i=2;i<plan.size();++i)
        require(plan[i]["projectId"]==(i%2==0?"a":"b"),"a busy project must not consume every dispatch opportunity");
    require(plan[2]["jobId"]=="a1" && plan[4]["jobId"]=="a2","jobs in a project must keep creation order");
    auto cursors=Json::parse(Json{{"3","a"}}.dump());
    plan=fatfish::team_dispatch_plan(projects,cursors,100,3);
    require(plan.size()==3 && plan[2]["projectId"]=="b","persisted cursor must give the next project its turn after restart");
    projects["a"]["jobs"]["a1"]["retryAt"]=101;
    projects["a"]["jobs"]["a2"]["historical"]=true;
    projects["a"]["jobs"]["ar"]["state"]="running";
    plan=fatfish::team_dispatch_plan(projects,{{"3","removed-project"}},100);
    require(plan.size()==3 && plan[1]["projectId"]=="b","future retries, historical and running jobs must not dispatch; stale cursors must wrap");
    require(fatfish::team_dispatch_plan(projects,cursors,100,0).empty(),"zero budget must not dispatch");
    projects["a"]["status"]="cancelled";
    require(fatfish::team_dispatch_plan(projects,cursors,100,1).size()==1,"attempt budget must be enforced");
    std::cout<<"Scheduler passed: cross-project rotation, restart cursor, global role priority, stable order and bounded attempts.\n";
}
