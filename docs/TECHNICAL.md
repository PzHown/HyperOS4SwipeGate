# Technical Notes

本文记录 HyperOS4 SwipeGate 当前版本的逆向目标、Hook 策略、运行链路与安全约束。普通使用请直接查看项目根目录的 [README](../README.md)。

## 目标环境

目标兼容范围：

- HyperOS 4 System Launcher 8.0+
- 已测试版本：`RELEASE-8.01.02.5459-260807-08242024-R`
- 已测试版本：`RELEASE-8.01.02.5465-260807-08262034-R`
- 已测试版本：`RELEASE-8.01.02.6174-260818-08281208-R`
- Launcher 包名：`com.miui.home`
- SystemUI 包名：`com.android.systemui`
- Launcher 进程入口：`/system_ext/bin/hyos_spawner`
- native 库：`libapp_launcher.so`
- 架构：arm64-v8a
- LSPosed Modern API 102 `java_init` + `native_init`

### 必需作用域

SwipeGate 当前需要同时启用两个 LSPosed 作用域：

```text
com.miui.home
com.android.systemui
```

两者职责不同：

- `com.miui.home`：承载 Launcher / HyOS native 目标与手势 Hook。
- `com.android.systemui`：承载 `SystemUiBridgeModule`，负责 App 与 HyOS Runtime 之间的实时配置和状态中继。

因此 **系统桌面与系统界面缺一不可**。`com.android.systemui` 不是为了诊断而附加的可选作用域，而是当前控制链路的一部分。

模块不会根据 Launcher `versionName` 做硬编码拒绝，也不再使用固定 offset 直接定位 Hook。当前会扫描 `libapp_launcher.so` 的可执行段，并只在语义解析或已验证 exact Pattern 能唯一确认目标时继续安装 Hook。

## 原厂 88 dp 边界

逆向 `GestureInputBackHelper::on_swipe_process` 后可以确认，Launcher 会将真实横向距离传入原厂侧边栏状态机。

当前已验证版本中：

```text
110 dp × 0.8 = 88 dp
```

因此 `88 dp` 是当前已验证 Launcher 版本的原厂侧边栏转换边界。

SwipeGate 不主动调用不稳定的 Launcher Rust 辅助函数，而是使用已验证的原厂边界常量，并自行完成 dp → px 换算。

## Hook 目标与动态定位

目标函数：

```text
GestureInputBackHelper::on_swipe_process
```

最初逆向的 `RELEASE-8.01.02.5459-260807-08242024-R` 中该函数曾位于：

```text
libapp_launcher.so + 0x816fc4
```

这个 offset 现在仅作为历史诊断参考，不参与 Hook 定位。

当前主路径为语义解析器，并保留 exact Pattern 作为已验证版本的保守兜底：

```text
libapp_launcher.so loaded
        ↓
解析 ELF / PLT import 与 MotionEvent 调用图
        ↓
验证 on_swipe_process 行为结构
        ↓
semantic candidate 唯一 → 接受
        ↓
必要时与 exact Pattern 交叉验证 / fallback
        ↓
冲突、0 candidate、2+ candidate → fail closed
```

更完整的语义定位规则见 [SEMANTIC_RESOLVER.md](SEMANTIC_RESOLVER.md)。

## Exact Pattern fallback

当前保留两个已验证 Pattern：

- `8.01.02.5459-v1`
- `8.01.02.6174-v2`

规则：

1. semantic resolver 唯一，exact 无结果：接受 semantic
2. semantic 与 exact 都唯一且地址一致：接受 semantic
3. semantic 与 exact 指向不同地址：fail closed
4. semantic 无法唯一确认，但 exact 唯一：使用 exact fallback
5. 两边均无法唯一确认：fail closed

后续验证新的 Launcher 8.0+ 版本时，如果目标行为结构仍可被唯一确认，不需要因为 RVA/offset 变化重新适配。

## 安装前安全校验

扫描得到目标后仍不会立即无条件 Hook。

安装前会再次确认：

