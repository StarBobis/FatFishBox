# 肥鱼重工

独立的 Windows DSH 管理程序。桌面界面链接 `C:\Dev\NativeWinUI` 的 `NativeUI::UI`。调度、DSH 通信、状态存储和 MCP 由 C++ 实现。

## 运行

- 打开 `FatFishBox.exe`。在「设置 → DSH 连接」点击「自动识别」，从下拉列表选择可连接的配置，也可选择「新建配置」手动填写绝对路径。自动识别检查 `DSH_WORKERS_CONFIG`、用户目录中的 DSH 配置和本机服务状态。托管服务通过 `services/current.json` 跟随当前实例。
- 首次连接官方 DSH 桌面版时，先打开 DSH 完成初始化。在肥鱼重工点击「自动识别 → 自动授权」，程序会从 EXE 解出接入插件，安装后打开 DSH 并验证连接。如果 DSH 正在运行，请从它的「应用」菜单退出，授权会继续。验证成功后选择保存或导入。
- 点击「测试连接」检查所选配置。「保存并启用」切换派发使用的连接，「导入并启用」同时导入模型。每份连接各自保留模型池和预设，已派发会话继续使用原所属服务。授权凭据保存在 DSH 的原文件中。
- 在「账号与模型池」的完整列表启用或停用模型。收起账号后，已启用的模型仍然可见。上方的「已启用」列表用于调整并发和思考强度。并发输入按回车或离开输入框时保存。自动派发优先使用活动会话较少的账号，并检查各模型的并发上限。重新导入会保留模型设置。DSH 暂时移除已保存的思考强度时，派发使用 DSH 默认值。
- 在「会话」查看运行状态与处理进度。右键活动会话可选择「标记为失败」，会话随即进入待处理并停止提交重试；已创建的远端会话会尝试取消，取消未确认时失败原因会提示核对。派发准备中断且超过两分钟的「启动中」占位会自动转为失败，正常派发期间由进程锁保护。页面每 5 秒重读登记，也可点击「同步会话」立即检查。进行中和已完成待处理分别显示，支持滚轮和滚动条。已完成待处理按完成时间从早到晚排列。
- 在「设置 → DSH 连接」从实时目录选择权限与 Agent 预设。更换连接配置后可以刷新预设。
- 默认关闭窗口时会询问是进入托盘还是退出，勾选「记住我的选择」可将本次行为设为默认。在「设置 → 常规」可更改。托盘菜单提供打开窗口、同步会话、复制接入说明、设置和退出。进入托盘后本机 MCP 继续运行。
- 设置分组可同时展开。程序会记住每组的展开状态，左侧设置内容超出窗口时可以滚动。

会话页的未缓存输入、输出、缓存读取、缓存命中率和平均每小时用量都按本地日期统计。跨日会话以今日的 DSH 用量结算事件为准。没有可用样本时显示「—」，暂未同步的会话会单独计数。MCP 的 `usage.today` 返回当天分类统计，`usage.total` 保留累计口径。

「会话」页每 5 秒启动一轮后台同步。同步读取插件登记，再按会话归属查询 DSH，优先校验进行中和最近结束的会话。其余历史会话分批复查。界面显示最近同步时间和未能确认的实时状态数。

历史交付可从 DSH 桌面保存的 v4 会话日志读取，支持普通 JSONL 和多帧 Zstandard。FatFishBox 校验会话 ID 和事件序号，用已完成轮次的原始消息、结束时间和失败信息生成结果。桌面详情会显示「DSH 本机历史」，MCP 可用 `cluster_get_session_result({queueId,sessionId})` 直接查看已登记的会话，返回 `resultSource: "dsh_history"`。领取和回执继续使用该轮的结果版本。

通过 `service.attachAuthFile` 接入 DSH 桌面时，FatFishBox 在后台同步和领取交接前检查桌面 Host。Host 意外退出后，FatFishBox 使用配置中的桌面程序自动重新启动，等待桌面发布可用连接，再继续领取。已登记的桌面会话可通过恢复后的连接核对原结果版本。

桌面和 MCP 使用当前用户的 `%LOCALAPPDATA%\FatFishBox`。测试或隔离使用时，可给两个进程设置同一个 `FATFISHBOX_DATA_DIR` 绝对路径。桌面会话页同时读取 Codex 插件的登记快照和版本化回执。肥鱼重工和 Codex 插件共用领取状态，Codex 会话的处理回执仍写入原登记旁的回执文件。

