#pragma once
#include "cluster.h"
#include <memory>

namespace fatfish {
inline constexpr std::size_t mcp_max_request_size=1024*1024;
inline constexpr char mcp_handoff_instructions[]=
    "两层循环：先读team_get_prompt(principal)。主代理拆分不超过可用并发数的独立任务，写逐步计划与验收门禁，启动唯一设有goal的项目经理。"
    "主代理每次等待60秒后读team_get_project，running继续等；不处理日常开发队列。"
    "项目经理先team_dispatch_ready派发主代理计划中全部就绪事项，再循环team_next_action：开发完成派独立审查，审查通过亲自team_verify_candidate运行候选分支测试，通过才accept；失败修复后重新审查测试。"
    "空队列仍有在途不可完成。全部验收后team_finish_domain并完成goal。主代理再逐项验收、合入main并team_record_merge；不通过则team_update_charter提交新的完整计划返工。"
    "工具的domainId/token仅用于经理身份兼容，不代表多领域。开发在自动创建的隔离分支执行。用户仅粘贴接入说明时等待任务。";

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