1. 目标结构仍满足刚才的解析结果
2. 保存当前函数入口原始指令
3. Hook 返回成功且 trampoline 有效
4. Hook 后入口确实已经改变
5. semantic 与 exact resolver 不发生冲突

如果扫描不到目标、出现多个候选、解析结果在安装前发生变化或检测到未知第三方 patch，都会拒绝安装。

## Rootless 运行时控制链路

当前实时配置与状态主链路不再依赖 `su`、`resetprop` 或 `setprop`，也不要求 Launcher 提供稳定的 libxposed Java runtime。

```text
SwipeGate App
   ↓ NativeControlBridge private query
SystemUiBridgeModule in com.android.systemui
   ↓ Xiaomi protected com.android.systemui.fsgesture carrier
HyOS / Launcher native runtime
   ↓ native control receiver / gate
Gesture Hook
   ↓ authenticated native reply
SystemUiBridgeModule
   ↓ private reply
SwipeGate App
```

这也是为什么 `com.android.systemui` 必须加入作用域。

App 会把以下运行参数放入实时控制消息：

```text
threshold_dp
log_level
```

native 端收到后会更新当前运行状态，并把需要持久化的值写入 Launcher 自有 cache，以便后续进程重启继续使用。

### RemotePreferences 兼容通道

LSPosed API 102 RemotePreferences 仍然保留，用于兼容普通 Java target / 历史配置镜像：

```text
App
   ↓ XposedService API 102
RemotePreferences (group: swipegate)
   ↓ ModuleMain compatibility bridge
Launcher cache
```

但对 HyperOS 4 HyOS native 主路径而言，**SystemUI 中继是实时控制与状态的关键链路**，不能只依赖 RemotePreferences 判断功能完整性。

旧版本留下的系统属性：

```text
persist.hyperos4swipegate.threshold_dp
```

仅作为升级迁移 fallback，不再是新配置的写入通道。其他系统属性（包括 density）不会被修改。

App 当前提供的可修改范围：

```text
88..300 dp
```

其中：

- `88 dp` 为原厂边界
- `89–300 dp` 才会延后原厂侧边栏触发
- 旧版本保存的 `0` 继续兼容为原厂 `88 dp` alias，但新界面不再提供 `0–87 dp`

## App 状态检测

模块 App 不通过 `su -c pidof/logcat/getprop` 判断状态。

当前状态检测至少包含：

1. XposedService API 102 已连接
2. LSPosed 版本满足要求
3. **系统桌面 `com.miui.home` 已加入作用域**
4. **系统界面 `com.android.systemui` 已加入作用域**
5. `/system_ext/bin/hyos_spawner` 存在
6. `getRunningTargets()` 直接命中 Launcher / 相同 UID / `hyos_spawner`，或满足完整 capability fallback
7. Native Hook 状态通过运行时通道返回且保持 fresh

对部分不暴露 native-only child 的 HYOS Runtime，兼容激活证据必须同时满足：

```text
LSPosed API 102
+ supported framework
+ com.miui.home in scope
+ com.android.systemui in scope
+ hyos_spawner present
```

缺少任意一个必需作用域时，主页应显示未激活，并明确指出缺少的是系统桌面、系统界面，或两者。

诊断页会分别显示：

- 系统桌面作用域
- 系统界面作用域
- Launcher 完整版本号
- HyOS Runtime
- Native Hook / profile / detail

这些激活证据仍不能替代 native Pattern 健康检查。Native Hook 是否成功安装、是否遇到解析冲突，以实时 Hook status 与 `HOOK_SCAN` / `HOOK_HEALTH` 为准。

## Hook 策略

`on_swipe_process` 会收到本次手势的真实横向距离。

当用户设置的门槛高于 `88 dp` 且实际距离尚未达到用户门槛时，SwipeGate 不阻断整个返回手势，而是把传给原函数的距离限制在原厂 `88 dp` 边界之前。

```text
if customThreshold > 88dp and horizontalDistance < customThreshold:
    effectiveDistance = just below 88dp
else:
    effectiveDistance = horizontalDistance

call original on_swipe_process(effectiveDistance)
```

