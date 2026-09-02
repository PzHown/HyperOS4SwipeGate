# Semantic Resolver

本文记录 SwipeGate 当前用于定位 HyperOS 4 Launcher 8.x `GestureInputBackHelper::on_swipe_process` 的语义解析策略。旧文档中关于“仅 dev 验证、main 仍停留在 0.6.0”的描述已经过时；当前主 Hook 以 exact fingerprint 为兼容性契约；当 fingerprint 出现多个候选时，使用 `BackGestureUtils::convert_offset` 行为链消歧。MotionEvent semantic resolver 目前仅用于诊断与交叉验证，不再在没有 exact 目标时单独接管主 Hook。

## 目标

不依赖 Launcher `versionName`，也不依赖固定 RVA。定位流程：

```text
libapp_launcher.so
        ↓
解析 mapped ELF / DT_JMPREL / PLT imports
        ↓
定位稳定的 MotionEvent imported API
        ↓
扫描 AArch64 executable segments
        ↓
验证手势处理调用图与 frame family
        ↓
得到 semantic candidate
        ↓
与 exact fingerprint 交叉验证
        ↓
唯一可信目标 → Hook
无法证明唯一 → fail closed
```

当前 MotionEvent 语义锚点包括：

```text
input_MotionEvent_getActionMasked
input_MotionEvent_getActionIndex
input_MotionEvent_getRawX
input_MotionEvent_getActionIndex
input_MotionEvent_getRawY
```

函数重新链接、RVA 变化、部分栈偏移变化时，只要完整行为图仍存在且唯一，就可以继续解析。

## AArch64 frame family

当前 resolver 接受几类已验证 frame family：

- legacy：5459 系列
- modern：6174 / 6179 系列
- compact：更紧凑的 pre-index SIMD 保存结构

不会把完整函数入口绑定成单一绝对机器码；但寄存器保存族、MotionEvent import 顺序与关键控制流仍必须成立。

如果编译器把函数彻底拆分、关键参数迁移到 stack-only ABI、MotionEvent 调用图明显变化，resolver 应拒绝而不是猜测。

## Exact fingerprint fallback

当前保留：

```text
gesture-frame-v1
gesture-frame-v2
```

它们代表已验证实现的代码指纹，不是版本白名单。

决策原则：

1. exact 唯一：保持 exact-authoritative；semantic 可 corroborate，但不能否决已验证 exact
2. exact 多候选：逐个验证 `convert_offset` 行为链；仅一个通过时接受 `exact-behavior-disambiguated`
3. exact 多候选且 0 个或多个行为候选通过：fail closed
4. exact 无候选：MotionEvent semantic 仅报告候选，不安装主 Hook；当前 ABI 未经证明时 fail closed
5. semantic 与 exact 指向同一唯一地址：记录 `semantic-corroborated`；冲突时记录但仍由唯一 exact authoritative

6179 实机当前主 Hook 日志可出现：

```text
detail=semantic-conflict-exact-authoritative
pattern=gesture-frame-v2
```

这表示 semantic 图在该编译版本上与 exact 结果不完全一致，但 `gesture-frame-v2` 仍是唯一已验证目标，因此采用 exact authoritative 路径；不是使用硬编码 RVA。

## ABI-transparent wrapper

主 Hook wrapper 使用 AArch64 汇编保存：

- `x0..x8`
- `q0..q7`
- `x30`

只读取：

- `w1`：`readyFinish`
- `w2`：`side`
- `s0`：horizontal distance

调用 gate helper 后恢复原调用现场，再进入 LSPosed trampoline。

这消除了早期版本根据 Pattern 在多个 C++ prototype 间切换的做法，但兼容前提仍是关键参数遵循当前 AAPCS64 布局。

## 从主目标继续派生其他语义目标

当前 `on_swipe_process` 不只用于主 gate，也作为进一步解析稳定 native 行为的锚点。

### BackGestureUtils::convert_offset

Launcher 8.x 共享返回进度函数：

```text
BackGestureUtils::convert_offset
```

生产代码不会硬编码已知 RVA，而是从 `on_swipe_process` caller 内部寻找重复 BL target，并验证：

- 调用后出现 `fmov s1, #20.0`
- candidate body 出现已验证输入保存与负值 guard
- candidate body 包含 `110.0f` 总进度尺度
- `on_swipe_process` 内至少 3 个 corroborating callsites
- qualified candidate 必须唯一

APK 静态参考：

```text
5459: RVA 0x773814
6174: RVA 0x60bb80
```

解析成功后由 `PROGRESS_V1` Hook 把共享进度坐标同步到用户阈值；失败则保持 legacy clamp fallback。

### Stock Back Release haptic callsite

真实 Release 触觉不是通过旧的 `GestureBackArrowView::check_and_perform_haptic_feedback` helper 进入 6179，而是来自：

```text
GestureStubViewWindow::handle_back_gesture
```

6174 静态逆向和 6179 运行时 trace 都确认参考 callsite RVA：

```text
0x654298
```

生产 resolver 验证完整：

```text
get_global_runtime
→ save runtime
→ constant 0
→ HapticFeedback_perform_ext_haptic_feedback
→ restore runtime
→ Runtime_dec_strong
```

只有唯一候选才允许 Ready→Release 750ms 去重；否则 stock Release haptic 保持不变。

## 安全约束

所有 semantic / derived resolver 都遵守同一原则：

- mapped ELF 元数据必须可验证
- imported symbol / relocation 必须唯一
- candidate 必须位于当前 `libapp_launcher.so` mapping
- 安装前重新检查目标结构
- 不从未知版本猜 RVA
- exact / semantic 冲突时优先 fail closed，只有已验证 authoritative 条件满足才接受
- Hook 后保存入口 probe，用于 restored / foreign patch 检测
- repair 前等待 active hook calls 退出
- mapping 变化后清空旧目标并重新解析

## 典型日志

主目标：

```text
HOOK_SCAN resolved ... pattern=gesture-frame-v2 resolver=gesture-frame-v2 detail=semantic-conflict-exact-authoritative
DP_GATE hook installed ... abi=transparent-s0
```

共享进度：

```text
PROGRESS_V1 convert_offset hook ready ... corroboratedCalls=...
```

Release haptic：

```text
HAPTIC_V2 release callsite ready ... feature=stock-back-release-haptic-v1
```

不确定：

```text
HOOK_SCAN install refused ... semantic/exact resolvers found no unique validated target
PROGRESS_V1 convert_offset unresolved ... keeping legacy clamp fallback
HAPTIC_V2 release callsite unresolved ... stock release remains untouched
```

## 参考实现

设计与安全模型曾对照：

- `wxxsfxyzm/MiuiBackGestureHook`：mapped ELF / PLT import、AArch64 behavior graph、unique-candidate fail-closed，以及 HyperRT 页面保护思路
- `zilewang7/HyperOS4SmallWindowInputFilter`：MiuiHome behavior-structure matcher 与完整候选约束

具体归属见根目录 [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md)。
