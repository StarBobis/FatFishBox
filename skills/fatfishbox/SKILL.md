---
name: fatfishbox
description: 通过肥鱼重工管理 DeepSeek Harness 的账号、模型池、会话与交接。
---

用 `cluster_overview` 读取总量，`cluster_list_sessions` 分页查阅会话。需要检查早先完成的交接时传入 `handoffOnly: true`。需要实时状态时调用 `cluster_refresh_sessions` 或 `cluster_refresh_session`。

用 `cluster_import_models` 从当前已授权的 DSH 配置导入模型目录。编辑前调用 `cluster_get_settings` 获取 revision。通过 `cluster_configure` 设置调度策略，`cluster_update_account` 和 `cluster_update_pool` 写入并发、角色和模型设置。

用 `cluster_start_session` 向授权范围内的工作区派发，提供角色和稳定批次 ID。默认自动选池，指定池时传入 `poolId`。只在明确授权时调用 `cluster_cancel_session`。

用 `cluster_read_result` 核对结果。仅对验证过的当前版本调用 `cluster_mark_receipt`。需要后续行动时标记已读，验收后才确认处理。
