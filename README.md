# 肥鱼重工

独立的 Windows DSH 管理程序。桌面界面链接 `C:\Dev\NativeWinUI` 的 `NativeUI::UI`。调度、DSH 通信、状态存储和 MCP 由 C++ 实现。

## 运行

- 打开 `FatFishBox.exe`。在「设置 → DSH 连接」点击「自动识别」，从下拉列表选择可连接的配置，也可选择「新建配置」手动填写绝对路径。自动识别检查 `DSH_WORKERS_CONFIG`、用户目录中的 DSH 配置和本机服务状态。托管服务通过 `services/current.json` 跟随当前实例。
- 首次连接官方 DSH 桌面版时，先打开 DSH 完成初始化。在肥鱼重工点击「自动识别 → 自动授权」，程序会从 EXE 解出接入插件，安装后打开 DSH 并验证连接。如果 DSH 正在运行，请从它的「应用」菜单退出，授权会继续。验证成功后选择保存或导入。
- 点击「测试连接」检查所选配置。「保存并启用」切换派发使用的连接，「导入并启用」同时导入模型。每份连接各自保留模型池和预设，已派发会话继续使用原所属服务。授权凭据保存在 DSH 的原文件中。
- 在「账号与模型池」的完整列表启用或停用模型。收起账号后，已启用的模型仍然可见。账号以可折叠卡片展示，可筛选已启用模型并调整思考强度。自动派发优先使用活动会话较少的账号，所有角色从已启用模型自动选择。重新导入会保留模型设置。DSH 暂时移除已保存的思考强度时，派发使用 DSH 默认值。
- 在「会话」查看运行状态与处理进度。右键活动会话可选择「标记为失败」，会话随即进入待处理并停止提交重试；已创建的远端会话会尝试取消，取消未确认时失败原因会提示核对。派发准备中断且超过两分钟的「启动中」占位会自动转为失败，正常派发期间由进程锁保护。页面每 5 秒重读登记。进行中和已完成待处理分别显示，支持滚轮和滚动条。已完成待处理按完成时间从早到晚排列。
- 新建会话固定使用 `danger-full-access` 权限和 `ptc` Agent，无需选择预设。已有会话保持原设置。
- 默认关闭窗口时会询问是进入托盘还是退出，勾选「记住我的选择」可将本次行为设为默认。在「设置 → 常规」可更改。托盘菜单提供打开窗口、同步会话、复制接入说明、设置和退出。进入托盘后本机 MCP 继续运行。
- 设置分组可同时展开。程序会记住每组的展开状态，左侧设置内容超出窗口时可以滚动。

会话页的未缓存输入、输出、缓存读取、缓存命中率和平均每小时用量都按本地日期统计。跨日会话以今日的 DSH 用量结算事件为准。没有可用样本时显示「—」，暂未同步的会话会单独计数。MCP 的 `usage.today` 返回当天分类统计，`usage.total` 保留累计口径。

「会话」页每 5 秒启动一轮后台同步。同步读取插件登记，再按会话归属查询 DSH，优先校验进行中和最近结束的会话。其余历史会话分批复查。界面显示最近同步时间和未能确认的实时状态数。

历史交付可从 DSH 桌面保存的 v4 会话日志读取，支持普通 JSONL 和多帧 Zstandard。FatFishBox 校验会话 ID 和事件序号，用已完成轮次的原始消息、结束时间和失败信息生成结果。桌面详情会显示「DSH 本机历史」，MCP 可用 `cluster_get_session_result({queueId,sessionId})` 直接查看已登记的会话，返回 `resultSource: "dsh_history"`。领取和回执继续使用该轮的结果版本。

通过 `service.attachAuthFile` 接入 DSH 桌面时，FatFishBox 在后台同步和领取交接前检查桌面 Host。Host 意外退出后，FatFishBox 使用配置中的桌面程序自动重新启动，等待桌面发布可用连接，再继续领取。已登记的桌面会话可通过恢复后的连接核对原结果版本。

桌面和 MCP 使用当前用户的 `%LOCALAPPDATA%\FatFishBox`。测试或隔离使用时，可给两个进程设置同一个 `FATFISHBOX_DATA_DIR` 绝对路径。桌面会话页同时读取 Codex 插件的登记快照和版本化回执。肥鱼重工和 Codex 插件共用领取状态，Codex 会话的处理回执仍写入原登记旁的回执文件。

## MCP

## 团队执行：两层循环

团队执行只有两层循环：主代理（对话层）与唯一的项目经理（执行层）。界面上原「领域负责人」统一显示为「项目经理」；旧字段仅为兼容映射，不代表新的用户流程。

