# 两层循环操作手册（0.1.4）

## 职责与状态
主代理只负责需求、拆分、计划、启动、每分钟等待、最终逐项验收与main合并。项目经理只负责单goal会话内的开发/独立审查/亲自测试/修复循环。执行者只完成单项任务。新项目只创建一个经理；旧domains/domainId字段用于保持存储和接口兼容。
项目运行状态：starting（创建队列/经理）→running（经理goal与队列循环）→finishing（全部候选已验收，经理正在完成goal）→completed（经理已自然退出，供主代理验收）。暂停和取消停止派发并核对会话取消结果，不把请求取消当作已经取消。
工作项：开发候选→待独立审查→审查结果待处理→经理测试→accepted。任何失败进入修复，新SHA重新经过审查和测试。集成状态integrated只在主代理真正合并main后登记，不是经理结束前置。

## 第一步：主代理准备完整计划
1. 读取用户要求、AGENTS.md、README及已有接口；记录Git完整HEAD、工作区路径、目标分支及现有改动。已有改动先按授权提交或保留，不暗中覆盖。
2. team_get_settings读取用户模型和并发。总并发包含一个经理，独立计划事项不能超过totalConcurrency-1。共享文件指派唯一写入者，有依赖步骤放在同一事项顺序执行。
3. 为每个事项写明：名称、目标、触发输入、基线、允许文件、禁止文件、公共接口、逐步实现、正常路径、边界/异常路径、对抗审查攻击点、测试程序与参数、预期退出码、证据位置、验收条款、失败修复与复审入口。提交计划文档，让所有子分支可从同一基线读到它。
4. 六部分charter必须完整：objective写结果；acceptance写可执行门禁；constraints写兼容与禁止项；architecture引用已提交计划路径及任务边界；authority写可自主操作范围及main/发布授权；escalation写真正不能自行解决的异常。
5. team_start_project传稳定requestId、绝对workspace、charter、tasks（每项name/steps/acceptance/files/dependsOn/startCondition/tests）、domains恰好一个经理定义（name/mission/acceptance/dependencies=[]）。有queueId沿用原队列；响应不明原参数重试。
6. team_get_project核对唯一经理的leadSessionId、leadState及goal.phase。程序通过DSH /goal创建目标并通过session/projections.values.goal校验active和objective；不是自报或本地伪造。

## 第二步：主代理MCP等待循环
记录projectId。调用team_wait_decisions（timeoutMs最长300000毫秒）收到反馈后立即读取team_get_project。starting/running/finishing继续同样等待。completed才开始最终验收。不要轮询每个开发会话、读取整批历史或插手经理队列。等待必须由MCP执行，禁止自行sleep或用代码计时；有结果即返回，不补足等待时间。项目经理使用team_wait_actions，同样最长五分钟，子任务完成或有可处理事项即返回。工具超时不代表任务失败或完成。用户停止/修改任务时立即遵从。

## 第三步：项目经理派发并消费队列
1. 使用启动消息凭据读取team_domain_context，再读取tasks和完整计划。get_goal核对本session的真实目标。读取team_get_contract；外层operation与内层action不能混淆。
2. team_delegate派developer，taskName对应计划name，task包含全部契约。后台为每个执行者创建codex/task-<jobId>独立Git分支和worktree，核对baseSha。queued已经持久保存，不能重复派发。
3. 循环team_next_action。子会话完成加入待处理队尾，不打断已排队事项；处理一个结果并安排后继后继续下一项，不等待整批开发完成。
4. candidate_needs_review：核对原jobId、sessionId、resultVersion、候选完整SHA及修改边界，派reviewer，sourceJobId绑定原开发任务，baseSha必须等于结构化交付的candidateSha。
5. review_result_unhandled：读取原始报告，team_domain_action登记record_review，绑定reviewer sessionId/resultVersion/candidateSha。passed必须与真实报告一致；关键缺陷或证据缺失不能登记passed。
6. 审查失败/blocked或执行失败：add_issue记录复现、预期/实际、候选及验收。派repair，沿用sourceJobId或workItemId，在待修SHA上实施；新的候选必须新的独立reviewer。
7. 审查通过：经理亲自调用team_verify_candidate，提供workItemId/candidateSha/executable/arguments/requestId，必要时timeoutMs。服务在对应候选分支实际创建进程，保存日志，记录退出码、超时、运行前后HEAD与工作树；不采信执行者自报。
8. 任一测试失败则派repair并重审。一个验证脚本可以包含多条计划测试，但每条失败必须传播非零退出码；不能用无条件成功脚本代替实际检查。
9. 测试通过且源码未变、缺陷已逐项证实修复后resolve_issue，再accept，details必须带candidateSha/verificationId/notes。验证ID必须属于同一工作项与SHA且passed=true，否则服务拒绝验收。
10. 队列为空时检查在途和未派计划。有未派项先派，有在途则在同一goal session等待再读队列。checkpoint只保存进度，不要求退出重建经理。程序把active goal的轮次间隙视为running。

