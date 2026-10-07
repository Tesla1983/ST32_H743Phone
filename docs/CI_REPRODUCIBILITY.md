# CI 与构建可复现性

结论先行：**GitHub Actions 编出来的固件，与板上正在跑的那份逐字节相同。**
不是"编译通过了"，而是 `CI 产物 == 本地产物 == 机内 flash`，1 576 644 字节一个字节不差。

## 1. 工作流做了什么

文件：`.github/workflows/firmware-build.yml`，触发条件 `push main` / `pull_request` / 手动。

| 步骤 | 作用 | 失败意味着 |
| --- | --- | --- |
| 构建输入完整性 | `ci/check_build_inputs.py` 检查 `gb2312_glyphs.bin`、`pinyin_gb2312.bin` 存在且字节数精确（982016 / 116350） | 有人改了 `.gitignore` 或漏传了"反向例外"的两个构建输入 |
| 装工具链 | 钉死 **Arm GNU Toolchain 14.3.Rel1**，官方 tar.xz 直下 + 缓存 | 下载源变了 |
| Configure / Build | `cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake -G Ninja` → `cmake --build build --target ymgui-bin` | 干净环境下编不出来（依赖退化） |
| 可复现性闸门 | `ci/check_firmware_hash.py build ci/firmware_reference.sha256` | **编出来的不是板上那份**，详细见 §3 |
| 上传产物 | elf / bin / hex / map，保留 30 天 | — |

参考哈希存在 `ci/firmware_reference.sha256`，它代表的是**板上正在跑的固件**（确立方法见 §2）。

## 2. 参考哈希是怎么确立的（可复跑）

```bash
# ① 干净重建，确保产物确实来自当前源码、不是陈年残留
rm -rf build_ci
cmake -S . -B build_ci -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake -G Ninja
cmake --build build_ci --target ymgui-bin

# ② 把 STM32H743 的 flash 全量读回来（约 1 分钟），与 .bin 逐字节比对
python tools/verify_board_image.py build/ymgui-h743.bin

# ③ 通过后再把新哈希写进参考文件
sha256sum build/ymgui-h743.bin build/ymgui-h743.elf
#  → 手工填进 ci/firmware_reference.sha256，并在 commit message 里写清为什么更新
```

中间那一步是关键：`tools/verify_board_image.py` 用 SWD 从 `0x08000000` 读 1 576 644 字节回来，
和本地 `.bin` 做逐字节 diff，不一致就打印**第一个不同的偏移**和哪些 4 KB 块有差异。
不支持"看起来差不多"这种判据。

## 3. 两轮实测：踩到的两个"跨主机会不一致"

### 第一轮：`bin` 一致、`elf` 差 20 字节 → 红灯

CI 输出：

```
[OK  ] ymgui-h743.bin  sha256=22ec63a8…  size=1576644
[DIFF] ymgui-h743.elf  size=1660416      （期望 1660436）
```

`readelf -SW` 逐节对比后，差异精确定位到一处：

| 节 | CI | 本地 |
| --- | --- | --- |
| `.debug_line_str` | 461 B | 483 B |
| 其余所有节（`.text` `.data` `.bss*` `.ARM.attributes` `.debug_frame`） | 完全一致 | 完全一致 |

把 `.debug_line_str` 打印出来就真相大白了 —— 里面根本不是本项目的源码路径，而是
**Arm GNU Toolchain 自带的预编译库 `libgcc` / `libc_nano`，记录它们在 ARM 编译服务器上被构建时的路径**：

```
Windows 版工具链：/data/jenkins/workspace/GNU-toolchain/arm-14-5/build-arm-none-eabi/obj/gcc2/...
Linux  版工具链：/data/jenkins/workspace/GNU-toolchain/arm-14/build-arm-none-eabi/obj/gcc2/...
```

同一个 release（14.3.Rel1），但两个 host 包是两次 Jenkins 任务编出来的，路径差 22 字节。
这部分**编译期 `-ffile-prefix-map` 覆盖不到**（库是预编译的），只能在链接期丢掉。

### 修复：`-Wl,--strip-debug`

写在 `CMakeLists.txt` 的 `target_link_options` 里（注释就在那里）。

为什么用 `--strip-debug` 而**不是** `-s`：
`tools/bench.py`、`tools/read_vars.py` 等 SWD 诊断脚本靠 `arm-none-eabi-nm` 从 `.symtab`
解析 `g_loop_count` / `g_cfg_brightness` 这类诊断量地址。`-s` 会把符号表一起剥掉，那些脚本就全瞎了。
只删 `.debug_*`，符号表留着。

代价：ELF 从 1 660 436 字节降到 1 657 436 字节（-3000），`.debug_frame` 没了，
意味着无法用它做栈回溯 —— 本项目本来也没有源码级调试会话，无所谓。

**验证：重编后 `.bin` 的哈希**纹丝不动**（`22ec63a8…`），证明剥离调试节没动到一条指令，
板上固件完全不受影响；只有 `.elf` 的参考哈希跟着更新。

### 第二轮：全绿

```
可复现性检查：2 项
  [OK  ] ymgui-h743.bin  sha256=22ec63a893c1b4d3f0eed19734a130cab98b5fe94e20c83237ae89e29ad09c22  size=1576644
  [OK  ] ymgui-h743.elf  sha256=a1fba87ba112717c45426a0766d71dc5f1ad41e48a8187ee6d3ce5e6af4ab25e  size=1657436
[PASS] 2/2 项字节级一致 —— CI 产物与板上固件相同。
```

## 4. 端到端闭环

CI 的 artifact 下载下来后，直接拿它对板：

```
cmp .ci_artifact_ci/ymgui-h743.bin build/ymgui-h743.bin   → 完全相同
cmp .ci_artifact_ci/ymgui-h743.elf build/ymgui-h743.elf   → 完全相同
python tools/verify_board_image.py .ci_artifact_ci/ymgui-h743.bin
  → [PASS] 机内 flash 与 .ci_artifact_ci/ymgui-h743.bin 完全一致（1576644 字节）
```

三条边都实测过了：**GitHub CI ⇄ 本地 ⇄ 板上**，任意两边都字节一致。

## 5. 什么时候更新参考哈希（写清规则，避免"变红了就顺手改哈希"）

只允许这三种情况，且每次都要在 commit message 里写原因：

1. **有意为之的功能改动**导致固件内容变化 —— 新版本落成新基准
2. **工具链版本升级** —— 必须同步改 `.github/workflows/firmware-build.yml` 里的 `TOOLCHAIN_VERSION`
3. **构建参数调整** —— 优化等级 / 宏开关 / 链接选项

如果"没人改代码却变红了"，那不是哈希的问题，是**环境漂移**，要去查环境。
最常见的原因是有人在本机装了新版本的工具链，导致自己编出来的和 CI 不一样 —— 看 §3 就知道该查什么。

## 6. 已知的"故意不比"的东西

- **`.map` 文件**：里面全是绝对路径（本机的 `E:/ymgui-h743`、CI 的 `/home/runner/work/...`），
  必然不一致，所以不参与哈希比对。链接器 map 是给人看的，不是给机器做一致性校验的。
- **`CMakeLists.txt` 里 `-Wl,-Map=${ROOT}/build/ymgui-h743.map` 写死了 `${ROOT}/build`**，
  所以 CI 里构建目录必须叫 `build`，改名会让 map 跑到别的地方去（不影响固件内容，但会误导排查）。