在「设置 → 常规」只设置一个最大并发数量，默认 20（包含项目经理）。所有角色从账号池中已启用的模型按全局账号负载自动选择，思考强度沿用对应模型设置。升级保留已保存的最大并发值，降低上限仅限制后续派发，不取消在途会话。旧角色模型与角色并发配置不再参与调度。

在「会话」选择当前队列，默认使用团队协作，点击「复制主代理提示词」，粘贴给主代理，再描述任务即可。团队进度、待决策与会话都归属于当前队列，不需要单独选择项目。已有团队运行记录的队列直接续接原记录，项目经理会话显示在「团队协作 · 执行中」区域。用户无需进入子会话或创建 worktree。专用主代理 MCP 为 `http://127.0.0.1:17863/team`（自定义端口沿用 `FATFISHBOX_MCP_PORT`），stdio 为 `FatFishBox.exe --team-mcp`。此入口只公布决策与状态工具，不携带原始集群和项目经理操作工具。普通 `/mcp` 保留旧功能，也能调用团队工具。

### 主代理层：启动、MCP 等待、逐项验收

- Codex 的 FatFishBox MCP 连接可在对应的 `[mcp_servers.<名称>]` 段设置 `tool_timeout_sec = 360`，为五分钟服务端等待留出返回时间。主代理调用 `team_wait_decisions`，传入 `timeoutMs: 300000`。工具在后台运行时继续等同一次调用。收到普通超时就带返回的游标安静续等。若客户端有不可调整的 60 秒上限，则每次传 45000 毫秒；调用中断时沿用最后收到的游标。经理执行期间只响应待决策、明确异常或用户指令，完成后再检查改动并合并。
- 项目使用干净、已提交的 Git HEAD 作为基线；未提交修改不会被暗中忽略。工作树不干净时主代理须先在授权范围内整理基线，不能丢弃用户修改。每个执行任务由后台建立独立 worktree，项目经理使用独立元数据目录；工作树和提交保留在应用数据目录的 `team-workspaces` 中，结束项目不会自动删除成果。
- 用户给出任务和授权后，主代理先完成并保存计划草稿，再显式启动或按队列状态续接团队，用 `team_get_project` 核对唯一项目经理会话的 leadSessionId/leadState 与 goal 已真实建立、running 正常，然后进入等待循环。仅在用户没有给任何任务或既有授权时才待命。
- 主代理用 `team_wait_decisions` 以最长 300000 毫秒（五分钟）有界等待并保存游标，下一次作为 afterCursor 续接，再用 `team_get_project` 读取项目经理状态：running 就继续等待，不消费开发队列、不代调度、不轮询子会话。等待超时只表示本轮到时，不是失败也不是完成；主代理不以一次工具超时结束任务，也不把“用户再发一条继续”当正常运行步骤。
- 断线或宿主强制结束时，持久保存 projectId、游标与执行状态，在恢复处续接；不声称任意宿主都能主动唤醒。
- 用户追加要求时，用 `team_update_charter` 提交完整新授权书与 changeSummary，绑定 charterVersion；扩大范围或越界时升级为具体事实与方案，不让工作悬停等待用户回答。

### 操作计划必须可审阅

启动项目经理前，主代理写出逐项操作计划，作为 architecture 与验收矩阵的落地，让第三方能按计划复核。每项任务逐项写明：目的、输入、边界、步骤、分支、候选完整 SHA、审查标准、可执行测试、证据、失败处置。候选完整 SHA 在产出后绑定，提交前未知写“待定”并在交付时补全完整 SHA，禁止编造。

### 项目经理层：消费待处理列表的循环

- 项目经理是持续岗位，不因完成一个事项或一个 session 结束而离岗。它维护待处理列表：取队首一条，处理或派出后继后继续下一条；子任务结果只排到队尾，不抢占当前项、不要求审查完成就立即接上修复。`priorityResults` 与 `team_priority_results` 仅为兼容结果查询字段，不表示抢占优先级。
- 开发结果与审查结果分别处理：开发结果先为每个新候选派独立 reviewer；审查结果先登记真实结论——passed 才进入项目经理亲自在候选对应分支执行真实测试，failed/blocked 则登记复现、预期、实际、严重度并派修复。
- 修复产生新候选完整 SHA 后必须重新派独立审查，旧版本通过结论不沿用；测试失败等同审查失败，继续修复复审。任何失败不标完成。
- 待处理列表为空但仍有在途子任务时不完成；全部事项 accepted、审查与测试证据绑定同一个完整候选、无在途、无未解决缺陷后，项目经理提交完成并自然结束 goal。无须独立 integrator、无须预先合并全部候选即可完成；main 合入只在主代理最终验收阶段。
- 检查点只保存进度；运行中等待留在同一goal session，不因处理一项或进入等待而重建经理。断线按持久检查点与稳定 requestId 恢复，未完历史项不静默丢失、不误标通过。

