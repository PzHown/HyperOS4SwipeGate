<div align="center">

<img src="app/src/main/res/drawable-nodpi/swipegate_logo.webp" width="144" alt="HyperOS4 SwipeGate Logo" />

# HyperOS4 SwipeGate

**延后 HyperOS 4 侧滑停顿触发，不改变返回手势。**

[![Build APK](https://github.com/PzHown/HyperOS4SwipeGate/actions/workflows/build.yml/badge.svg)](https://github.com/PzHown/HyperOS4SwipeGate/actions/workflows/build.yml)
[![Release](https://img.shields.io/github/v/release/PzHown/HyperOS4SwipeGate?include_prereleases&label=release)](https://github.com/PzHown/HyperOS4SwipeGate/releases)
[![Downloads](https://img.shields.io/github/downloads/PzHown/HyperOS4SwipeGate/total?label=downloads)](https://github.com/PzHown/HyperOS4SwipeGate/releases)
![Android 17](https://img.shields.io/badge/Android-17-3DDC84?logo=android&logoColor=white)
![LSPosed API 102](https://img.shields.io/badge/LSPosed-API%20102-blue)
[![License](https://img.shields.io/github/license/PzHown/HyperOS4SwipeGate)](LICENSE)

</div>

## 介绍

HyperOS 4 系统桌面支持通过「侧滑停顿」呼出手机管家侧边栏。

SwipeGate 可以提高这项手势的触发距离：**未达到设定距离时仍执行系统返回，达到后才允许原厂侧边栏停顿逻辑继续触发。**

## 下载

前往 [Releases](https://github.com/PzHown/HyperOS4SwipeGate/releases) 获取最新构建。

当前提供自动构建版本，适配范围请以本页「当前支持」为准。

## 当前支持

| 项目 | 支持范围 |
| --- | --- |
| 系统 | HyperOS 4 |
| 系统桌面 | Launcher 8.0+ |
| 已测试版本 | `RELEASE-8.01.02.5459-260807-08242024-R`<br>`RELEASE-8.01.02.5465-260807-08262034-R`<br>`RELEASE-8.01.02.6174-260818-08281208-R` |
| Android | Android 17 / API 37 |
| 架构 | arm64-v8a |
| LSPosed | Modern API 102 · versionCode ≥ 7846 |
| Zygisk Next | 1.5.0+（HyOS Runtime） |
| 必需作用域 | **系统桌面** `com.miui.home` + **系统界面** `com.android.systemui` |
| Root | 不需要 |

> LSPosed versionCode ≥ 7846、Zygisk Next 1.5.0+ 与两个必需作用域缺一不可。主页会以无 Root 方式检测 LSPosed 版本、系统桌面作用域、系统界面作用域和实际 HyOS Runtime 能力。
>
> `com.android.systemui` 不是可选作用域。SwipeGate 的 App ↔ HyOS Runtime 配置与状态通道需要模块代码运行在 SystemUI 中，再通过小米现有的 `com.android.systemui.fsgesture` 通道中继到 Launcher native runtime。
>
> Launcher 8.0+ 为目标兼容范围。模块不会依赖固定 Hook offset，而是扫描 Launcher native 可执行代码并寻找已验证的唯一代码特征。仅地址变化通常无需重新适配；如果目标函数本身的代码特征发生变化，则会停止安装 Hook 并保持原厂行为。

## 安装与使用

1. 安装 APK。
2. 在 LSPosed 中启用 **HyperOS4 SwipeGate**，作用域同时勾选：
   - **系统桌面**（`com.miui.home`）
   - **系统界面**（`com.android.systemui`）
3. 重启系统桌面和系统界面，或直接重启设备，使两个作用域中的模块代码完成加载。
4. 在手机管家中将侧边栏呼出方式设为「侧滑停顿呼出」。
5. 打开 SwipeGate，在「主页」中调整触发距离。

实时配置与状态主链路为 `App → SystemUI → HyOS Runtime / Launcher native`，不需要授予 Root 权限。RemotePreferences 仍保留兼容镜像，但不是 HyperOS 4 native 运行时的唯一配置通道。

## 触发距离

可修改范围为 **88–300 dp**：

- `88 dp`：原厂触发距离
- `89–300 dp`：使用设定距离延后侧边栏触发

数值越大，需要向屏幕内侧滑得越远才会进入侧边栏停顿触发。

旧版本留下的 `0` 仍会被兼容解释为原厂 `88 dp`，但新界面不再提供低于 `88 dp` 的可调值。

## 排查

**显示「未激活」**  
确认 LSPosed 已启用模块，并确认作用域同时包含 **系统桌面 `com.miui.home`** 和 **系统界面 `com.android.systemui`**。缺少任意一个作用域，SwipeGate 都不会判定为完整激活。随后确认设备存在 HYOS Runtime，并重启对应进程或设备。App 会综合 LSPosed API 102、两个必需作用域、Launcher UID 和 HYOS Runtime 证据判断状态，不再依赖 Root/logcat。

**显示「LSPosed 未连接」**  
确认正在使用支持 Modern API 102 的 LSPosed，并重新打开模块 App。

**功能没有生效**  
进入 App 的「诊断」页检查 **系统桌面作用域**、**系统界面作用域**、Native Hook 状态并复制完整诊断；必要时再检查 LSPosed 日志中的 `HyperOS4SwipeGateNative` / `HOOK_SCAN` / `HOOK_HEALTH`。

**升级了系统桌面**  
如果只是 native 地址重新排列，模块会自动重新定位；如果目标函数被重新编译或重构导致 Pattern 不再匹配，则需要追加新版本 Pattern。反馈时请附带 Launcher **完整版本号**和诊断信息。

## 技术说明

模块通过 LSPosed Modern API 102 `native_init` 进入 HyperOS 4 Launcher 的 native/Rust 进程，扫描 `libapp_launcher.so` 的可执行段，在唯一代码特征匹配后才安装 Hook。

运行时控制链路为 `App → SystemUiBridgeModule（com.android.systemui）→ 小米 fsgesture 广播 → Launcher / HyOS native runtime`。系统桌面与系统界面因此都是必需作用域；RemotePreferences / Launcher cache 继续作为兼容与持久化辅助通道，全程无需 `su`。

逆向目标、Pattern 扫描、Hook 策略、安全校验与构建说明见 [docs/TECHNICAL.md](docs/TECHNICAL.md)。

## 许可

本项目采用 [MIT License](LICENSE)。

---

本项目与 Xiaomi / 小米官方无关。

## 鸣谢

- [HyperCeiler](https://github.com/ReChronoRain/HyperCeiler)
- [Miuix](https://github.com/compose-miuix-ui/miuix)
- [libxposed](https://github.com/libxposed/api)
- [LSPosed](https://github.com/LSPosed/LSPosed)
