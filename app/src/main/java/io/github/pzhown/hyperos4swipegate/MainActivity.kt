package io.github.pzhown.hyperos4swipegate

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.asPaddingValues
import androidx.compose.foundation.layout.defaultMinSize
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.navigationBars
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.preference.PreferenceManager
import io.github.pzhown.hyperos4swipegate.ui.liquid.LiquidFloatingBottomBar
import io.github.pzhown.hyperos4swipegate.ui.liquid.LiquidFloatingBottomBarItem
import java.util.concurrent.TimeUnit
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import top.yukonga.miuix.kmp.basic.Button
import top.yukonga.miuix.kmp.basic.Card
import top.yukonga.miuix.kmp.basic.CardDefaults
import top.yukonga.miuix.kmp.basic.Icon
import top.yukonga.miuix.kmp.basic.NavigationBar
import top.yukonga.miuix.kmp.basic.NavigationBarDisplayMode
import top.yukonga.miuix.kmp.basic.NavigationBarItem
import top.yukonga.miuix.kmp.basic.Scaffold
import top.yukonga.miuix.kmp.basic.SmallTitle
import top.yukonga.miuix.kmp.basic.Text
import top.yukonga.miuix.kmp.basic.TopAppBar
import top.yukonga.miuix.kmp.blur.isRuntimeShaderSupported
import top.yukonga.miuix.kmp.blur.layerBackdrop
import top.yukonga.miuix.kmp.blur.rememberLayerBackdrop
import top.yukonga.miuix.kmp.icon.MiuixIcons
import top.yukonga.miuix.kmp.icon.extended.Home
import top.yukonga.miuix.kmp.icon.extended.Info
import top.yukonga.miuix.kmp.icon.extended.ListView
import top.yukonga.miuix.kmp.icon.extended.Settings
import top.yukonga.miuix.kmp.preference.SliderPreference
import top.yukonga.miuix.kmp.preference.SwitchPreference
import top.yukonga.miuix.kmp.theme.ColorSchemeMode
import top.yukonga.miuix.kmp.theme.MiuixTheme
import top.yukonga.miuix.kmp.theme.ThemeController
import kotlin.math.roundToInt

private const val PREF_UI_FLOATING_BAR = "ui_floating_bottom_bar"
private const val PREF_UI_LIQUID_GLASS = "ui_liquid_glass"
private const val PREF_DP_MIGRATED = "threshold_dp_migrated_v1"

private enum class MainTab(val title: String, val icon: ImageVector) {
    Overview("概览", MiuixIcons.Home),
    Settings("设置", MiuixIcons.Settings),
    Logs("日志", MiuixIcons.ListView),
    About("关于", MiuixIcons.Info),
}

private enum class HookUiState { Loading, Active, Repairing, Conflict, Error, Inactive, Unknown }

private data class HookStatusSnapshot(
    val state: HookUiState,
    val launcherVersion: String = "",
    val thresholdDp: Int? = null,
    val detail: String = "",
) {
    companion object {
        fun loading() = HookStatusSnapshot(HookUiState.Loading)
    }
}

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        setContent { SwipeGateManager() }
    }
}