### 主代理逐项验收与完整返工

项目经理“报告完成”不是最终证据。主代理逐项核对：每项验收条件 outcome 为 passed 且 evidence 指向对应提交/日志/清单；每个候选有独立审查结论且审查与测试证据绑定同一完整候选 SHA；失败项未被标完成；空队列有在途未被当完成；未把 integrator 门禁或预合并当完成前置；没有未解决阻塞；现有用户数据与未完历史项未被静默丢失或误标通过。

任一条件不满足就不宣称完成，给出新的完整返工计划（目的、输入、边界、步骤、分支、候选完整 SHA、审查标准、可执行测试、证据、失败处置、具体缺口、返工范围与验收标准），同项目返工用 `team_update_charter` 更新计划。确需永久关闭时，用 `team_control_project` 预览并确认取消，待旧会话退出后在同一队列保存新草稿；不替项目经理接管日常派发。全部通过才按授权逐项合入 main，随后完成构建、发布与打包（版本只增一次）。仅在项目经理就真实阻塞升级时，主代理用 `team_resolve_decision` 按决策版本回复具体决定（不作为日常干预），用 `team_update_charter` 更新完整授权书。

后台从全部已启用模型按账号负载选择，原子检查最大并发数量，再交给现有 DSH 派发器。并发检查包含正在运行的项目经理；容量不足保持排队，不改变已排好的处理顺序。开发、返修、审查按任务总账绑定同一工作项；reviewer 必须绑定原交付中的精确候选 SHA。

后台同步、取消、回执和待办更新失败时，团队摘要返回 `runtimeErrors`，会话页团队标题显示异常数量；展开进度查看详情、原因和计划重试时间。成功后只清除该操作的异常；相同故障日志最多每分钟重复一次。团队派发先保存稳定 launch/requestId，再提交会话，最后关联 sessionId；两份状态写入之间中断时，重新派发会恢复原会话而非另建。

详细规程源文件为 `prompts/principal.md`、`prompts/lead.md`、`prompts/worker.md`，构建时嵌入 EXE，安装时另附可阅读副本。主代理可通过 `team_get_prompt` 或资源 `fatfish://team/principal` 获取。修改提示词后必须重新构建。

## 兼容旧数据与直接管理模式（历史）

本章仅用于读取和迁移升级前已保存的旧数据与历史队列，是兼容映射，不是新机制的默认流程；实现字段（token、expectedRevision、domainId、revision 等）不是用户流程步骤。新的默认流程以上面「团队执行：两层循环」为准。历史回执不证明已合入 main。

- 在「会话」页选择队列查看历史会话与 Token 用量；已完成的会话分在「待处理」和「已确认」页签。接入说明与队列创建、`cluster_find_queues`、`cluster_bind_queue`、`cluster_create_queue`、删除队列和 stdio/HTTP 连接等入口保持兼容，便于读取旧数据。
- 历史会话结果可用 `cluster_get_session_result({queueId,sessionId,resultVersion,offset,limit})` 分页读取，`limit` 以 UTF-8 字节计、范围 4–24000，按 `nextOffset` 翻页；最多扫描 128 页、16 MiB，超出报 `truncated`。`cluster_wait_handoff`、`cluster_resume_handoff` 保留给旧队列的领取兼容。
- 旧任务总账用 `cluster_list_work_items` / `cluster_get_work_item` 读取；`cluster_update_work_item` 的 add_issue/resolve_issue、attach_session、record_review、accept、integrate、reopen/block/cancel、reconcile 等仅为兼容旧数据结构，integrate 只记录 main（或指定分支）的提交 SHA，不执行合并。
- 回执保留 checksManifest / evidenceManifest 与 `cluster_get_evidence_manifest` 校验；采集时 HEAD 不证明 EXE 构建来源（sourceBinding 为 capture_time_only），测试退出码仍是自报来源（testExecutionVerified 为 false），不会因生成清单就变成实际执行通过。
- `cluster_send_message` / `cluster_get_message` 为旧会话追加消息，accepted 只表示入队；这些兼容操作不参与两层循环的默认路径。

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

