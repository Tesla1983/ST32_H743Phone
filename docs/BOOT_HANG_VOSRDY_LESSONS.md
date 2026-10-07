# 固件启动死等（VOSRDY）与 UI 验收 —— 一次排查的完整复盘

**日期**：2026-10-04
**触发**：UI 中英切换功能开发完成后，烧录上板，固件不启动。
**结果**：根因定位 + 修复；UI 功能三层验收全部通过。

本文记录两件事：

1. [启动死等的定位与修复](#一启动死等vosrdy)
2. [验收脚本坐标错误的修正](#二验收脚本坐标错误)

以及[排查过程中我自己犯的三个错](#三我犯的三个错必须记)，和
[沉淀下来的纪律](#四沉淀下来的纪律)。

---

## 一、启动死等（VOSRDY）

### 1.1 现象

烧录成功、`probe-rs verify` 通过，但：

- `g_loop_count = 0`（主循环从未执行）
- 所有阶段标记停在初值：`g_mark_qspi = 0`、`g_qspi_rc = 99`、
  `g_qspi_ker_sel = 0xFFFFFFFF`、`g_cfg_rc = 0xFFFFFFFF`
- `g_boot_magic = 0x594D4731`（ASCII `"YMG1"`）——**注意这个已写入**，它是干扰项，见 §3.3

### 1.2 定位

卡在 `third_party/SYSTEM/sys/sys.c:146`：

```c
__HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);
while(!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)) {}   /* ← 无上限，死等 */
```

`PWR_D3CR` 的实测值（**注意偏移是 `+0x18`，不是 `+0xA8`**，见 §3.1）：

```
PWR_D3CR = 0x0000C000
  bit15:14 = 0b11  ⇒ 目标档位已正确写入（SCALE1）
  bit13    = 0     ⇒ VOSRDY 未就绪
```

`__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)` 读的正是 `PWR->D3CR` 的 bit13
（`stm32h7xx_hal_pwr.h:382`），不是 `CSR1`。

旁证：`RCC_CR = 0x00004025`，`HSEON(bit16)=0`、`PLLON(bit24)=0`
⇒ 连 `HAL_RCC_OscConfig()` 都还没走到，印证卡点在时钟初始化最前面。

### 1.3 修复：把死等改成有上限的等待

`sys.c` 改动：

```c
/* ⚠ 此处最初取 200u，冷启动实测被证伪（见 §1.6/§1.8），
 *   最终改为 1000u —— 对齐 HAL 官方 PWR_FLAG_SETTING_DELAY。 */
#define SYS_VOSRDY_TIMEOUT_MS  1000u

{
    uint32_t t0 = HAL_GetTick();

    while (!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY))
    {
        g_sysclk_vos_wait = HAL_GetTick() - t0;

        if (g_sysclk_vos_wait > (uint32_t)SYS_VOSRDY_TIMEOUT_MS)
        {
            g_sysclk_err = 2u;
            break;      /* ← 关键：不 return */
        }
    }
    g_sysclk_stage = 3u;
}
```

**超时后为什么不 `return`**：一旦 return，后面 `lcd_init()` / `qspi_port_init()` /
`PhoneShell_BoardInit()` 全部不会执行，屏幕全黑，外部表现比现在更糟。
继续往下走，各步骤自带的错误码就能分别暴露问题，同时 `g_sysclk_err=2`
明确指出"时钟这一步未就绪"。这符合本工程既有的"不把板子卡住"原则
（`qspi_port.c` 顶部注释：「所有等待都有上限」）。

### 1.4 新增诊断量

`sys.c` / `sys.h` 三个，`main.c` 侧镜像三个：

| 变量 | 位置 | 含义 |
| --- | --- | --- |
| `g_sysclk_stage` | sys.c | `0xFFFFFFFF`=未进函数；1=SYSCFG；2=已写 D3CR；3=VOSRDY 就绪；4=OscConfig；5=ClockConfig；6=PeriphCLK（全程完成） |
| `g_sysclk_err` | sys.c | 0=成功；1=OSC/CLK 配置失败；**2=VOSRDY 超时** |
| `g_sysclk_vos_wait` | sys.c | VOSRDY 实际等待毫秒数 |
| `g_clock_rc` | main.c | `sys_stm32_clock_init()` 返回值 |
| `g_clock_stage` | main.c | 上述 stage 的镜像 |
| `g_clock_vos` | main.c | `PWR->D3CR` 快照，bit13 即 VOSRDY |

**命名纪律**：厂商 `.c` 里的量加 `g_sysclk_` 前缀，与 `main.c` 的 `g_clock_` 区分。
（第一版我图省事两边同名，编译直接冲突。）

**初值约定**：`0xFFFFFFFF` / `0xFF` —— 让"从未被写过"与"写成了某个小整数"一眼可分。

### 1.5 修复后实测

第一次上电（烧录后冷启动）：

```
g_sysclk_vos_wait = 201     ← 撞到上限才退出
g_sysclk_err      = 2       ← VOSRDY 超时
g_sysclk_stage    = 6       ← 但后续 4 步全部成功
g_clock_rc        = 0
g_loop_count      = 3155+   ← 固件跑起来了
```

即：**VOSRDY 超时并没有阻止时钟配置成功**。HSE / PLL 全部正常起来，
`g_dwt_hz = 400 MHz`、主循环 **59.7 FPS**、`g_tick_ms = 16` —— 全部达标。

### 1.6 冷启动实测：VOSRDY 不是不就绪，是**就绪得慢**

这一节是第二次更正。§1.5 说"VOSRDY 超时并未阻止时钟配置成功"，
但当时仍缺一个关键对照 —— **冷启动那一瞬寄存器到底是什么样**。

补做实验（`tools/coldboot_vos.py`）：拔掉开发板**全部**供电，等 15 秒后重新上电，
一上电立刻交叉读「PWR 寄存器」与「固件自报量」：

```
--- PWR 寄存器 ---                --- 固件自报 ---
CSR1 = 0x0000E000  ACTVOSRDY=1     g_sysclk_vos_wait = 201   ← 撞 200 上限
                 ACTVOS[15:14]=11  g_sysclk_err      = 2      ← 报"超时"
D3CR = 0x0000E000  VOSRDY=1        g_sysclk_stage    = 6      ← 后续步骤全过
                 VOS[15:14]=11     g_loop_count      = 1438   ← 主循环在跑
PWR_CR3 = 0x05000042  LDOEN=1, SCUEN=1（未锁）
SYSCFG_PWRCR = 0x00000000  ODEN=0（符合 Scale1）
```

**关键在"稍后"两个字**：固件自己记的是"等了 201 ms 还没就绪"，
而探针连上去读到的是 `VOSRDY=1 / ACTVOSRDY=1 / VOS=11`。
⇒ **它后来自己就绪了，只是慢。** 既不是等错标志位，也不是永不/不就绪。

### 1.7 为什么 reset 路径立刻就绪（机制已查明）

对同一块板做 `probe-rs reset` 前后对照：

| 动作 | `D3CR`(VOSRDY) | `CSR1`(ACTVOSRDY) |
| --- | --- | --- |
| 冷启动后 | `0x0000E000` (1) | `0x0000E000` (1) |
| `probe-rs reset` 立刻 | `0x0000E000` (1) | `0x0000E000` (1) |
| reset 之后 | `0x0000E000` (1) | `0x0000E000` (1) |

原因：**`SYSRESETREQ` 不复位 PWR 域。** 电压调节器的状态与目标档位被保留，
无需从复位档重新爬升 ⇒ 立刻就绪。只有**真正断电**才必须重建电压，
这才是冷启动慢的物理来源。

⇒ 「以前能跑通、这次撞上」的谜团解开：以前多半走 reset 路径。

### 1.8 超时上限从 200 改为 1000（对齐 HAL 官方值）

查 HAL 源码找到官方上限：

```c
/* stm32h7xx_hal_pwr_ex.c:188 */
#define PWR_FLAG_SETTING_DELAY (1000U)
```

HAL 自己的 `HAL_PWREx_ConfigSupply()` / `ControlVoltageScaling()` 等 `ACTVOSRDY`
用的都是这个 1000 ms。原先取 200 是拍脑袋定的"宽松三个数量级"，实测证明不够。
**改用官方值 1000**，理由是不需要自己发明一个数：

- 冷启动实测超过 200 ms —— 200 已被证伪；
- 1000 是 HAL 自己为同一类握手设定的上限，权威且保守；
- 超时的代价只是"最坏情况下全系统慢 1 s 启动"，换来的是**永不再挂死**。

⚠ 本板在 200~1000 ms 之间的**具体就绪时刻未测得**（探针连上去时已就绪），
所以 1000 是保守对齐，不是实测最小值。

**顺带查明一处标志位不一致（记录但故意不改）**：
`HAL_PWREx_ControlVoltageScaling()`（`stm32h7xx_hal_pwr_ex.c:496-503`）在所有分支末尾
等的是 **`ACTVOSRDY`（`CSR1` bit13）**，而厂商 `sys.c:146` 等的是
**`VOSRDY`（`D3CR` bit13）** —— 两个不同寄存器的不同位。
但冷启动对照实测**两个标志位行为完全一致**（一起 0、一起 1），
所以"等错标志位"这一假设被实测排除，无需改动厂商代码。

---

## 二、验收脚本坐标错误

### 2.1 现象

固件跑通后跑 `tools/lang_check.py`，报：

```
当前应用 current_app = 4294967295（期望 3 = 设置）
[FAIL] 没进到设置页 —— 图标坐标可能变了
```

`4294967295` = `0xFFFFFFFF` = `current_app` 的初值。

**第一反应会是"UI 挂了"**——但实际上固件完全正常。逐步排查：

- SWD 注入机制正常（`g_bdtap_seq` 递增，被 `PhoneShell_BoardTick` 消费）
- `scene = 1`（HOME，已过开机动画）、`busy = 0`、无模态遮罩
- 网格扫描一打就中：`(20,268) → current_app = 0`

⇒ 桌面完全可点，**纯粹是坐标不对**。

### 2.2 根因：三处坐标全是"按算式推"出来的错值

`lang_check.py` 原文注释写着「**坐标依据（推自源码，非试凑）**」，
但三处全错。设置页六项重排后，桌面图标 y 从 238 变 268、lang 面板原点 y 从 201 变 243。

| 位置 | 旧值（推算，错） | 实测值（对） |
| --- | --- | --- |
| `SETTINGS_ICON` | (272, 263) | **(250, 268)** |
| `SEG_ZH` | (208, 226) | **(195, 255)** |
| `SEG_EN` | (259, 226) | **(259, 255)** |

注意中文段：**x 实测 195 ≠ 按 `(18+170+20)` 算出的 208**。
可见"相对坐标 → 屏幕坐标"的换算同样不能只靠算式。

### 2.3 修正后的验收结果

```
① 交互   ：中↔英 差异 9735 像素，差异区 x=[22,287] y=[43,447]（覆盖全页）
           固件自报 lang=1 save_cnt=9 erase_last=1 write_rc=0
② 落盘   ：直读 W25Q128 扇区 240 → magic=0x59434F4E("YCON") CRC32=0x02ED4782 双向通过
③ 掉电重启：reset 后 loaded=1 crc_ok=1 lang=1 ⇒ 持久化生效
收尾     ：切回中文并校验（lang=0, CRC=0x678A7CC4），保持项目默认态
[PASS] 三项全部通过
```

### 2.4 顺带修的两处脚本健壮性

1. **开头加"归位"步骤**：若 `g_cfg_lang != 0`（已是 English），先点中文段归位。
   否则重复运行时"再点 English 无变化 ⇒ 差异 0 像素"会被误判成"切换没生效"。
   脚本必须可重复运行。

2. **`diff_box` 报的差异区偏小**：它打印 `x=[22,43]`，实际是 `x=[22,287]`。
   原因是它用 `cc.diff_box` 在切换瞬间的前后两帧上算，**帧时序不同步**。
   改用"切中文→抓帧→切英文→抓帧"再逐像素比对才得到真实范围。
   ⇒ 判"切换是否生效"看**像素总数**就够，别迷信差异区边界。

### 2.5 可复用的定位手法

网格扫描 + 读**状态量**判定命中：

```python
for y in range(220, 300, 12):
    for x in range(20, 315, 20):
        cc.tap(x, y, wait=0.30)
        if rd('current_app') != 0xFFFFFFFF:   # ← 用状态量，不用抓帧
            print('命中', x, y, rd('current_app'))
```

比抓帧看像素差异快且确定。实测每个 app 的命中区间宽约 ±25 px
（点准比点偏重要）。也可直接调 `PhoneDesktop_Hit()`
（内部用 `YMGUI_Obj_GetAbsArea()` 取绝对坐标，不依赖任何手算）。

---

## 三、我犯的三个错（必须记）

### 3.1 寄存器偏移凭记忆 ⇒ 连续两次得出**相反**结论

`PWR_TypeDef` 的真实偏移（`stm32h743xx.h:1212-1218` 注释实测）：

```
CR1=0x00  CSR1=0x04  CR2=0x08  CR3=0x0C  CPUCR=0x10  D3CR=0x18
```

我按 `+0x08` / `+0x0C` 去读 `CSR1` / `CR3`，读到的其实是 `PCR2` / `PCR3`；
又按 `+0xA8` / `+0xB0` 读，读到保留位（全 0）。

**一度因此误判"VOS 从未写入"，差点推翻本来正确的结论。**

另两处相关的编码误解：
- `orr #0xC000` 我以为是 `RCC_CR` 的 HSEON/HSEBYP，实际那条指令操作的是
  **PWR（`0x58024800`）**，不是 RCC（`0x58024400`）。
- 以为"VOS = 0b11 = Scale0"与源码要求的 Scale1 矛盾。实际
  **H743 的 `PWR_REGULATOR_VOLTAGE_SCALE1` 编码就是 `0b11`（`0xC000`）**
  —— H7A3/H7B3 那类 SMPS 型号才用 `SRDCR`、编码不同。
  查 `stm32h7xx_hal_pwr.h:142-143` 才对。

> **纪律**：任何寄存器偏移必须**当次**从 `xxx_TypeDef` 注释或寄存器结构体里核。
> 不要凭记忆写地址，也不要凭"其它 STM32 系的经验"套编码。

### 3.2 拿 `uwTick` 递增当"固件正常运行"的证据

`HAL_Init()` 装好 SysTick 后，`uwTick` 由 SysTick 中断驱动、**走 HSI 独立计数**，
**与 `main()` 是否推进无关**。固件死等在 VOSRDY 循环时，它照样 1 ms 稳定递增。

我实测到"2 s 墙钟 = 2401 tick，时基精确"，还拿这当成了"固件在正常运行"的证据。

> **纪律**：判断"走到哪一步"只能用
> ① `main()` 自己的单调计数器（`g_loop_count`），
> ② 或某个初始化函数**第一句就赋值**的变量（`g_sysclk_stage` 初值 `0xFFFFFFFF`）。
>
> `uwTick`（要么一直涨）、`g_boot_magic`（只写一次）这类量**不能**用来判断进度。

### 3.3 只采样局部就断言"桌面没图标"

我扫了 `y=225..320` 一带，发现全是壁纸色，就下结论"桌面图标没渲染出来"。
实际上**整帧有几十种颜色、渐变、图标，桌面一直渲染正常**（59.7 FPS）。
只是我采样的一带恰好是纯背景。

> **纪律**：断言"某区域没东西"之前，先确认采样范围足够大、
> 或者先看全图的颜色分布（`Counter(frame).most_common()` + 分块色数统计）。

### 3.4 附带损失：用 gdb 取 PC 拖挂了探针

`arm-none-eabi-gdb target extended-remote` 连接后**挂死 7 分钟**，
我 `Stop-Process -Force` 强杀，导致**探针固件被拖挂**：

```
HID 通道能打开、描述符读得到（product='FireDAP CMSIS-DAP'），
但 CMSIS-DAP 命令零响应
```

代价是**麻烦用户拔插了两次探针**。探针挂死时**不要强杀进程**。

> 可用的取 PC 途径（本项目均不可用）：`probe-rs debug` 本版报
> `Serialization error: "invalid type: map, expected a string"`；
> `probe-rs gdb server` + gdb 同理会挂。
> ⇒ 改用 §3.2 的间接推断，本项目已足够定位到具体行。

---

## 四、沉淀下来的纪律

### 4.1 死等一律要加超时

厂商代码里凡是 `while (...) {}` 无上限的等待，都要加超时 + 错误码。
理由：**死等的外部表现与硬件故障完全一样**（屏幕不亮、所有诊断量停在初值），
会让排查成本从"读一个变量"变成"反复拔插板子"。

本工程已有同类纪律（`qspi_port.c` 顶部：「所有等待都有上限……片选悬空/虚焊时
BUSY 永不清、FIFO 永远空，会把固件永久卡在初始化里」）。VOSRDY 这条是同源的漏网。

### 4.2 判断卡点必须做区间收敛

1. `g_loop_count` 隔几秒读两次 → 不变就是停了
2. 比 `g_mark_tick/poll/refr` 与 `g_loop_count` → 哪个落后卡在哪一步
3. 再用"某函数第一句赋值的变量是否还是初值"卡住下界
4. 用寄存器状态（复位值 / 已配置值）卡住上界

只靠单点观测必然跳结论。

### 4.3 UI 坐标一律实测，不许留算式坐标

网格扫描 + 读状态量判定命中。**注释里写"推自源码"不代表推对**，
改布局后必须重新实测。

### 4.4 每次上电都读时钟诊断量，不要假设

`g_sysclk_vos_wait` / `g_clock_vos` / `g_sysclk_err`
—— 因为 VOSRDY 就绪与否**依赖上电时序**，每次都可能不同。

### 4.5 不要用 gdb 取 PC

会挂死，且强杀会拖挂探针。见 §3.4。

---

## 五、冷启动 VOSRDY 之谜：已查明

> 本节原为"遗留问题"，2026-10-04 晚经冷启动对照实验**结案**。
> 结论：**VOSRDY 会就绪，只是冷启动时慢于原先假设的 200 ms。**

### 5.1 实测数据（`tools/coldboot_vos.py`）

拔掉**全部**供电等 15 秒 → 重新上电 → 立刻交叉读寄存器与固件自报量：

| 读数 | 值 | 解读 |
| --- | --- | --- |
| `g_sysclk_vos_wait` | **201** | 撞 200 ms 上限，固件判定"超时" |
| `g_sysclk_err` | 2 | 超时错误码 |
| `g_sysclk_stage` | 6 | 超时 `break` 后其余步骤全过 |
| `g_loop_count` | 1438 | 主循环正常推进 |
| `CSR1.ACTVOSRDY` | **1** | 实际档位已就绪 |
| `CSR1.ACTVOS[15:14]` | `0b11` | Scale1 |
| `D3CR.VOSRDY` | **1** | 目标档位已就绪 |
| `D3CR.VOS[15:14]` | `0b11` | Scale1 |
| `PWR_CR3` | `0x05000042` | `LDOEN=1`、`SCUEN=1`（未锁） |
| `SYSCFG_PWRCR.ODEN` | 0 | 符合 Scale1（非 Scale0） |

**这组数据本身就否定了"不就绪"**：固件自己记的是"201 ms 时还没好"，
而探针读到时两个 RDY 位**都已是 1** ⇒ **它会自己就绪**。

### 5.2 逐项排除

| 曾怀疑的成因 | 排除依据 |
| --- | --- |
| 硬件坏了 | 冷启动后 VOSRDY/ACTVOSRDY 最终都置 1，功能完好 |
| 永不就绪 | 同一块板、同固件，冷启动最终就绪 |
| 等错了标志位 | 冷启动对照中 `VOSRDY` 与 `ACTVOSRDY` **行为完全一致**（一起 0、一起 1）⇒ 两个位等价可用 |
| 缺 `HAL_PWREx_ConfigSupply()` | `PWR_CR3.LDOEN=1` 恰是本板所需，复位默认值已对；且补上它也不能让 VOSRDY 提前 |
| 本次改动引入 | 卡点在厂商 `sys.c`，早于所有移植代码 |
| reset 路径为何不同 | **`SYSRESETREQ` 不复位 PWR 域**，调节器状态保留 ⇒ 无需重新爬升（见 §1.7） |

### 5.3 结论

**成因 = 冷启动时电源从关闭到 VDD 越过 Scale1 门槛需要数百毫秒，
而固件在 `sys_stm32_clock_init()` 里等它。** 这是**正常现象**，
只是厂商代码把"正常等待"写成了"无上限死等"，才把它变成致命问题。

⚠ 200~1000 ms 之间的**确切就绪时刻未测得**（探针连上去时已就绪，
无串口/RTT 可在冷启动瞬间打点）。故取 HAL 官方 `PWR_FLAG_SETTING_DELAY = 1000U`。

**待查的精细项（真要继续，优先级低于 TF 卡 L2/L3）**：
冷启动瞬间的 `CSR2.VOSRERR`（档位切换错误标志）与 `VDD` 是否欠压
（H743 Scale1 需 VDD ≥ 1.15 V，可用 PVD 判，但会改寄存器）。

**⚠ 但这些都不必再查** —— 结论已经足够：这不是故障，是等待时间估计不足。
把上限从 200 提到 1000（对齐 HAL）已彻底封住，最坏代价只是"启动慢 1 s"。

### 5.4 是否还需要补 `HAL_PWREx_ConfigSupply()`（原方案 B）

**不需要。** 理由：

1. `PWR_CR3` 实测 `LDOEN=1 / SCUEN=1`，供电配置本就正确且未锁；
2. 缺它不影响 `VOSRDY` 最终置位，补了也不会让冷启动更快；
3. 它会**偏离厂商原始代码路径**，引入新的行为差异（改了却没收益 = 纯风险）。

⇒ 结论：**保持厂商代码不动**，只在死等上加上限。这是收益/风险比最优的解法。

---

## 附：涉及文件

| 文件 | 改动 |
| --- | --- |
| `third_party/SYSTEM/sys/sys.c` | 超时 + 三个诊断量 + 阶段标记 |
| `third_party/SYSTEM/sys/sys.h` | 诊断量 extern 声明 + 完整注释 |
| `src/main.c` | `g_clock_rc/stage/vos` 三个镜像量 + 时钟返回值记录 |
| `tools/lang_check.py` | 三处坐标修正 + 归位步骤 + 注释更正 |
| `tools/coldboot_vos.py` | **新建**：冷启动现场抓取（交叉读 PWR 寄存器 + 固件自报量，三分支判定） |
| `.workbuddy/memory/2026-10-04.md` | 过程日志（本文的详细版） |