@Composable
private fun SwipeGateManager() {
    val context = androidx.compose.ui.platform.LocalContext.current
    val prefs = remember { PreferenceManager.getDefaultSharedPreferences(context) }
    val themeController = remember { ThemeController(ColorSchemeMode.System) }
    var selectedTab by remember { mutableIntStateOf(0) }
    var floatingBar by remember { mutableStateOf(prefs.getBoolean(PREF_UI_FLOATING_BAR, true)) }
    var liquidGlass by remember { mutableStateOf(prefs.getBoolean(PREF_UI_LIQUID_GLASS, true)) }
    val blurSupported = isRuntimeShaderSupported()

    MiuixTheme(controller = themeController) {
        val surfaceColor = MiuixTheme.colorScheme.background
        val backdrop = rememberLayerBackdrop {
            drawRect(surfaceColor)
            drawContent()
        }
        val currentTab = MainTab.entries[selectedTab]
        val captureForLiquid = floatingBar && liquidGlass && blurSupported

        val bottomBar: @Composable () -> Unit = {
            Box(modifier = Modifier.fillMaxWidth()) {
                if (floatingBar) {
                    LiquidFloatingBottomBar(
                        modifier = Modifier
                            .align(Alignment.BottomCenter)
                            .padding(
                                bottom = 12.dp + WindowInsets.navigationBars
                                    .asPaddingValues()
                                    .calculateBottomPadding(),
                            ),
                        selectedIndex = { selectedTab },
                        onSelected = { selectedTab = it },
                        backdrop = backdrop,
                        tabsCount = MainTab.entries.size,
                        isBlurEnabled = liquidGlass && blurSupported,
                    ) {
                        MainTab.entries.forEachIndexed { index, tab ->
                            LiquidFloatingBottomBarItem(
                                onClick = { selectedTab = index },
                                modifier = Modifier.defaultMinSize(minWidth = 76.dp),
                            ) {
                                Icon(imageVector = tab.icon, contentDescription = tab.title)
                                Text(
                                    text = tab.title,
                                    fontSize = 11.sp,
                                    lineHeight = 14.sp,
                                    maxLines = 1,
                                    softWrap = false,
                                    overflow = TextOverflow.Visible,
                                )
                            }
                        }
                    }
                } else {
                    StandardBottomBar(
                        modifier = Modifier
                            .align(Alignment.BottomCenter)
                            .fillMaxWidth(),
                        selectedTab = selectedTab,
                        onSelected = { selectedTab = it },
                    )
                }
            }
        }

        Scaffold(
            topBar = {
                TopAppBar(
                    title = if (currentTab == MainTab.Overview) "HyperOS4 SwipeGate" else currentTab.title,
                    largeTitle = if (currentTab == MainTab.Overview) "HyperOS4 SwipeGate" else currentTab.title,
                    subtitle = "",
                )
            },
            bottomBar = bottomBar,
        ) { innerPadding ->
            Box(
                modifier = Modifier
                    .fillMaxSize()
                    .then(if (captureForLiquid) Modifier.layerBackdrop(backdrop) else Modifier),
            ) {
                when (currentTab) {
                    MainTab.Overview -> OverviewPage(innerPadding)
                    MainTab.Settings -> SettingsPage(innerPadding)
                    MainTab.Logs -> LogsPage(innerPadding)
                    MainTab.About -> AboutPage(
                        contentPadding = innerPadding,
                        floatingBar = floatingBar,
                        liquidGlass = liquidGlass,
                        blurSupported = blurSupported,
                        onFloatingBarChanged = {
                            floatingBar = it
                            prefs.edit().putBoolean(PREF_UI_FLOATING_BAR, it).apply()
                        },
                        onLiquidGlassChanged = {
                            liquidGlass = it
                            prefs.edit().putBoolean(PREF_UI_LIQUID_GLASS, it).apply()
                        },
                    )
                }
            }
        }
    }
}

@Composable
private fun StandardBottomBar(
    modifier: Modifier,
    selectedTab: Int,
    onSelected: (Int) -> Unit,
) {
    NavigationBar(
        modifier = modifier,
        color = MiuixTheme.colorScheme.surface,
        showDivider = true,
        mode = NavigationBarDisplayMode.IconAndText,
    ) {
        StandardBottomItems(selectedTab, onSelected)
    }
}

@Composable
private fun RowScope.StandardBottomItems(selectedTab: Int, onSelected: (Int) -> Unit) {
    MainTab.entries.forEachIndexed { index, tab ->
        NavigationBarItem(
            selected = selectedTab == index,
            onClick = { onSelected(index) },
            icon = tab.icon,
            label = tab.title,
        )
    }
}