### 项目经理两层循环（新机制）

项目经理承担执行层的循环职责，开发和审查仍执行单项任务。它维护持久的待处理列表：取队首一条，处理或派后继后继续下一条；子任务完成只把新阶段排到队尾，不发抢占式通知，不要求审查完成就立即接上修复。每条任务按轮到的阶段完成开发、独立审查、项目经理亲自测试、失败修复与复审；审查与测试证据必须绑定同一个完整候选 SHA。全部 accepted 且无在途后提交完成并自然结束 goal，不要求独立 integrator、不预先合并全部候选。空待处理列表但有在途不算完成；失败不标完成。

队列顺序跨项目经理接续及程序重启保留，已处理或已有在途后继的阶段自动离队；只阻塞本项，不暂停独立任务。上下文将满保存检查点，由宿主在同一goal session压缩与续行，队列真正为空且无在途才结束。`priorityResults` 与 `team_priority_results` 为旧接口兼容字段，不再具有抢占优先含义；domains/domainId保留为单经理身份兼容；旧多领域数据仍可读取，新项目只允许一个经理。

## v0.1.4 实际执行门禁

项目经理启动前通过DSH `/goal`创建目标，并读取`session/projections.values.goal`校验active及objective。goal运行期间的轮次间隙仍显示running；经理逐项验收后项目进入finishing，等goal结束及经理退出才completed。未完成而goal退出会报告真实异常，不自动重建经理。宿主自身的goal轮次上限仍然有效。

`team_verify_candidate`由经理调用，在独立候选分支启动真实测试进程，记录退出码、日志、超时和源码变化，返回verificationId；accept必须引用同一SHA的通过验证。`team_record_merge`只在经理completed后供主代理登记实际main合并，核对候选祖先关系。新项目`domains`恰好一项；`tasks`提供name/steps/acceptance/files/dependsOn/startCondition/tests，数量不超过总并发减一个经理。

长结果详情安全显示前段，保留原文供“复制结果/复制原因”完整复制；不会再触发多行文本框65536 UTF16单元上限。

## 0.1.5：goal 投影兼容修复

实际 DSH 投影路径为 `values.goal.goal`；FatFishBox 统一解析该嵌套快照，并兼容旧扁平投影。创建 goal 只发送一次命令，最多20次只读回查、间隔100毫秒，总连接期限5秒；瞬时可重试传输失败计入期限。只按 DSH 的 JavaScript trim 规则忽略目标首尾空白，目标内容不一致或非active仍拒绝派发，并记录观察到的phase及匹配状态。运行中状态刷新使用相同解析器。`team_start_project.tasks` schema明确要求name、steps、acceptance，steps是单个字符串，多步用换行分隔。

会话页默认团队协作，不再显示执行方式选择器。项目经理任务直接显示在团队协作状态区域；暂停执行、领域进度、负责人显隐及手动同步入口已移除，定时同步机制保持不变。

## 0.1.6：项目经理首次执行的职责与凭据

激活goal前先生成完整manager-bootstrap.md（规程、授权、任务计划、工具凭据），并记录激活前的session cursor。goal首轮明确项目经理职责，第一步读取该文档并派发开发；不再随后排队发送启动prompt。凭据保留在本地启动文档，不放进goal目标或投影摘要。开发/审查/修复会话仍使用原来的单次prompt派发。历史失败经理不再阻断新一轮经理的正常完成。

## 0.1.7：主代理任务契约与就绪批量派发

