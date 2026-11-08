@AGENTS.md

## 本 fork 的推送与发布

- 远端：`origin` = 本人 fork `C-Q-D/Notepad3`（公开），`upstream` = 原版 `rizonesoft/Notepad3`。只推 `origin`，从不推 `upstream`。
- 打包：先完整编译 Release x64（`msbuild Notepad3.sln /m /p:Configuration=Release /p:Platform=x64`），再运行 `pwsh -NoProfile -File Build\scripts\Make-ForkRelease.ps1 -Version <版本>`。产物在 `Build\Packages\Notepad3_<版本>_x64_Portable.zip`（目录已被 .gitignore 忽略）。只出 x64 便携版，不含 grepWin。
- 版本号与 tag：`<上游版本>-fork.<n>`，上游版本取 `src\VersionEx.h` 的 `MAJOR.MINOR.REV.BUILD`，`n` 在同一上游版本上从 1 递增；tag 为 `v<版本>`，例如 `v7.26.1107.1-fork.1`。
- 发布：`gh release create v<版本> <zip> --repo C-Q-D/Notepad3 --title "Notepad3 <版本>" --notes-file <说明>`。说明用中文，写本版相对上一版（首版相对原版）新增和修复了什么、包里有什么、已知限制。