@Composable
private fun OverviewPage(contentPadding: PaddingValues) {
    var snapshot by remember { mutableStateOf(HookStatusSnapshot.loading()) }
    LaunchedEffect(Unit) {
        snapshot = withContext(Dispatchers.IO) { collectHookStatusSnapshot() }
    }

    val dark = isSystemInDarkTheme()
    val statusBackground = when (snapshot.state) {
        HookUiState.Active -> if (dark) Color(0xFF183D28) else Color(0xFFD9F7E2)
        HookUiState.Repairing -> if (dark) Color(0xFF4B3B12) else Color(0xFFFFF1BF)
        HookUiState.Conflict, HookUiState.Error -> if (dark) Color(0xFF4A2424) else Color(0xFFFFE0E0)
        else -> MiuixTheme.colorScheme.surfaceContainer
    }
    val statusContent = when (snapshot.state) {
        HookUiState.Active -> if (dark) Color(0xFFB9F6CA) else Color(0xFF102E1A)
        HookUiState.Repairing -> if (dark) Color(0xFFFFE08A) else Color(0xFF4B3700)
        HookUiState.Conflict, HookUiState.Error -> if (dark) Color(0xFFFFB4AB) else Color(0xFF5A1010)
        else -> MiuixTheme.colorScheme.onSurfaceContainer
    }
    val primaryLabel = when (snapshot.state) {
        HookUiState.Active -> "已激活"
        HookUiState.Repairing -> "正在修复"
        HookUiState.Conflict, HookUiState.Error -> "异常"
        HookUiState.Inactive -> "未激活"
        HookUiState.Unknown -> "状态未知"
        HookUiState.Loading -> "检测中"
    }
    val hookLabel = when (snapshot.state) {
        HookUiState.Active -> "正常"
        HookUiState.Repairing -> "修复中"
        HookUiState.Conflict -> "冲突"
        HookUiState.Error -> "失败"
        HookUiState.Inactive -> "未加载"
        HookUiState.Unknown -> "未知"
        HookUiState.Loading -> "检测中"
    }

    LazyColumn(
        modifier = Modifier.fillMaxSize(),
        contentPadding = PaddingValues(
            start = 20.dp,
            top = contentPadding.calculateTopPadding() + 4.dp,
            end = 20.dp,
            bottom = contentPadding.calculateBottomPadding() + 18.dp,
        ),
        verticalArrangement = Arrangement.spacedBy(14.dp),
    ) {
        item {
            Card(
                cornerRadius = 20.dp,
                colors = CardDefaults.defaultColors(color = statusBackground, contentColor = statusContent),
            ) {
                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(horizontal = 24.dp, vertical = 22.dp),
                    horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Column {
                        Text(primaryLabel, fontSize = 28.sp, fontWeight = FontWeight.Bold)
                        Spacer(modifier = Modifier.height(8.dp))
                        Text("Hook $hookLabel", fontSize = 17.sp)
                    }
                    if (snapshot.state == HookUiState.Active) {
                        Text("✓", fontSize = 60.sp, fontWeight = FontWeight.Bold)
                    }
                }
            }
        }

        item {
            Card(cornerRadius = 20.dp) {
                OverviewInfoRow(
                    "当前门槛",
                    snapshot.thresholdDp?.let { if (it == 0) "默认（88 dp）" else "$it dp" } ?: "读取中",
                )
                OverviewInfoRow("Launcher", snapshot.launcherVersion.ifBlank { "RELEASE-8.01.02.5459" })
            }
        }

        if (snapshot.state in listOf(HookUiState.Conflict, HookUiState.Error, HookUiState.Unknown)) {
            item {
                SmallTitle("状态说明")
                Card(cornerRadius = 20.dp) {
                    Text(
                        text = snapshot.detail.ifBlank { "请查看日志了解详情。" },
                        modifier = Modifier.padding(horizontal = 24.dp, vertical = 18.dp),
                    )
                }
            }
        }
    }
}

@Composable
private fun OverviewInfoRow(label: String, value: String) {
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .padding(horizontal = 24.dp, vertical = 14.dp),
    ) {
        Text(label, fontSize = 18.sp)
        Spacer(modifier = Modifier.height(3.dp))
        Text(value, fontSize = 15.sp, color = MiuixTheme.colorScheme.onSurfaceVariantSummary)
    }
}

