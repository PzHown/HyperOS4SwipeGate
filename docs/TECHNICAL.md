# Technical Notes

本文记录 HyperOS4 SwipeGate 当前版本的逆向目标、Hook 策略与安全约束。普通使用请直接查看项目根目录的 [README](../README.md)。

## 目标环境

目标兼容范围：

- HyperOS 4 System Launcher 8.0+
- 已测试版本：`RELEASE-8.01.02.5459-260807-08242024-R`
- 包名：`com.miui.home`
- 进程入口：`/system_ext/bin/hyos_spawner`
- native 库：`libapp_launcher.so`
- 架构：arm64-v8a
- LSPosed Modern API 102 `native_init`

模块不会根据 Launcher `versionName` 做硬编码拒绝，也不再使用固定 offset 直接定位 Hook。当前会扫描 `libapp_launcher.so` 的可执行 `PT_LOAD` 段，并只在找到唯一已验证 Pattern 时继续安装 Hook。

## 原厂 88 dp 边界

逆向 `GestureInputBackHelper::on_swipe_process` 后可以确认，Launcher 会将真实横向距离传入 `BackGestureUtils::convert_offset(distance)`，再基于归一化结果判断侧边栏停顿状态。

已测试版本中：

```text
110 dp × 0.8 = 88 dp
```

因此 `88 dp` 是当前已验证 Launcher 版本的原厂侧边栏转换边界。

SwipeGate 不主动调用 `BackGestureUtils::convert_offset`。该 Rust 实现在 Launcher 状态尚未初始化时可能触发 `Option::unwrap(None)`，因此模块只使用逆向得到的原厂边界常量，并自行完成 dp → px 换算。

## Hook 目标与动态定位

目标函数：

```text
GestureInputBackHelper::on_swipe_process
```

已测试版本中该函数曾位于：

```text
libapp_launcher.so + 0x816fc4
```

这个 offset 现在仅作为诊断参考，不参与 Hook 定位。

实际流程：

```text
libapp_launcher.so loaded
        ↓
枚举 executable PT_LOAD segments
        ↓
按 4-byte ARM64 alignment 扫描 Pattern database
        ↓
0 candidate  → fail closed
1 candidate  → 二次确认 Pattern → Hook
2+ candidates → fail closed
```

因此只要 Launcher 更新后函数代码特征仍保持兼容，即使 native 库重新链接、函数 offset 改变，也可以自动找到新的目标地址。

## Pattern database

当前 Pattern 基础设施支持：

- 多个 Pattern 版本
- byte mask / wildcard
- 在所有 native executable segments 中扫描
- 对不同 Pattern 命中的相同地址去重
- 只有唯一目标地址才接受

当前已登记 Pattern：

```text
8.01.02.5459-v1
FF 83 05 D1 EA 7B 00 FD E9 A3 0F 6D FD FB 10 A9
```

当前 V1 Pattern 的 16 字节均为有效匹配位。扫描器本身已经支持 mask，但在没有第二个 Launcher build 做交叉验证前，不主动放宽 V1 的 immediate 字段，以免错误命中其他 Rust 函数。

后续验证新的 Launcher 8.0+ 版本时，如果只是 offset 变化，不需要增加 Pattern；只有函数机器码发生变化时才需要新增经过验证的 Pattern。

## 安装前安全校验

扫描得到目标后仍不会立即无条件 Hook。

安装前会再次确认：

1. 目标地址仍满足刚才命中的 Pattern
2. 保存当前函数入口原始 16 字节
3. Hook 返回成功且 trampoline 非空
4. Hook 后入口确实已经改变

如果扫描不到目标、出现多个候选、扫描与实际安装之间 Pattern 发生变化，都会拒绝安装。

## 距离门槛

配置存储在系统属性：

```text
persist.hyperos4swipegate.threshold_dp
```

范围：

```text
0..320 dp
```

其中：

- `0` 表示原厂默认，即 `88 dp`
- `1–88 dp` 的有效值仍为 `88 dp`
- `89–320 dp` 才会延后原厂侧边栏触发

