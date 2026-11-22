#pragma once
#include "cluster.h"
namespace fatfish {
Json capture_evidence(const fs::path& data,const std::string& queue,const Json& row,const Json& delivery,const Json& options=Json::object());
Json read_evidence_manifest(const fs::path& data,const std::string& queue,const std::string& reference);
Json evidence_manifest_page(const fs::path& data,const Json& manifest,const Json& input);
Json attach_result_evidence(const fs::path& data,const std::string& queue,const Json& row,Json report);
void remember_result_evidence(const fs::path& data,const std::string& queue,const Json& row,const Json& summary);
}