private fun collectHookStatusSnapshot(): HookStatusSnapshot {
    val script = """
        PID=$(pidof com.miui.home 2>/dev/null | awk '{print $1}')
        echo "PID=${'$'}PID"
        echo "THRESHOLD=$(getprop persist.hyperos4swipegate.threshold_dp)"
        VERSION=$(dumpsys package com.miui.home 2>/dev/null | sed -n 's/^[[:space:]]*versionName=//p' | head -n 1)
        echo "LAUNCHER_VERSION=${'$'}VERSION"
        echo "__HOOK_LOG__"
        logcat -d -v raw -s HyperOS4SwipeGateNative:* 2>/dev/null | tail -n 160
    """.trimIndent()

    return try {
        val process = ProcessBuilder("su", "-c", script).redirectErrorStream(true).start()
        val lines = process.inputStream.bufferedReader().use { it.readLines() }
        process.waitFor(6, TimeUnit.SECONDS)

        var pid = ""
        var launcherVersion = ""
        var thresholdDp: Int? = null
        var state = HookUiState.Unknown
        var detail = ""
        var inLog = false

        for (line in lines) {
            when {
                line.startsWith("PID=") -> pid = line.substringAfter("PID=").trim()
                line.startsWith("THRESHOLD=") -> thresholdDp = line.substringAfter("THRESHOLD=").trim().toIntOrNull()
                line.startsWith("LAUNCHER_VERSION=") -> launcherVersion = line.substringAfter("LAUNCHER_VERSION=").trim()
                line == "__HOOK_LOG__" -> inLog = true
                inLog -> when {
                    line.contains("foreign patch detected", ignoreCase = true) ||
                        line.contains("entry became foreign", ignoreCase = true) -> {
                        state = HookUiState.Conflict
                        detail = "Hook 与其他模块冲突。"
                    }
                    line.contains("starting unhook+rehook repair", ignoreCase = true) -> {
                        state = HookUiState.Repairing
                        detail = "Hook 正在恢复。"
                    }
                    line.contains("repaired successfully", ignoreCase = true) ||
                        line.contains("DP_GATE hook installed", ignoreCase = true) ||
                        line.contains("HOOK_HEALTH healthy", ignoreCase = true) -> {
                        state = HookUiState.Active
                        detail = "Hook 工作正常。"
                    }
                    line.contains("hook_func failed", ignoreCase = true) ||
                        line.contains("repair failed", ignoreCase = true) ||
                        line.contains("repair unavailable", ignoreCase = true) ||
                        line.contains("repair aborted", ignoreCase = true) -> {
                        state = HookUiState.Error
                        detail = "Hook 未正常工作，请查看日志。"
                    }
                }
            }
        }

        if (pid.isBlank()) {
            state = HookUiState.Inactive
            detail = "系统桌面未运行。"
        }

        HookStatusSnapshot(state, launcherVersion, thresholdDp, detail)
    } catch (e: Exception) {
        HookStatusSnapshot(HookUiState.Unknown, detail = "状态读取失败。")
    }
}

