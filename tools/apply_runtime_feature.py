from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    if old not in text:
        raise RuntimeError(f"patch target not found: {label}")
    return text.replace(old, new, 1)


main_path = Path("app/src/main/java/io/github/pzhown/hyperos4swipegate/MainActivity.kt")
main = main_path.read_text(encoding="utf-8")

main = replace_once(
    main,
    "import android.content.ClipboardManager\nimport android.content.Context\nimport android.content.Intent\n",
    "import android.content.ClipboardManager\nimport android.content.ComponentName\nimport android.content.Context\nimport android.content.Intent\nimport android.content.pm.PackageManager\n",
    "imports",
)

main = replace_once(
    main,
    'private const val PREF_DP_MIGRATED = "threshold_dp_migrated_v1"\n',
    'private const val PREF_DP_MIGRATED = "threshold_dp_migrated_v1"\n'
    'private const val LAUNCHER_ALIAS = "io.github.pzhown.hyperos4swipegate.LauncherAlias"\n',
    "launcher alias constant",
)

main = replace_once(
    main,
    '''private data class HookStatusSnapshot(
    val state: HookUiState,
    val launcherVersion: String = "",
    val thresholdDp: Int? = null,
    val detail: String = "",
) {''',
    '''private data class HookStatusSnapshot(
    val state: HookUiState,
    val launcherVersion: String = "",
    val thresholdDp: Int? = null,
    val detail: String = "",
    val lsposedStatus: String = "检测中",
    val hyosRuntimeStatus: String = "检测中",
    val zygiskNextStatus: String = "检测中",
) {''',
    "status fields",
)

main = replace_once(
    main,
    '        HookUiState.Error -> "模块异常"\n',
    '''        HookUiState.Error -> when {
            snapshot.detail.startsWith("LSPosed 版本不支持") -> "LSPosed 版本不支持"
            snapshot.detail.startsWith("Zygisk Next 版本不支持") -> "Zygisk Next 版本不支持"
            snapshot.detail.startsWith("未检测到 HyOSRuntime") -> "HyOSRuntime 不可用"
            else -> "模块异常"
        }
''',
    "error summary",
)

main = replace_once(
    main,
    '''                OverviewInfoRow(
                    "Launcher",
                    snapshot.launcherVersion.ifBlank { "读取中" },
                )
                OverviewInfoRow("配置通道", "LSPosed API 102 · 无 Root")''',
    '''                OverviewInfoRow(
                    "Launcher",
                    snapshot.launcherVersion.ifBlank { "读取中" },
                )
                OverviewInfoRow("LSPosed", snapshot.lsposedStatus)
                OverviewInfoRow("HyOS Runtime", snapshot.hyosRuntimeStatus)
                OverviewInfoRow("Zygisk Next", snapshot.zygiskNextStatus)
                OverviewInfoRow("配置通道", "LSPosed API 102 · 无 Root")''',
    "environment rows",
)

