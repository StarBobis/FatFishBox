#include "team_scheduler.h"
#include "result_preview.h"
#include "goal_projection.h"
#include <iostream>
#include <stdexcept>
using fatfish::Json;
void require(bool yes,const char* message) {if (!yes) throw std::runtime_error(message);}
Json job(const char* id,const char* role,long long created) {return {{"jobId",id},{"role",role},{"state","queued"},{"created",created}};}
int main() {
    Json projects={{"a",{{"projectId","a"},{"status","running"},{"jobs",{{"d",job("d","developer",1)},{"r",job("r","reviewer",3)}}}}},
        {"b",{{"projectId","b"},{"status","running"},{"jobs",{{"l",job("l","lead",2)}}}}}};
    auto plan=fatfish::team_dispatch_plan(projects,Json::object(),100);
    require(plan.size()==3 && plan[0]["jobId"]=="d" && plan[1]["jobId"]=="l" && plan[2]["jobId"]=="r","FIFO must ignore role priority");
    projects["a"]["jobs"]["d"]["retryAt"]=101;
    projects["a"]["jobs"]["r"]["historical"]=true;
    require(fatfish::team_dispatch_plan(projects,Json::object(),100).size()==1,"exclude retry and history");
    require(fatfish::team_dispatch_plan(projects,Json::object(),100,0).empty(),"budget zero");
    projects["b"]["status"]="cancelled";
    require(fatfish::team_dispatch_plan(projects,Json::object(),100).empty(),"cancelled project must not dispatch");
    std::wstring text(200000,L'x');const auto original=text;
    require(fatfish::result_preview(text).size()<=65536 && text==original,"long result safe and full copy retained");
    require(fatfish::result_preview(std::wstring(65536,L'x')).size()==65536,"exact boundary retained");
    const auto preview=fatfish::result_preview(text);const auto cut=preview.find(L"\r\n\r\n[");
    text[cut-1]=0xD83D;text[cut]=0xDE00;
    const auto unicode=fatfish::result_preview(text);
    require(unicode[cut-2]==L'x' && unicode[cut-1]==L'\r',"do not split a UTF16 surrogate pair");
    const Json goal={{"id","real-goal"},{"phase","active"},{"objective"," 任务\r\n"}};
    const Json nested={{"values",{{"goal",{{"goal",goal},{"roundsStarted",1}}}}}};
    require(fatfish::goal_matches(fatfish::goal_snapshot(nested),"\xE3\x80\x80任务 "),"real nested wire and JavaScript trim");
    require(fatfish::goal_matches(fatfish::goal_snapshot({{"values",{{"goal",goal}}}}),"任务"),"legacy flat host compatibility");
    require(fatfish::goal_snapshot({{"values",{{"goal",{{"goal",nullptr}}}}}}).is_null(),"pending nested goal is not active");
    require(!fatfish::goal_matches(fatfish::goal_snapshot(nested),"其他任务"),"never accept a different objective");
    auto paused=goal;paused["phase"]="paused";
    require(!fatfish::goal_matches(paused,"任务"),"paused is not a running goal");
    require(fatfish::normalized_goal_objective("a  b")!="a b","internal whitespace remains significant");
    std::cout<<"FIFO dispatch and long Unicode result preview passed.\n";
}
