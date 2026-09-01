<div align="center">

<img src="app/src/main/res/drawable-nodpi/swipegate_logo.webp" width="144" alt="HyperOS4 SwipeGate Logo" />

# HyperOS4 SwipeGate

**延后 HyperOS 4 侧滑停顿触发，并尽量保持原厂返回手势、动画与触觉语义。**

[![Build APK](https://github.com/PzHown/HyperOS4SwipeGate/actions/workflows/build.yml/badge.svg)](https://github.com/PzHown/HyperOS4SwipeGate/actions/workflows/build.yml)
[![Release](https://img.shields.io/github/v/release/PzHown/HyperOS4SwipeGate?include_prereleases&label=release)](https://github.com/PzHown/HyperOS4SwipeGate/releases)
[![Downloads](https://img.shields.io/github/downloads/PzHown/HyperOS4SwipeGate/total?label=downloads)](https://github.com/PzHown/HyperOS4SwipeGate/releases)
![Android 17](https://img.shields.io/badge/Android-17-3DDC84?logo=android&logoColor=white)
![LSPosed API 102](https://img.shields.io/badge/LSPosed-API%20102-blue)
[![License](https://img.shields.io/github/license/PzHown/HyperOS4SwipeGate)](LICENSE)

</div>

## 介绍

HyperOS 4 系统桌面支持通过「侧滑停顿」呼出手机管家侧边栏。SwipeGate 把这项手势的触发距离从原厂约 `88 dp` 延后到用户设定值：**门槛前仍按系统返回手势运行，达到门槛后才允许原厂侧边栏停顿逻辑进入下一阶段。**

当前 0.8.2 Beta 还包含两项可选实验功能：

- **丰富侧滑震动反馈**：进入 Ready 时补充一次原生 HyperRT 触觉，并对短时间 Ready→Release 的原厂松手震动做精确去重。
- **Break-open Beta**：通过函数级 Hook 开启 Launcher 的 merge-back break-open 支持，不修改全局 backing flag。

## 下载

前往 [Releases](https://github.com/PzHown/HyperOS4SwipeGate/releases) 获取最新构建。开发分支会持续产生自动构建，实际兼容范围请以本页「当前支持」与诊断页为准。

## 当前支持

| 项目 | 支持范围 |
| --- | --- |
| 系统 | HyperOS 4 |
| 系统桌面 | Launcher 8.0+ |
| 已测试版本 | `RELEASE-8.01.02.5459-260807-08242024-R`<br>`RELEASE-8.01.02.5465-260807-08262034-R`<br>`RELEASE-8.01.02.6174-260818-08281208-R`<br>`RELEASE-8.01.02.6179-260818-08292132-R` |
| Android | Android 17 / API 37 |
| 架构 | arm64-v8a |
| LSPosed | Modern API 102 · versionCode ≥ 7846 |
| Zygisk Next | 1.5.0+（HyOS Runtime） |
| 必需作用域 | **系统桌面** `com.miui.home` + **系统界面** `com.android.systemui` |
| Root | 不需要 |

> `com.android.systemui` 不是可选作用域。SwipeGate 的 App ↔ HyOS Runtime 配置与状态通道需要模块代码运行在 SystemUI 中，再通过小米现有的 `com.android.systemui.fsgesture` 通道中继到 Launcher native runtime。
>
> 模块不按 Launcher `versionName` 硬编码放行，也不直接依赖固定 Hook offset。native resolver 会扫描已加载的 `libapp_launcher.so`，只有在语义结构或已验证 Pattern 能唯一确认目标时才安装 Hook；不确定时保持原厂行为。

## 安装与使用

1. 安装 APK。
2. 在 LSPosed 中启用 **HyperOS4 SwipeGate**，作用域同时勾选：
   - **系统桌面**（`com.miui.home`）
   - **系统界面**（`com.android.systemui`）
3. 重启系统桌面和系统界面，或直接重启设备。
4. 在手机管家中将侧边栏呼出方式设为「侧滑停顿呼出」。
5. 打开 SwipeGate，在主页调整触发距离；实验功能可按需开启。

实时控制主链路为 `App → SystemUI → HyOS Runtime / Launcher native`，不需要 `su`。RemotePreferences 与 Launcher cache 仅作为兼容和持久化辅助通道。

## 触发距离与动画

可修改范围为 **88–300 dp**：

- `88 dp`：原厂边界
- `89–300 dp`：延后侧边栏停顿触发

Launcher 8.x 的返回手势不是简单在 88dp 处做一次布尔判断。逆向确认 `BackGestureUtils::convert_offset` 使用约 `110 dp` 的总坐标，并在 `0.8` 处进入 Ready：

```text
110 dp × 0.8 = 88 dp
```

因此当前实现会同步缩放这套共享进度坐标，使 **动画 Ready、状态判断和用户设置的门槛保持一致**，而不是在 88dp 后把动画冻结到门槛再突然跳变。达到自定义门槛后仍继续使用小米原生的非线性 easing。

旧版本保存的 `0` 继续兼容解释为原厂 `88 dp`，但新界面不再提供低于 `88 dp` 的值。

## 丰富侧滑震动反馈 Beta

Launcher 8.0 的原厂 Back 手势在松手真正返回时会触发 HyperRT 震动。SwipeGate 的 Beta 模式会在进入 Ready 时补充同类型原生触觉，并保持以下规则：

```text
Ready → Release < 750ms        补 Ready，去重原厂 Release
Ready → Release >= 750ms       补 Ready，保留原厂 Release
Ready → Threshold → Release    Threshold/Release 保持小米语义
Ready → Threshold → Ready      第二次 Ready 重新建立 750ms 窗口
```

去重只匹配逆向与运行时验证过的 `GestureStubViewWindow::handle_back_gesture` Release HyperRT 调用点，不会全局吞掉其他 `constant=0` 触觉。

## Break-open Beta

Break-open 使用 `WindowTransitionUtil::is_merge_back_break_open_anim_support` 的函数级 Hook：开启时只把该能力判断提升为 true，关闭时返回原厂结果。不会修改 backing flag，也不会全局改写 Launcher 状态。

## 诊断与排查

**显示「未激活」**  
确认 LSPosed、Zygisk Next / HyOS Runtime 与两个必需作用域都满足要求。App 会通过 XposedService 与运行时握手判断状态，不依赖 Root/logcat。

**功能没有生效**  
进入「诊断」页复制完整诊断。重点关注 `HOOK_HEALTH`、`PROGRESS_V1`、`HAPTIC_V2`、`BREAK_OPEN_HEALTH` 与 `CONTROL_CARRIER`。如果 Launcher 更新后 resolver 无法唯一确认目标，模块会 fail closed，而不是尝试未知 RVA。

**动画在门槛前被压缩或触发时跳一下**  
当前版本已经增加 `BackGestureUtils::convert_offset` 进度缩放。诊断中应能看到 `PROGRESS_V1 convert_offset hook ready`；若未解析到该函数，会保守回退到旧的 88dp clamp 路径。

## 技术说明

核心实现包括：

- `GestureInputBackHelper::on_swipe_process` 语义 resolver + exact Pattern fallback
- `BackGestureUtils::convert_offset` 共享进度坐标解析与缩放
- HyperRT 原生 Ready 触觉与真实 Release callsite 级去重
- `MADV_DONTNEED` 16 KB Hook 页面保护与 watchdog 修复
- Break-open 函数级安全 Hook
- App → SystemUI → HyOS Runtime 的 Rootless 控制与诊断通道

详细逆向、Hook 结构与安全约束见 [docs/TECHNICAL.md](docs/TECHNICAL.md)，主目标定位规则见 [docs/SEMANTIC_RESOLVER.md](docs/SEMANTIC_RESOLVER.md)。

## 许可

本项目采用 [MIT License](LICENSE)。

---

本项目与 Xiaomi / 小米官方无关。

## 鸣谢

- [HyperCeiler](https://github.com/ReChronoRain/HyperCeiler)
- [Miuix](https://github.com/compose-miuix-ui/miuix)
- [libxposed](https://github.com/libxposed/api)
- [LSPosed](https://github.com/LSPosed/LSPosed)