start = main.index("private fun collectHookStatusSnapshot(context: Context): HookStatusSnapshot {")
end = main.index("\n@Composable\nprivate fun LogsPage", start)
new_collect = '''private fun collectHookStatusSnapshot(context: Context): HookStatusSnapshot {
    val serviceSnapshot = XposedServiceBridge.snapshot(context)
    val runtimeSnapshot = RuntimeRequirementsBridge.snapshot(context)
    val launcherVersion = DiagnosticsCollector.readLauncherVersion(context)
    val threshold = serviceSnapshot.thresholdDp()

    val frameworkDisplay = listOf(
        runtimeSnapshot.frameworkName(),
        runtimeSnapshot.frameworkVersion(),
    ).filter { it.isNotBlank() }.joinToString(" ").ifBlank { "LSPosed" }

    val lsposedStatus = when {
        !runtimeSnapshot.serviceConnected() -> "未连接"
        runtimeSnapshot.lsposedSupported() ->
            "$frameworkDisplay · ${runtimeSnapshot.frameworkVersionCode()} ✓"
        else ->
            "$frameworkDisplay · ${runtimeSnapshot.frameworkVersionCode()}（需要 ≥ 7846）"
    }
    val hyosRuntimeStatus = when {
        !runtimeSnapshot.hyosSpawnerPresent() -> "系统未提供"
        runtimeSnapshot.hyosRuntimeDetected() -> "支持 ✓"
        else -> "未检测到"
    }
    val zygiskNextStatus = if (runtimeSnapshot.zygiskNextSupported()) {
        "≥ 1.5.0 · 支持 ✓"
    } else {
        "需要 ≥ 1.5.0"
    }

    fun status(state: HookUiState, detail: String = "") = HookStatusSnapshot(
        state = state,
        launcherVersion = launcherVersion,
        thresholdDp = threshold,
        detail = detail,
        lsposedStatus = lsposedStatus,
        hyosRuntimeStatus = hyosRuntimeStatus,
        zygiskNextStatus = zygiskNextStatus,
    )

    if (!serviceSnapshot.serviceConnected() || !runtimeSnapshot.serviceConnected()) {
        return status(
            HookUiState.Unknown,
            serviceSnapshot.error().ifBlank {
                runtimeSnapshot.error().ifBlank { "LSPosed 服务未连接。" }
            },
        )
    }

    // HyperOS Runtime support requires BOTH a sufficiently new LSPosed and
    // Zygisk Next 1.5.0+. Neither requirement is optional.
    if (!runtimeSnapshot.lsposedSupported()) {
        return status(
            HookUiState.Error,
            "LSPosed 版本不支持：需要 versionCode ≥ 7846。",
        )
    }

    if (!runtimeSnapshot.hyosSpawnerPresent()) {
        return status(
            HookUiState.Error,
            "未检测到 HyOSRuntime。",
        )
    }

    if (!runtimeSnapshot.launcherInScope()) {
        return status(
            HookUiState.Inactive,
            "LSPosed 作用域未包含系统桌面 com.miui.home。",
        )
    }

    if (!runtimeSnapshot.zygiskNextSupported()) {
        return status(
            HookUiState.Error,
            "Zygisk Next 版本不支持：需要 1.5.0+（HyOSRuntime）。",
        )
    }

    if (!serviceSnapshot.launcherLoaded()) {
        return status(
            HookUiState.Inactive,
            "未检测到 Launcher/HYOS 激活证据，请检查模块启用状态。",
        )
    }

    return when (serviceSnapshot.targetState()) {
        "RELOADING" -> status(
            HookUiState.Repairing,
            "LSPosed 正在更新模块代码。",
        )
        "FAILED" -> status(
            HookUiState.Error,
            "LSPosed 报告目标进程的模块更新失败。",
        )
        "STALE" -> status(
            HookUiState.Active,
            "系统桌面仍加载上一版本模块，重启系统桌面后可更新。",
        )
        else -> status(HookUiState.Active)
    }
}
'''
main = main[:start] + new_collect + main[end:]

main = replace_once(
    main,
    ''') {
    val context = androidx.compose.ui.platform.LocalContext.current

    LazyColumn(
        modifier = Modifier.fillMaxSize(),
        contentPadding = pagePadding(contentPadding),
        verticalArrangement = Arrangement.spacedBy(10.dp),
    ) {''',
    ''') {
    val context = androidx.compose.ui.platform.LocalContext.current
    var launcherIconHidden by remember { mutableStateOf(isLauncherIconHidden(context)) }

    LazyColumn(
        modifier = Modifier.fillMaxSize(),
        contentPadding = pagePadding(contentPadding),
        verticalArrangement = Arrangement.spacedBy(10.dp),
    ) {''',
    "about icon state",
)

