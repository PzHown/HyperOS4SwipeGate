<div align="center">

# HyperOS4 SwipeGate

**延后 HyperOS 4 侧滑停顿触发，不改变返回手势。**

[![Build APK](https://github.com/PzHown/HyperOS4SwipeGate/actions/workflows/build.yml/badge.svg)](https://github.com/PzHown/HyperOS4SwipeGate/actions/workflows/build.yml)
[![Release](https://img.shields.io/github/v/release/PzHown/HyperOS4SwipeGate?include_prereleases&label=release)](https://github.com/PzHown/HyperOS4SwipeGate/releases)
![Android 17](https://img.shields.io/badge/Android-17-3DDC84?logo=android&logoColor=white)
![LSPosed API 102](https://img.shields.io/badge/LSPosed-API%20102-blue)

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
| 已测试版本 | `RELEASE-8.01.02.5459-260807-08242024-R` |
| Android | Android 17 / API 37 |
| 架构 | arm64-v8a |
| LSPosed | Modern API 102 |
| 作用域 | `com.miui.home` |

> Launcher 8.0+ 为目标兼容范围。模块不会依赖固定 Hook offset，而是扫描 Launcher native 可执行代码并寻找已验证的唯一代码特征。仅地址变化通常无需重新适配；如果目标函数本身的代码特征发生变化，则会停止安装 Hook 并保持原厂行为。

## 安装与使用

1. 安装 APK。
2. 在 LSPosed 中启用 **HyperOS4 SwipeGate**，作用域选择 **系统桌面（`com.miui.home`）**。
3. 重启系统桌面或设备，使模块加载。
4. 在手机管家中将侧边栏呼出方式设为「侧滑停顿呼出」。
5. 打开 SwipeGate，在「设置」中调整触发距离。

首次修改触发距离时需要授予 Root 权限。之后调整即时生效，无需重启桌面。

## 触发距离

- `0`：使用原厂 `88 dp`
- `1–88 dp`：实际仍按原厂 `88 dp`
- `89–320 dp`：使用设定距离

数值越大，需要向屏幕内侧滑得越远才会进入侧边栏停顿触发。

## 排查

**显示「未激活」**  
确认 LSPosed 已启用模块、作用域包含 `com.miui.home`。如果使用其他 Launcher 8.0+ 版本，也可能因为目标函数代码特征发生变化而未能安装 Hook。

**显示「异常」**  
进入 App 的「日志」页刷新并复制诊断信息，再通过 [Issues](https://github.com/PzHown/HyperOS4SwipeGate/issues) 反馈。

**升级了系统桌面**  
如果只是 native 地址重新排列，模块会自动重新定位；如果目标函数被重新编译或重构导致 Pattern 不再匹配，则需要追加新版本 Pattern。反馈时请附带 Launcher 完整版本号和 App 日志。

## 技术说明

模块通过 LSPosed Modern API 102 `native_init` 进入 HyperOS 4 Launcher 的 native/Rust 进程，扫描 `libapp_launcher.so` 的可执行段，在唯一代码特征匹配后才安装 Hook。

逆向目标、Pattern 扫描、Hook 策略、安全校验与构建说明见 [docs/TECHNICAL.md](docs/TECHNICAL.md)。

---

本项目与 Xiaomi / 小米官方无关。