## 第四步：经理完成门禁
核对每一计划事项都有对应工作项且accepted；每项审查和测试绑定精确候选；无未解决缺陷、无活动执行任务、无未处置交接。保存当前charter版本checkpoint，team_finish_domain提交逐条criterion/outcome=passed/evidence和总结。任何门禁不满足都不允许完成。
提交成功后get_goal读取真实id/revision，按DSH update_goal schema将goal标记complete，随后自然结束session。程序等经理真正退出且goal完成才把项目标为completed。宿主自身的轮次上限/暂停/连接错误仍可能中止goal，必须报告真实异常，不能伪称完成或保证宿主无限执行。

## 第五步：主代理逐项验收与合并
从项目摘要中取verifications（workItemId/candidateSha/workspace/实际命令/exitCode/logPath）、经理证据和对应分支，逐项检查代码与契约，必要时重跑独立验证。通过一个合并一个到main，冲突解决后重跑受影响测试。实际合并成功后team_record_merge登记真实integrationSha及targetBranch=main。服务核对main引用与候选祖先关系，回执可原参数重试。
不通过的候选不合入，写新的完整返工计划，明确缺口、当前main基线、允许修改、修复步骤、新审查/测试/验收标准。team_update_charter传完整六项新授权和expectedVersion，经理下一轮沿用同一队列与原工作项，已合入事项保留。主代理重新进入MCP等待循环。

## 最终交付
按AGENTS.md只增加一次补丁版本，Release编译成功后才安装发布；完整路径识别并强制结束原发布实例，安装后校验EXE哈希，原先运行则重启确认。package-release.ps1从全新安装暂存树创建桌面版本ZIP；验证可读、全包CRC及EXE哈希一致。日志、转储、运行状态与凭据不进入ZIP。最终报告版本、验证结果、桌面ZIP绝对路径及任何实际未完成项。

## v9：直接可派发的任务表

每项定义{name, steps, acceptance, files, dependsOn, startCondition, tests}。主代理确认环境前提与文件唯一写入者，按真实数据依赖连边，不按文字顺序连边。计划总量不超过totalConcurrency-1，必要时分阶段提交完整计划。不要把整个系统审计和所有修复打成一个巨型工作项。

例如：A开发审计工具（tools/audit.py）、B修复已知页面1（gallery/page1.cpp）、C修复已知页面2（gallery/page2.cpp），三项无依赖；D使用工具审计页面1-64（evidence/part1/**）、E审计页面65-127（evidence/part2/**）仅依赖A；F汇总台账（docs/defects.md）依赖D/E。B/C不因等待全量审计而被推迟。未知缺陷按实际台账由主代理在下一轮提交新的完整任务表，不让经理临时规划全部开发。

经理首次team_dispatch_ready就应排队A/B/C。A经历独立审查、经理真实测试并accept后，再次调用即可同时排队D/E；D/E都验收后F解锁。每个开发工作者收到自己的完整契约与前置候选SHA，必要时只在其分支合入依赖。重复requestId重放原结果，不重复建工人；每次新队列阶段用新稳定ID。旧项目缺字段须由主代理显式更新tasks，程序不推断业务依赖。

等待规程v10：主代理只用team_wait_decisions，项目经理只用team_wait_actions。单次最长300000毫秒；完成、异常或待处理事项出现即返回反馈，不补时、不自行sleep。经理使用team_get_contract刷新新增工具schema。
