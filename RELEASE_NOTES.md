1. 新增 HyperOS 4 Launcher 8.0+「丰富侧滑震动反馈」实验功能：进入 Ready 时补充一次原生 HyperRT 震动。
2. 6174 静态逆向 + 6179 实机 trace 已确认真实 Back Release HyperRT 调用点；仅在 Ready→Release 小于 750ms 时精确去重松手震动，Threshold/Three 与其他原生震动保持小米行为。
3. 重构 `BackGestureUtils::convert_offset` 共享进度映射：Ready 前把原厂 `0→88dp` 连续映射到 `0→自定义阈值`，让动画 Ready、状态判断与用户门槛一致，修复高阈值时动画被压缩、Ready 动画短一截和触发瞬间跳进度的问题。
4. 修正高阈值下 Ready→完整进度距离随阈值一起变长的问题：Ready 后固定保留原厂 `22dp` 的 `88→110dp` 区间，并使用 C1 连续的 cubic Hermite 过渡恢复到 1:1 距离尺度，避免新的速度断层。
5. 增强 Launcher 8.x semantic/exact resolver、16 KB `MADV_DONTNEED` 页面保护与 Hook watchdog repair；解析不确定时 fail closed。
6. Break-open Beta 使用 `WindowTransitionUtil::is_merge_back_break_open_anim_support` 函数级安全 Hook，不修改 backing flag；已验证 Launcher 8.01.02.6174 / 6179。
7. 完善 App → SystemUI → HyOS Runtime / Launcher native 的 Rootless 控制与诊断链路，并更新 README / TECHNICAL / SEMANTIC_RESOLVER 中的兼容性与排查说明。
