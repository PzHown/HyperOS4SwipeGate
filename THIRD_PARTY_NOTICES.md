# Third-party notices

## MiuiBackGestureHook

Parts of the HyperOS Rust Runtime broadcast interoperability design, including the AArch64 `send_broadcast` runtime-capture ABI preservation strategy and the use of Xiaomi's existing `com.android.systemui.fsgesture` carrier, were informed by the open-source MiuiBackGestureHook project. The dev semantic target resolver also follows the same general safety model: parse the mapped ELF/PLT imports, validate a stable AArch64 behavior graph, require a unique candidate, and fail closed when the structure is missing or ambiguous.

- Project: `wxxsfxyzm/MiuiBackGestureHook`
- Reference revision: `afce2aa8aa96f40f2351952cf17ec494982b9dec`
- License: Apache License 2.0

SwipeGate's resolver is independently reduced to the `on_swipe_process` threshold hook and keeps its own exact-pattern fallback, hook-health repair, and ABI-transparent `s0` forwarding. It does not include MiuiBackGestureHook's predictive-back, drawer/overview, Dart, SystemUI monitor, or full launcher-profile feature set.

## HyperOS4SmallWindowInputFilter

The runtime behavior-structure matching approach and the "one complete candidate or fail closed" rule were also compared against the open-source HyperOS4SmallWindowInputFilter project:

- Project: `zilewang7/HyperOS4SmallWindowInputFilter`
- License: MIT

No HyperOS4SmallWindowInputFilter source is vendored into SwipeGate; SwipeGate uses its own resolver implementation and target semantics.
