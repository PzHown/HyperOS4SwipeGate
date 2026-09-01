# Technical Notes

本文记录 HyperOS4 SwipeGate 当前 0.8.2 系列的逆向目标、Hook 策略、运行链路与安全约束。普通使用请查看项目根目录的 [README](../README.md)。

## 目标环境

当前目标兼容范围：

- HyperOS 4 System Launcher 8.0+
- Android 17 / API 37
- arm64-v8a
- Launcher：`com.miui.home`
- SystemUI：`com.android.systemui`
- Launcher 进程入口：`/system_ext/bin/hyos_spawner`
- native 库：`libapp_launcher.so`
- LSPosed Modern API 102：`java_init` + `native_init`

已明确测试：

- `RELEASE-8.01.02.5459-260807-08242024-R`
- `RELEASE-8.01.02.5465-260807-08262034-R`
- `RELEASE-8.01.02.6174-260818-08281208-R`
- `RELEASE-8.01.02.6179-260818-08292132-R`

模块不会根据 `versionName` 做硬编码放行，也不会直接使用固定 RVA 安装 Hook。所有固定地址只作为逆向与诊断参考。

## 必需作用域与 Rootless 控制链路

必须同时启用：

```text
com.miui.home
com.android.systemui
```

职责：

- `com.miui.home`：Launcher / HyOS native 目标、手势、进度、触觉与 break-open Hook。
- `com.android.systemui`：`SystemUiBridgeModule`，负责 App 与 HyOS Runtime 的实时配置和状态中继。

实时控制链路：

```text
SwipeGate App
   ↓ private query / control
SystemUiBridgeModule in com.android.systemui
   ↓ Xiaomi com.android.systemui.fsgesture carrier
HyOS / Launcher native runtime
   ↓ control receiver
Native hooks
   ↓ authenticated reply
SystemUiBridgeModule
   ↓ private reply
SwipeGate App
```

不需要 `su`、`setprop` 或 `resetprop`。RemotePreferences 与 Launcher cache 继续保留作为兼容和持久化辅助通道。

控制消息当前可携带：

```text
threshold
logLevel
haptic
breakOpen
nonce / version handshake
```

native 端会回报当前加载的 `SWIPEGATE_VERSION_CODE`，App 据此区分“已连接但旧 native 尚未重启”和“已加载当前版本”。

## 主 Hook：GestureInputBackHelper::on_swipe_process

主目标：

```text
GestureInputBackHelper::on_swipe_process
```

历史参考 RVA：

```text
5459: libapp_launcher.so + 0x816fc4
6174: libapp_launcher.so + 0x657080
```

这些 RVA 不参与生产定位。

当前定位策略：

```text
libapp_launcher.so loaded
        ↓
解析 mapped ELF / DT_JMPREL / PLT imports
        ↓
以 MotionEvent imported API 调用图寻找候选
        ↓
验证 AArch64 frame family 与行为结构
        ↓
semantic candidate 唯一 → 接受
        ↓
与 gesture-frame-v1 / gesture-frame-v2 exact fingerprint 交叉验证
        ↓
冲突、0 candidate、multiple candidates → fail closed
```

详细规则见 [SEMANTIC_RESOLVER.md](SEMANTIC_RESOLVER.md)。

## Exact Pattern fallback

当前 exact fingerprint 名称：

- `gesture-frame-v1`
- `gesture-frame-v2`

它们是行为实现的已验证代码特征，不是 Launcher 版本白名单。

处理规则：

1. semantic 唯一，exact 无结果：接受 semantic
2. semantic 与 exact 唯一且地址一致：接受
3. semantic 与 exact 指向不同地址：拒绝安装
4. semantic 无法唯一确认，exact 唯一：允许 exact-authoritative fallback
5. 两边均不能唯一确认：拒绝安装

6179 实机曾出现 `semantic-conflict-exact-authoritative`，当前 resolver 会在已验证 exact fingerprint 唯一且完整时采用保守 authoritative 路径，而不是猜测新的 RVA。

## ABI-transparent wrapper

AArch64 wrapper 保存：

- `x0..x8`
- `q0..q7`
- `x30`

读取：

- `w1`：`readyFinish`
- `w2`：`side`
- `s0`：horizontal distance

wrapper 调用 `swipegate_hook_enter_and_gate()` 后恢复寄存器，再调用 LSPosed trampoline。这样不再依赖旧版本中 Point 参数是指针还是拆分浮点的 C++ prototype 差异。

