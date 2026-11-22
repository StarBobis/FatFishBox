#pragma once
#include "cluster.h"
#include <algorithm>
#include <deque>
#include <map>
#include <vector>

namespace fatfish {
inline int team_role_priority(const Json& job) {
    const auto role=job.at("role").get<std::string>();
    return role=="reviewer"?0:role=="lead"?1:role=="integrator"?2:3;
}

// Pure planning: capacity and authorization are still checked atomically by
// the dispatcher. Persist the served project's cursor before each attempt.
inline std::vector<Json> team_dispatch_plan(const Json& projects,const Json& cursors,
                                          std::int64_t time,std::size_t budget=16) {
    std::map<int,std::map<std::string,std::deque<Json>>> groups;
    for (const auto& project:projects) {
        if (project.at("status")!="running") continue;
        std::vector<Json> jobs;
        for (const auto& job:project.at("jobs"))
            if (!job.value("historical",false) && (job.at("state")=="queued" || job.at("state")=="starting") && job.value("retryAt",0LL)<=time)
                jobs.push_back(job);
        std::sort(jobs.begin(),jobs.end(),[](const Json& a,const Json& b) {
            const auto at=a.value("created",0LL),bt=b.value("created",0LL);
            return at==bt?a.at("jobId")<b.at("jobId"):at<bt;
        });
        for (const auto& job:jobs) {
            const int rank=team_role_priority(job);
            const auto pid=project.at("projectId").get<std::string>();
            groups[rank][pid].push_back({{"projectId",pid},{"jobId",job.at("jobId")},{"priority",std::to_string(rank)}});
        }
    }
    std::vector<Json> plan;
    for (auto& [rank,projects_at_rank]:groups) {
        std::deque<std::string> rotation;
        auto start=projects_at_rank.upper_bound(cursors.value(std::to_string(rank),std::string()));
        for (auto i=start;i!=projects_at_rank.end();++i) rotation.push_back(i->first);
        for (auto i=projects_at_rank.begin();i!=start;++i) rotation.push_back(i->first);
        while (!rotation.empty() && plan.size()<budget) {
            auto pid=rotation.front();rotation.pop_front();
            auto& jobs=projects_at_rank.at(pid);
            plan.push_back(jobs.front());jobs.pop_front();
            if (!jobs.empty()) rotation.push_back(std::move(pid));
        }
        if (plan.size()>=budget) break;
    }
    return plan;
}
}
