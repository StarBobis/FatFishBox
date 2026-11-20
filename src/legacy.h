#pragma once

#include "cluster.h"

namespace fatfish {
struct LegacySessions {
    Json rows = Json::object();
    int sources = 0;
    int unreadable = 0;
};

LegacySessions read_legacy_sessions(const Json& settings);
Json legacy_result(const Json& settings, const std::string& id);
void complete_legacy_handoff(const Json& settings, const std::string& id, const std::string& version,
                             const std::string& outcome);
}
