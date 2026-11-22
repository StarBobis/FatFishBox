#include "diagnostics.h"
#include "mcp.h"
#include "team.h"
#include "dsh.h"
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <cstdio>
#include <memory>
#include <semaphore>
#include <thread>
#include <array>
#include <set>

namespace {
using fatfish::Json;
using File = std::unique_ptr<FILE, decltype(&std::fclose)>;
constexpr auto workflow=R"workflow(# 肥鱼重工 · 主代理工作流

## 团队执行优先入口
用户给出任务并要求团队执行、全程自主或一次委托时，先读取 team_get_prompt({role:"principal"}) 并执行团队规程。主代理通过 team_get_queue/team_start_project 启动或续接团队，后台创建领域负责人，负责人管理开发、审查、返修；主代理只做授权、关键决定和最终验收。历史队列和 direct 模式都可以接管，禁止以历史工作项为由要求另建空队列或退回主代理亲自管理集群。已给出任务时不要再问用户“继续返修还是新建工作”。下文原始 cluster_* 流程仅适用于明确选择旧版直接管理的工作，在团队模式下由负责人对应的带凭据工具承担。

首次接入时，队列可能为空。用户只粘贴接入说明时，简短确认接入状态，等待用户给出任务、指示派发或要求续接。收到任务和派发授权后，先确定工作范围、工作区和验收条件，再派发子会话。领取返回 `empty: true` 且队列无进行中任务时，结束本次检查。

当前工作仍在进行、但尚无交接时，可调用 `cluster_wait_handoff({queueId})` 挂起等待。返回 `ready: true` 后调用 `cluster_next_handoff({queueId})` 领取。超时返回 `timedOut: true`，如果仍需等待可再次调用。

领取凭据遗失、调用响应丢失或恢复聊天后，调用 `cluster_resume_handoff({queueId})` 取回原领取凭据、到期时间和交接摘要。它不续期。返回 `resumed: false, reason: "no_active_claim"` 时重新调用 `cluster_next_handoff` 领取。收到 `busy: true` 时先恢复领取。

若客户端工具目录缺少 `cluster_wait_handoff`，重新连接 fatfishbox MCP 并刷新工具列表。旧版 `cluster_retry_review` 已移除。历史 `blocked` 状态须结合已有 reviewer 结果核对，主代理据此决定是否派复审。

用户已授权使用子会话执行当前批次时，范围内的验收返修沿用该授权，无需逐次请示。主代理负责核对证据和验收，发现缺陷就派给子会话返修。在用户授权范围内派发新批次，超出范围时先请示。

先确定工作的 `queueId`。桌面「复制接入说明」给出当前队列的 UUID。续接时可用 `cluster_find_queues({clientKey})` 查询已绑定的队列，也可按 `workKey` 或 `workspace` 查找。工作区匹配到多个队列时，核对归属后再继续。独立工作尚无队列时，用 `cluster_create_queue({name,requestId,workKey,workspace})` 创建。若客户端有稳定会话标识，用 `cluster_bind_queue({queueId,clientKey})` 保存下次续接所需的绑定。

每个开发或返修子会话一旦交付候选，主代理应及时为该会话的具体候选 SHA 单独派发 role: reviewer 的独立对抗审查，不得等整批开发结束后只安排一次总审。审查任务须明确原 sessionId、workItemId、候选 SHA 和验收条件；每条结果分别核对审查结论，审查通过后才能验收该结果。返修产生新候选 SHA 后必须重新独立审查，旧版本的通过结论不沿用。整批最终审查可追加，但不能替代逐项审查。

开发完成后先交接给主代理。主代理读取交付和证据后，使用 cluster_start_session 并设置 role 为 reviewer，主动派发独立对抗审查，任务中明确候选 SHA、工作区和验收条件。审查受阻、失败或发现缺陷时，审查状态和完整内容仍交接给主代理，由主代理安排返修和复审。直到审查无问题，主代理才进行最终验收并提交 outcome。

以下示例以「第 15 轮透明度蒙版」为例。将路径、池 ID 和提交哈希替换成实际值。

派发前，主代理必须按任务是否写入文件选择工作区，而不是一律创建 worktree：
- 只读分析、日志查询、不写文件的审查可共用现有目录。若工具会生成缓存、构建产物或测试文件，应按写入任务处理。
- 并行修改代码、返修或执行会写入文件的构建测试时，主代理须先为各会话创建独立 Git worktree 和各自的构建输出目录，核对绝对路径、分支和基线 SHA，再将该路径作为 workspace 派发。不得让并行写入共享主工作区或 main；不得仅在任务中要求子会话自行隔离后就派发到主目录。
- 单会话修改可按任务选择现有目录，无需无条件创建 worktree；main 的修改或集成须符合用户授权。非 Git 任务按需要准备独立目录。
- cluster_start_session 和 cluster_repair_handoff 只使用传入的现有 workspace，不会自动创建 worktree。子会话只在分配目录内执行任务，交付候选后由主代理按授权验收、集成。

1. 主代理确定本轮基线和验收条件。需要隔离写入时，在主仓库核对 HEAD 后，从已核对的提交创建独立工作树，例如 `git worktree add -b codex/mask-b15 C:/Dev/worktrees/mask-b15 <基线 SHA>`。为每个并行写入的子会话准备独立工作树。
2. 用 `cluster_get_settings` 查询已启用模型的池 ID，然后调用 `cluster_start_session`。例如 `{"queueId":"<队列 UUID>","requestId":"mask-b15-implementation","batch":"mask-b15","title":"实现透明度蒙版","workspace":"C:/Dev/worktrees/mask-b15","allowedPoolIds":["<GitHub 池 ID>","<Outlook 池 ID>"],"task":"实现蒙版并运行专项测试。提交候选，保存截图，报告实际命令与退出码。"}`。批次首次设置允许池后，后续派发及返修沿用该清单。省略 `poolId` 可在范围内自动换池。若返回派发诊断，检查 `pools[].reason`、`occupied`、`concurrency` 和 `availablePools`。超时或返回 `dispatchState: "unconfirmed"` 时，复用原参数和 `requestId` 让工具核对原派发。若返回 `dsh_recovering`，等待 `retryAfterMs` 后用原参数重试。
3. 子会话负责实现、自测和证据，最终回复包含 `fatfish-delivery` 围栏里的 JSON，例如：

```fatfish-delivery
{"candidateSha":"<完整 Git SHA>","worktreeClean":true,"tests":[{"command":"ctest -C Release -R mask","exitCode":0}],"evidencePaths":["C:/Dev/worktrees/mask-b15/build/mask.png"]}
```

4. 主代理调用 `cluster_next_handoff({queueId})` 领队首，保存 `queueId`、`claimToken` 和 `session.resultVersion`。核对 `delivery` 中自报的提交、工作树、测试和证据。主代理在候选上跑累计构建和回归，亲自打开实窗检查布局、交互以及截图覆盖不到的行为。
5. 发现问题时，从已核对的候选 SHA 建返修工作树，例如 `git worktree add -b codex/mask-b15-repair C:/Dev/worktrees/mask-b15-repair <候选 SHA>`。调用 `cluster_repair_handoff`，传入 `queueId`、`claimToken`、原 `session.resultVersion` 作为 `originalVersion`、稳定的 `requestId`、`candidateSha`、`screenshotPaths`、`acceptance`、`title`、返修 `workspace` 和 `task`。例如验收条件可以写「在目标缩放下打开实窗，标题与按钮仍可读，累计构建和蒙版回归通过」。超时后用完全相同的参数重试，工具会核对原派发。未确认时原领取继续有效。
6. 用 `cluster_get_session_status({queueId,sessionId})` 核对已有返修的标题和关联。确认返修与原交接同队列、同批次且覆盖同一缺陷后，调用 `cluster_attach_handoff_repair({queueId,claimToken,originalVersion,sessionId})`。开发交接和独立评审若依赖同一份返修，可用各自领取凭据关联同一 sessionId。派发或绑定返回 `claimReleased: true` 后，立即在同一队列领取其他结果。
7. 返修完成后只排一次队。轮到它时，`cluster_next_handoff({queueId})` 返回新领取凭据、`session.repairLinks` 和 `delivery`。主代理对照所有关联版本做累计回归和实窗验收，用新凭据提交验收结果。首次关联已完成返修时从关联时刻开始排队。
8. 核对时发现误挂，在确认返修前调用 `cluster_detach_handoff_repair({queueId,sessionId,originalSessionId,originalVersion})`。`sessionId` 是返修会话 ID，后两项来自 `repairLinks` 里误挂的同一条记录。返修已领取时传入该返修的当前 `claimToken`，并按返回的剩余关联验收。解绑的原交接若仍待处理，会按原队列顺序恢复。

## 精简交接与增量等待

工具目录只说明用途和特例。本资源首次接入读取一次，后续无需重复读取。

`cluster_next_handoff` 和 `cluster_resume_handoff` 默认返回结构化 `delivery`、最终回复的短摘要及 `logPath`。
交付和原始结果按队列、会话和 `resultVersion` 持久保存。交付缺失时 `delivery.status` 为 `missing`，不会猜测通过。
需要细节时调用 `cluster_get_session_result({queueId,sessionId,resultVersion,offset:0,limit:6000})`，
用 `nextOffset` 分页读取；不要默认拉取完整日志。`truncated` 表示源读取受到上限限制。
子会话的 `fatfish-delivery` 可包含 `workItemId`（服务端归属为准）、`candidateSha`、`baseSha`、
`status`（passed/failed/blocked）、`issues`、`checksManifest`、`evidenceManifest`、`nextAction`，
兼容原有 `worktreeClean`、`tests` 和 `evidencePaths`。这些是代理自报，主代理仍需核验证据后验收。旧格式未提供 status 时为 reported，缺少 nextAction 时为 inspect_result；缺失基线使用登记值或 null。

用 `cluster_wait_events({targets:[{queueId}],timeoutMs:0})` 建立游标，返回当前活动/待处理摘要及计数。
随后携带各队列原样返回的 `cursor` 作为 `afterCursor`，一次最多等待八个队列：
`cluster_wait_events({targets:[{queueId,afterCursor}],timeoutMs:25000})`。
返回的 `events` 仅含游标之后的状态变化，不包含会话历史或完整返修关联；更新所有返回队列的游标。
有 `hasMore` 时继续用返回游标读取下一页。游标跨进程重启有效；本队列存在未消费且已清理的事件或游标来自其他数据目录时，
`resetRequired:true` 并返回新基线，按当前摘要重新核对。等待不领取结果，交接仍用原领取工具。


## 任务总账与增量返修

独立事项可先用 `cluster_create_work_item({queueId,title,requestId,task,baseSha})` 创建。
`cluster_start_session` 传 `workItemId` 和实际 `baseSha`，开发和独立 reviewer 复用同一事项；省略时自动创建事项。
返修自动沿用原事项。共享返修会同时出现在各原事项的会话列表，不重复计算事项。
`cluster_list_work_items` 返回全队列计数及分页摘要，`counts.remaining` 包括已验收但尚未登记集成的事项。
旧记录无法证明合入 main，`migratedNeedsReconciliation` 单独计数，不能当作已完成或直接推断仍需实现。
总账仅登记已核对的集成提交，不执行 git merge，也不把处理回执当成最终验收。

`cluster_get_work_item({queueId,workItemId})` 返回最新 `revision` 和 `taskRef`，仅 `includeTask:true` 返回原任务文本。
`taskRef` 是本机只读规格元数据，包含原任务、基线及稳定缺陷 ID；代码修改仍限于分配的工作区。
`cluster_update_work_item` 传 `workItemId,expectedRevision,requestId,action,details`，每次新的操作使用新的 requestId。
响应丢失时用完全相同的参数重试；revision 冲突时重读、核对变化后用新 requestId 更新。

- `add_issue`：details 为 `message,acceptance`，可带 `candidateSha`，返回稳定 `issueId`。
- `resolve_issue`：`issueId,notes`，记录实际修复验证。
- `attach_session`：`sessionId`，关联已有会话；外部 Codex 会话可明确记录 `role`。
- `record_review`：`sessionId,resultVersion,candidateSha,verdict,notes`。verdict 为 passed/failed/blocked，
  由主代理读取并核实独立 reviewer 的原版本后登记，不从“执行成功”自动推断审查通过。
- `accept`：`candidateSha,notes`。须无活动会话和未解决缺陷，且该候选有已登记通过的独立审查。
  审查/开发结果或关联会话发生变化后，旧验收会失效，必须重新核对。
- `integrate`：`integrationSha,notes`，可带 targetBranch（默认 main）；须先完成 accept。
- `reopen`、`block`、`cancel`：`notes`。cancel 不取消远端会话，活动会话结束或取消后才能关闭事项。

返修可仅传 `issueIds` 和新增要求，不必重复原任务：
`cluster_repair_handoff({queueId,claimToken,originalVersion,requestId,candidateSha,acceptance,title,workspace,issueIds})`。
只有旧事项没有保存原任务时才必须补 `task`。原任务和缺陷定义通过 taskRef 引用，仍保留原领取、返修版本与去重语义。


## 运行中追加消息

`cluster_send_message({queueId,sessionId,requestId,message,baseSha,issueIds,mode})` 用于本机派发的活动 DSH 会话。
提供 message、baseSha 或 issueIds 至少一项；baseSha 是补充的新基线信息，不会偷偷修改原任务或重置工作树。
默认 steer 在下一 step 边界读取，不打断正在执行的模型/工具调用；queue 等下一 turn。
正文携带稳定事项和只读原任务引用，既有工作区、模型及授权范围保持不变。

FatFishBox 在发送前持久保存正文与原参数，独立消息 ID 与初次派发分开。严格复用原参数和 requestId，
Host 的 requestId 去重不比较正文，不能自行用同一 ID 更改内容。
accepted:true 只说明 DSH 已持久入队，executionConfirmed:false 不表示执行成功。
用 `cluster_get_message({queueId,sessionId,requestId})` 查询；只有 includeMessage:true 才返回原消息内容。
HTTP 502/超时等可重试错误会返回 reason、stage、retryable、retryAfterMs，并由后台复用原 ID 恢复。

发送前会检查活动状态。DSH 协议没有原子的“仅运行中接收”前提，因此预检与接收之间仍有结束竞态；
steer 在 idle 时可能开启下一轮。重试时若发现会话已空闲，首次未发送的消息为 not_sent，
曾经提交但响应不明的为 unconfirmed_terminal（needsInspection:true），停止自动重发，先核对结果再决定后续工作。
手动标记会话失败也停止未确认消息的自动重试。每会话最多保存 128 条追加消息，正文上限 16000 字节。

## 自动证据清单

领取时在日志截短之外自动生成 `delivery.checksManifest` 与 `delivery.evidenceManifest`，默认只返回引用和计数。
清单采集 Git HEAD/工作树状态、CMakeCache 中可观察的配置、EXE/截图/日志的 SHA256 和原始路径，
复制实际文件字节到按哈希保存的本机对象；未变化的内容复用同一对象，不重复附入交接。
自动采集仅限已分配工作区内的报告路径及常见构建配置/测试日志，缺失或越界文件会列出 unavailable，不能当成证据已存在。

主代理显式采集共享构建目录等其他授权产物时调用：
`cluster_capture_evidence({queueId,sessionId,resultVersion,candidateSha,configuration,files:[{path,kind}],tests:[{command,exitCode,logPath}]})`。
kind 为 screenshot/executable/test_log/build_config/other。configuration 为调用者声明，CMake 配置来自实际保存的缓存文件。
单文件及每次总量上限均为 512 MiB，最多 128 个文件。不会执行测试，也不会打开截图来推断画面是否正确。

`cluster_get_evidence_manifest({queueId,manifestId,offset,limit})` 按引用分页读取。它校验清单内容哈希与队列归属，
并验证本页文件对象的字节哈希；blobVerified:false 代表对象丢失或损坏，不能当作有效证据。
测试退出码保留 agent_reported/caller_reported 来源，testExecutionVerified:false；附带的日志另存真实字节哈希。
采集时 HEAD 与某个既有 EXE 的同次哈希不证明该 EXE 由该 HEAD 构建，sourceBinding 固定为 capture_time_only，
matchesExpectedCandidate、worktreeClean 和 sourceChangedDuringCapture 帮助主代理识别不一致。

记录 record_review、accept 或 integrate 时可在 details 带 checksManifest/evidenceManifest，
服务端校验事项会话归属和声明候选 SHA，保存精确引用。清单不替代主代理对构建、测试与截图来源的核验。

)workflow";

