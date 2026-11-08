# Notepad3（侧栏文件夹树版）

本仓库是 [rizonesoft/Notepad3](https://github.com/rizonesoft/Notepad3) 的个人 fork。原版的功能、用法和文档以[原版仓库](https://github.com/rizonesoft/Notepad3)为准，这里只说明本 fork 在原版之上做了什么。

下载：[Releases](https://github.com/C-Q-D/Notepad3/releases)（x64 便携版，解压即用）。

## 为什么改

原版 Notepad3 一次只打开一个文件。经常要在同一个文件夹里来回看几个文件、偶尔改几处时，每次都得走一遍"打开文件"对话框。本 fork 加了一个左侧文件夹树：打开一次文件夹，之后点文件名就能切换。

## 新增：侧栏文件夹树

| 功能 | 说明 |
|---|---|
| 打开文件夹 | 文件 → 打开文件夹…，选一个目录，侧栏以树形显示它的内容 |
| 显示 / 隐藏侧栏 | 查看 → 面板 → 显示侧栏，快捷键 `Alt+Shift+B` |
| 逐层展开 | 点文件夹前的展开按钮才读取下一层，大目录也不会卡 |
| 排序 | 文件夹在前、文件在后；按自然顺序排，`b2` 排在 `b10` 前面；隐藏文件和系统文件照常显示 |
| 单击切换文件 | 单击文件即打开；当前打开的文件在树里保持选中 |
| 切换前自动保存 | 当前文件有改动时，切换前静默保存（不论原有"自动保存"开关是否打开，用的是原有保存机制）。新建的、还没有路径的文档弹出原有的保存询问；取消或保存失败（如只读文件）则不切换 |
| 侧栏里的按键 | 侧栏有焦点时：`Enter` 打开文件或展开 / 收起文件夹，`F5` 刷新树（保留展开状态和选中项），`Esc` / `Tab` 回到编辑区 |
| 调整宽度 | 拖动侧栏右边的分隔条 |
| 记住上次状态 | 重启后恢复上次的文件夹、侧栏是否显示、侧栏宽度 |
| 亮色 / 暗色主题 | 跟随 Notepad3 的主题设置 |

菜单文字只有中文（简体）和英文是正式翻译，其余语言暂时显示英文。

## 新增的配置项

写在 `Notepad3.ini` 的 `[Settings]` 节，一般不用手改：

| 键 | 默认值 | 说明 |
|---|---|---|
| `ShowSidebar` | `false` | 是否显示侧栏 |
| `SidebarWidth` | `240` | 侧栏宽度，96 DPI 下的逻辑像素，范围 100–4000 |
| `SidebarFolder` | 空 | 侧栏根目录，可写相对路径或环境变量 |

## 改动范围

- 新增 `src/Sidebar.c`、`src/Sidebar.h`：侧栏的全部实现。
- `src/Notepad3.c`、`src/Config/Config.cpp` 等：接入菜单命令、布局、主题、启动恢复和配置读写。原有的 `AutoSaveDoWork` 改为返回保存是否成功，原调用方的行为不变。
- `language/*/menu_*.rc`、`strings_*.rc`：26 种语言各加两个菜单项和一个对话框标题。
- 新增测试：`test/TestSidebarSwitch.ps1`（切换策略、排序、宽度、路径比较）、`test/TestSidebarGui.ps1` + `test/TestSidebarGui.ahk`（用 AutoHotkey v2 驱动真实程序的界面测试）。

## 构建

与原版相同，见 [Windows build and test](docs/build-and-test/windows-build-and-test.md)。本 fork 用 VS 2022 Build Tools（v143）编译 Release x64：

```powershell
msbuild Notepad3.sln /m /p:Configuration=Release /p:Platform=x64
```

运行侧栏测试：

```powershell
pwsh -NoProfile -File test\TestSidebarSwitch.ps1
pwsh -NoProfile -File test\TestSidebarGui.ps1   # 需要 AutoHotkey v2
```

## 与原版同步

`upstream` 指向原版仓库。原版有更新时从 `upstream/master` 合并到本仓库的 `master`。

## 许可

沿用原版的 [BSD 3-Clause](Build/Docs/License.txt) 许可。Notepad3 基于 Florian Balmer 的 Notepad2 和 XhmikosR 的 Notepad2-mod。
