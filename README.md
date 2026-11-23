# 🐟 肥鱼重工 · FatFishBox

把开发、审查和返修交给小模型，给主代理留出做决策和最终验收的额度。

肥鱼重工是一款 Windows 桌面工具，通过 DSH 调度小模型和免费模型，并用 MCP 接入主代理。主代理拆解任务，项目经理带着开发和审查会话并行推进。每项改动都能经过独立挑错、经理复核和主代理验收，发现问题就派回修，直到交付满足要求。

## 🧪 三层审查，发现问题就返修

| 层级 | 怎么做 |
| --- | --- |
| 🔍 **独立对抗审查** | 开发模型在独立 Git worktree 里实现改动并做相关自测，再由独立审查会话针对具体提交找缺陷、核查边界情况。发现问题就派返修，新提交重新进入审查。这是递归对抗审查的基本循环。 |
| 🧰 **项目经理测试审查** | 项目经理核对改动、自测结果和审查意见，按风险补跑针对性测试。确认有缺陷就派单返修并安排复审，确认通过后才验收该任务。 |
| ✅ **主代理最终验收** | 项目经理完成交付后，主代理对照原始需求检查实际改动和测试结果，做必要的集成检查。若仍有缺口，更新计划再交给经理执行。通过后按授权合入主分支。 |

主代理负责规划、关键决策和最终验收。执行期间，它大部分时间都在 MCP 上等待，由项目经理持续处理待办。遇到需要决策的阻塞、明确异常或用户追加要求时，主代理再介入。

## ⏱️ 用时间换主代理额度

按目前的使用估算，相比由主代理直接推进同类任务，**主代理的模型消耗可节省 95% 以上**。代价是多轮审查、测试和返修拉长了等待时间，**从启动到交付通常需要普通方式的 3–5 倍时间**。这两个数字是经验估算，实际结果随任务、模型和返修轮次而变。节省幅度只指主代理的消耗，工作模型另行消耗各自的额度。

## 🚀 快速开始

1. 在 Windows x64 上下载并解压 `FatFishBox-v<版本号>-win-x64.zip`，运行 `FatFishBox.exe`。需要 Microsoft Visual C++ 2015–2022 x64 运行库。也可以从源码构建。[GitHub Releases](https://github.com/StarBobis/FatFishBox/releases)
2. 打开「设置 → DSH 连接」，点击「自动识别」选择已有配置，或手动填写配置文件路径。首次连接 DSH 桌面版时，先打开 DSH 完成初始化，再到肥鱼重工点击「自动授权」，按界面提示完成插件接入。
3. 点击「测试连接」，再选「导入并启用」。进入「账号与模型池」，启用打算用于团队执行的小模型或免费模型。派发会按已启用账号的负载自动调度，最大并发可在「设置 → 常规」调整，默认 20，包含项目经理。
4. 给主代理配置 MCP。在「队列管理」新建或切换到目标队列，再到「会话」点击「复制主代理提示词」，连同任务一起发给主代理。目标 Git 工作区需要有已提交的干净基线，便于建立独立 worktree。
5. 在「会话」查看执行进度、待决策、失败原因和 Token 用量。程序进入托盘后，本机 MCP 会继续运行。

## 🔌 主代理 MCP

Codex 可在配置文件中使用以下连接，`tool_timeout_sec` 为服务端单次最长五分钟的等待留出余量：

```toml
[mcp_servers.fatfishbox]
url = "http://127.0.0.1:17863/team"
tool_timeout_sec = 360
```

stdio 客户端可使用 `FatFishBox.exe --team-mcp`。完整 MCP 接口为 `http://127.0.0.1:17863/mcp` 或 `FatFishBox.exe --mcp`。可用 `FATFISHBOX_MCP_PORT` 更改 HTTP 端口。队列草稿、启动、等待、返修和恢复的操作细节见[团队工作流](docs/two-loop-workflow.md)及[主代理](prompts/principal.md)、[项目经理](prompts/lead.md)和[执行者](prompts/worker.md)提示词。

## 🛠️ 从源码构建

需要 Windows x64、Visual Studio 2022 的 MSVC v143 工具集和 CMake 3.26 或更新版本。使用 Release 配置和 `/MD` 运行库。

```powershell
git clone https://github.com/StarBobis/FatFishBox.git
cd FatFishBox
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DBUILD_TESTING=OFF
cmake --build build --config Release --parallel
cmake --install build --config Release --prefix "out/肥鱼重工"
```

桌面 UI 静态链接 NativeWinUI v0.1.1。CMake 首次配置时下载 `NativeWinUI-0.1.1-win-x64-lib.zip`，校验固定的 SHA-256，再链接预编译的 `NativeWinUI::UI`。构建无需本地 NativeWinUI 源码，安装时附带 SDK 的 MIT 许可和第三方许可，位于 `licenses/NativeWinUI/`。[NativeWinUI v0.1.1 发布页](https://github.com/StarBobis/NativeWinUI-Release/releases/tag/v0.1.1)

开发时可安装 Node.js 并重新配置 `-DBUILD_TESTING=ON`，构建后运行 `ctest --test-dir build -C Release --output-on-failure`。

## 📦 发布新版本

完成 Release 构建后运行 `./scripts/package-release.ps1`。脚本从全新的安装目录打包，在 `build/releases/` 暂存 `FatFishBox-v<版本号>-win-x64.zip`，并输出 ZIP 和 EXE 的 SHA-256。将对应源码提交和 `v<版本号>` 标签推送到 GitHub，再把 ZIP 上传为对应 Release 的附件。[GitHub Releases](https://github.com/StarBobis/FatFishBox/releases)

## 🔒 权限与本地数据

HTTP MCP 只监听本机，当前无需访问令牌。新建 DSH 会话使用 `danger-full-access` 和 `ptc` Agent，模型可访问运行账户有权访问的文件。请在可信的本机环境和工作区使用。

应用状态默认保存在 `%LOCALAPPDATA%\FatFishBox`，可用 `FATFISHBOX_DATA_DIR` 指定隔离目录。程序旁的 `logs` 保存诊断日志和崩溃转储。分享程序时使用打包脚本生成的 ZIP，避免把日志、转储、DSH 授权文件或团队运行数据一起上传。

## 📄 许可证

FatFishBox 使用 MIT 许可证，发布包附带原文。[LICENSE](LICENSE)
