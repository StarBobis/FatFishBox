#pragma once
#include "cluster.h"
#include <memory>

namespace fatfish {
inline constexpr std::size_t mcp_max_request_size=1024*1024;
inline constexpr char mcp_handoff_instructions[]=
    "团队执行模式：用户一次给出任务和授权后，主代理先 team_get_prompt({role:principal})，再 team_get_queue、team_start_project 启动/续接团队并核实负责人实际启动，持续跟进到最终验收，不能只接入待命或反复要求用户指示。direct 和历史工作项不禁止团队接管。主代理只用 team_* 授权和决策，负责人派开发审查返修。"
    "领域负责人必须先 team_domain_context，使用带领域凭据的 team_* 执行；以下 cluster_* 规则仅用于旧版直接管理模式，不覆盖团队角色职责。"
    "负责人岗位持续消费队列积压，不能做完一项就退出；每批逐条核对后 state=continue 自动接续，单项在途不阻塞其余交接，不以占满并发为目标。"
    "负责人通过 team_next_action 机械循环处理统一FIFO，子任务结果排队而不抢占；处理本条后继续下一条，不能因一项等待停止整个循环。"
    "用户只粘贴接入说明时，简短确认接入状态，等用户给出任务、指示派发或要求续接。"
    "首次使用先读取一次 fatfish://workflow，公共派发、领取、审查和返修规则均在该资源。"
    "按任务是否写入文件选择工作区：只读分析、日志查询和不写文件的审查可共用目录；"
    "并行修改代码、返修或执行会写入文件的构建测试时，主代理须先为各会话创建独立 Git worktree，"
    "核对路径、分支和基线后再传入 workspace 派发，不得让并行写入共享主工作区或 main。"
    "单会话修改可按任务选择现有目录；main 的修改或集成须符合用户授权。非 Git 任务按需使用独立目录。"
    "每个开发或返修子会话一旦交付候选，主代理应及时为该会话的具体候选 SHA 单独派发 role: reviewer 的独立对抗审查，不得等整批开发结束后只安排一次总审。审查任务须明确原 sessionId、workItemId、候选 SHA 和验收条件；每条结果分别核对审查结论，审查通过后才能验收该结果。返修产生新候选 SHA 后必须重新独立审查，旧版本的通过结论不沿用。整批最终审查可追加，但不能替代逐项审查。"
    "派发接口不会自动创建 worktree。为当前工作使用同一个 queueId；操作须在用户授权范围内。默认交接为摘要，完整日志按需读取。";
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
