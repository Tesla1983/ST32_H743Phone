# 预览亮度：出厂默认 50 + 用户自定义后重启保持 —— 验收报告

日期：2026-10-04
需求原文：「把默认的预览亮度初始化调为50，然后如果用户自定义亮度后保持重启不改变亮度值」

---

## 1. 需求与最终结论

| 需求 | 实现 | 验收 |
| --- | --- | --- |
| 出厂默认亮度 50 | `app_config.c` 的 `blob_defaults()`、`phone_shell.c` 的 `display_brightness` 初值、滑杆 range下限 | ✅ 擦扇区 + 重启后 `g_cfg_brightness = 50` |
| 用户自定义后重启不变 | blob 的 `brightness` 字段（本来就存在，只是从未被写过）+ 持久化回调桥 | ✅ 拖到 82 → flash 校验 → 掉电重启仍是 82 |
| （附带修复）一次拖动只擦一次扇区 | 拖动/松手两段式 | ✅ `g_cfg_save_cnt +1`（修复前是 +6） |

**验收结论：通过。** `tools/brightness_check.py` 三项全 PASS，`tools/ribbon_check.py` 回归四项全 PASS。

---

## 2. 亮度语义（三层必须一致）

| 层 | 位置 | 值 |
| --- | --- | --- |
| 持久化 | `AppConfigBlob.brightness` | 0..100，出厂 50 |
| 运行时 | `phone_shell.c` 的 `display_brightness` | 同上|
| 屏侧效果 | `phone_flush` 叠 `alpha = (100-v) * 160 / 100` 的黑罩 | 100 = 不叠罩 |

⚠ **亮度值不是线性感知亮度** —— 屏侧alpha 系数固定 `160/100`，不是 255。别当 gamma 用。

⚠ **下限取 1 而不是 0**：`brightness=0` 会叠一层 `alpha=160` 的全屏黑罩，屏上什么都看不见，与"屏坏了"无法区分。config 侧的 `brightness_valid()` 与滑杆 range（`SetRange(1,100)`）用同一条下界。

⚠ **钳制后必须重算 CRC**：越界值被钳回默认时动了 `s_cur` 的内容，crc 不跟着重算的话，`s_flashed` 快照带着旧 crc，下次 `Save` 写出的 blob 会 `crc != blob_crc(blob)`，重启后 CRC 校验直接判失败 ⇒ **整套设置（语言/半透明/亮度）全丢**。

---

## 3. 三个实现要点

### 3.1 持久化桥（第三份，复制已有两例）

`phone_shell` 是从 YMGUI 移植的库侧代码，不允许 include 应用层 `src/app_config.h`。项目里已有两套同构先例（语言、半透明），照抄第三份：

```
settings.c  brightness_commit()
   → PhoneHost_SetBrightness()
      → s_bright_persist_cb()
         → main.c: app_brightness_persist()
            → AppConfig_SetBrightness() + AppConfig_Save()
```

扇区 240 = `0x000F0000`，magic `"YCON"` + CRC32 双校验，损坏回退默认（中文 / 半透明开 / 亮度 50）不阻塞启动。

### 3.2 拖动中 vs 松手后：两段式落盘（闪存磨损）

**问题**：`YMGUI_Slider` 的回调在 `Pressed` 和**每一次** `Pressing` 都触发（`YMGUI_Slider.c:101`），而 `AppConfig_Save` 是"值变就擦一个扇区"。板级实测：一次拖动 `g_cfg_save_cnt +6` ⇒ **擦了 6 次 W25Q128 扇区**（标称 10 万次/扇区）。用户正常调亮度一天几十次 ⇒ 用不到一年磨光该扇区。

**"值相同不写"挡不住** —— 那只挡重复值，拖动经过 5 个**不同**的中间值就是 5 次不同。

**做法**：拖动中走`PhoneHost_SetBrightnessPreview()`（只改内存 + 重绘 + 不碰持久化回调），松手（`Released` / `ReleasedOff`）时才调`PhoneHost_SetBrightness()` 落盘一次。由 `scroll_wrap_event` 在转发之前调 `brightness_commit()`。

**为什么不用 debounce 定时器**：主循环是 tick 驱动、无 RTOS，加"松手后 500 ms 再写"要多一个状态 + 一个时间戳；而松手事件在 wrapper 里本来就看得见。

**为什么 `Released` 和 `ReleasedOff` 都要收**：手指按下后滑出控件范围再抬起，引擎发的是 `ReleasedOff`。只认 `Released` 会在"拖出界外松手"时丢掉落盘 ⇒ 屏上是新值、flash 里是旧值，重启后亮度"自己变回去"。