## MCP

## 团队执行：主代理只处理决策

在「设置 → 团队执行」为领域负责人、开发与返修、独立审查、集成分别选择已启用的模型，设置总并发、开发并发和审查并发，点击「保存团队方案」。模型选择用于新项目；并发配置限制所有团队项目后续派发，降低上限不会强杀已运行的会话。模型池自身的额度限制仍然生效。

在「会话」选择当前队列，将「执行方式」设为「团队协作」，点击「复制主代理提示词」，粘贴给主代理，再描述任务即可。团队进度、待决策与会话都归属于当前队列，不需要单独选择项目。已有团队运行记录的队列直接续接原记录，负责人会话默认折叠，可点击「显示负责人」。用户无需进入子会话或创建 worktree。专用主代理 MCP 为 `http://127.0.0.1:17863/team`（自定义端口沿用 `FATFISHBOX_MCP_PORT`），stdio 为 `FatFishBox.exe --team-mcp`。此入口只公布和接受决策工具，不携带原始集群和负责人操作工具。普通 `/mcp` 保留旧功能，也能调用团队工具。

主代理用 `team_get_queue` 查询队列的执行方式和绑定记录。启动新团队时，用 `team_start_project` 提交当前 queueId、六部分授权书（objective、acceptance、constraints、architecture、authority、escalation）和按依赖顺序排列的领域定义。现有历史队列可直接接管，只要尚未绑定其他团队；direct 不是禁用团队的限制。省略 queueId 时沿用自动创建队列的方式。项目使用干净、已提交的 Git HEAD 作为基线；未提交修改不会被暗中忽略。若工作树不干净，主代理须先在授权范围内整理基线，不能丢弃用户修改。每个执行任务由后台自动建立独立 detached worktree，负责人使用独立元数据目录。工作树和提交保留在应用数据目录的 `team-workspaces` 中，结束项目不会自动删除成果。

用户一次给出任务和授权后，主代理必须启动或续接团队，核对领域摘要中的 leadSessionId/leadState，持续处理关键决定并完成最终验收。领域负责人实际管理开发、审查、返修，不能让主代理直接承担日常集群操作，也不能只报告“已接入，等待下一步”。仅在用户没有给任何任务或既有授权时才待命。授权范围外的行为不自动扩权，优先选择范围内方案；无可行路径时保存具体阻塞作为最终交付的一部分。

历史续接由负责人使用 `team_domain_history` 分页核对剩余总账，再用 `team_claim_work_item` 原子接管本领域工作项与关联会话，沿用原 workItemId、缺陷、审查和回执。新返修可用 `team_delegate({role:"repair",workItemId:原ID,...})`，新候选仍需精确绑定的独立审查。已集成和已取消项不会重新启动；范围外的未完成项可明确记录 disposition=defer/reason，保持原状态。历史待处理交接数不等于剩余工作数，不会为清空交接而批量重做。

后台将任务路由至已配置的角色模型，原子检查总/角色并发，再交给现有 DSH 派发器。负责人保存 checkpoint 后结束当前会话；子任务结束、授权更新或决策回复会触发新的负责人会话，带上持久化上下文，避免负责人持续轮询。总并发也包含正在运行的负责人；等待中的负责人结束后释放名额。负责人处理待办的FIFO与执行任务的角色名额分开；执行器保留负责人和审查的调度机会，容量不足保持排队，不改变负责人已排好的处理顺序。开发、返修、审查按任务总账绑定同一工作项；reviewer 必须绑定原交付中的精确候选 SHA，领域完成要求独立审查验收及逐项证据。

负责人是持续岗位，不因完成一个事项或一个 session 结束而离岗。首个无依赖领域默认承担积压交接值守，用 `team_backlog_status` 核对待处置、可处理和在途等待数量，再用 `team_next_handoff` 逐条领取、核对和登记处置。confirm 要求已验收/集成/取消的总账及证据引用；linked 必须关联同一工作项的真实在途任务；blocked 必须关联已登记决策。处置绑定 claimToken、结果版本和持久 requestId，成功后返回下一条；这只登记交接处置，不改变工作项验收结论。原有返修等待不能堵住其他积压。

