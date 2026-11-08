@AGENTS.md

## 本 fork 的推送与发布

- 远端：`origin` = 本人 fork `C-Q-D/Notepad3`（公开），`upstream` = 原版 `rizonesoft/Notepad3`。只推 `origin`，从不推 `upstream`。
- 打包：先完整编译 Release x64（`msbuild Notepad3.sln /m /p:Configuration=Release /p:Platform=x64`），再运行 `pwsh -NoProfile -File Build\scripts\Make-ForkRelease.ps1 -Version <版本>`。产物在 `Build\Packages\Notepad3_<版本>_x64_Portable.zip`（目录已被 .gitignore 忽略）。只出 x64 便携版，不含 grepWin。
- 版本号与 tag：`<上游版本>-fork.<n>`，上游版本取 `src\VersionEx.h` 的 `MAJOR.MINOR.REV.BUILD`，`n` 在同一上游版本上从 1 递增；tag 为 `v<版本>`，例如 `v7.26.1107.1-fork.1`。
- 本机部署（用户 2026-11-08 指定）：每次编译出新的可用版本、打完包后，都更新到 `D:\可直接运行程序\Notepad3`：`pwsh -NoProfile -File Build\scripts\Deploy-Portable.ps1 -Zip <zip> -Target 'D:\可直接运行程序\Notepad3'`。脚本保留已有的 `Notepad3.ini`、`minipath.ini`、`Favorites`；该目录下的 Notepad3 正在运行时脚本会报错退出，请用户先关掉再部署。`D:\可直接运行程序` 下还有用户的其他程序，只动 `Notepad3` 子目录。
- 发布：`gh release create v<版本> <zip> --repo C-Q-D/Notepad3 --title "Notepad3 <版本>" --notes-file <说明>`。说明用中文，写本版相对上一版（首版相对原版）新增和修复了什么、包里有什么、已知限制。
