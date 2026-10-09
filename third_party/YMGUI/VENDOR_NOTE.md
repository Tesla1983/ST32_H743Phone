# 关于 `third_party/YMGUI`：这是「vendored」而不是「submodule」

本目录是上游仓库的**完整源码快照 + 本地移植补丁**，不是一个 git submodule。

## 为什么不用 submodule

本工程的移植对 YMGUI 做了**侵入式修改**（渲染优化、事件分发、IME、电话外壳），基准时相对上游有 **38 个文件被改动、5 个文件为本地新增**。submodule 只能指向一个远端 commit，本地补丁一旦不被推送到远端，`git clone --recursive` 之后拿到的是**没有补丁的上游版本**，工程直接编译失败或行为不一致。所以这里选择 vendoring：一次入库，克隆即完整可构建。

## 上游出处

| 项 | 值 |
| --- | --- |
| 上游仓库 | https://github.com/Yao-Mi/YMGUI |
| 快照 commit | `665f1bbe205fdf78e9efc2845d1a2b0f50b8eb37` |
| 快照日期 | 2026-10-03（本地目录时间戳） |
| 所属相对路径 | `third_party/YMGUI/` |

> 上游快照的 `.git` 目录（292 MB 的历史）已移出本仓库；如需与上游重新 diff，
> 可把它挂回来：
> `git --git-dir=<备份位置> --work-tree=third_party/YMGUI status`
> 备份文件 `YMGUI/CONFIG/YMGUI_Mem.c.vendor_orig` 保留了对改得最狠的那个内存分配器的原始副本，便于对照。

## 本地新增（上游没有）

| 文件 | 作用 |
| --- | --- |
| `project_Demo/phone_shell/phone_lang.c/.h` | 语言运行时（`T("中文原文")` 翻译桥） |
| `project_Demo/phone_shell/phone_shell_board.h` | 板级接口（烧屏侧的诊断量注入、持久化回调注册） |

## 本地改动中要特别注意的几处

改了 **YMGUI 库本体**（不只在 demo 层打补丁），因此升级上游时这几处必然冲突：

| 文件 | 改了什么 |
| --- | --- |
| `YMGUI/CONFIG/YMGUI_Mem.c` | 内存池改适配本工程的 SRAM2 静态池，原著另存为 `.vendor_orig` |
| `YMGUI/CORE/YMGUI_DrawPx.h` | 逐像素光栅化热点：把跨文件的 `lcd_wr_data()` 内联到本文件（`-O2` 不跨编译单元内联） |
| `YMGUI/CORE/YMGUI_DrawFill.c` / `DrawArc.c` / `DrawImg.c` / `DrawText.c` | 渲染优化，累计实测 2.88× |
| `YMGUI/GUI/YMGUI_Invalidate.c/.h` | 脏区计算 |
| `YMGUI/GUI/YMGUI_Event.c` | 事件分发（注意：`sendEvent` **不冒泡**、`event_cb == NULL` **静默丢弃**） |
| `YMGUI/WIDGET/YMGUI_EditView.c` / `TextInput.c` | 输入法相关的编辑控件 |
| `project_Demo/phone_shell/apps/settings.c` | **自建滚动容器 + 自绘滚动条** —— 上游 YMGUI **没有滚动条控件**（引擎只有对象级 `scroll_y` 与 `ClipChildren`；`List`/`EditView`/`TextView`/`Table`/`Grid` 都能滚但都不画条）。本页把七项卡片改可滚动并补了一根 3 px 灰条。上游若新增同类控件，这里最可能冲突；用法与几何算式见 `docs/SCROLL_VIEW.md` |
| `project_Demo/phone_shell/*` | 电话外壳本身：桌面、最近任务、IME、壁纸 ribbon、设置页、**状态栏 WiFi 图标**（`phone_shell.c` 的 `status_draw`：原来硬画的 4 格假信号柱已换成按 `$WF` 真值 RSSI 分档的 WiFi 扇面，数据经 `BoardNet_WifiUp/WifiBars` 取自 `src/uart_link.c`） |
| `project_Demo/phone_shell/apps/weather.c` | **整个页面重写为真数据**（2026-10-10）。原来城市/温度/天气三张表 + 副标题"离线示例天气"全是写死的字符串；现在三城（杭州/上海/成都）走 `BoardNet_City*`（`src/uart_link.c`），真值来自"本板 `$?WEA,<城市码>` → 对端 HTTP → `$WX`"。上游若改 `apps/weather.c` 或 `PhoneUI_app_header`，这里必冲突（本页**不复用** `app_header`：它不返回副标题句柄，而副标题要按真实状态刷新，所以照它的版式自建了两个 label） |

## 未入库的部分（`.gitignore` 排除）

| 路径 | 原因 |
| --- | --- |
| `extern_lib/FFmpeg`（100 MB） | 上游 `video_player` 演示依赖，本工程不构建 |
| `extern_lib/GeneralUser-GS`（48 MB） | 上游 `music_player` 演示的音源库，本工程不构建 |
| `project_Demo/music_player/*.mp3`、`video_player/*.mp4` | 受版权保护的第三方音视频素材 |

⚠ **反向例外**：`tools/gb2312_glyphs.bin`（982 KB）与 `project_Demo/chinese_ime/pinyin_gb2312.bin`（116 KB）
是固件的**构建输入**（`CMakeLists.txt` 用 `objcopy` 把它们内嵌进 `.rodata`），所以根目录 `.gitignore`
里 `*.bin` 的排除规则被显式取反，这两个文件必须入库 —— 丢了它们 pull 下来编译不过。