主代理负责完整拆分，每项tasks含name、steps、acceptance、files、dependsOn、startCondition、tests。files使用仓库相对路径（目录用/**），dependsOn用精确任务名称，无依赖用[]。服务拒绝缺字段、越界边界、循环/未知依赖和未排序的共享写入。列表顺序不表示依赖。

经理启动后调用team_dispatch_ready一次排队所有依赖已验收的任务，之后回到完成结果FIFO；每次处理结果尤其accept后再次派就绪项。容量不足由原调度器排队；不是把全部任务交给一个工作者，也不是强求占满并发。工作者自动收到完整契约和前置候选SHA。有就绪未派项时不能checkpoint waiting。team_delegate单项开发同样不能越过依赖门禁。

旧项目可读且不被自动改写。dispatchPlan.planNeedsUpdate明确指出缺失契约，主代理用team_update_charter附完整tasks数组修订；经理不猜依赖。startCondition中的环境前提由主代理明确、执行者实际核对，程序自动强制的启动门禁是dependsOn对应工作项accepted/integrated。

## 0.1.8：团队账号池负载均衡

团队角色配置选定模型与思考强度；新派发在所有已启用且模型/思考强度一致的账号池中，使用原有的全局负载、容量、实时可用性和冷却规则选择账号，并继续遵守批次允许池限制。角色配置不变。已派发会话及其请求重放保持原会话ID和原账号，不迁移或重派；尚未派发的旧排队任务自动使用新策略。旧版直接接口显式poolId仍固定账号。

主代理与项目经理禁止自行 sleep 或用代码计时等待。经理使用 `team_wait_actions`，主代理使用 `team_wait_decisions`；MCP 有可处理结果、就绪任务或完成状态即提前返回反馈，不补足五分钟。经理通过 `team_get_contract` 获取最新工具 schema。

## 0.1.9 设置与使用引导

「队列管理」独立提供创建、切换、重命名和删除；执行中的团队队列不能删除。MCP 地址位于标题栏，点击可复制；主代理提示词仍在会话页复制。首次未配置连接时自动进入四步引导：连接并导入 DSH、启用模型、创建队列、复制提示词开始任务。在「设置 → 常规」可重新进入引导，也可选择稍后设置。引导完成状态保存在本机，不触碰正在执行的任务。

当前团队流程以 0.1.11 说明为准：开发相关自测、独立审查、经理判断确认，不要求取证或强制二次构建。

内置 principal/lead/worker 提示词、MCP 工作流与工具说明、复制的接入文本、goal 启动上下文及运行规程通知均使用英文。中文界面、用户提交的需求和授权内容保持原文。

### 0.1.10 门禁反馈优化（历史行为，已由 0.1.11 替代）

普通日志附件缺失、文件名误写和采集失败改为警告，经理可说明限制后继续登记真实通过的审查。team_capture_evidence 已进入经理契约，支持采集已声明/工作树内的文件，以及用 reportedPath/replacementPath/reason 勘误；原始声明和真实替代文件哈希均保留。审查身份、实际 verdict/精确 SHA、开发先构建自测和经理最终 verify/accept 保持门禁。错误返回 expected/actual；契约提供每个总账操作的完整模板。真正无法处置的 FIFO 项可按 actionId/jobId 绑定决策暂停，MCP 等待不再被反复唤醒。旧版仅因附件缺失产生的阻塞自动转为警告，不取消任何会话。

## 0.1.11：移除质量门禁与取证

默认流程仅为开发完成相关自测、独立审查、经理判断并确认。accept不再要求verificationId，完成项目不再要求证据数组/验收证明清单。读取结果不再自动采集文件、检查Git状态、计算文件哈希或建立证据归档。取证工具从默认目录移除，历史取证调用返回已停用，不继续扫描；旧归档保留。提示词禁止把局部修复扩展成全量页面/配置/截图取证；当前经理和执行者会收到停止额外取证的新规程。仅运行与实际改动相关的功能检查，不能虚报跳过的覆盖已完成。

经理启动状态单独返回 `managerStartup`。草稿显式启动后先显示 starting，只有真实会话与 active goal 建立后才显示 running。显式启动计划时持久化唯一经理任务，后台先派发再复查历史会话；兼容恢复旧 pending 项目。超过 30 秒未建立 goal 或出现派发错误时，`team_wait_decisions` 提前返回 `needsAttention`、启动阶段、原因及重试时间；不会把空等当作正常执行。计划文件冲突会列明任务与路径。


团队计划与队列恢复（主代理 v15）。`team_start_project` 保存草稿，`team_update_charter` 可修订草稿。规划完成后，用 `team_activate_project({projectId,requestId,expectedVersion})` 启动已核对的版本。运行中的项目可用 `team_update_charter` 一次提交完整修订。暂停项目需等旧会话退出再恢复，已完成项目可在队列可用时重新打开为草稿后启动。

`team_control_project` 的 cancel 是终态。先用 `dryRun:true` 查看影响，再用 `confirm:true` 生效。旧会话确认退出后，`team_get_queue` 返回可接管的队列、上一项目以及 `allowedActions`。新草稿继续使用原 `queueId`，并可用 `replaceProjectId` 防止误接管已变化的历史。队列的工作项、会话和回执沿用原记录。取消或完成的项目带 `archived` 和 `closedAt`，可按 ID 查询，也可用 `team_list_projects({includeArchived:true})` 列出。

MCP 工具目录公布输入与输出 schema，成功和团队操作错误均含 `structuredContent`，同时保留文本兼容。若宿主仅显示文本，`team_get_prompt({role:"principal"})` 也提供关键工具的输入 schema。
