# 滚动容器与滚动条（设置页实现参考）

> 结论先说：**上游 YMGUI 没有滚动条控件**。引擎只提供了「滚动语义」（对象上的
> `scroll_x/scroll_y` 偏移 + 子裁剪），**从头到尾没有画过一根滚动条**。
> 本工程设置页那根灰色细条是**本地新增**：`project_Demo/phone_shell/apps/settings.c`
> 自建滚动容器 + 自绘滚动条。
>
> 本文把这个差异、实现方式、几何算式、踩过的坑和验收判据一次写清，
> 以后在别的页面要加滚动时照这份抄。

---

## 1. 上游有什么、没有什么（证据，不是印象）

对照快照：上游 `https://github.com/Yao-Mi/YMGUI` commit `665f1bbe20`（2026-10-03），
本仓库 vendored 在 `third_party/YMGUI/`。

### 1.1 有：滚动语义（引擎级）

| 位置 | 内容 |
| --- | --- |
| `YMGUI/OPOBJ/YMGUI_Obj.h:71` | `GYcoord scroll_x, scroll_y;  //内容滚动偏移(子对象绘制/命中时减去)` |
| `YMGUI/OPOBJ/YMGUI_Obj.c:290` | `ay -= p->parent->scroll_y;` —— 子对象绝对坐标的**绘制与命中都算这份偏移** |
| `YMGUI/ANIM/YMGUI_Anim.c:305,430` | 有专门的 `scroll_y` 补间（`animate scroll`）⇒ 可做平滑滚动动画 |
| `tests/test_scroll.c` | 官方单测（2026-07-01）：**"滚动+子裁剪机制"** —— 验证父 scroll 使子 abs 偏移、`ClipChildren` 把超出父区的子裁掉 |
| `YMGUI/COMMON` 的 `GY_STATE_ClipChildren` | 子裁剪的状态位（`YMGUI_Obj.h`），滚动的前提 |

也就是说：**"能滚"这件事上游是有的**，而且是官方单测覆盖过的语义。

### 1.2 有：会滚的控件（但没有一根条）

`YMGUI/WIDGET/` 共 **54** 个文件，带 `scroll_y` 的控件是这五个：

| 控件 | 滚动实现 | 有没有画滚动条 |
| --- | --- | --- |
| `YMGUI_List.c` | 拖动改容器 `scroll_y` + clamp（`:43`），`YMGUI_List_SetScroll/GetScroll` | ❌ 无 |
| `YMGUI_EditView.c` | 光标自适应滚动（`:288-291`），`SetScroll` | ❌ 无 |
| `YMGUI_TextView.c` | 同上 | ❌ 无 |
| `YMGUI_Table.c` | 同上 | ❌ 无 |
| `YMGUI_Grid.c` | 同上 | ❌ 无 |

**判据**：在整个 `third_party/YMGUI/YMGUI/` 里搜 `滚动条` / `scrollbar` / `ScrollBar`
⇒ **零命中**；`WIDGET/` 里也没有任何 `*ScrollBar*` 文件。

上游要"滚动条"时的实际做法是**拿 Slider 冒充**：
`project_Demo/video_stidio/ui/workspace.c:172` 把播放位置做成
`YMGUI_Creat_Slider_Creat(...)` 并命名为 `s->scrollbar` —— 那是**时间轴 seek 条**，
不是列表滚动条（不跟内容高度/视口高度联动）。

### 1.3 结论

| 能力 | 上游 YMGUI | 本工程 |
| --- | --- | --- |
| 对象级滚动偏移 + 子裁剪 | ✅ 有 | 用 |
| 官方滚动语义单测 | ✅ `tests/test_scroll.c` | 用 |
| 现成"可滚容器"控件（List/EditView…） | ✅ 有，但只服务自己的 item 模型 | 不用（见 §2） |
| **滚动条（视觉滑块）** | ❌ **没有** | **自绘**（`settings.c`） |

---

## 2. 为什么没直接用库里的 `YMGUI_List`

`YMGUI_List_AddItem`（`YMGUI_List.c:163`）只能加**文字条目**：它内部 `malloc` 一个
`GYitem_data` 塞进 `user_data` 并挂上 `itemDrawCb`（画底色 + 默认字体 + 底部分隔线）。
设置页的卡片是 `PhoneUI_panel` + 一堆子控件，塞进去会被它自己的 draw_cb 覆盖掉。