负责人统一调用 `team_next_action` 机械循环处理持久FIFO。新完成结果、待验收/集成阶段和历史交接都入队等待；新结果不打断当前项，也不阻止领取原历史交接。处理本条或派出后继后继续取队首，直到队列为空。`team_priority_results`仅保留兼容结果查询功能。

上下文接近上限时用 `team_checkpoint(state="continue")` 保存进度并自动接续；统一队列为空且没有可处理积压、确实只有在途任务或服务退避时才允许 waiting。仍有待办结果或积压时不能 finish。运行中的旧负责人会收到幂等调度规程/授权变更通知，不需用户重启任务；没有负责人时自动补位。连续接续无进展由主代理处理具体异常，不以凑满并发为目标。

最终集成须给 integrator 提供 `sourceWorkItemIds`（本领域已验收来源），新建集成工作项保存来源关系，避免把新会话挂回已关闭源项。集成候选还需独立审查；验收后交主代理按授权合入 main，再登记集成项与来源项的实际 integrationSha。声明了日志证据但采集结果缺失/目录外时不能直接登记审查 passed；须补证或由主代理显式核验采集。

负责人优先使用带领域凭据的 `team_*` 工具。若 DSH 没有配置 MCP，它可使用元数据工作区自动生成的 `team-call.ps1`，通过内置 `--team-call <request.json> <response.json>` 本机桥接调用同一数据目录，无需用户另外安装插件。请求为 `{ "tool":"team_domain_context", "arguments":{ "projectId":"…", "domainId":"…", "token":"…" } }`，响应为 `{ "ok":true, "result":{} }` 或明确错误。领域 token 用于防止任务误操作其他领域，不是对本机文件访问者的安全隔离。

主代理用 `team_wait_decisions` 只接收关键决策、异常、授权变更和里程碑，使用返回的 cursor 增量续接；普通 session 状态不进入主代理上下文。用 `team_resolve_decision` 按决策版本回复，用 `team_update_charter` 更新完整授权书。会话页当前队列的团队摘要展示进度和待决策数量，展开「领域进度」查看详情，支持暂停/恢复；原有同步和复制入口随队列工作。暂停/取消停止新派发，并在后台请求取消活动会话；必须观察真实终态，不假定取消成功。恢复时负责人重新核对被取消的任务。

后台调度先同步各项目，再按审查、负责人、集成、开发/返修的角色优先级跨项目轮转，同一角色每个项目依次取得一次派发机会；轮转位置持久化，重启不会让同一项目重新插队。每轮最多尝试 16 次派发，并在两次派发之间检查 20 秒预算；单次 DSH/Git 操作仍使用各自超时。全局及角色并发仍由会话预留锁原子检查。负责人处理待办的 FIFO 顺序不受派发轮转影响。

后台同步、取消、回执和待办更新失败时，团队摘要返回 `runtimeErrors`，会话页团队标题显示异常数量，展开「领域进度」查看具体操作、关联会话/领域、原因和计划重试时间。成功后只清除该操作的异常；相同故障日志最多每分钟重复一次。负责人终态回执失败仍会重试，不会提前标为已处理。团队派发先保存稳定 launch/requestId，再提交会话，最后关联 sessionId；若进程在两份状态写入之间中断，重新派发会恢复原会话而非另建。

后台能独立持续运行，但 MCP 无法保证主动唤醒任意客户端中已结束的主代理轮次。当前采用最长 30 秒的有界等待和持久化游标；宿主不支持主动恢复时，在原主对话续接即可。此限制不影响后台已授权任务继续执行。团队报告 completed 表示成果可供主代理最终验收，不等于已经核验所有用户要求或完成发布。

详细规程源文件为 `prompts/principal.md`、`prompts/lead.md`、`prompts/worker.md`，构建时嵌入 EXE，安装时另附可阅读副本。负责人规程覆盖理解检查、任务契约、独立审查、返修、集成、授权升级、异常与接续；修改提示词后必须重新构建。主代理可通过 `team_get_prompt` 或资源 `fatfish://team/principal` 获取。

## 旧版直接管理模式

