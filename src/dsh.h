#pragma once
#include "cluster.h"
#include <optional>

namespace fatfish {
inline constexpr std::string_view supported_dsh_version="0.2.2-rc.5";

struct DshError : std::runtime_error {
    DshError(std::string message,std::string error_code,std::string phase={})
        :std::runtime_error(std::move(message)),code(std::move(error_code)),stage(std::move(phase)) {}
    std::string code,stage;
};

Json dsh_error_diagnostics(const DshError& error);

struct DshConnection {
    fs::path config;
    fs::path auth;
    std::string permission;
    std::string preset;
};

Json read_json(const fs::path& path, std::size_t limit = 1024 * 1024);
DshConnection discover(const Json& settings, const Json& input);
Json discover_dsh_connections(const Json& hints);
fs::path extract_dsh_desktop_plugin();
void authorize_dsh_desktop();
fs::path config_relative_path(const fs::path& config, std::string_view value);
bool recover_desktop_dsh(const Json& settings,std::stop_token stop={},bool wait_for_restart=true);

class Dsh {
public:
    explicit Dsh(const fs::path& auth,int timeout_ms=15000,int total_timeout_ms=0);
    Json request(std::string_view method, const Json& args = nullptr) const;
    Json call(std::string_view method, const Json& args) const;
    void probe() const;
    std::string runtime_version() const;
    const std::string& owner() const { return owner_; }
    const std::string& origin() const { return origin_utf8_; }
    unsigned long desktop_pid() const { return desktop_pid_; }
    bool matches_owner(std::string_view owner) const;
    bool matches_desktop_origin(std::string_view origin) const;
private:
    void load_desktop_history();
    fs::path auth_path_;
    Json desktop_history_;
    std::wstring host_, origin_, cookie_;
    std::string owner_, origin_utf8_, recorded_version_;
    int timeout_ms_;
    std::uint64_t deadline_{};
    unsigned long desktop_pid_{};
    unsigned short port_{};
    bool https_{};
};

struct TurnEnd {
    std::string status;
    std::int64_t seq;
    std::int64_t time;
    Json failure;
};
Json failure_details(const Json& reason);
bool has_queued_prompt(const Json& state, std::string_view request_id = {});
std::optional<TurnEnd> latest_turn_end(const Dsh& dsh, const Json& session_id,
                                      std::int64_t start, std::int64_t cursor);
}
