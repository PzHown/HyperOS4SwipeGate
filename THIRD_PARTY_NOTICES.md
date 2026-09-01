# Third-party notices

## MiuiBackGestureHook

Parts of SwipeGate's HyperOS Rust Runtime interoperability and safety design were informed by the open-source MiuiBackGestureHook project, including:

- AArch64 `send_broadcast` runtime-capture ABI preservation strategy
- reuse of Xiaomi's existing `com.android.systemui.fsgesture` carrier
- mapped ELF / PLT import parsing and behavior-graph based target resolution
- unique-candidate / fail-closed safety model
- HyperRT `MADV_DONTNEED` inline-hook page protection concept

Project information:

- Project: `wxxsfxyzm/MiuiBackGestureHook`
- Reference revision used during the original design comparison: `afce2aa8aa96f40f2351952cf17ec494982b9dec`
- License: Apache License 2.0

SwipeGate's implementation is independently reduced to its own targets and semantics. It maintains its own `GestureInputBackHelper::on_swipe_process` resolver, exact fingerprints, ABI-transparent wrapper, Hook health repair, `BackGestureUtils::convert_offset` progress resolver, HyperRT Ready/Release policy and function-scoped break-open implementation. It does not vendor MiuiBackGestureHook source and does not include that project's predictive-back, drawer/overview, Dart, SystemUI monitor or full launcher-profile feature set.

## HyperOS4SmallWindowInputFilter

The runtime behavior-structure matching approach and the "one complete candidate or fail closed" rule were also compared against the open-source HyperOS4SmallWindowInputFilter project:

- Project: `zilewang7/HyperOS4SmallWindowInputFilter`
- License: MIT

No HyperOS4SmallWindowInputFilter source is vendored into SwipeGate; SwipeGate uses its own resolver implementation and target semantics.