在「会话」页选择队列，查看进行中的会话及 Token 用量。已完成的会话分在「待处理」和「已确认」两个页签。点击会话会自动读取结果，失败原因在详情中单独显示。「会话」和「设置 → 队列与 MCP」都可复制当前队列的接入说明，包含队列名、UUID、连接地址和交接规则。桌面切换只改变当前视图和复制内容。HTTP 未启动时会提供 stdio 接入信息。

一个工作使用一个队列。Agent 开始已获授权的独立工作时，直接调用 MCP 的 `cluster_create_queue`。可用 `cluster_find_queues` 按 `workKey` 或工作区查找。客户端若提供稳定的聊天标识，可用 `cluster_bind_queue({queueId,clientKey})` 记录上次队列，下次用 `cluster_find_queues({clientKey})` 取回。队列名可修改，UUID 保持固定。派发、状态查询、同步、验收和返修都携带 `queueId`，同名批次的允许池清单按队列独立保存。账号和模型池额度全局共享。

在桌面端打开「设置 → 队列与 MCP」，输入新队列名称并点击「新建队列」。创建成功后会自动切换到新队列，可直接点击「复制接入说明」。首次将说明粘贴给主代理时，可以同时给出任务并授权派发。只粘贴说明时，主代理会先确认接入，等待任务或续接指示。接入说明要求按任务选择工作区：只读任务可使用现有目录，并行写入任务须由主代理先准备独立 worktree 后再派发；单会话修改按任务选择目录，main 的修改或集成须符合用户授权。派发接口不会自动创建 worktree。

无头环境可用 `FatFishBox.exe --mcp` 或 `FatFishBox.exe --mcp-http`，调用 `cluster_create_queue` 时传入 `name` 和 `requestId` 即可创建队列，无需配置 DSH。

删除只在「设置 → 队列与 MCP」里操作，点「删除队列」后须在弹窗确认。已删除的队列会退出桌面和 MCP 目录，会话、回执和返修记录保留在本机。已派发的 DSH 会话继续运行。系统保留「原有队列」供历史工作续接。

升级时现有任务和原 Codex 插件交接归属「原有队列」，现有领取凭据可继续使用。独立工作请创建新队列。未领取、未确认且尚无返修关联的外部 Codex 会话，可用 `cluster_assign_legacy_session` 指定新队列。

双击 `FatFishBox.exe` 会自动启动 Streamable HTTP 本机监听，默认地址是 `http://127.0.0.1:17863/mcp`，可用 `FATFISHBOX_MCP_PORT` 修改。本机客户端可直接连接，无需认证。Codex 可通过 `codex mcp add fatfishbox --url http://127.0.0.1:17863/mcp` 注册。关闭桌面会关闭随窗口启动的监听。用 `FatFishBox.exe --mcp-http` 可运行无界面服务。

在 Codex 用 `/mcp` 确认 `fatfishbox` 已连接，再直接调用它的工具。若已注册但会话里仍看不到工具，在 MCP 设置中选择重启。HTTP 脚本可用于诊断服务，Codex 会把这类调用显示为终端执行。

同一个 EXE 带 `--mcp` 参数时以标准输入输出提供 MCP。Codex 的 stdio 配置示例见下方。

```toml
[mcp_servers.fatfishbox]
command = 'C:\Dev\FatFishBox\out\肥鱼重工\FatFishBox.exe'
args = ["--mcp"]
```

Agent 通过 `cluster_start_session` 向授权工作区派发会话，通过 `cluster_next_handoff({queueId})` 领取该队列中实际完成时间最早且可确认的未处理对话。桌面会话页会标明已领取的结果。采取处置后用同一工具提交 `{queueId, claimToken, outcome}`，回执生效后返回下一条。提交 `{queueId, claimToken}` 可重读当前结果。回执绑定结果版本。领取有效期为 30 分钟，过期后原结果会重新排队。MCP 还提供计数、导入、调度、同步和取消工具，以及 `fatfish://workflow` 流程资源。

当前工作仍在进行但尚无交接时，用 `cluster_wait_handoff({queueId, timeoutMs: 25000})` 挂起等待。它在同一次调用中返回 `ready: true` 或 `timedOut: true`。收到 `ready: true` 后用 `cluster_next_handoff` 领取。等待时不会占用领取凭据，超时后可按需再次等待。

