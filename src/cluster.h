#pragma once

#include <filesystem>
#include <memory>
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
struct HandoffObservations;
struct DispatchError : std::runtime_error {
    DispatchError(const std::string& message,Json diagnostics)
        :std::runtime_error(message),details(std::move(diagnostics)) {}
    Json details;
};

class Cluster {
public:
    explicit Cluster(fs::path data = {});
    Cluster for_queue(std::string_view queue_id) const;
    Json invoke(std::string_view tool, const Json& input = Json::object()) const;
    Json invoke_mcp(std::string_view tool,const Json& input) const;
    Json update_model_field(const std::string& id,const std::string& field,
                            const Json& expected,const Json& value) const;
    Json team_session_reservations(std::string_view project_id) const;
    Json desktop_snapshot(DesktopMonitor* monitor=nullptr,std::stop_token stop={}) const;
    Json mark_desktop_failed(const std::string& session_id) const;
    void delete_desktop_queue(std::string_view queue_id) const;
    void migrate_repair_queue() const;
    void refresh_usage(std::stop_token stop) const;
    void run_prechecks(std::stop_token stop) const;
    static Json tools();
private:
    Json invoke_impl(std::string_view tool,const Json& input,bool internal_read) const;
    void advance_prechecks(std::stop_token stop) const;
    Json start_session(const Json& input,const std::string& key,const Json& repair=Json(),const Json& operation=Json()) const;
    fs::path handoff_file(bool lock=false) const;
    void require_open_queue() const;
    fs::path data_;
    std::string legacy_queue_id_,queue_id_;
    std::shared_ptr<HandoffObservations> handoff_observations_;
};
}
