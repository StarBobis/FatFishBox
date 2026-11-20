---
name: fatfishbox
description: 通过肥鱼重工管理 DeepSeek Harness 的账号、模型池、会话与交接。
---

用 `cluster_overview` 读取进行中和未处理数量。用 `cluster_refresh_sessions` 同步本地已派发会话。

用 `cluster_import_models` 从当前已授权的 DSH 配置导入模型目录。编辑前调用 `cluster_get_settings` 获取 revision。用 `cluster_update_pool` 启用模型，再更新该模型的并发和思考强度。

用 `cluster_start_session` 向授权工作区派发并提供稳定批次 ID。按任务选择角色，可省略。默认优先使用正在处理的会话更少的账号，指定池时传入 `poolId`。只在明确授权时调用 `cluster_cancel_session`。

用 `cluster_next_handoff({})` 领取最早完成的未处理结果。采取实际处置后，提交同一工具的 `{claimToken, outcome}`。工具确认当前版本并返回下一条。仅传领取令牌会重读本条。遇到版本变化，重新领取后核对新结果。