若已领取但凭据遗失，或领取返回 `busy: true`，用 `cluster_resume_handoff({queueId})` 恢复原 `claimToken` 和完整交接。该操作保持原到期时间。返回 `no_active_claim` 表示当前没有有效领取，可重新领取。

更新后若客户端工具列表没有 `cluster_wait_handoff`，在客户端重新连接 fatfishbox MCP，重新读取工具列表。旧版 `cluster_retry_review` 已从目录移除，复审由主代理通过 `cluster_start_session` 的 `role: "reviewer"` 主动安排。`cluster_review_issues` 返回的历史受阻记录须结合已有审查结果判断。

首次领取时，如果客户端要求为 `claimToken`、`outcome` 和 `claimNext` 都提供值，可将这三个字段设为 `null`。

每个开发或返修子会话一旦交付候选，主代理应及时为该会话的具体候选 SHA 单独派发 role: reviewer 的独立对抗审查，不得等整批开发结束后只安排一次总审。审查任务须明确原 sessionId、workItemId、候选 SHA 和验收条件；每条结果分别核对审查结论，审查通过后才能验收该结果。返修产生新候选 SHA 后必须重新独立审查，旧版本的通过结论不沿用。整批最终审查可追加，但不能替代逐项审查。

开发完成后先交接给主代理。主代理读取交付和证据后，使用 cluster_start_session 并设置 role 为 reviewer，主动派发独立对抗审查，任务中明确候选 SHA、工作区和验收条件。审查受阻、失败或发现缺陷时，审查状态和完整内容仍交接给主代理，由主代理安排返修和复审。直到审查无问题，主代理才进行最终验收并提交 outcome。

提交回执时可传 `claimNext: false`，只确认并释放本条。普通派发和返修建议保存原参数，并为每次新派发提供稳定的 `requestId`。插件会在提交前保存请求。提交响应丢失后，插件先同步原会话，仍未确认时按退避间隔向同一 DSH 会话重送同一请求，DSH 按请求 ID 去重。`dispatchRetryAt` 是下次可重试的时间戳。也可用完整原参数和 `requestId` 查询或继续原派发。确认返修已派发后才释放原领取。若返回 `dsh_recovering`，可按 `retryAfterMs` 稍后重试。

首次派发可用 `allowedPoolIds` 为批次保存允许池清单。自动派发可在清单内处理模型不可用或账号限流，固定池使用 `poolId`。提交任务后遇到状态不明时先同步，避免重复派发。派发诊断提供池的状态、占用、上限和备选池。

用户已授权使用子会话执行当前批次时，范围内的验收返修沿用该授权。主代理发现缺陷就用 `cluster_repair_handoff` 派给子会话，收到 `claimReleased: true` 后立即领取下一条。返修完成后独立按时间排队，由主代理用新的领取凭据验收。在用户授权范围内派发新批次，超出范围时先请示。结构化 `delivery` 包含子会话自报的提交、工作树状态、测试退出码和证据路径。完整操作示例见 `fatfish://workflow`。

可以用 `cluster_get_session_status({queueId,sessionId})` 查询已知会话，传入派发返回的 `id` 或 `dshId`。返回的 `sessionId` 是 FatFishBox 登记 ID。若 `dshId` 对应多条记录，请使用 `id`。同队列同批次已有返修时，用 `cluster_attach_handoff_repair({queueId,claimToken,originalVersion,sessionId})` 关联并释放原领取。关联时已完成的返修按关联时刻排队。

同批次的开发交接和独立评审可以分别用自己的领取凭据关联同一个返修 ID。每次关联都释放当前领取，返修只排一次队，验收时通过 `session.repairLinks` 查看所有原交接及版本。

关联前用 `cluster_get_session_status` 核对返修标题和已有交接，确认覆盖当前缺陷。误挂时，在确认返修前用 `cluster_detach_handoff_repair({queueId,sessionId,originalSessionId,originalVersion})` 解绑指定关联。若返修已领取，附上该返修的当前 `claimToken`。原交接符合待处理条件时恢复原队列顺序，其余关联继续随返修验收。

## 精简 MCP 交接与增量事件

首次连接读取一次 `fatfish://workflow`。初始化说明和工具描述只保留入口、参数语义与特例。

