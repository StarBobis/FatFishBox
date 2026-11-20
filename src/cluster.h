#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <stop_token>
#include <nlohmann/json.hpp>

namespace fatfish {
using Json = nlohmann::json;
namespace fs = std::filesystem;

std::wstring widen(std::string_view value);
std::string narrow(std::wstring_view value);
std::string utf8(const fs::path& path);
inline fs::path path_from_utf8(std::string_view value) { return fs::path(widen(value)); }
std::string id();
std::int64_t now();
struct DesktopMonitor;

class Cluster {
public:
    explicit Cluster(fs::path data = {});
    Json invoke(std::string_view tool, const Json& input = Json::object()) const;
    Json update_model_field(const std::string& id,const std::string& field,
                            const Json& expected,const Json& value) const;
    Json desktop_snapshot(DesktopMonitor* monitor=nullptr,std::stop_token stop={}) const;
    void refresh_legacy_usage(std::stop_token stop) const;
    const fs::path& data_directory() const { return data_; }
    static Json tools();
private:
    fs::path data_;
};
}
