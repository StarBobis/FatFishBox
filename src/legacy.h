#pragma once

#include "cluster.h"
#include <map>

namespace fatfish {
struct LegacySessions {
    Json rows = Json::object();
    Json usage_candidates = Json::object();
    int sources = 0;
    int unreadable = 0;
    int live_checked = 0;
    int live_stale = 0;
};

struct LegacyObservation {
    Json revision;
    Json live;
    std::int64_t checked = 0;
    bool stale = false;
};
using LegacyObservations = std::map<std::string,LegacyObservation>;
struct DesktopMonitor {
    LegacyObservations legacy;
    std::map<std::string,std::int64_t> native_attempts;
};

LegacySessions read_legacy_sessions(const Json& settings,bool include_usage_candidates=false,
                                    LegacyObservations* observations=nullptr,std::stop_token stop={});
Json live_legacy_session(const Json& settings,const std::string& id);
Json legacy_result(const Json& settings, const std::string& id);
void complete_legacy_handoff(const Json& settings, const std::string& id, const std::string& version,
                             const std::string& outcome);
}
