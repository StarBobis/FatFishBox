#pragma once
#include "cluster.h"
#include <memory>

namespace fatfish {
inline constexpr std::size_t mcp_max_request_size=1024*1024;
bool mcp_protocol_supported(std::string_view version);
Json dispatch_mcp(const Cluster& cluster, std::string_view line, bool* initialized=nullptr);
void mcp_diagnostic(std::string_view message);
int run_mcp_stdio();
int run_mcp_http();

class McpHttpServer {
public:
    explicit McpHttpServer(const Cluster& cluster);
    ~McpHttpServer();
    void wait();
    std::string url() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
