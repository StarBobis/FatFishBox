#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <stop_token>
#include <stdexcept>
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
struct DispatchError : std::runtime_error {
    DispatchError(const std::string& message,Json diagnostics)
        :std::runtime_error(message),details(std::move(diagnostics)) {}
    Json details;
};

class Cluster {
public:
    explicit Cluster(fs::path data = {});
    Json invoke(std::string_view tool, const Json& input = Json::object()) const;
    Json update_model_field(const std::string& id,const std::string& field,
                            const Json& expected,const Json& value) const;
    Json desktop_snapshot(DesktopMonitor* monitor=nullptr,std::stop_token stop={}) const;
    void migrate_repair_queue() const;
    void refresh_usage(std::stop_token stop) const;
    static Json tools();
private:
    Json start_session(const Json& input,const std::string& key,const Json& repair=Json()) const;
    fs::path data_;
};
}