领取和恢复领取默认返回 `delivery`、最终回复短摘要及本地 `logPath`。结果按队列、会话及结果版本写入 `%LOCALAPPDATA%/FatFishBox/results`，后续读取沿用该归档，DSH 暂时离线不影响已归档内容。结构化交付在截短日志前提取；无有效交付时明确返回 `delivery.status: "missing"`，失败结果则为 `failed`，不会推断验收通过。摘要中的 `source: "agent_reported"` 表示子会话自报，仍需主代理核验。

完整交付和日志片段使用 `cluster_get_session_result({queueId,sessionId,resultVersion,offset:0,limit:6000})`，按返回的 `nextOffset` 翻页。`limit` 以 UTF-8 字节计，范围 4–24000。结果读取最多扫描 128 页、16 MiB 的会话事件，达到上限会报告 `truncated`。源日志不会默认附在交接中。

`cluster_wait_events({targets:[{queueId}],timeoutMs:0})` 返回当前基线和游标。后续传入 `targets:[{queueId,afterCursor}]`，最多同时等待八个队列，仅返回游标后的变化、最新计数和新游标。每个队列每页默认 32 条，最多 100 条；`hasMore` 表示继续翻页。该操作不领取交接，不回传完整返修关联或历史。事件最多保留 4096 条，过旧、错误队列或其他数据目录的游标返回 `resetRequired` 及新基线。`snapshotTruncated` 表示初始摘要超出页数上限，应结合计数并按已知会话 ID 查询。

DSH 传输错误提供 `reason`、`stage`、`retryable`、`retryAfterMs`。未确认的提交继续复用原请求 ID 核对；明确未提交且已失败的记录需要新的派发请求，不能盲目重复旧派发。启动准备中断的失败记录也会携带 `failureStage`。

## 任务总账

`cluster_create_work_item` 创建稳定 `workItemId`，派发开发或 reviewer 时显式传入；省略时自动创建。返修继承原事项，共享返修可覆盖多个事项。原任务及缺陷定义保存在本机只读 `taskRef`，返修可仅指定 `issueIds` 和新增要求。旧版传完整 task 的方式继续支持。

用 `cluster_list_work_items` 读取全队列计数、分页事项和剩余数量；`remaining` 包括已验收待集成项。旧历史的回执不能证明已合入，迁移记录另标 `reconciliationRequired` 并计入 `migratedNeedsReconciliation`，应逐项核对。`cluster_get_work_item` 返回缺陷、关联会话、审查和验收记录，原任务仅在 `includeTask:true` 时返回。

`cluster_update_work_item` 使用 `expectedRevision` 防止覆盖并发修改，`requestId` 支持原参数重试。它提供 add_issue/resolve_issue、attach_session、record_review、accept、integrate、reopen/block/cancel。审查通过须由主代理按候选 SHA 和结果版本明确登记；验收要求无活动会话、无未解决缺陷且有独立审查通过。结果版本或会话关联变化会使旧验收失效。集成仅记录 main（或指定分支）的提交 SHA，不执行合并。已确认的会话与已集成的事项分别计数。

## 运行中追加消息与证据引用

用 `cluster_send_message` 给本机派发的活动会话补充基线 SHA、约束或缺陷。默认 steer 在下一 step 边界读取，queue 等下一 turn；不会改模型或重置工作树。消息正文和原参数在提交前持久保存，独立消息 ID 严格去重。accepted 仅表示入队，执行结果仍须核对。`cluster_get_message` 查询状态；正文仅按需返回。发送前预检活动状态，但 DSH 没有原子的 running-only 接口；响应不明且随后空闲时会停止重发，返回 unconfirmed_terminal/needsInspection，避免自动唤醒新一轮。

领取结果自动附带 checksManifest 和 evidenceManifest。清单记录采集时的 Git 身份、可观察的 CMake 配置，以及 EXE、截图和日志的实际字节哈希与路径。文件按内容保存和复用；自动采集只读取分配工作区内的报告路径。共享构建产物等外部授权路径可由主代理用 `cluster_capture_evidence` 的 files 明确选择。

`cluster_get_evidence_manifest` 按引用分页读取并校验清单/文件对象哈希。缺失、越界和损坏会显式报告。采集时 HEAD 不证明已有 EXE 的构建来源，sourceBinding 为 capture_time_only；测试退出码保留自报来源，testExecutionVerified 为 false，不会因为生成了清单就变成实际执行通过。主代理仍按真实证据验收，可在审查、验收和集成记录中绑定精确清单引用。单文件和单次采集总量均最多 512 MiB。