File open_stream(DWORD standard_handle, const char* mode, int flags) {
    HANDLE handle = GetStdHandle(standard_handle);
    HANDLE duplicate{};
    if (!handle || handle == INVALID_HANDLE_VALUE ||
        !DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(), &duplicate, 0,
                         FALSE, DUPLICATE_SAME_ACCESS)) return File(nullptr, &std::fclose);
    const int descriptor = _open_osfhandle(reinterpret_cast<intptr_t>(duplicate), flags | _O_BINARY);
    if (descriptor < 0) {
        CloseHandle(duplicate);
        return File(nullptr, &std::fclose);
    }
    auto* stream = _fdopen(descriptor, mode);
    if (!stream) _close(descriptor);
    return File(stream, &std::fclose);
}

bool read_request(FILE* input, std::string& line) {
    line.clear();
    for (int ch; (ch = std::fgetc(input)) != EOF;) {
        if (ch == '\n') return true;
        if (line.size() <= fatfish::mcp_max_request_size) line.push_back(static_cast<char>(ch));
    }
    return !line.empty();
}

Json failure(const Json& id, int code, std::string message) {
    return {{"jsonrpc","2.0"},{"id",id},{"error",{{"code",code},{"message",message}}}};
}

Json model_visible_payload(const Json& data) {
    return {{"type","text"},{"text",data.dump(-1,' ',false,Json::error_handler_t::replace)}};
}
Json invoke_tool(const fatfish::Cluster& cluster,std::string_view tool,const Json& arguments) {
    if (tool!="cluster_wait_handoff" && tool!="cluster_wait_events" && tool!="team_wait_decisions") return cluster.invoke_mcp(tool,arguments);
    // Leave HTTP workers available for ordinary requests.
    static std::counting_semaphore<2> slots{2};
    if (!slots.try_acquire()) throw fatfish::DispatchError("等待调用已满，请稍后重试",
        {{"reason","wait_capacity"},{"retryAfterMs",1000}});
    struct ReleaseSlot { ~ReleaseSlot() { slots.release(); } } release;
    return cluster.invoke_mcp(tool,arguments);
}
}