⇒ 所以是**自建容器，只复制库里那两件事**：

1. 开 `ClipChildren`（裁剪超出视口的子对象）；
2. 子对象绝对坐标减 `scroll_y` —— 这件引擎已经做了（`YMGUI_Obj.c:290`），不用自己写。

保留这条结论是为了避免下次再问"为什么不能用现成的 List"。

---

## 3. 本工程实现（`apps/settings.c`）

### 3.1 数据结构

```c
typedef struct {
    GYOBJ   view;         /* 视口：开 ClipChildren，scroll_y 挂在它身上 */
    int32_t content_h;    /* 内容总高（最后一张卡片的底边），本页 = 491 */
    int32_t bar_h;        /* 滑块高度缓存（省得每帧做除法） */
    uint8_t dragging, moved;
    uint8_t ime_shrunk;   /* 视口是否已按软键盘收缩过（去重用） */
    int32_t press_ptr_y, press_scroll;
    GYOBJ   bar;          /* 滚动条：view 的**兄弟**对象，画在卡片之上 */
    /* 诊断量现算用的对象指针（放这里绕开 AppState 的声明顺序问题） */
    GYOBJ   sw_probe, btn_probe, input_probe;
    int32_t input_bottom; /* 布局期记下的输入框底边（与 IME 改 area 无关） */
} ScrollCtx;
```

### 3.2 六个函数

| 函数 | 职责 |
| --- | --- |
| `scroll_clamp()` | `scroll_y` 钳到 `[0, content_h - 视口高]`；**同时镜像诊断量**（`g_diag_scroll_y/_max`）。这是 `scroll_y` 的**唯一写入口**，放这里才不会漏 |
| `scroll_place_bar()` | 算滑块**高度**与**位置**（§4 的算式），内容没超出视口时 `SetHidden(bar,1)` |
| `scroll_moved()` | clamp + 摆条 + **"真的变了才标脏"**（否则整视口白刷，见 §5.6） |
| `scroll_apply_ime(int)` | 软键盘弹起时**收缩视口高度**（而不是挪控件），并把焦点输入框滚进视野；`ime_shrunk` 去重避免每帧改几何 |
| `scroll_drag(GYEvent)` | 拖动主体：`Pressed` 记锚点、`Pressing` 按位移改 `scroll_y`、`Released` 收尾 |
| `scroll_wrap_tree()` / `scroll_wrap_event()` | 给子树**每个对象**装一层事件转发 wrapper（§5.2） |

### 3.3 事件转发：为什么每个对象都要包一层

`YMGUI_Event.c:68-72` 的 `sendEvent` **不向父对象冒泡**，只发给命中的那个对象；
且 `event_cb == NULL` 时**静默丢弃**。命中测试取的是**最深的命中对象**。

三个后果，逐个都被踩过：

1. 手指按在 Switch 上 ⇒ `Pressing` 只进 Switch，滚动容器收不到 ⇒ **滚不动**。
2. 外面再包一层对象也没用 ⇒ 最深的还是里面的控件。
3. 卡片是 `PhoneUI_panel`（只有 `draw_cb`、**没有 `event_cb`**）⇒ 按在卡片空白处时
   命中 panel，`event_cb == NULL` ⇒ 直接丢弃 ⇒ **按在卡片上怎么拖都不动**。

⇒ 最终做法：`scroll_wrap_tree()` **递归给子树里每个对象**都装 wrapper。
wrapper 备份原 `event_cb` 指针（Switch 的 `swEventCb`、Button 的点击回调都不能丢），
按事件类型分派：拖动类转给滚动容器，其余转回原回调。

⚠ 包装索引**不能塞进 `obj->user_data`** —— 那是 Switch/Button 自己的数据指针
（`GYsw_data` 是 `YMGUI_Switch.c` 里的私有类型，头文件看不到），覆盖它会把开关状态读成垃圾。
本工程用独立的 `s_wrap[]` 数组，索引另存。

---

## 4. 几何算式（改动布局时照这个重算）