@Composable
private fun SettingsPage(contentPadding: PaddingValues) {
    val context = androidx.compose.ui.platform.LocalContext.current
    val prefs = remember { PreferenceManager.getDefaultSharedPreferences(context) }
    var thresholdDp by remember { mutableFloatStateOf(loadAndMigrateThresholdDp(context).toFloat()) }
    var applyStatus by remember { mutableStateOf("") }

    LazyColumn(
        modifier = Modifier.fillMaxSize(),
        contentPadding = PaddingValues(
            start = 20.dp,
            top = contentPadding.calculateTopPadding() + 4.dp,
            end = 20.dp,
            bottom = contentPadding.calculateBottomPadding() + 18.dp,
        ),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        item { SmallTitle("手势") }
        item {
            Card {
                val value = thresholdDp.roundToInt()
                SliderPreference(
                    value = thresholdDp,
                    onValueChange = {
                        thresholdDp = it.roundToInt().toFloat()
                        applyStatus = ""
                    },
                    onValueChangeFinished = {
                        val applied = thresholdDp.roundToInt().coerceIn(0, ConfigBridge.MAX_THRESHOLD_DP)
                        prefs.edit().putInt(ConfigBridge.PREF_KEY_THRESHOLD_DP, applied).apply()
                        ConfigBridge.applyThresholdDpAsync(context, applied) { result ->
                            applyStatus = when {
                                !result.success() -> "应用失败：${result.message()}"
                                result.value() == 0 -> "已应用：默认（88 dp）"
                                result.value() <= ConfigBridge.STOCK_THRESHOLD_DP -> "已应用：88 dp（系统下限）"
                                else -> "已应用：${result.value()} dp"
                            }
                        }
                    },
                    title = "侧边栏触发距离",
                    summary = if (value == 0) "默认（88 dp）" else "$value dp",
                    valueRange = 0f..ConfigBridge.MAX_THRESHOLD_DP.toFloat(),
                    steps = ConfigBridge.MAX_THRESHOLD_DP - 1,
                    showKeyPoints = false,
                )
                if (applyStatus.isNotBlank()) {
                    Text(
                        text = applyStatus,
                        modifier = Modifier.padding(start = 24.dp, end = 24.dp, bottom = 16.dp),
                        color = if (applyStatus.startsWith("应用失败")) MiuixTheme.colorScheme.error
                        else MiuixTheme.colorScheme.onSurfaceVariantSummary,
                    )
                }
            }
        }

        item { SmallTitle("说明") }
        item {
            Card {
                Text(
                    "超过 88 dp 后才会延后侧边栏，返回手势不变。",
                    modifier = Modifier.padding(horizontal = 24.dp, vertical = 18.dp),
                )
            }
        }
    }
}

@Composable
private fun LogsPage(contentPadding: PaddingValues) {
    val context = androidx.compose.ui.platform.LocalContext.current
    val scope = rememberCoroutineScope()
    var logs by remember { mutableStateOf("读取日志…") }
    var loading by remember { mutableStateOf(true) }

    fun refresh() {
        loading = true
        scope.launch {
            logs = withContext(Dispatchers.IO) { LogFragment.collectDiagnostics() }
            loading = false
        }
    }

    LaunchedEffect(Unit) { refresh() }

    LazyColumn(
        modifier = Modifier.fillMaxSize(),
        contentPadding = PaddingValues(
            start = 20.dp,
            top = contentPadding.calculateTopPadding() + 4.dp,
            end = 20.dp,
            bottom = contentPadding.calculateBottomPadding() + 18.dp,
        ),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        item {
            Card {
                Row(
                    modifier = Modifier.fillMaxWidth().padding(16.dp),
                    horizontalArrangement = Arrangement.spacedBy(10.dp),
                ) {
                    Button(onClick = { refresh() }, enabled = !loading, modifier = Modifier.weight(1f)) {
                        Text(if (loading) "读取中" else "刷新")
                    }
                    Button(
                        onClick = {
                            val clipboard = context.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
                            clipboard.setPrimaryClip(ClipData.newPlainText("HyperOS4 SwipeGate logs", logs))
                            Toast.makeText(context, "日志已复制", Toast.LENGTH_SHORT).show()
                        },
                        enabled = logs.isNotBlank() && !loading,
                        modifier = Modifier.weight(1f),
                    ) { Text("复制") }
                }
            }
        }
        item { SmallTitle("诊断") }
        item {
            Card {
                Text(
                    text = logs,
                    modifier = Modifier.padding(18.dp),
                    fontFamily = FontFamily.Monospace,
                    fontSize = 11.sp,
                )
            }
        }
    }
}

