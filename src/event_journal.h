#pragma once
#include "cluster.h"
#include <algorithm>

namespace fatfish {
inline Json event_projection(const Json& row) {
    Json value=Json::object();
    for (const auto* key:{"id","title","status","resultVersion","processed","dispatchState","dispatchRetryAt","workItemId","baseSha","cursor","stale"})
        if (row.contains(key)) value[key]=row.at(key);
    if (row.contains("failure")) value["failureCode"]=row.at("failure").value("code","");
    value["repairLinkCount"]=row.value("repairLinks",Json::array()).size()+(row.contains("repairOf") && !row.contains("repairLinks")?1:0);
    return value;
}
inline Json& event_journal(Json& state) {
    if (!state.contains("eventJournal")) state["eventJournal"]={{"epoch",id()},{"sequence",0},{"events",Json::array()},{"observed",Json::object()}};
    auto& journal=state.at("eventJournal");
    if (!journal.contains("prunedThrough")) {
        journal["prunedThrough"]=Json::object();
        journal["legacyRetentionFloor"]=journal.at("events").empty()?std::int64_t(0):journal.at("events").front().at("sequence").get<std::int64_t>()-1;
    }
    return journal;
}
inline void record_event(Json& state,const std::string& queue,const std::string& kind,Json data) {
    auto& journal=event_journal(state);
    const auto seq=journal.at("sequence").get<std::int64_t>()+1;
    journal["sequence"]=seq;
    auto& events=journal.at("events");
    events.push_back({{"sequence",seq},{"queueId",queue},{"type",kind},{"time",now()},{"data",std::move(data)}});
    if (events.size()>4096) {
        const auto count=events.size()-4096;
        for (std::size_t index=0;index<count;++index) {
            const auto& old=events.at(index);
            journal["prunedThrough"][old.at("queueId").get<std::string>()]=old.at("sequence");
        }
        events.erase(events.begin(),events.begin()+static_cast<std::ptrdiff_t>(count));
    }
}
inline void record_session_changes(Json& state,const Json& previous) {
    const bool first=!state.contains("eventJournal");
    event_journal(state);
    const auto legacy=state.value("legacyQueueId","");
    for (const auto& row:state.at("sessions")) {
        const auto key=row.at("id").get<std::string>();
        const auto projection=event_projection(row);
        if (!first && previous.contains(key) && event_projection(previous.at(key))==projection) continue;
        record_event(state,row.value("queueId",legacy),previous.contains(key)?"session.updated":"session.created",projection);
    }
}
inline bool record_external_observations(Json& state,const std::string& queue,const Json& rows) {
    bool changed=false;
    for (const auto& row:rows) {
        const auto key=queue+":"+row.at("id").get<std::string>();
        const auto projection=event_projection(row);
        auto& observed=event_journal(state).at("observed");
        if (observed.contains(key) && observed.at(key)==projection) continue;
        observed[key]=projection;
        record_event(state,queue,"session.observed",projection);
        changed=true;
    }
    return changed;
}
inline std::string event_cursor(const Json& journal,const std::string& queue,std::int64_t sequence) {
    return journal.at("epoch").get<std::string>()+"/"+queue+":"+std::to_string(sequence);
}
inline Json events_after(const Json& state,const std::string& queue,const Json& target,int limit) {
    const auto& journal=state.at("eventJournal");
    const auto latest=journal.at("sequence").get<std::int64_t>();
    const auto after=target.value("afterCursor","");
    const auto& events=journal.at("events");
    const auto oldest=events.empty()?latest+1:events.front().at("sequence").get<std::int64_t>();
    const auto prefix=journal.at("epoch").get<std::string>()+"/"+queue+":";
    std::int64_t sequence=latest;
    bool reset=false;
    if (!after.empty()) {
        if (!after.starts_with(prefix)) reset=true;
        else {
            const auto digits=after.substr(prefix.size());
            if (digits.empty() || digits.size()>18 || digits.find_first_not_of("0123456789")!=digits.npos)
                throw std::runtime_error("afterCursor 无效，请原样使用返回的 cursor");
            sequence=std::stoll(digits);
            const auto legacy_floor=journal.value("legacyRetentionFloor",oldest-1);
            const auto pruned=journal.value("prunedThrough",Json::object()).value(queue,std::int64_t(0));
            reset=sequence<legacy_floor || sequence<pruned || sequence>latest;
        }
    }
    Json result{{"queueId",queue},{"events",Json::array()},{"cursor",event_cursor(journal,queue,latest)},
        {"resetRequired",reset},{"hasMore",false},{"initial",after.empty()}};
    if (reset || after.empty()) return result;
    auto through=latest;
    for (const auto& event:events) {
        if (event.at("sequence").get<std::int64_t>()<=sequence || event.at("queueId")!=queue) continue;
        if (result.at("events").size()>=static_cast<std::size_t>(limit)) {
            result["hasMore"]=true;
            break;
        }
        auto compact=event;
        compact.erase("queueId");
        result["events"].push_back(std::move(compact));
        through=event.at("sequence").get<std::int64_t>();
    }
    if (result.at("hasMore").get<bool>()) result["cursor"]=event_cursor(journal,queue,through);
    return result;
}
}
