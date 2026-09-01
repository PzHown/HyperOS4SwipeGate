1. 新增 HyperOS 4 Launcher 8.0+「丰富侧滑震动反馈」Beta：进入 Ready 时补充一次原生 HyperRT 震动。
2. 6174 静态逆向 + 6179 实机 trace 已确认真实 Back Release HyperRT 调用点；仅在 Ready→Release 小于 750ms 时精确去重松手震动，Threshold/Three 与其他原生震动保持小米行为。
3. 新增 `BackGestureUtils::convert_offset` 语义解析与进度缩放：把 Launcher 原厂 `110dp × 0.8 = 88dp` 的 Ready 坐标同步扩展到用户阈值，修复高阈值时动画被压缩、Ready 动画短一截和触发瞬间跳进度的问题。
4. 增强 Launcher 8.x semantic/exact resolver、16 KB `MADV_DONTNEED` 页面保护与 Hook watchdog repair；解析不确定时 fail closed。
5. Break-open Beta 使用 `WindowTransitionUtil::is_merge_back_break_open_anim_support` 函数级安全 Hook，不修改 backing flag；已验证 Launcher 8.01.02.6174 / 6179。
6. README / TECHNICAL / SEMANTIC_RESOLVER 已按 0.8.2 当前实现重写，并补充 6179、Rootless 控制链路、Haptic V2、Progress V1 与 Page Guard 说明。