### 3.3 Preview 不发 sync 命令（页面被弹回顶部）

**现象**：拖动滑杆完全没反应。扫y=300..360 全范围无命中。

**根因链**：`brightness_apply()` 里调`PhoneApps_Command(SETTINGS, "sync")` → `app_show()` → **把 `scroll_y` 复位成 0**。于是滑杆屏幕 y 从 330 跳到 465，而手指还在 330 处，后续位移全部落空。表现为"拖不动"。

**修法**（两处，都必要）：
1. `brightness_apply(nv, send_sync)` 加参数：Preview 传 0（不发sync）、Set 传 1。
2. `settings.c` 拆出 `settings_sync_controls(int reset_scroll)`：`app_show` 走 `reset_scroll=1`，`app_command("sync")` 走 `reset_scroll=0`。后者是根本修法 —— sync 命令语义是"页面已在屏上、宿主改了个值要刷新文案"，本来就不该动用户的滚动位置。

---

## 4. 踩过的三个坑（都有板级证据）

### 4.1 两段式的"值相同直接 return"会把落盘吃掉

**现象**：三个计数器 `sld_press=1 sld_change=1 sld_commit=1` 全部 +1（事件通路完好），而 `g_cfg_brightness` 纹丝不动。

**根因**：`PhoneHost_SetBrightness` 开头有"值相同直接 return"（原本是为了避免开机时每次擦扇区）。但两段式里 Preview 已经把 `display_brightness` 推到 82，松手时 `SetBrightness(GetBrightness())` 传的**正是同一个值** ⇒ 命中 return ⇒ `persist_cb` 永不被调用。

**修法**：加 `s_bright_dirty` 粘滞标志（Preview 置位、Set 清位），判据改成"值变了**或**用户动过"：

```c
if ((display_brightness == nv) && (s_bright_dirty == 0u)) return;
s_bright_dirty = 0u;
if (display_brightness != nv) brightness_apply(nv, 1);
if (s_bright_persist_cb) s_bright_persist_cb((uint8_t)display_brightness);
```

⚠ `s_bright_dirty` 只在 Preview 里置位，所以开机那次同值同步仍被挡住（dirty=0）⇒ 不会退化成"每次开机擦一次扇区"。

### 4.2 `PhoneHost_SetBrightness` 在 `ctx == NULL` 时会野指针

`main.c` 在 `PhoneShell_BoardInit` **之前**用配置里的值调它同步首帧（那时 `build_ui()` 会首次上屏，晚一步会先画一帧错亮度再纠正，用户看得见闪一下）。而 `ctx` 是 `static GYCTX ctx;` 初值 NULL，只在 BoardInit 里才赋值。函数内部若无条件 `YMGUI_Obj_Invalidate(ctx->root)` 就是 NULL 解引用 → 硬 Fault。

⇒ `brightness_apply()` 里有 `if (ctx == NULL) return;` 守卫，此时只记住值。

### 4.3 验收脚本的坐标不能按算式推，也不能信"上一版布局的实测值"

`lang_check.py` 里的语言段坐标 `(195,255)/(259,255)` 是**上一版布局**实测下来的。改成滚动版并插入半透明卡片后，卡片整体下移 ⇒ 点空，报 "点 English 段后 `g_cfg_lang` 仍= 0（坐标不可点？）"。

同类坑已经踩过三轮（桌面设置图标、语言两段、半透明开关）。**统一收敛到固件每帧现算**：

| 诊断量 | 用途 |
| --- | --- |
| `g_diag_lang_zh_x/_y`、`g_diag_lang_en_x/_y` | 语言两段 |
| `g_diag_bright_x0/_x1/_y` | 亮度滑杆两端与纵向中心 |
| `g_diag_ribbon_sw_x/_y` | 半透明开关 |
| `g_diag_power_btn_x/_y` | 关机按钮 |
| `g_diag_sld_press/_change/_commit` | 亮度滑杆事件通路分段计数 |

⚠ `Settings_UpdateDiag()` 必须**每帧**现算（`app_tick` 里调），不能只在布局变化时算：`app_page` 自己会停在 `y=480`（`on_recent` 把它推到屏幕下方等`open_app` 的归位动画），"进页时算一次"的坐标会凭空多 480。

⚠ 读这些诊断量前必须先 `open_settings()` +等归位动画结束，否则拿到的是动画中途的旧值（2026-10-04 因此误报"开关点不到"）。

⚠ `Settings_UpdateDiag` 里引用 `state.*`，所以它必须定义在 `static AppState state;` **之后**（曾因定义在前面而编译不过）。

