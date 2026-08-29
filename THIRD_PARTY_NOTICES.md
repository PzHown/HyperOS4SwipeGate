# Third-party notices

## MiuiBackGestureHook

Parts of the HyperOS Rust Runtime broadcast interoperability design, including the AArch64 `send_broadcast` runtime-capture ABI preservation strategy and the use of Xiaomi's existing `com.android.systemui.fsgesture` carrier, were informed by the open-source MiuiBackGestureHook project:

- Project: `wxxsfxyzm/MiuiBackGestureHook`
- Reference revision: `afce2aa8aa96f40f2351952cf17ec494982b9dec`
- License: Apache License 2.0

SwipeGate's implementation is independently scoped to its threshold/configuration/status protocol and does not include MiuiBackGestureHook's gesture, predictive-back, SystemUI monitor, Dart resolver, or launcher-profile implementation.