bool fatfish::mcp_protocol_supported(std::string_view version) {
    constexpr std::array<std::string_view,4> versions{"2024-11-05","2025-03-26","2025-06-18","2025-11-25"};
    return std::find(versions.begin(),versions.end(),version)!=versions.end();
}

fatfish::Json fatfish::dispatch_mcp(const Cluster& cluster,std::string_view line,bool* initialized,bool principal_only) {
    Json request_id=nullptr;
    try {
        if (line.size()>mcp_max_request_size) return failure(nullptr,-32600,"Request too large");
        const auto message=Json::parse(line);
        if (!message.is_object() || !message.contains("jsonrpc") || message.at("jsonrpc")!="2.0")
            return failure(nullptr,-32600,"Invalid request");
        if (!message.contains("method") && !message.contains("id")) return failure(nullptr,-32600,"Invalid request");
        if (!message.contains("method")) {
            if (message.contains("result") || message.contains("error")) return nullptr;
            return failure(nullptr,-32600,"Invalid request");
        }
        if (!message.at("method").is_string()) return failure(nullptr,-32600,"Invalid request");
        if (!message.contains("id")) return nullptr;
        request_id=message.at("id");
        if (!request_id.is_string() && !request_id.is_number_integer() && !request_id.is_null())
            return failure(nullptr,-32600,"Invalid request id");
        const auto method=message.at("method").get<std::string>();
        const auto params=message.value("params",Json::object());
        const auto available_tools=[&] {
            auto result=principal_only?team_tools():Cluster::tools();
            if (principal_only) {
                const std::set<std::string> allowed{"team_get_settings","team_get_prompt","team_start_project","team_get_project","team_list_projects","team_get_queue","team_wait_decisions","team_resolve_decision","team_update_charter","team_control_project"};
                auto filtered=Json::array();for (auto& entry:result) if (allowed.contains(entry.at("name").get<std::string>())) filtered.push_back(std::move(entry));return filtered;
            }
            return result;
        };
        Json value;
        if (method=="initialize") {
            const auto version=params.value("protocolVersion","");
            value={{"protocolVersion",mcp_protocol_supported(version)?version:"2025-11-25"},
                {"capabilities",{{"tools",Json::object()},{"resources",Json::object()}}},
                {"serverInfo",{{"name","fatfishbox"},{"title","肥鱼重工"},{"version",FATFISHBOX_VERSION}}},
                {"instructions",principal_only?"主代理只用团队决策通道。先 team_get_prompt({role:principal})；用户一次给任务后，team_start_project 启动/续接负责人并持续跟进最终验收，不再索要调度指示。direct/历史队列可接管。确实仅贴连接而无任务或既有授权时才待命。":mcp_handoff_instructions}};
            if (initialized) *initialized=true;
        } else if (method=="ping") {
            value=Json::object();
        } else if (initialized && !*initialized) {
            return failure(request_id,-32002,"Initialize before calling tools");
        } else if (method=="tools/list") {
            value={{"tools",available_tools()}};
        } else if (method=="resources/list") {
            value={{"resources",Json::array({{{"uri",principal_only?"fatfish://team/principal":"fatfish://workflow"},{"name",principal_only?"principal":"workflow"},{"mimeType","text/markdown"}}})}};
        } else if (method=="resources/templates/list") {
            value={{"resourceTemplates",Json::array()}};
        } else if (method=="resources/read" && params.value("uri","")=="fatfish://team/principal") {
            value={{"contents",Json::array({{{"uri","fatfish://team/principal"},{"mimeType","text/markdown"},{"text",team_prompt("principal")}}})}};
        } else if (method=="resources/read" && !principal_only && params.value("uri","")=="fatfish://workflow") {
            value={{"contents",Json::array({{{"uri","fatfish://workflow"},{"mimeType","text/markdown"},
                {"text",workflow}}})}};
        } else if (method=="tools/call") {
            try {
                const auto tool=params.at("name").get<std::string>();
                const auto available=available_tools();
                if (std::ranges::none_of(available,[&](const Json& entry){return entry.at("name")==tool;}))
                    throw std::runtime_error("此操作不对 MCP 开放");
                auto data=invoke_tool(cluster,tool,params.value("arguments",Json::object()));
                if (tool=="cluster_refresh_sessions") data={{"synced",data.at("sessions").size()},
                    {"errors",data.at("errors").size()},{"total",data.at("total")},{"queueId",data.at("queueId")}};
                if (tool=="cluster_cancel_session") data={{"status",data.at("status")},{"queueId",data.at("queueId")}};
                // Some clients only project text blocks into the model's tool result.
                auto content=Json::array({model_visible_payload(data)});
                value={{"content",std::move(content)},{"structuredContent",std::move(data)}};
            } catch (const fatfish::DispatchError& error) { fatfish::diagnostics::caught();
                value={{"isError",true},{"structuredContent",error.details},
                    {"content",Json::array({{{"type","text"},{"text",error.what()}},model_visible_payload(error.details)})}};
            } catch (const fatfish::DshError& error) { fatfish::diagnostics::caught();
                auto details=fatfish::dsh_error_diagnostics(error);
                value={{"isError",true},{"structuredContent",details},
                    {"content",Json::array({{{"type","text"},{"text",error.what()}},model_visible_payload(details)})}};
            } catch (const std::exception& error) { fatfish::diagnostics::caught();
                value={{"isError",true},{"content",Json::array({{{"type","text"},{"text",error.what()}}})}};
            }
        } else {
            return failure(request_id,-32601,"Method not found");
        }
        return {{"jsonrpc","2.0"},{"id",request_id},{"result",std::move(value)}};
    } catch (const Json::parse_error&) { fatfish::diagnostics::caught();
        return failure(nullptr,-32700,"Invalid JSON");
    } catch (const std::exception&) { fatfish::diagnostics::caught();
        return failure(request_id,-32602,"Invalid request arguments");
    } catch (...) { fatfish::diagnostics::caught();
        return failure(request_id,-32603,"Internal error; see executable logs directory");
    }
}

