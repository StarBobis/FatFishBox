#pragma once
#include "cluster.h"
#include <memory>

namespace fatfish {
inline constexpr std::size_t mcp_max_request_size=1024*1024;
inline constexpr char mcp_handoff_instructions[]=
    "Read team_get_prompt(principal). The principal commits a complete executable plan, splits independent tasks within capacity, and starts one project manager with a real persistent goal. "
    "The principal must wait through team_wait_decisions (up to 300000 ms), handle early feedback immediately, and read team_get_project. Never sleep or consume the development queue. "
    "The manager dispatches all ready planned work with team_dispatch_ready and handles team_next_action FIFO. Developers build/test first and report successful command/exitCode results (log attachments are advisory) before independent review. Only after review passes does the manager use team_verify_candidate and accept with a passing verificationId. Repairs require fresh review. "
    "The manager waits exclusively through team_wait_actions (up to 300000 ms); an empty queue with active workers or decisions is not completion. When every planned item is accepted, team_finish_domain and complete the goal. The principal then independently accepts and actually merges main before team_record_merge, or supplies a complete replacement plan through team_update_charter. "
    "Use team_retire_work_item for obsolete terminal work instead of fake repairs. Evidence attachments are advisory; managers can collect/correct declared or in-worktree files with team_capture_evidence. Verdict/SHA and final verification gates remain mandatory. domainId/token identify one manager, not multiple domains. Preserve existing sessions and exact SHA/resultVersion bindings. When the user only pastes connection instructions, wait for a task. ";

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
