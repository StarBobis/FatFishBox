#pragma once
#include "cluster.h"
#include <algorithm>

namespace fatfish {
// Public startup state must describe a real goal, not merely a saved project.
inline Json team_manager_startup(const Json& p,std::int64_t time) {
    const Json* latest=nullptr;
    for (const auto& job:p.at("jobs")) if (job.at("role")=="lead" &&
        (!latest || job.value("created",0LL)>=latest->value("created",0LL))) latest=&job;
    const bool live=p.at("status")=="running" || p.at("status")=="starting";
    const auto goal=latest?latest->value("goal",Json()):Json();
    const bool confirmed=live && latest && !latest->value("sessionId","").empty() &&
        latest->value("state","")=="running" && goal.is_object() && goal.value("phase","")=="active";
    const bool pending=live && (!latest || latest->value("state","")=="queued" || latest->value("state","")=="starting" ||
        (latest->value("state","")=="running" && !confirmed));
    const auto since=latest?latest->value("created",p.value("updated",time)):p.value("updated",time);
    const bool stalled=pending && time-since>=30000;
    const bool ended=live && latest && !pending && !confirmed;
    const auto error=latest?latest->value("error",""):std::string();
    Json out={{"pending",pending},{"confirmed",confirmed},{"stalled",stalled},
        {"needsAttention",live && (stalled || !error.empty() || ended)},
        {"stage",confirmed?"goal_active":pending?(latest?latest->value("state","queued"):"manager_not_queued"):ended?"manager_ended":"inactive"},
        {"since",since},{"elapsedMs",std::max<std::int64_t>(0,time-since)},
        {"reason",!error.empty()?error:stalled?"Manager startup has not established an active goal within 30 seconds; inspect the scheduler and DSH connection.":ended?"Manager session ended before project completion; inspect its goal and pending decisions.":""}};
    if (latest) {
        out["jobId"]=latest->at("jobId");
        out["sessionId"]=latest->value("sessionId","");
        out["retryAt"]=latest->value("retryAt",0LL);
    }
    return out;
}
}