native 端会定期读取属性，因此修改后不需要重启 Launcher。

## Hook 策略

`on_swipe_process` 会收到本次手势的真实横向距离。

当用户设置的门槛高于 `88 dp` 且实际距离尚未达到用户门槛时，SwipeGate 不阻断整个返回手势，而是把传给原函数的距离限制在原厂 `88 dp` 边界之前。

简化后的逻辑：

```text
if customThreshold > 88dp and horizontalDistance < customThreshold:
    effectiveDistance = just below 88dp
else:
    effectiveDistance = horizontalDistance

call original on_swipe_process(effectiveDistance)
```

这样可以保留原厂返回动画和状态机，只延后进入侧边栏停顿分支的时机。

## Density 解析

只有自定义门槛高于 `88 dp` 时才需要进行 dp → px 换算。

当前按以下顺序解析 density：

1. `persist.sys.miui_resolution`
2. `persist.sys.dpi`
3. `ro.sf.lcd_density`
4. `qemu.sf.lcd_density`

如果无法获得有效 density，模块不会尝试自定义距离换算，而是回退到原厂行为。

## Hook 健康检查

模块会持续检查已经安装的目标函数入口状态。

主要状态包括：

- 当前 Hook patch 仍存在：保持运行
- 原始指令被恢复：尝试执行一次 unhook + rehook 修复
- 入口变成非原厂、也不是本模块 patch：视为其他补丁或未知修改，不强行覆盖
- Launcher native 映射发生变化：清除旧目标并重新进行 Pattern 扫描

修复前会等待当前 Hook 调用退出，避免在函数仍执行时直接替换入口。

这套逻辑的目标是：**可以恢复自己的 Hook，但不抢占未知的第三方 patch。**

## 扫描频率

正常 Hook 已安装时不会反复扫描整个 native text。

- library load callback：立即扫描一次
- 尚未找到目标时：watchdog 最多每 5 秒重新扫描一次
- Hook 正常后：只检查已解析的目标入口

这样避免每 500 ms 对整个 `libapp_launcher.so` 做全量扫描。

## 日志

native 日志 Tag：

```text
HyperOS4SwipeGateNative
```

同时会尝试写入 Launcher cache 中的模块日志文件，App 的「日志」页会汇总相关诊断信息。

常见日志关键字：

```text
DP_GATE
HOOK_SCAN
HOOK_HEALTH
resolvedOffset
hook installed
repaired successfully
install refused
foreign
```

其中 `resolvedOffset` 是扫描得到的实际地址相对 library base 的偏移；`referenceOffset=0x816fc4` 只用于对比已测试版本的位置变化。

## 构建

本地 Debug 构建：

```bash
gradle :app:assembleDebug
```

GitHub Actions 还会检查：

- LSPosed API 102 模块元数据
- `java_init` / `native_init` 打包入口
- `com.miui.home` scope
- arm64-v8a native 库
- `native_init` 导出符号
- 16 KB ELF LOAD 对齐
- APK 16 KB zip alignment

## 适配与验证其他 Launcher 8.0+ 版本

Launcher 8.0+ 是目标兼容范围，但当前只有 `RELEASE-8.01.02.5459-260807-08242024-R` 完成明确测试。

测试其他版本时先观察日志：

```text
HOOK_SCAN resolved ...
```

如果成功解析到唯一目标，重点验证手势行为即可；不需要因为 offset 与 `0x816fc4` 不同就重新适配。

只有出现 `candidates=0` 或目标函数机器码明显变化时，才需要重新提取 Pattern。新增 Pattern 前至少需要确认：

1. `on_swipe_process` 的真实语义和参数布局没有变化
2. 原厂侧边栏边界仍然等价于当前逻辑
3. 新 Pattern 在目标 executable segments 中唯一
4. arm64 调用约定与 trampoline 正常
5. 返回手势在门槛前后的动画和可逆状态没有回归
6. Hook 被恢复、冲突、0 candidate、multiple candidates 时均能 fail closed

当前策略优先保证未验证版本在不兼容时维持原厂行为，而不是为了扩大版本号范围降低 Pattern 校验强度。