# 项目交付流程

- 每次完成一轮修改并交付时，必须将 `CMakeLists.txt` 中 `project(FatFishBox VERSION 主版本.次版本.补丁版本)` 的第三个数字加 1，例如 `0.1.1 → 0.1.2`、`0.1.154 → 0.1.155`。第三个数字持续递增，不逢十或逢百进位。前两个数字只有用户明确指定时才允许修改。一次交付只递增一次，修复同轮构建/测试失败、重复编译或重新打包不额外递增；文档、提示词、脚本等修改也遵守此规则。
- 每次完成修改后，必须运行 `cmake --build build --config Release --parallel` 编译新版本。首次构建按 `README.md` 配置 CMake。编译成功后再更新发布目录。
- 默认发布目录为 `out/肥鱼重工`。如果用户正在运行其他发布目录中的 `FatFishBox.exe`，以该实例的可执行文件实际路径为准。替换前按完整路径识别该发布实例，使用 Stop-Process -Force 强制结束该路径对应的进程，并确认进程已退出；不得仅按进程名结束其他目录中的实例。
- 发布更新时，无论窗口可见、最小化或进入托盘，都直接按上述完整路径强制结束进程，无需再次请示，也无需使用 computer-use 或操作托盘菜单；保留用户的关闭行为偏好。
- 使用 `cmake --install build --config Release --prefix "out/肥鱼重工"` 将本次编译的产物复制到发布目录。如果用户使用其他发布目录，将 `--prefix` 改为该目录。确认发布目录中的 `FatFishBox.exe` 与本次构建产物一致。
- 如果更新前发布版正在运行，安装成功后从原发布路径重新打开 `FatFishBox.exe`，确认新进程已启动，让用户可以立即看到修改效果。完成这些步骤后再交付。
- 每轮修改交付前，运行 `./scripts/package-release.ps1`，在 `build/releases/` 暂存 `FatFishBox-v<完整版本号>-win-x64.zip`。脚本从全新的安装目录获取本轮 Release 产物，包含程序、README、许可证、skills 和 prompts。核对 ZIP 可读取且包内 EXE 与本轮构建及本地发布目录的 EXE 一致。
- 推送本轮源码到 `StarBobis/FatFishBox` 的 `main`，为本轮提交创建并推送 `v<完整版本号>` 标签，然后将已校验的 ZIP 上传到该标签对应的 GitHub Release。确认附件可以下载且 SHA-256 与本地 ZIP 一致后，向用户报告版本号及 GitHub Release 链接。保留历史版本附件，桌面无需生成新 ZIP。