main = replace_once(
    main,
    '''            Card(modifier = Modifier.fillMaxWidth(), cornerRadius = 20.dp) {
                SwitchPreference(
                    checked = floatingBar,''',
    '''            Card(modifier = Modifier.fillMaxWidth(), cornerRadius = 20.dp) {
                SwitchPreference(
                    checked = launcherIconHidden,
                    onCheckedChange = { hidden ->
                        if (setLauncherIconHidden(context, hidden)) {
                            launcherIconHidden = hidden
                        } else {
                            Toast.makeText(context, "桌面图标设置失败", Toast.LENGTH_SHORT).show()
                        }
                    },
                    title = "隐藏桌面图标",
                    summary = if (launcherIconHidden) {
                        "桌面图标已隐藏；仍可从 LSPosed 模块页打开"
                    } else {
                        "隐藏 SwipeGate 的桌面图标"
                    },
                )
                SwitchPreference(
                    checked = floatingBar,''',
    "about icon switch",
)

main = replace_once(
    main,
    '''private fun pagePadding(contentPadding: PaddingValues): PaddingValues = PaddingValues(''',
    '''private fun isLauncherIconHidden(context: Context): Boolean {
    val component = ComponentName(context.packageName, LAUNCHER_ALIAS)
    return context.packageManager.getComponentEnabledSetting(component) ==
        PackageManager.COMPONENT_ENABLED_STATE_DISABLED
}

private fun setLauncherIconHidden(context: Context, hidden: Boolean): Boolean = runCatching {
    val component = ComponentName(context.packageName, LAUNCHER_ALIAS)
    context.packageManager.setComponentEnabledSetting(
        component,
        if (hidden) {
            PackageManager.COMPONENT_ENABLED_STATE_DISABLED
        } else {
            PackageManager.COMPONENT_ENABLED_STATE_ENABLED
        },
        PackageManager.DONT_KILL_APP,
    )
    true
}.getOrDefault(false)

private fun pagePadding(contentPadding: PaddingValues): PaddingValues = PaddingValues(''',
    "launcher icon helpers",
)

main_path.write_text(main, encoding="utf-8")

manifest_path = Path("app/src/main/AndroidManifest.xml")
manifest = manifest_path.read_text(encoding="utf-8")
manifest = replace_once(
    manifest,
    '''        <activity
            android:name=".MainActivity"
            android:exported="true">
            <intent-filter>
                <action android:name="android.intent.action.MAIN" />
                <category android:name="android.intent.category.LAUNCHER" />
            </intent-filter>
        </activity>''',
    '''        <activity
            android:name=".MainActivity"
            android:exported="true">
            <intent-filter>
                <action android:name="android.intent.action.MAIN" />
                <category android:name="de.robv.android.xposed.category.MODULE_SETTINGS" />
            </intent-filter>
        </activity>

        <activity-alias
            android:name=".LauncherAlias"
            android:enabled="true"
            android:exported="true"
            android:icon="@mipmap/ic_launcher"
            android:label="@string/app_name"
            android:targetActivity=".MainActivity">
            <intent-filter>
                <action android:name="android.intent.action.MAIN" />
                <category android:name="android.intent.category.LAUNCHER" />
            </intent-filter>
        </activity-alias>''',
    "launcher alias manifest",
)
manifest_path.write_text(manifest, encoding="utf-8")

readme_path = Path("README.md")
readme = readme_path.read_text(encoding="utf-8")
readme = replace_once(
    readme,
    '| LSPosed | Modern API 102 |\n| 作用域 | `com.miui.home` |',
    '| LSPosed | Modern API 102 · versionCode ≥ 7846 |\n| Zygisk Next | 1.5.0+（HyOS Runtime） |\n| 作用域 | `com.miui.home` |',
    "readme requirements table",
)
readme = replace_once(
    readme,
    '> Launcher 8.0+ 为目标兼容范围。',
    '> LSPosed versionCode ≥ 7846 与 Zygisk Next 1.5.0+ 两项要求缺一不可。主页会以无 Root 方式检测 LSPosed 版本和实际 HyOSRuntime 能力。\n>\n> Launcher 8.0+ 为目标兼容范围。',
    "readme requirement note",
)
readme_path.write_text(readme, encoding="utf-8")

print("runtime feature patch applied")