---

## 5. 验收实测数据

```
① 出厂默认值：擦掉配置扇区后重启
  擦除并重启后：g_cfg_loaded=0 g_cfg_brightness=50        [ OK ]
  （顺带确认 g_cfg_lang=0 / g_cfg_ribbon=1 也回到默认）

② 拖动滑杆（分 5 段，刻意制造多个中间值）
  滑杆几何（固件现算）：x0=32 x1=288 y=330
  拖到 80% 后：g_cfg_brightness=82（50 → 82）
  事件通路：sld_press=5 sld_change=6 sld_commit=1
  g_cfg_save_cnt +1，g_cfg_erase_last=1                   [ OK ]延迟落盘生效
  拖动后 scroll_y 仍 = 135                                [ OK ]页面没被复位

③ 落盘 + 掉电重启
  拖动后 扇区 240：magic=0x59434F4E ver=1 lang=0 brightness=82
                    crc=0x09A9505D（算得 0x09A9505D）       [ OK ]
  重启后：loaded=1 crc_ok=1 brightness=82[ OK ] 持久化生效

最终 flash：magic=0x59434F4E ver=1 lang=0 brightness=50 theme=0
           ribbon_translucent=1 reserved=0 crc=0x30E8DDDA（自洽）
```

**半透明回归**（因为我改了 `app_show`/`sync` 的结构，必须确认没破坏）：`tools/ribbon_check.py` 四项全 PASS —— 交互翻转 + 渲染差异 141670 px（`y=319..447`）+ 落盘 + 重启保持 + 滚动可用 + 关机开机闭环。

---

## 6. 复现方式

```bash
cd E:/ymgui-h743
python tools/flash_and_verify.py       # 烧录 + 复位 + 全量回归（约 4.5 分钟）
python tools/brightness_check.py       # 亮度专项验收（约 1.5 分钟，自带擦扇区 + 掉电重启）
python tools/ribbon_check.py# 半透明回归（约 2.5 分钟）
python tools/factory_reset.py# 恢复出厂默认
```

⚠ `brightness_check.py` 会**擦掉配置扇区**并做掉电重启，结束时配置停在出厂默认（亮度 50 / 中文 / 半透明开）。它自带 `reset_board()`（`probe-rs reset` 走 NRST，等价断电重启）。

---

## 7. 固件占用

- FLASH 75.09%（`ymgui-h743.elf` 1.66 MB）
- 新增诊断量：3 × `uint32_t`（`g_diag_sld_press/_change/_commit`）+ 4 × `int32_t`（滑杆/语言段坐标）+ 1 × `uint32_t`（`g_diag_last_event_obj`）≈ 32 字节 BSS
- heap1 峰值仍 97%（剩约 7 KB）—— 本次改动不新增动态分配

---

## 8. 写给下一个改这段代码的人的注意事项

1. **滑杆类控件必须两段式落盘**：`Pressed` + 每次 `Pressing` 都回调，一次拖动就是 5~6 个不同值 ⇒ 不延迟就是擦 5~6 次扇区。
2. **两段式的 `Set` 端不能有裸的"值相同直接 return"** —— Preview 已经把值设成最终值了，松手时传的正是同一个值。必须有 `dirty` 标志之类的旁路。
3. **`PhoneApps_Command(SETTINGS,"sync")` 不是无害的**：它会调到 `app_show` 复位滚动位置。拖动中发它会让页面弹回顶部、坐标全错。已在 `settings.c` 拆成 `settings_sync_controls(reset_scroll)` 区分。
4. **亮度下限是 1 不是 0**（config 侧钳制与滑杆 range 必须一致），否则 `brightness=0` = 全屏死黑。
5. **钳制 `s_cur` 后必须重算 `s_cur.crc`**，否则下次 Save 写出的 blob CRC 自相矛盾，重启后整套设置全丢。
6. **`main.c` 在 `PhoneShell_BoardInit` 之前会调 `PhoneHost_SetBrightness`**，那时 `ctx == NULL`，别在库里无条件解引用 `ctx->root`。
7. **验收脚本一律用固件现算坐标**，别按算式推、也别沿用旧布局的实测值。`brightness_check.py` 每段拖动都重读几何 —— 这样任何"页面被复位"的固件回归都会立刻暴露，而不是悄悄点空。
8. **`brightness_check.py` 的失败信息已内建三段判读**：`sld_press=0` ⇒ 没命中；`press>0 change=0` ⇒ wrapper 转了但 Slider 没回调；`change>0 commit=0` ⇒ 值变了没落盘。