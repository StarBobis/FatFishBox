# 肥鱼重工

独立的 Windows DSH 管理程序。桌面界面链接 `C:\Dev\NativeWinUI` 的 `NativeUI::UI`。调度、DSH 通信、状态存储和 MCP 由 C++ 实现。

## 运行

- 打开 `FatFishBox.exe`。在「连接与 MCP」选择「从 DSH 导入模型」，使用当前 `DSH_WORKERS_CONFIG` 指向的已授权本机配置。也可以填入配置或本机授权文件的绝对路径。导入读取实时目录，授权凭据保存在 DSH 的原文件中。
- 在「账号与模型池」的完整列表启用或停用模型。收起账号后，已启用的模型仍然可见。上方的「已启用」列表用于调整并发和思考强度。并发输入按回车或离开输入框时保存。自动派发优先使用活动会话较少的账号，并检查各模型的并发上限。重新导入会保留模型设置。DSH 暂时移除已保存的思考强度时，派发使用 DSH 默认值。
- 在「会话」查看运行状态与处理进度。页面每 5 秒重读登记，也可点击「同步会话」立即检查。进行中和已完成待处理分别显示，支持滚轮和滚动条。已完成待处理按完成时间从早到晚排列。
- 在「连接与 MCP」从 DSH 实时目录选择权限与 Agent 预设。更换连接配置后可以刷新预设。

Token 用量显示未缓存输入、输出、缓存读取、缓存命中和当天每小时平均用量。桌面程序在后台从已配置的 DSH 服务读取 Codex 会话用量，跨日会话读取当天历史事件。没有可用样本时显示「—」，暂未同步的会话会单独计数。

「会话」页每 5 秒启动一轮后台同步。同步读取插件登记，再按会话归属查询 DSH，优先校验进行中和最近结束的会话。其余历史会话分批复查。界面显示最近同步时间和未能确认的实时状态数。

桌面和 MCP 使用当前用户的 `%LOCALAPPDATA%\FatFishBox`。测试或隔离使用时，可给两个进程设置同一个 `FATFISHBOX_DATA_DIR` 绝对路径。桌面会话页同时读取 Codex 插件的登记快照和版本化回执。肥鱼重工和 Codex 插件共用领取状态，Codex 会话的处理回执仍写入原登记旁的回执文件。

## MCP

在「连接与 MCP」点击「复制接入说明」，把剪贴板内容粘贴给 Agent。说明包含当前 EXE 路径、通用 MCP 配置和派发与交接流程。支持本机 MCP 的客户端可按说明注册服务。

双击 `FatFishBox.exe` 会自动启动 Streamable HTTP 本机监听，默认地址是 `http://127.0.0.1:17863/mcp`，可用 `FATFISHBOX_MCP_PORT` 修改。客户端发送 `Authorization: Bearer <令牌>`，令牌从「连接与 MCP」复制，或从数据目录的 `mcp-http.token` 读取。关闭桌面会关闭随窗口启动的监听。用 `FatFishBox.exe --mcp-http` 可运行无界面服务。

同一个 EXE 带 `--mcp` 参数时以标准输入输出提供 MCP。Codex 的 stdio 配置示例见下方。

```toml
[mcp_servers.fatfishbox]
command = 'C:\Dev\FatFishBox\out\肥鱼重工\FatFishBox.exe'
args = ["--mcp"]
```

Agent 通过 `cluster_start_session` 向授权工作区派发会话，通过 `cluster_next_handoff({})` 领取最早完成的未处理对话。采取处置后用同一工具提交 `{claimToken, outcome}`，回执生效后返回下一条。仅提交令牌可重读当前结果。回执绑定结果版本。领取有效期为 30 分钟，过期后原结果会重新排队。MCP 还提供计数、导入、调度、同步和取消工具，以及 `fatfish://workflow` 流程资源。

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
