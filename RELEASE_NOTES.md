## 0.9.2 更新日志（待验证发布）

1. 配置不再依赖 App 常驻。阈值、日志级别、触觉和 Break-open 开关作为完整配置写入桌面持久目录，native 启动时自行恢复，并迁移可恢复的旧缓存值。
2. 修复相同 nonce 重试被 SystemUI 丢弃、最终回包丢失、旧请求覆盖新设置的问题。重复请求限频重传，失败落盘也会重试，只有明确确认持久化后才标记成功。
3. 回包认证不再相信消息自报 UID。加入只通过受保护 carrier 传递的随机返回凭证，并校验平台可提供的真实发送者身份。
4. 移除永久 App 心跳线程。仅前台查询和有期限的待完成请求会继续轮询，后台超时或完成后停止。
5. 三个控制通道 native Hook 统一使用页保护安装器；GOT 修改保留原 Hook 链，补齐失败回滚和权限恢复，无法完整回滚时停止继续安装。
6. 增加配置持久化、通道与生命周期、GOT 故障注入的主机回归测试。完整 APK 构建和真机验证仍是发布前置条件。

升级后需重启 SystemUI 和桌面（建议直接重启手机），再打开 App 完成一次配置同步。旧版未落盘的触觉开关无法凭空恢复；完成这次同步后不再需要 App 常驻。技术边界和验收步骤见 `docs/RUNTIME_RELIABILITY.md`。

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
