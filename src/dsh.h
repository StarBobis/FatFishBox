#pragma once
#include "cluster.h"

namespace fatfish {
struct DshConnection {
    fs::path config;
    fs::path auth;
    std::string permission;
    std::string preset;
};

Json read_json(const fs::path& path, std::size_t limit = 1024 * 1024);
DshConnection discover(const Json& settings, const Json& input);

class Dsh {
public:
    explicit Dsh(const fs::path& auth, int timeout_ms=15000);
    Json request(std::string_view method, const Json& args = nullptr) const;
    Json call(std::string_view method, const Json& args) const;
    const std::string& owner() const { return owner_; }
    const std::string& origin() const { return origin_utf8_; }
private:
    std::wstring host_, origin_, cookie_;
    std::string owner_, origin_utf8_;
    int timeout_ms_;
    unsigned short port_{};
    bool https_{};
};
}
