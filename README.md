# 肥鱼重工

独立的 Windows DSH 管理程序。桌面界面链接 `C:\Dev\NativeWinUI` 的 `NativeUI::UI`。调度、DSH 通信、状态存储和 MCP 由 C++ 实现。

## 运行

- 打开 `FatFishBox.exe`。在「连接与 MCP」选择「从 DSH 导入模型」，使用当前 `DSH_WORKERS_CONFIG` 指向的已授权本机配置。也可以填入配置或本机授权文件的绝对路径。导入读取实时目录，授权凭据保存在 DSH 的原文件中。
- 在「账号与模型池」设置账号及池的并发、角色额度、优先级、权重和思考强度。连接页可以切换最少负载、优先级优先、按权重三种策略。新导入的模型池默认关闭，重复导入会保留用户设置。
- 在「会话与交接」派发至已有的授权工作区。「同步会话」更新状态。「只看未处理」按完成时间排序。
- 在「连接与 MCP」从 DSH 实时目录选择权限与 Agent 预设。更换连接配置后可以刷新预设。

Token 用量显示未缓存输入、输出、缓存读取、缓存命中和当天每小时平均用量。跨日统计读取当天 DSH 历史事件，部分会话用量暂不可读取时标记为「部分」。

桌面和 MCP 使用当前用户的 `%LOCALAPPDATA%\FatFishBox`。测试或隔离使用时，可给两个进程设置同一个 `FATFISHBOX_DATA_DIR` 绝对路径。桌面会话页同时读取 Codex 插件的登记快照和版本化回执。肥鱼重工和 Codex 插件共用领取状态，Codex 会话的处理回执仍写入原登记旁的回执文件。

## MCP

双击 `FatFishBox.exe` 会自动启动 Streamable HTTP 本机监听，默认地址是 `http://127.0.0.1:17863/mcp`，可用 `FATFISHBOX_MCP_PORT` 修改。客户端发送 `Authorization: Bearer <令牌>`，令牌从「连接与 MCP」复制，或从数据目录的 `mcp-http.token` 读取。关闭桌面会关闭随窗口启动的监听。用 `FatFishBox.exe --mcp-http` 可运行无界面服务。

同一个 EXE 带 `--mcp` 参数时以标准输入输出提供 MCP。Codex 的 stdio 配置示例见下方。

```toml
[mcp_servers.fatfishbox]
command = 'C:\Dev\FatFishBox\out\肥鱼重工\FatFishBox.exe'
args = ["--mcp"]
```

Agent 通过 `cluster_next_handoff({})` 领取最早完成的未处理对话。采取处置后用同一工具提交 `{claimToken, outcome}`，回执生效后返回下一条。仅提交令牌可重读当前结果。回执绑定结果版本。领取有效期为 30 分钟，过期后原结果会重新排队。MCP 还提供计数、导入、调度、派发、同步和取消工具，以及 `fatfish://workflow` 流程资源。

## 构建与验证

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