自动兼容仍要求关键参数保持当前 AAPCS64 约定。

## Launcher 8.x 原厂进度坐标

后续对 Launcher APK 的直接逆向确认，原厂 `88 dp` 不只是一个侧边栏布尔阈值。

共享函数：

```text
BackGestureUtils::convert_offset
```

已知 RVA：

```text
5459: 0x773814
6174: 0x60bb80
```

6174 直接调用点包括：

```text
GestureStubViewWindow::handle_back_gesture
GestureInputBackHelper::on_swipe_process
GestureStubViewWindow::on_swipe_stop_direct
GestureBackArrowView::on_vsync
```

核心尺度：

```text
stock total coordinate ≈ 110 dp
READY progress = 0.8
110 dp × 0.8 = 88 dp
```

`on_swipe_process` 会把 `convert_offset(distance) / 20` 与 `0.8` 比较；`GestureBackArrowView::on_vsync` 和 Release 路径也复用同一换算。因此只在主 Hook 中把距离冻结到 88dp 附近会造成动画进度与自定义阈值脱节。

## 自定义阈值：共享进度缩放

0.8.2-beta2 起，首选策略不再是“88dp 后冻结距离直到用户阈值”。

当自定义门槛 `D > 88dp` 时，SwipeGate Hook `BackGestureUtils::convert_offset`，把输入映射为：

```text
mappedDistance = rawDistance × 88 / D
```

然后调用小米原函数。

这等价于把原厂 `110dp` 总坐标动态扩展为：

```text
D / 0.8 = D × 1.25
```

因此：

```text
rawDistance = D
→ mappedDistance = 88dp
→ Xiaomi progress = 0.8
→ READY 与用户门槛重合
```

优势：

- 原始手势距离继续传给 `on_swipe_process`，不破坏其他 raw distance / velocity 计算
- Ready 前动画连续，不再长期卡在约 0.8
- 到门槛时不会从 0.8 突跳到高进度
- Ready 之后仍使用 Xiaomi 自己的 nonlinear easing
- `on_vsync`、Release 和状态判断重新共享同一个进度坐标

### convert_offset resolver

不会硬编码上述 RVA。当前从已经解析成功的 `on_swipe_process` 内部反推 `convert_offset`：

- 扫描约 `0x900` 字节 caller 区域
- 候选必须由直接 `BL` 调用
- 调用后紧跟 `fmov s1, #20.0`
- 候选函数体必须出现已验证的 `s0 → s8` 输入保存、负值 guard 与 `110.0f` 常量结构
- 至少 3 个 caller corroboration
- 只能有 1 个 qualified candidate

5459 与 6174 的 APK 静态分析均满足该结构。

如果该 resolver 失败，模块不会猜测函数地址，而是保守回退到旧的 88dp clamp 逻辑。

## Legacy clamp fallback

只有进度 Hook 未成功安装时，主 gate 才保留旧 fallback：

```text
if customThreshold > 88dp
and rawDistance < customThreshold
and rawDistance > stockBoundary:
    effectiveDistance = just below 88dp
else:
    effectiveDistance = rawDistance
```

此路径主要用于 fail-safe，不再是 0.8.2-beta2 的首选动画方案。

## Haptic V2：Ready 触觉与 Release 去重

Launcher 8.0 的原生 Back Release 使用 HyperRT：

```text
get_global_runtime
→ HapticFeedback_perform_ext_haptic_feedback(..., constant=0)
→ Runtime_dec_strong
```

SwipeGate 在进入第一段 Ready 时，通过解析出的原生 HyperRT bridge 主动执行一次同类 `constant=0` 反馈。

### 真实 Release callsite

6179 运行时 trace 与 6174 静态逆向共同确认：

```text
GestureStubViewWindow::handle_back_gesture
HapticFeedback_perform_ext_haptic_feedback callsite RVA 0x654298
```

生产实现不简单硬编码该 RVA，而是验证其周边结构：

```text
bl get_global_runtime
mov x22, x0
...
sub x0, x29, #0xe8
mov w1, wzr
bl HapticFeedback_perform_ext_haptic_feedback
mov x0, x22
bl Runtime_dec_strong
```

只有唯一候选才发布 `stock-back-release-haptic-v1`。

### 750ms 去重规则

```text
Ready → Release < 750ms
    suppress stock Release HyperRT call

Ready → Release >= 750ms
    preserve stock Release

Ready → Threshold → Release
    eligibility 已清除，不套用 Ready→Release 去重

Ready → Threshold → Ready → Release
    第二次 Ready 更新时间戳并重新建立 eligibility
```

