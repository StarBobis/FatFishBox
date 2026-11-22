#pragma once
#include "cluster.h"
namespace fatfish {
std::string sha256(std::string_view bytes);
Json load_result_artifact(const fs::path& data,const std::string& queue,const std::string& session,const std::string& version);
Json save_result_artifact(const fs::path& data,const std::string& queue,const Json& row,Json report);
Json handoff_summary(const Json& artifact);
Json result_page(const Json& artifact,const Json& input);
}
