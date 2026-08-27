# HyperOS4SwipeGate

HyperOS 4 Rust 系统桌面侧滑停顿阈值控制 LSPosed 模块。

## 功能

HyperOS 4 的系统桌面支持“侧滑停顿呼出”手机管家侧边栏。本模块在桌面的原生停顿检测前增加一个横向距离门槛：

- 默认阈值：**55% 屏幕宽度**。
- 水平滑动距离低于阈值：持续重置侧滑停顿检测器，松手仍走系统返回。
- 达到阈值：停止干预，让 HyperOS 4 原生停顿检测继续工作，可正常呼出侧边栏。
- App 内可通过 MIUIX 横条实时调整 0–100% 阈值。

## 当前适配

- HyperOS 4 System Launcher `RELEASE-8.01.02.5459-260807-08242024-R`
- `com.miui.home`
- arm64-v8a
- LSPosed Modern API 102 `native_init`
- Android 17 / SDK 37

当前版本是**严格版本匹配**。模块会校验 `libapp_launcher.so` 关键函数入口指令；签名不匹配时不会安装 Hook，以避免未知桌面版本崩溃。

## 实现原理

HyperOS 4 Launcher 8.x 的核心手势逻辑位于 Rust/native `libapp_launcher.so`。当前适配通过该 SO 自带的 `.gnu_debugdata` 恢复本地 Rust 符号，并定位：

- `GestureInputBackHelper::on_swipe_process` — `0x816fc4`
- `IntentBallPauseDetector::reset` — `0x783aa8`
- `GestureTouchableRegion::get_screen_dimensions` — `0x77ae6c`

`on_swipe_process` 接收到的浮点参数是 `abs(rawX - downX)` 的横向像素距离。模块计算：

```text
progress = horizontalDistancePx / screenWidth
```

当 `progress < threshold` 时，对 `GestureInputBackHelper + 0x178` 内嵌的 `IntentBallPauseDetector` 调用原生 `reset()`，阻止停顿条件在门槛之前累计；返回手势本身不被拦截。

阈值由 App 通过 Root 写入 `persist.hyperos4swipegate.threshold`，native 端动态读取，修改后无需重启桌面。

## 使用

1. 安装 APK。
2. 在 LSPosed 中启用模块，作用域保持为“系统桌面 / `com.miui.home`”。
3. 手机管家 → 侧边工具箱 → 呼出方式选择“侧滑停顿呼出”。
4. 打开 HyperOS4 SwipeGate，在“设置”中调整触发阈值。
5. 首次写入阈值时允许 Root 授权。

## UI

设置界面使用 MIUIX：

- `FilterSortView2`：顶部“设置 / 关于”双 Tab。
- `SeekBarPreferenceCompat`：触发阈值横条。
- “关于”页采用 MIUIX LSP 项目常见的版本卡片 / 信息卡片结构，设计参考 HyperCeiler 的 About 页面，但未复制其自定义组件代码。

## 开发状态

当前为首个 alpha 适配版本，GitHub Actions 会对每次主分支提交构建 APK，并检查 LSPosed API 102 元数据、native 入口和 16 KB ELF 对齐。

## 风险提示

该项目会 Hook 系统桌面 native/Rust 代码。仅对上面列出的桌面版本启用；升级系统桌面后如签名变化，模块会 fail-closed 并保持原厂行为，需重新适配偏移。
