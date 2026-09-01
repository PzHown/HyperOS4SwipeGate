from pathlib import Path


def replace_once(path: str, old: str, new: str, label: str) -> None:
    p = Path(path)
    text = p.read_text()
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected 1 match, got {count}")
    p.write_text(text.replace(old, new, 1))


def replace_exact_count(path: str, old: str, new: str, count_expected: int, label: str) -> None:
    p = Path(path)
    text = p.read_text()
    count = text.count(old)
    if count != count_expected:
        raise SystemExit(f"{label}: expected {count_expected} matches, got {count}")
    p.write_text(text.replace(old, new))


replace_once(
    "app/build.gradle.kts",
    "        // dev.19: production cleanup after runtime-proven release-callsite dedup; caller trace removed.\n"
    "        versionCode = 61\n"
    "        versionName = \"0.8.1-dev.19\"\n",
    "        // 0.8.2-beta1: runtime-proven Ready/Release haptic dedup and production log convergence.\n"
    "        versionCode = 62\n"
    "        versionName = \"0.8.2-beta1\"\n",
    "version bump",
)

old_notes = (
    "1. 修复 0.8.0 正式版中 LSPosed Java 入口类被 R8 裁剪导致的 ClassNotFoundException\\n"
    "2. 保留资源与无用代码压缩，同时关闭类名和成员名混淆\\n"
    "3. 增加 Release DEX 入口类完整性校验，防止同类问题再次发布"
)
new_notes = (
    "1. 新增 HyperOS 4 Launcher 8.0+「丰富侧滑震动反馈」Beta：进入 Ready 时补充一次原生 HyperRT 震动\\n"
    "2. 精确识别真实 Back Release HyperRT 调用点，仅在 Ready→Release 小于 750ms 时去重松手震动；Threshold/Three 与其他原生震动保持小米行为\\n"
    "3. 增强 Launcher 8.x 语义解析、16 KB 页面保护与 Hook 健康修复，降低 HyperRT MADV_DONTNEED 导致的 Hook 丢失\\n"
    "4. Break-open Beta 使用函数级安全 Hook，不修改 backing flag；已验证 Launcher 8.01.02.6174 / 6179"
)

workflow = Path(".github/workflows/build.yml")
text = workflow.read_text()
old_release_block = f'''          gh release delete release-latest --yes --cleanup-tag || true\n          gh release delete "$RELEASE_TAG" --yes --cleanup-tag || true\n          gh release create "$RELEASE_TAG" \\\n            dist/SwipeGate-release.apk \\\n            --target "$GITHUB_SHA" \\\n            --title "SwipeGate v${{VERSION_NAME}}" \\\n            --notes $'{old_notes}'\n'''
new_release_block = f'''          gh release delete release-latest --yes --cleanup-tag || true\n          gh release delete "$RELEASE_TAG" --yes --cleanup-tag || true\n          RELEASE_NOTES=$'{new_notes}'\n          RELEASE_FLAGS=()\n          if [[ "$VERSION_NAME" == *-* ]]; then\n            RELEASE_FLAGS+=(--prerelease)\n          fi\n          gh release create "$RELEASE_TAG" \\\n            dist/SwipeGate-release.apk \\\n            --target "$GITHUB_SHA" \\\n            --title "SwipeGate v${{VERSION_NAME}}" \\\n            --notes "$RELEASE_NOTES" \\\n            "${{RELEASE_FLAGS[@]}}"\n'''
if text.count(old_release_block) != 1:
    raise SystemExit(f"project release block: expected 1 match, got {text.count(old_release_block)}")
text = text.replace(old_release_block, new_release_block, 1)

old_lsposed_notes = f"          RELEASE_NOTES=$'{old_notes}'\n"
new_lsposed_notes = f"          RELEASE_NOTES=$'{new_notes}'\n"
if text.count(old_lsposed_notes) != 1:
    raise SystemExit(f"LSPosed notes: expected 1 match, got {text.count(old_lsposed_notes)}")
text = text.replace(old_lsposed_notes, new_lsposed_notes, 1)
workflow.write_text(text)

# Release invariants: beta must be a prerelease and production caller spam must stay removed.
gradle = Path("app/build.gradle.kts").read_text()
main = Path("native/src/main.cpp").read_text()
workflow_text = workflow.read_text()
for required in [
    'versionCode = 62',
    'versionName = "0.8.2-beta1"',
]:
    if required not in gradle:
        raise SystemExit(f"missing version invariant: {required}")
for forbidden in [
    "HAPTIC_TRACE stock-call",
    "gHapticTraceSequence",
    "traceStockHapticCall",
]:
    if forbidden in main:
        raise SystemExit(f"production trace not converged: {forbidden}")
for required in [
    "kReadyReleaseDedupMs = 750",
    "ready-release-callsite",
    "stock-back-release-haptic-v1",
]:
    if required not in main:
        raise SystemExit(f"haptic invariant missing: {required}")
for required in [
    "RELEASE_FLAGS+=(--prerelease)",
    "0.8.2-beta1",
    "Ready→Release 小于 750ms",
]:
    if required not in workflow_text and required != "0.8.2-beta1":
        raise SystemExit(f"release workflow invariant missing: {required}")

# One-shot staging files should not remain in the release tree.
Path("scripts/apply_082_beta1_release.py").unlink()
Path(".github/workflows/run-082-beta1-release.yml").unlink()