这样可以保留原厂返回动画和状态机，只延后进入侧边栏停顿分支的时机。

## ABI-transparent Hook wrapper

当前 AArch64 wrapper 透明保存调用现场，只修改 `s0` 中的 horizontal distance，再转发到 LSPosed trampoline。

主要保存：

- `x0..x8`
- `q0..q7`
- `x30`

读取：

- `w1`：readyFinish
- `w2`：side
- `s0`：horizontalDistance

这降低了不同 Launcher codegen 在 Point 参数布局上的差异影响，但仍要求关键手势参数保持当前 AAPCS64 约定。

## Density 解析

只有自定义门槛高于 `88 dp` 时才需要进行 dp → px 换算。

当前按以下顺序解析 density：

1. `persist.sys.miui_resolution`
2. `persist.sys.dpi`
3. `ro.sf.lcd_density`
4. `qemu.sf.lcd_density`

如果无法获得有效 density，模块不会尝试自定义距离换算，而是回退到原厂行为。

## Hook 健康检查

模块会持续检查已经安装的目标函数入口状态：

- 当前 Hook patch 仍存在：保持运行
- 原始指令被恢复：尝试执行一次 unhook + rehook 修复
- 入口变成非原厂、也不是本模块 patch：视为其他补丁或未知修改，不强行覆盖
- Launcher native 映射发生变化：清除旧目标并重新进行解析

修复前会等待当前 Hook 调用退出，避免在函数仍执行时直接替换入口。

这套逻辑的目标是：**可以恢复自己的 Hook，但不抢占未知的第三方 patch。**

## 扫描频率

正常 Hook 已安装时不会反复扫描整个 native text。

- library load callback：立即解析一次
- 尚未找到目标时：watchdog 周期性重新解析
- Hook 正常后：主要检查已解析的目标入口与映射变化

## 日志

native 日志 Tag：

```text
HyperOS4SwipeGateNative
```

常见日志关键字：

```text
DP_GATE
HOOK_SCAN
HOOK_HEALTH
resolver=
hook installed
repaired successfully
install refused
foreign
```

App 的「诊断」页提供无 Root 的 LSPosed service、两个必需作用域、Launcher 完整版本、HYOS Runtime、Native Hook 与运行日志信息。

## 构建

本地 Debug 构建：

```bash
gradle :app:assembleDebug
```

GitHub Actions 会检查：

- LSPosed API 102 模块元数据
- `java_init` / `native_init` 打包入口
- `com.miui.home` scope
- `com.android.systemui` scope
- scope 总数严格为 2
- arm64-v8a native 库
- `native_init` 导出符号
- 16 KB ELF LOAD 对齐
- APK 16 KB zip alignment
- APK 签名证书

## 适配与验证其他 Launcher 8.0+ 版本

Launcher 8.0+ 是目标兼容范围，目前已明确测试：

- `RELEASE-8.01.02.5459-260807-08242024-R`
- `RELEASE-8.01.02.5465-260807-08262034-R`
- `RELEASE-8.01.02.6174-260818-08281208-R`

测试其他版本时先观察诊断页的 Launcher **完整版本号**与 Native Hook profile，再观察 native 日志：

```text
HOOK_SCAN resolved ...
```

如果成功解析到唯一目标，重点验证手势行为即可；不需要因为 offset 不同就重新适配。

只有出现 resolver 无法唯一确认、exact fallback 也不匹配，或目标函数行为结构明显变化时，才需要重新分析。新增适配前至少需要确认：

1. `on_swipe_process` 的真实语义和参数布局没有变化
2. 原厂侧边栏边界仍然等价于当前逻辑
3. 语义候选或 exact Pattern 在目标 executable segments 中唯一
4. arm64 调用约定与 trampoline 正常
5. 返回手势在门槛前后的动画和可逆状态没有回归
6. Hook 被恢复、冲突、0 candidate、multiple candidates 时均能 fail closed

当前策略优先保证未验证版本在不兼容时维持原厂行为，而不是为了扩大版本号范围降低校验强度。