@Composable
private fun AboutPage(
    contentPadding: PaddingValues,
    floatingBar: Boolean,
    liquidGlass: Boolean,
    blurSupported: Boolean,
    onFloatingBarChanged: (Boolean) -> Unit,
    onLiquidGlassChanged: (Boolean) -> Unit,
) {
    val context = androidx.compose.ui.platform.LocalContext.current

    LazyColumn(
        modifier = Modifier.fillMaxSize(),
        contentPadding = PaddingValues(
            start = 20.dp,
            top = contentPadding.calculateTopPadding() + 4.dp,
            end = 20.dp,
            bottom = contentPadding.calculateBottomPadding() + 18.dp,
        ),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        item { SmallTitle("界面") }
        item {
            Card {
                SwitchPreference(
                    checked = floatingBar,
                    onCheckedChange = onFloatingBarChanged,
                    title = "悬浮底栏",
                    summary = "底部导航改为悬浮样式",
                )
                SwitchPreference(
                    checked = liquidGlass && blurSupported,
                    onCheckedChange = onLiquidGlassChanged,
                    title = "液态玻璃",
                    summary = if (blurSupported) {
                        "启用模糊与折射效果"
                    } else {
                        "当前设备不支持"
                    },
                    enabled = floatingBar && blurSupported,
                )
            }
        }

        item { SmallTitle("模块") }
        item {
            Card {
                Column(modifier = Modifier.padding(horizontal = 24.dp, vertical = 20.dp)) {
                    Text("HyperOS4 SwipeGate")
                    Spacer(modifier = Modifier.height(6.dp))
                    Text(
                        "${BuildConfig.VERSION_NAME} (${BuildConfig.VERSION_CODE})",
                        color = MiuixTheme.colorScheme.onSurfaceVariantSummary,
                    )
                    Spacer(modifier = Modifier.height(14.dp))
                    Text("控制 HyperOS 4 侧滑停顿触发距离")
                    Spacer(modifier = Modifier.height(8.dp))
                    Text("适配 Launcher RELEASE-8.01.02.5459", color = MiuixTheme.colorScheme.onSurfaceVariantSummary)
                }
            }
        }

        item {
            Card {
                Button(
                    onClick = {
                        context.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse("https://github.com/PzHown/HyperOS4SwipeGate")))
                    },
                    modifier = Modifier.padding(16.dp).fillMaxWidth(),
                ) { Text("GitHub 项目") }
            }
        }
    }
}

private fun loadAndMigrateThresholdDp(context: Context): Int {
    val prefs = PreferenceManager.getDefaultSharedPreferences(context)
    if (prefs.getBoolean(PREF_DP_MIGRATED, false)) {
        return prefs.getInt(ConfigBridge.PREF_KEY_THRESHOLD_DP, ConfigBridge.DEFAULT_THRESHOLD_DP)
            .coerceIn(0, ConfigBridge.MAX_THRESHOLD_DP)
    }

    var migrated = prefs.getInt(ConfigBridge.PREF_KEY_THRESHOLD_DP, ConfigBridge.DEFAULT_THRESHOLD_DP)
    when {
        prefs.contains(ConfigBridge.LEGACY_PREF_KEY_THRESHOLD_PX) -> {
            val legacyPx = prefs.getInt(ConfigBridge.LEGACY_PREF_KEY_THRESHOLD_PX, 0)
            migrated = if (legacyPx <= 0) 0 else {
                val density = context.resources.displayMetrics.density
                if (density > 0f) (legacyPx / density).roundToInt() else ConfigBridge.DEFAULT_THRESHOLD_DP
            }
        }
        prefs.contains(ConfigBridge.LEGACY_PREF_KEY_EXTRA_DP) -> {
            val extraDp = prefs.getInt(ConfigBridge.LEGACY_PREF_KEY_EXTRA_DP, 0)
            migrated = if (extraDp <= 0) 0 else ConfigBridge.STOCK_THRESHOLD_DP + extraDp
        }
    }

    migrated = migrated.coerceIn(0, ConfigBridge.MAX_THRESHOLD_DP)
    prefs.edit()
        .putInt(ConfigBridge.PREF_KEY_THRESHOLD_DP, migrated)
        .putBoolean(PREF_DP_MIGRATED, true)
        .apply()
    return migrated
}
