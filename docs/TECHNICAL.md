# Technical Notes

本文记录 HyperOS4 SwipeGate 当前版本的逆向目标、Hook 策略与安全约束。普通使用请直接查看项目根目录的 [README](../README.md)。

## 目标环境

当前实现针对以下环境：

- HyperOS 4 System Launcher `RELEASE-8.01.02.5459-260807-08242024-R`
- 包名：`com.miui.home`
- 进程入口：`/system_ext/bin/hyos_spawner`
- native 库：`libapp_launcher.so`
- 架构：arm64-v8a
- LSPosed Modern API 102 `native_init`

模块只在目标进程与目标代码特征同时匹配时安装 Hook。

## 原厂 88 dp 边界

逆向 `GestureInputBackHelper::on_swipe_process` 后可以确认，Launcher 会将真实横向距离传入 `BackGestureUtils::convert_offset(distance)`，再基于归一化结果判断侧边栏停顿状态。

当前版本中：

```text
110 dp × 0.8 = 88 dp
```

因此 `88 dp` 是该 Launcher 版本的原厂侧边栏转换边界。

SwipeGate 不主动调用 `BackGestureUtils::convert_offset`。该 Rust 实现在 Launcher 状态尚未初始化时可能触发 `Option::unwrap(None)`，因此模块只使用逆向得到的原厂边界常量，并自行完成 dp → px 换算。

## Hook 目标

当前 Hook：

```text
GestureInputBackHelper::on_swipe_process
libapp_launcher.so + 0x816fc4
```

安装前会检查目标函数入口的 16 字节指令签名。入口与当前适配版本不一致时拒绝安装，避免在未知 Launcher 上继续写入 Hook。

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

模块会持续检查目标函数入口状态。

主要状态包括：

- 当前 Hook patch 仍存在：保持运行
- 原始指令被恢复：尝试执行一次 unhook + rehook 修复
- 入口变成非原厂、也不是本模块 patch：视为其他补丁或未知修改，不强行覆盖

修复前会等待当前 Hook 调用退出，避免在函数仍执行时直接替换入口。

这套逻辑的目标是：**可以恢复自己的 Hook，但不抢占未知的第三方 patch。**

## 日志

native 日志 Tag：

```text
HyperOS4SwipeGateNative
```

同时会尝试写入 Launcher cache 中的模块日志文件，App 的「日志」页会汇总相关诊断信息。

常见日志关键字：

```text
DP_GATE
HOOK_HEALTH
hook installed
repaired successfully
install refused
foreign
```

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

## 适配新 Launcher

不要仅修改 offset 后直接发布。

至少需要重新确认：

1. `on_swipe_process` 的真实语义和参数布局没有变化
2. 原厂侧边栏边界仍然等价于当前逻辑
3. 新目标函数入口签名
4. arm64 调用约定与 trampoline 正常
5. 返回手势在门槛前后的动画和可逆状态没有回归
6. Hook 被恢复、冲突以及签名不匹配时均能 fail closed

当前策略优先保证未知版本维持原厂行为，而不是扩大未经验证的兼容范围。