```text
app 视图区   abs y = 36 .. 448   （高 412）
头部（标题+副标题）  y = 0 .. 56   → SCROLL_HEAD_H = 56
滚动视口     建在 view 内 (0, 56)，320 × (412-56) = 320 × 356
             绝对区域 x=0..320, y=92..448
内容总高     content_h = 491（最后一张卡片底边）
clamp 上限   max = content_h - 视口高 = 491 - 356 = 135

滑块高       th = 视口高² / content_h = 356² / 491 = 258 px
             （并 clamp 到 [SCROLL_BAR_MIN=24, 视口高]）
滑块可移动量 th_max = 视口高 - th = 98
滑块 y       y = 视口顶 + (视口高 - th) × scroll_y / (content_h - 视口高)
             即 92 + 98 × scroll_y / 135
             ⇒ scroll_y=0 → y=92；scroll_y=135 → y=190

滚动条水平位置 x = 视口右 - SCROLL_BAR_W(3) - SCROLL_BAR_MARGIN(3) = 314 .. 317
```

`vh*vh` 用 `int64` 算（`(int64_t)vh * vh / ch`），防将来内容更高时溢出。
`scroll_y` 的分母是 `(content_h - 视口高)` 而**不是** `content_h`，写错会出现
"滚到底条还没到底"。

---

## 5. 六个坑（都上板实测过）

### 5.1 视口不透明 + 开 ClipChildren ⇒ 会盖住建在它之前的兄弟对象

视口原来从 `y=0` 起，而标题就在 `y=0/37` ⇒ **标题被完全遮住**
（2026-10-04 板级抓屏实测：标题区一个文字像素都没有）。
⇒ 视口必须从 `y=SCROLL_HEAD_H` 起（后建的画在上面）。

### 5.2 `sendEvent` 不冒泡 ⇒ 必须递归包装整棵子树

见 §3.3。只装"交互控件"是不够的（卡片空白处命中的是 panel，无 `event_cb`）。

### 5.3 滚动会误触发 Clicked ⇒ 用 SLOP + moved 吞掉

`Switch` 的 `swEventCb`（`YMGUI_Switch.c:46`）收到 `Clicked` 就**立刻翻转状态**。
用户"按住开关想上拖滚动、手指仍在开关上抬起" ⇒ 开关被误改。
⇒ wrapper 在 `Pressing` 里位移超过 `SCROLL_SLOP = 4` 时置 `moved`；
`Clicked` 时若 `moved` 则把开关拨回去、**不改业务状态**。

`moved` 的复位时机：`Released` **不能**清（`Clicked` 紧随其后发出，
见 `YMGUI_Event.c:349-350`），要留到点击回调里用完再清；
`Pressed` 时**必须**清（否则上次的滚动会吞掉这次的点击）。

### 5.4 拖动会把焦点丢给输入框 ⇒ 键盘莫名弹出来

引擎在 `Pressed` 时给命中的可聚焦对象置焦点（`YMGUI_Event.c:338-343`），
而设备名输入框就在滚动区里。拖动起点落在它上面 ⇒ **软键盘立刻弹出**、盖住 `y=226..448`，
之后所有点击都被键盘吃掉。
⇒ `scroll_drag()` 在**首次**判定为滚动的那一刻 `YMGUI_SetFocus(c, NULL)`（只做一次，
反复清会把用户真正的编辑也打断）。

### 5.5 软键盘弹起要收缩**视口**，不能让 IME 去挪控件

`PhoneIME_Show()` 里有一段"把被遮挡输入框往上挪/压扁"的避让逻辑，
它先取**绝对**坐标再改**局部** `area.y` —— 在定长页面里凑巧能用，
在滚动容器里必然错位（板级实测：键盘弹起后输入框直接压在"界面语言"卡片上，两行叠成乱码）。
⇒ 正确做法是**容器自己让出键盘高度**（视口变矮、内容可继续上滚），控件一个都不挪。

焦点输入框的定位**不能**用 `fo->area` 沿父链累加：`scroll_apply_ime` 由 `app_tick` 调用、
**跑在 `PhoneIME_Update` 之后**（`phone_shell.c:648` 先 IME_Update、`659` 才 `PhoneApps_Tick`），
读到的 `area` 已被避让改坏。⇒ 用布局期就记下的 `s_scroll.input_bottom`（与 IME 无关）。

### 5.6 `Pressing` 是无条件赋值 ⇒ 不判变化会白刷整个视口

