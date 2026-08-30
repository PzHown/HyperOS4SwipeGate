# Dev Semantic Resolver

本文记录 `dev` 分支正在验证的新一代 `on_swipe_process` 定位方式。它不会改变 `main` 上已发布的 0.6.0，只有验证完成并明确发布后才进入正式版。

## 目标

0.6.0 已经摆脱固定函数 offset，但仍把精确机器码 Pattern 作为主定位依据。新的 dev 架构把主路径改成：

```text
libapp_launcher.so
        ↓
解析 ELF / DT_JMPREL / PLT import
        ↓
定位稳定的 MotionEvent imported API
        ↓
扫描 AArch64 executable segments
        ↓
验证 on_swipe_process 所在手势处理结构
        ↓
恰好 1 个候选 → Hook
0 / 2+ 候选 → 尝试已验证 exact Pattern fallback / fail closed
```

当前语义锚点使用以下调用顺序：

```text
input_MotionEvent_getActionMasked
input_MotionEvent_getActionIndex
input_MotionEvent_getRawX
input_MotionEvent_getActionIndex
input_MotionEvent_getRawY
```

resolver 不依赖 Launcher versionName，也不依赖目标函数固定 RVA。函数重新链接、地址变化、部分栈帧大小和保存位置变化时，只要已验证的行为结构仍存在且唯一，就可以继续解析。

## 函数入口结构

当前 resolver 接受三类已知 AArch64 frame family：

- legacy：5459 系列的 `sub sp` + `str d10` + `stp d9,d8` + FP/LR 保存结构
- modern：6174 系列的 `sub sp` + `stp d11,d10` + `stp d9,d8` + FP/LR 保存结构
- compact：较紧凑的 pre-index SIMD 保存结构

栈偏移不做精确字节绑定，但保存寄存器族和后续 MotionEvent 调用图必须成立。它仍然不是通用反编译器：如果编译器彻底改变函数拆分、调用图或关键参数寄存器，resolver 会拒绝而不是猜测。

## Exact Pattern fallback

现有两个已验证 Pattern 不删除：

- `8.01.02.5459-v1`
- `8.01.02.6174-v2`

规则：

1. semantic resolver 唯一，exact 无结果：接受 semantic
2. semantic 与 exact 都唯一且地址一致：接受 semantic
3. semantic 与 exact 指向不同地址：fail closed
4. semantic 无法唯一确认，但 exact 唯一：使用 exact fallback
5. 两边均无法唯一确认：fail closed

这让新 resolver 可以扩大对重新编译版本的容忍度，同时保留 0.6.0 已验证版本的保守兜底。

## ABI-transparent Hook wrapper

0.6.0 根据 Pattern 在两个 C++ prototype 间选择：

- PointPointer V1
- InlinePointFloats V2

新的 dev wrapper 改为 AArch64 汇编透明转发。入口保存：

- `x0..x8`
- `q0..q7`
- `x30`

只取出：

- `w1`：readyFinish
- `w2`：side
- `s0`：horizontalDistance

调用 SwipeGate gate helper 后恢复所有原始寄存器，只用新的距离覆盖 `s0`，再调用 LSPosed trampoline。

因此 Point 是指针还是拆成浮点寄存器，不再由 Pattern 决定 Hook prototype。

边界条件：这不是对任意未来 ABI 的保证。自动兼容要求 `horizontalDistance` 仍以 AAPCS64 标量浮点参数位于 `s0`，`readyFinish/side` 仍保持当前寄存器约定，且关键参数没有迁移到 stack-only ABI。

## 安全约束

- semantic candidate 必须唯一
- ELF dynamic table、symbol table、PLT relocation 必须可验证
- 每个作为锚点的 import 必须唯一解析
- 安装前再次重新验证目标结构
- semantic 与 exact resolver 冲突时不 Hook
- Hook 后仍保存原入口 probe，并沿用原有 foreign patch / restored patch 检测
- 修复 Hook 前仍等待 active hook calls 退出
- semantic 目标执行 unhook + rehook 前会重新运行语义验证

## 日志

成功时预期出现：

```text
HOOK_SCAN resolved ... resolver=semantic-motion-graph detail=legacy|modern|compact
DP_GATE hook installed ... abi=transparent-s0
```

如果 semantic resolver 未命中、但已知 Pattern 仍有效：

```text
HOOK_SCAN resolved ... resolver=8.01.02.5459-v1
```

不确定时：

```text
HOOK_SCAN install refused ... semantic/exact resolvers found no unique validated target
```

## 参考实现

设计对照了：

- `wxxsfxyzm/MiuiBackGestureHook` 的 Runtime Profile Resolver：ELF/PLT import + AArch64 调用图 + unique-candidate fail-closed
- `zilewang7/HyperOS4SmallWindowInputFilter` 的 MiuiHome behavior-structure matcher

具体归属见根目录 `THIRD_PARTY_NOTICES.md`。