void fatfish::mcp_diagnostic(std::string_view message) {
    if (const auto diagnostics=open_stream(STD_ERROR_HANDLE,"wb",_O_WRONLY)) {
        std::fwrite(message.data(),1,message.size(),diagnostics.get());
        std::fputc('\n',diagnostics.get());
    }
}

int fatfish::run_mcp_stdio(bool principal_only) {
    const auto input = open_stream(STD_INPUT_HANDLE, "rb", _O_RDONLY);
    const auto response_stream = open_stream(STD_OUTPUT_HANDLE, "wb", _O_WRONLY);
    if (!input || !response_stream) return 1;
    const auto send = [&](const Json& value) {
        const auto line = value.dump(-1, ' ', false, Json::error_handler_t::replace) + '\n';
        return std::fwrite(line.data(), 1, line.size(), response_stream.get()) == line.size() &&
            std::fflush(response_stream.get()) == 0;
    };
    try {
        Cluster cluster;
        std::jthread prechecks([&](std::stop_token stop) {cluster.run_prechecks(stop);});
        bool initialized = false;
        for (std::string line; read_request(input.get(),line);) {
            const auto response=dispatch_mcp(cluster,line,&initialized,principal_only);
            if (!response.is_null() && !send(response)) return 1;
        }
    } catch (const std::exception& error) { fatfish::diagnostics::caught();
        mcp_diagnostic(error.what());
        return 1;
    }
    return std::ferror(input.get()) ? 1 : 0;
}
