---
name: fatfishbox
description: 使用肥鱼重工 MCP 按两层循环管理 DSH 派发、审查与任务总账；用于已有接入或用户明确请求的肥鱼重工任务。
---

两层循环（单项目经理 + 持久 goal）：用户给出任务与授权后，主代理按并发上限拆分独立任务、写完整可审阅操作计划、启动唯一项目经理；随后每次等待完整60秒，再用 team_get_project 读取经理状态，running 就继续，不消费开发队列、不代调度，不因一次工具超时结束任务。项目经理循环消费待处理列表，派开发、为每个候选派独立审查、亲自在候选分支测试、失败则派修复并复审，全部 accepted 且证据绑定同一候选后结束 goal；不要求独立 integrator、不预先合并全部候选。经理完成后主代理逐项最终验收，通过才合入 main，失败则给出新的完整计划启动下一轮。用户只粘贴接入说明而无任务时，简短确认并等待，不自行编造任务，也不要求用户决定如何派发。

首次使用读取一次 `fatfish://workflow`。参数、特例与准确 schema 看工具目录、team-tools.json 与 team_get_contract，不反复加载整套流程。

续接先核对 queueId 与绑定的团队记录，独立事项使用稳定 workItemId；默认交接是摘要，完整交付与日志按需分页读取；进度用事件游标批量等待。domains/domainId保留为唯一项目经理身份字段；priorityResults等旧摘要仅作兼容，新默认流程只使用待处理队首；不要把实现字段暴露成用户流程步骤。

派发、返修、追加消息和总账更新都保存原参数与 requestId，响应不明原样核对，不换 ID 重复提交；expectedRevision 从当前状态读取。追加消息 accepted 只表示入队。代理自报测试、采集时 HEAD、文件哈希分别表示不同证据，不能相互替代；历史回执不证明已合入 main，未完历史项不静默丢失或误标通过。

派发前按任务是否写文件选择工作区：只读分析、日志查询可不写文件；并行修改、返修或写文件的构建测试须由项目经理在独立 Git worktree 中进行，核对路径、分支、基线后再派发；团队team_delegate由程序自动创建独立分支和worktree；旧cluster_start_session仍需调用方准备工作区，不让并行写入共享主工作区或 main。

每个开发或返修子会话一旦交付候选，项目经理应及时为该候选完整 SHA 单独派 role: reviewer 的独立对抗审查，不等整批结束才总审。审查任务写明原 sessionId、workItemId、候选完整 SHA、验收条件；审查通过后项目经理亲自在候选分支执行真实测试，不能把测试又派给执行者代替自己核验。失败则派修复，新候选 SHA 必须重新独立审查，旧版本通过不沿用。修复 → 复审 → 测试循环直到通过；空待处理列表且有在途不算完成，无须 integrator 门禁即可在全部 accepted 后结束。

主代理每 60 秒读取经理状态直到完成，逐项验收合并并提供完整返工计划；断线按持久游标与 requestId 恢复，不以一次工具超时结束任务。详细规程见 prompts/principal.md、prompts/lead.md、prompts/worker.md。

项目经理使用team_verify_candidate真实执行候选测试，accept引用通过的verificationId。全部通过后team_finish_domain，再完成DSH goal；主代理等status=completed后逐项实际合并main并team_record_merge，失败team_update_charter提交完整返工计划。准确参数见schema。
