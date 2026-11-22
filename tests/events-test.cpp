#include "event_journal.h"
#include "handoff_artifacts.h"
#include <iostream>
#include <stdexcept>
using fatfish::Json;
void require(bool condition,const char* message) {if (!condition) throw std::runtime_error(message);}
int main() {
 try {
    Json state=Json::object();auto& journal=fatfish::event_journal(state);
    const auto a0=fatfish::event_cursor(journal,"a",0);
    fatfish::record_event(state,"a","session.updated",{{"id","a"}});
    const auto a1=fatfish::event_cursor(journal,"a",1);
    const auto b1=fatfish::event_cursor(journal,"b",1);
    for (int i=0;i<5000;++i) fatfish::record_event(state,"b","session.updated",{{"id","b"},{"step",i}});
    require(journal.at("events").size()==4096,"journal must remain bounded");
    auto quiet=fatfish::events_after(state,"a",{{"afterCursor",a1}},10);
    require(!quiet.at("resetRequired").get<bool>() && quiet.at("events").empty(),"another busy queue must not reset a fully consumed quiet queue");
    require(fatfish::events_after(state,"a",{{"afterCursor",a0}},10).at("resetRequired").get<bool>(),"an unconsumed pruned event must require a reset");
    require(fatfish::events_after(state,"b",{{"afterCursor",b1}},10).at("resetRequired").get<bool>(),"a pruned busy-queue cursor must require a reset");
    for(int i=0;i<5;++i) {fatfish::record_event(state,"a","session.updated",{{"id","a"},{"step",i}});fatfish::record_event(state,"b","session.updated",{{"id","b"}});}
    const auto first=fatfish::events_after(state,"a",{{"afterCursor",a1}},2);
    require(first.at("events").size()==2 && first.at("hasMore").get<bool>(),"first page must be bounded");
    const auto second=fatfish::events_after(state,"a",{{"afterCursor",first.at("cursor")}},10);
    require(second.at("events").size()==3 && !second.at("hasMore").get<bool>(),"next page must contain the remaining events");
    for(const auto& event:second.at("events")) require(event.at("data").at("id")=="a","events must stay in their own queue");
    require(fatfish::events_after(state,"b",{{"afterCursor",first.at("cursor")}},10).at("resetRequired").get<bool>(),"foreign queue cursors must be rejected");
    bool invalid=false;
    try {fatfish::events_after(state,"a",{{"afterCursor",journal.at("epoch").get<std::string>()+"/a:bad"}},10);} catch(const std::exception&) {invalid=true;}
    require(invalid,"malformed cursor must not silently skip events");
    Json artifact{{"workItemId","W-fixture"},{"logPath","fixture-result.json"},{"messages",Json::array({{{"type","assistant/message"},{"text","Review found a defect"}}})},
        {"delivery",{{"workItemId","W-fixture"},{"candidateSha",std::string(40,'1')},{"baseSha",std::string(40,'0')},{"status","failed"},
            {"issues",Json::array({{{"code","DEFECT"},{"message",std::string(4000,'x')}}})},{"source","agent_reported"},
            {"checksManifest","fatfish://checks/"+std::string(64,'1')},{"evidenceManifest","fatfish://evidence/"+std::string(64,'2')},{"nextAction","inspect_failure"}}}};
    const auto summary=fatfish::handoff_summary(artifact);
    for(const auto* key:{"workItemId","candidateSha","baseSha","status","issues","checksManifest","evidenceManifest","nextAction"})
        require(summary.at("delivery").contains(key),"compact delivery must retain its core fields");
    require(summary.at("delivery").at("issues").at(0).at("code")=="DEFECT","failed summaries must keep a concrete error preview");
    require(summary.dump().size()<3000,"large issue reports must not expand the default handoff");
    std::cout<<"Event retention passed: queue isolation, exact pruning boundary, quiet queue continuity and pagination.\n";
    return 0;
 }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