去重只匹配真实 Release callsite。`0x885000`、Threshold 触觉与其他 native `constant=0` 调用保持原厂行为。

被 suppress 时只跳过 `perform_ext_haptic_feedback` 本身；Launcher 会从下一条指令继续执行 `Runtime_dec_strong` 和原有 bookkeeping。

## Break-open Beta

当前只 Hook：

```text
WindowTransitionUtil::is_merge_back_break_open_anim_support
```

逻辑：

```text
beta enabled  → return true
beta disabled → return stock result
```

不修改：

- backing flag
- `BackControllApi::can_use_break_open_anim_impl`
- handler 全局状态

旧的 backing-flag 实验曾导致 Launcher startup `SIGBUS BUS_ADRALN`，已废弃，不能重新引入。

## 16 KB Page Guard

HyperRT 可能对包含 inline hook 的页执行：

```text
madvise(..., MADV_DONTNEED)
```

这会恢复被 inline Hook 修改的代码页。当前实现借鉴 MiuiBackGestureHook 的思路，在 `libhyper_os_flutter.so` 的 live GOT 上窄范围拦截 `madvise`：

- 非 `MADV_DONTNEED` 原样放行
- 不覆盖本模块 Hook 页的范围原样放行
- 只跳过与已登记 Hook 页重叠的页面
- 其余区间仍调用原始 `madvise`

页面保护与 watchdog repair 双保险并存。

## Hook 健康检查

watchdog 持续检查主 Hook 入口：

- patch 仍存在：`HEALTHY`
- 原始指令被恢复：等待 active calls 退出后尝试 unhook + rehook
- 入口既不是原始字节也不是本模块 patch：判定 foreign patch，拒绝覆盖
- Launcher mapping 变化：清理旧跟踪并重新解析

附加功能 resolver 也会在未就绪时重试：

- Release callsite
- HyperRT runtime bridge / capture hook
- Back progress `convert_offset`
- Break-open

## Density 与阈值

UI 可调范围：

```text
88..300 dp
```

native 最大接受值：

```text
320 dp
```

`0` 作为历史配置 alias 解释为 stock `88 dp`。

进度缩放的 `88 / customThreshold` 比例本身与 density 无关；density 主要用于主 gate 的自定义阈值像素判断与 legacy fallback。

## 诊断日志

native Tag：

```text
HyperOS4SwipeGateNative
```

主要关键字：

```text
CONTROL_CARRIER
HOOK_SCAN
HOOK_HEALTH
DP_GATE
PROGRESS_V1
HAPTIC_V2
BREAK_OPEN_HEALTH
PAGE_GUARD
```

典型健康状态应包括：

```text
HOOK_HEALTH healthy ...
PROGRESS_V1 convert_offset hook ready ...
HAPTIC_V2 release callsite ready ...
BREAK_OPEN_HEALTH healthy ...       # 开启 Beta 时
```

诊断页同时展示版本、Launcher 完整版本、LSPosed 连接、两个 scope、HyOS Runtime、主 Hook profile 与 native 日志。

## 构建与 CI

本地 Debug：

```bash
gradle :app:assembleDebug
```

GitHub Actions 检查：

- LSPosed API 102 metadata
- `java_init` / `native_init`
- scope 恰好为 `com.miui.home` 与 `com.android.systemui`
- arm64-v8a native 库
- `native_init` 导出
- 16 KB ELF LOAD alignment
- APK 16 KB zip alignment
- 签名证书连续性

## 新 Launcher 版本验证原则

新 Launcher 8.0+ 不应因为 RVA 变化就新增硬编码 offset。至少验证：

1. `on_swipe_process` semantic / exact resolver 能唯一确认目标
2. AArch64 参数布局未破坏当前 wrapper 假设
3. `convert_offset` resolver 唯一；否则确认 fallback 行为可接受
4. 自定义门槛前后的动画连续、Ready 可逆、Release 正常
5. Haptic Release callsite 结构仍能唯一解析；否则保持 stock haptic
6. Break-open target 唯一；否则不开启 Beta
7. page guard / watchdog 不覆盖第三方未知 patch
8. 任何 0 candidate、multiple candidates 或冲突都 fail closed

当前优先级始终是：**不确定时保持小米原厂行为，而不是为了扩大兼容范围降低验证强度。**
