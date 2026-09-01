1. 新增 HyperOS 4 Launcher 8.0+「丰富侧滑震动反馈」Beta：进入 Ready 时补充一次原生 HyperRT 震动
2. 精确识别真实 Back Release HyperRT 调用点，仅在 Ready→Release 小于 750ms 时去重松手震动；Threshold/Three 与其他原生震动保持小米行为
3. 增强 Launcher 8.x 语义解析、16 KB 页面保护与 Hook 健康修复，降低 HyperRT MADV_DONTNEED 导致的 Hook 丢失
4. Break-open Beta 使用函数级安全 Hook，不修改 backing flag；已验证 Launcher 8.01.02.6174 / 6179
