#pragma once
#include "cluster.h"
#include <algorithm>
#include <vector>
namespace fatfish {
// One arrival-ordered dispatcher. Capacity is enforced atomically at launch.
inline std::vector<Json> team_dispatch_plan(const Json& projects,const Json&,std::int64_t time,std::size_t budget=16) {
    std::vector<Json> ready;
    for (const auto& p:projects) {
        if (p.at("status")!="running") continue;
        for (const auto& j:p.at("jobs"))
            if (!j.value("historical",false) && (j.at("state")=="queued" || j.at("state")=="starting") && j.value("retryAt",0LL)<=time)
                ready.push_back({{"projectId",p.at("projectId")},{"jobId",j.at("jobId")},{"created",j.value("created",0LL)}});
    }
    std::sort(ready.begin(),ready.end(),[](const Json& a,const Json& b) {
        return a.at("created")==b.at("created")?a.at("jobId")<b.at("jobId"):a.at("created")<b.at("created");
    });
    if (ready.size()>budget) ready.resize(budget);
    return ready;
}
}
