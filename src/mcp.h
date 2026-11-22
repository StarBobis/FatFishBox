#pragma once
#include "cluster.h"
#include <memory>

namespace fatfish {
inline constexpr std::size_t mcp_max_request_size=1024*1024;
inline constexpr char mcp_handoff_instructions[]=
    "Read team_get_prompt(principal). Plan only the requested work, start one persistent-goal manager, and wait through team_wait_decisions (up to 300000 ms). "
    "The manager queues all ready tasks with team_dispatch_ready and handles team_next_action FIFO. Developers perform relevant self-tests; independent review follows; the manager decides acceptance. team_verify_candidate is optional, never a prerequisite. "
    "No proof collection, manifests, hashes, screenshot archives, exhaustive page/configuration/frame audits, verificationId or acceptance-proof checklist is required. Never enlarge a local fix into full-product auditing. Stop extra proof-only capture requested by older checklists, preserving actual development and sessions. Report what actually ran. "
    "Wait only through team_wait_actions/ team_wait_decisions, never sleep. After all work is confirmed, team_finish_domain with summary, then complete the goal. The principal integrates main through team_record_merge or gives a clear repair plan. When the user only pastes connection instructions, wait for a task.";

bool mcp_protocol_supported(std::string_view version);
Json dispatch_mcp(const Cluster& cluster, std::string_view line, bool* initialized=nullptr,bool principal_only=false);
void mcp_diagnostic(std::string_view message);
int run_mcp_stdio(bool principal_only=false);
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