## 构建与验证

每轮修改交付时，只将 `CMakeLists.txt` 项目版本的第三个数字递增 1（例如 `0.1.154 → 0.1.155`）；前两个数字只有用户明确指定时才改变。同一轮重试构建和打包不再次递增。发布后运行 `./scripts/package-release.ps1`，会在系统解析的当前用户桌面生成 `肥鱼重工V<版本号>.zip`，从全新的 Release 安装树打包并校验 EXE 哈希，保留旧版本 ZIP，不包含运行目录中的日志和用户数据。

需要 Windows、MSVC 和 CMake 3.26 或更高版本。

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DBUILD_TESTING=OFF
cmake --build build --config Release --parallel
cmake --install build --config Release --prefix "out/肥鱼重工"
node tests/mcp-smoke.mjs "out/肥鱼重工/FatFishBox.exe"
node tests/mcp-http-smoke.mjs "out/肥鱼重工/FatFishBox.exe"
```

默认从 `C:\Dev\NativeWinUI` 编译界面依赖，也可用 `-DNATIVE_WIN_UI_SOURCE=...` 指定对应 checkout。应用和 MCP 在运行时只需 Windows 系统库和对应的 VC 运行库。Node 只用于测试夹具。

设置 `FATFISH_TEST_LIVE_DSH=1` 额外验证当前 DSH 的真实模型目录。测试使用独立数据目录，并通过模拟服务验证会话派发和回执。

`FatFishBox.exe --render-offscreen settings queue-settings.png` 将「设置」页渲染为 PNG，同时展开「队列与 MCP」。也可指定 `sessions` 或 `pools`，以及在文件名后追加像素宽度、高度和 DPI，例如 `925 650 120`。该模式使用桌面布局和渲染器，无需打开窗口。可运行 `ctest --test-dir build -C Release -R offscreen_render` 或 `node tests/offscreen-render.mjs <FatFishBox.exe>` 验证。

## 异常与诊断日志

程序启动即在实际运行的 `FatFishBox.exe` 旁创建 `logs` 文件夹，每次运行使用带日期时间和进程 ID 的 UTF-8 `.log` 文件。窗口消息、桌面操作、后台同步和 MCP 请求捕获异常时，记录异常类型与内容、源码位置、时间、进程/线程 ID、Win32 错误码和调用栈地址（模块及偏移）。日志不主动记录完整请求、任务或凭据，但异常原文可能包含业务信息，分享前请检查。

可恢复的 C++ 异常在操作边界处理，尽量保留程序运行；未处理系统异常、线程未捕获异常、terminate、abort 和 CRT 参数错误会尽可能落盘，并生成同名 `.log.dmp` 小型转储供诊断。访问违规等严重异常后不继续使用可能损坏的进程。强制杀进程、断电、系统 fast-fail 等无法保证捕获。日志目录需要写权限，日志和转储请按需清理；转储可能包含内存中的敏感信息。

### 负责人机械循环（规程 v6）
负责人承担领域内执行主代理职责，开发和审查仍执行单项任务。统一入口 `team_next_action` 返回持久FIFO队首：处理本条、派必要后继、继续下一条。子任务完成只将新阶段排入队尾，不发抢占式通知，不要求审查完成就立即接上。每条任务按轮到的阶段完成开发、独立审查、返修复审、测试、领域集成与累计验证，再交上层主代理终审。上层只决策和终审。
队列顺序跨负责人接续及程序重启保留，已处理/已有在途后继的阶段自动离队；只阻塞本项，不暂停独立任务。队列非空时不能因为某项在途而等待。上下文将满保存continue，队列真正为空才等待。`priorityResults`与`team_priority_results`为旧接口兼容字段，不再具有抢占优先含义。

负责人元数据工作区包含 `team-tools.json` 准确schema与调用模板；也可调用 `team_get_contract`。总账更新的外层operation为cluster_update_work_item，record_review等放内层arguments.action。CLI错误保留结构化details/contract，规程要求修正一次后仍失败就升级，禁止猜接口空转。
