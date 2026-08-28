# HyperOS4 SwipeGate

[![Build APK](https://github.com/PzHown/HyperOS4SwipeGate/actions/workflows/build.yml/badge.svg)](https://github.com/PzHown/HyperOS4SwipeGate/actions/workflows/build.yml)

为 HyperOS 4 系统桌面的「侧滑停顿呼出侧边栏」增加可调触发距离。

普通侧滑仍然返回；只有横向滑动达到设定距离后，才允许系统进入侧边栏停顿触发。

## 功能

- 调整侧边栏停顿触发距离
- 保留原厂返回手势与动画
- 修改后即时生效，无需重启桌面
- 模块状态与 Hook 异常检测
- 内置诊断日志与一键复制

## 使用

1. 安装 APK。
2. 在 LSPosed 中启用模块，作用域选择 **系统桌面（`com.miui.home`）**。
3. 在手机管家中启用「侧滑停顿呼出」侧边栏。
4. 打开 HyperOS4 SwipeGate，在「设置」中调整触发距离。
5. 首次修改时授予 Root 权限。

### 触发距离

| 设置 | 实际行为 |
| --- | --- |
| `0` | 使用系统默认 `88 dp` |
| `1–88 dp` | 受系统原厂下限限制，仍为 `88 dp` |
| `89–320 dp` | 使用自定义触发距离 |

数值越大，需要向屏幕内侧滑得越远才会进入侧边栏停顿触发。

## 当前适配

- HyperOS 4
- System Launcher `RELEASE-8.01.02.5459`
- Android 17 / SDK 37
- arm64-v8a
- LSPosed Modern API 102
- 作用域：`com.miui.home`

> 当前采用严格版本匹配。未适配的 Launcher 不会强行安装 Native Hook。

## 工作方式

HyperOS 4 Launcher 的相关手势逻辑位于 Rust/native `libapp_launcher.so`。

SwipeGate 在原厂侧边栏停顿逻辑前增加距离门槛：未达到门槛时继续保持返回手势；达到门槛后停止干预，由系统原生逻辑继续判断是否呼出侧边栏。

模块会检查目标代码特征。发现版本不匹配或 Hook 冲突时会停止覆盖，尽量保持原厂行为。

阈值通过系统属性 `persist.hyperos4swipegate.threshold_dp` 动态读取，因此调整后无需重启桌面。

## 下载

最新自动构建可在 [Releases](https://github.com/PzHown/HyperOS4SwipeGate/releases) 或 [Actions](https://github.com/PzHown/HyperOS4SwipeGate/actions) 获取。

当前仍处于适配阶段，建议升级系统桌面后先确认兼容版本。

## 构建

```bash
gradle :app:assembleDebug
```

GitHub Actions 会自动检查：

- LSPosed API 102 模块元数据
- `native_init` 入口
- arm64 native 库
- 16 KB ELF / APK 对齐

## 注意

本项目会 Hook 系统桌面的 Native/Rust 代码。系统桌面升级后，内部实现可能发生变化；如果出现未激活或异常状态，请先查看 App 内「日志」。

非小米官方项目。