#pragma once

#include "cluster.h"
#include <functional>
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
    fs::file_time_type history_write{};
};
using LegacyObservations = std::map<std::string,LegacyObservation>;
enum class LegacyRefresh { incremental, handoff, archive };
using LegacyFilter=std::function<bool(std::string_view)>;
struct DesktopMonitor {
    LegacyObservations legacy;
    std::map<std::string,std::int64_t> native_attempts;
};

LegacySessions read_legacy_sessions(const Json& settings,bool include_usage_candidates=false,
                                    LegacyObservations* observations=nullptr,std::stop_token stop={},
                                    LegacyRefresh refresh=LegacyRefresh::incremental,const LegacyFilter& include={});
Json live_legacy_session(const Json& settings,const std::string& id);
Json legacy_result(const Json& settings,const std::string& id,const Json* observed=nullptr);
bool complete_legacy_handoff(const Json& settings, const std::string& id, const std::string& version,
                             const std::string& outcome);
}
