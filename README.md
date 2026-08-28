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

> Launcher 8.0+ 为目标兼容范围。当前不会按版本号直接拒绝加载，而是校验目标代码特征；如果内部布局或指令签名发生变化，模块会停止安装 Hook 并保持原厂行为。

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
确认 LSPosed 已启用模块、作用域包含 `com.miui.home`。如果使用其他 Launcher 8.0+ 版本，也可能因为内部代码差异而未能安装 Hook。

**显示「异常」**  
进入 App 的「日志」页刷新并复制诊断信息，再通过 [Issues](https://github.com/PzHown/HyperOS4SwipeGate/issues) 反馈。

**升级了系统桌面**  
Launcher 8.0+ 属于目标兼容范围，但当前仅明确测试过上方列出的版本。升级后如出现异常，请附带 Launcher 完整版本号和 App 日志反馈。

## 技术说明

模块通过 LSPosed Modern API 102 `native_init` 进入 HyperOS 4 Launcher 的 native/Rust 进程，并在目标代码特征校验通过后调整原厂手势距离输入。

逆向目标、Hook 策略、安全校验与构建说明见 [docs/TECHNICAL.md](docs/TECHNICAL.md)。

---

本项目与 Xiaomi / 小米官方无关。