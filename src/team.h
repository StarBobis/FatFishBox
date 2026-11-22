#pragma once
#include "cluster.h"

namespace fatfish {
// Team state is independent of the legacy queue protocol. No network I/O is done
// while its file lock is held. Session reservations still use the cluster DB lock.
Json team_invoke(const Cluster& cluster,const fs::path& data,std::string_view tool,const Json& input);
Json team_tools();
void advance_teams(const Cluster& cluster,const fs::path& data,std::stop_token stop);
bool team_managed_queue(const fs::path& data,std::string_view queue);
void check_team_reservation(const fs::path& data,const Json& sessions,const Json& input);
std::string team_prompt(std::string_view role);
}
