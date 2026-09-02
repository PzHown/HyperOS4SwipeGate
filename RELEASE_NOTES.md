## 0.9.1 更新日志

1. 修复 Launcher `8.01.02.6230` 下 Native Hook 目标识别失败的问题。
   新版 Launcher 中旧版 exact fingerprint 会同时命中两个函数，0.9.1 不再因为多候选直接拒绝安装，而是使用 `BackGestureUtils::convert_offset` 的完整行为链逐个验证候选，仅在能够唯一证明真实 `on_swipe_process` 时安装 Hook。

2. 保持 fail-closed，不为 6230 写死 RVA。
   本次兼容没有加入版本白名单、固定地址、优先新版 pattern 或“取第一个候选”等不安全 fallback；如果多个候选仍无法唯一验证，模块会继续保持原厂行为。

3. 收紧 semantic resolver 的主 Hook 权限。
   反编译确认当前 MotionEvent semantic graph 在 6230 会定位到外层 `MotionEvent*` 事件处理函数，与主 Hook 的 `w1 / w2 / s0` ABI 不一致。因此 semantic 目前只用于诊断和交叉验证，不再在没有可信 exact 目标时单独接管主 Hook，降低 Launcher 崩溃风险。

4. 改进 Native diagnostics。
   多候选时会分别记录 candidate RVA、fingerprint、`convert_offset` RVA、corroborated callsites 和 qualification 结果；失败日志也会区分 exact / semantic candidate 数量及具体 failure reason，方便后续适配 Launcher 更新。

5. 保持已有 Launcher 兼容路径不变。
   对原本只有一个已验证 exact fingerprint 的 5459 / 6174 / 6179 等版本，仍沿用原来的 exact-authoritative 路径，不额外强制新的行为验证条件，尽量减少回归面。

### 本次重点

- 新增 Launcher 8.01.02.6230 安全兼容
- 多 exact 候选可通过行为证据自动消歧
- semantic-only 错 ABI 风险收口
- 诊断信息更完整
