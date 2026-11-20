# 肥鱼重工

独立的 Windows DSH 管理程序。桌面界面链接 `C:\Dev\NativeWinUI` 的 `NativeUI::UI`。调度、DSH 通信、状态存储和 MCP 由 C++ 实现。

## 运行

- 打开 `FatFishBox.exe`。在「连接与 MCP」选择「从 DSH 导入模型」，使用当前 `DSH_WORKERS_CONFIG` 指向的已授权本机配置。也可以填入配置或本机授权文件的绝对路径。导入读取实时目录，授权凭据保存在 DSH 的原文件中。
- 在「账号与模型池」设置账号及池的并发、角色额度、优先级、权重和思考强度。连接页可以切换最少负载、优先级优先、按权重三种策略。新导入的模型池默认关闭，重复导入会保留用户设置。
- 在「会话与交接」派发至已有的授权工作区。通过「同步会话」更新状态。待交接按完成时间排序，处理确认绑定当前结果版本。

Token 用量显示未缓存输入、输出、缓存读取、缓存命中和当天每小时平均用量。跨日统计读取当天 DSH 历史事件，部分会话用量暂不可读取时标记为「部分」。

桌面和 MCP 使用当前用户的 `%LOCALAPPDATA%\FatFishBox`。测试或隔离使用时，可给两个进程设置同一个 `FATFISHBOX_DATA_DIR` 绝对路径。会话页同时读取 Codex 插件的登记和精确版本回执，点击「同步会话」重读。列表标记「Codex」的行显示登记快照，取消和确认由原插件执行。原插件的实时探测与磁盘写入之间可能有短暂差异。

## MCP

同一个 `FatFishBox.exe` 双击打开桌面，带 `--mcp` 参数时通过标准输入输出提供 MCP。Codex 配置示例：

```toml
[mcp_servers.fatfishbox]
command = 'C:\Dev\FatFishBox\out\肥鱼重工\FatFishBox.exe'
args = ["--mcp"]
```

`tools/list` 提供状态、分页、导入、调度设置、派发、同步、取消、取结果与版本化确认。MCP Resource `fatfish://workflow` 提供简要流程。随包的 `skills/fatfishbox/SKILL.md` 可用于客户端的 Skill 发现机制。

## 构建与验证

需要 Windows、MSVC 和 CMake 3.26 或更高版本。

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DBUILD_TESTING=OFF
cmake --build build --config Release --parallel
cmake --install build --config Release --prefix "out/肥鱼重工"
node tests/mcp-smoke.mjs
```

默认从 `C:\Dev\NativeWinUI` 编译界面依赖，也可用 `-DNATIVE_WIN_UI_SOURCE=...` 指定对应 checkout。应用和 MCP 在运行时只需 Windows 系统库和对应的 VC 运行库。Node 只用于测试夹具。

设置 `FATFISH_TEST_LIVE_DSH=1` 额外验证当前 DSH 的真实模型目录。测试使用独立数据目录，并通过模拟服务验证会话派发和回执。