拖动处理器挂在**整个滚动视口**上、是页内所有控件的祖先，
而 `Pressing` 分支是无条件 `scroll_y = ...` + 标脏 ⇒ 设置页里**任何**拖动
（**包括亮度滑杆**）冒泡上来都会把 320×356 的视口整块标脏。
板级实测：一次亮度拖动恒定刷 **113 920 px**（= 11 条整宽 band + 一条 320×4，正好是整个视口），
而横向拖动时 `scroll_y` 从头到尾没变 ⇒ 纯白刷。
⇒ `scroll_moved()` 里加"`scroll_y` 与**滚动条几何**都没变就直接 return"。
（滚动条也要比：内容高度变化时 `scroll_y` 可能不变而条长/位置变，漏比会留"条画在原地"的残影。）

---

## 6. 验收

**数据 + 视觉两路，缺一不可** —— 数据对不代表条画对了（条可能被遮住/位置算错/没显示）。

### 6.1 数据：`tools/ribbon_check.py` ③

拖动 → `scroll_y` 变 → 反复拖到 `clamp` 上限 → 滚到底后关机按钮可点（打开面板）。
诊断量由固件每帧现算：
`g_diag_scroll_y` / `g_diag_scroll_max` / `g_diag_scroll_content`
（`scroll_y` 是**对象字段**，脚本按 ELF 符号取不到成员地址，必须固件镜像出来）。

### 6.2 视觉：`tools/scroll_shot.py`

进设置 → 抓顶部态 → 拖到底 → 抓底部态，在 **x=315 列**（条占 314..316）扫条色
`0x7C32`（= `RGB(120,132,150)` 的 RGB565，`GY_OPA_COVER` 不透明 ⇒ 精确值），
取最长连续段的上下边当滑块，与几何算式对账。

实测（2026-10-09）：

```text
① scroll_y=0  上限=135  内容总高=491
② 顶部态：滑块 y=92..350   高 258   （期望 356²/491 = 258）
③ 拖到底：scroll_y=135（= 上限）
   底部态：滑块 y=190..448  高 258
④ 滑块下移 98 px（期望 356-258 = 98）
⇒ PASS
```

⚠ 判定**不能**用"取该列众数当背景、再找异色段"那套 —— 滑块（258 px）比页面背景
（110 px）**更长**，众数取出来是滑块自己的颜色，反而把背景段当成滑块
（实测报成高 110 px 且"滚到底后上移"）。直接用条色比。

同一帧顺便裁出右侧竖条放大图（`build/scroll_top_bar.png` / `scroll_bottom_bar.png`）供人眼确认。
⚠ 裁剪必须在**这次抓屏的同一帧**上做：`caret_check.frame()` 读的是**当前**显存，
"先抓两次、回头再裁"会得到两遍都是最后一次的画面（已踩过）。

---

## 7. 想在新页面加滚动，照这个顺序

1. `PhoneUI_panel` / 容器里建一个 **view** 子对象：位置从头部之下开始，开 `ClipChildren`，
   宽 = 内容宽、高 = 可视高。
2. 把内容卡片全部建成 **view 的子对象**，`y` 用**内容坐标**（从 0 起，不考虑滚动）。
3. 在 `view` 之外（兄弟层）建 **bar** 对象，`draw_cb` 自绘（本工程是 3 px 圆角细条）。
4. `content_h` 在布局末尾按"最后一张卡片底边"算好；`clamp` 上限 = `content_h - 视口高`。
5. 给子树**递归**装 wrapper（§3.3），wrapper 里转发 `Pressed/Pressing/Released`，
   并用 `SLOP + moved` 吞掉滚动误触的 `Clicked`（§5.3）。
6. 首次判定为滚动时撤焦点（§5.4）。
7. 设备有软键盘的话，收缩**视口高度**（§5.5），别去挪控件。
8. 加诊断量（`scroll_y` / `max` / `content`）**在 `clamp` 里镜像**，脚本才读得到。
9. 跑 `tools/ribbon_check.py`（数据）+ `tools/scroll_shot.py`（视觉）。
10. 改了视口几何/卡片坐标 ⇒ 同步 `phone_shell/phone_selftest.inc` 里的坐标断言，
    否则自检会失败。

⚠ 已知未修：视口顶 `y=80..88` 仍有 79~82 个灰字像素溢出（IME 避让的残留），
用户已叫停；详见 `docs/MEMORY_ARCHIVE.md` 的 IME 章节。
