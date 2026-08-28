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
import androidx.compose.foundation.horizontalScroll
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
import androidx.compose.foundation.rememberScrollState
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
import io.github.pzhown.hyperos4swipegate.ui.liquid.LiquidFloatingBottomBar
import io.github.pzhown.hyperos4swipegate.ui.liquid.LiquidFloatingBottomBarItem
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
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

private enum class HookUiState { Loading, Active, Repairing, Error, Inactive, Unknown }

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
        loadAndMigrateThresholdDp(this)
        XposedServiceBridge.initialize(this)
        setContent { SwipeGateManager() }
    }
}

@Composable
private fun SwipeGateManager() {
    val context = androidx.compose.ui.platform.LocalContext.current
    val prefs = remember { ConfigBridge.localPreferences(context) }
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
                                start = 16.dp,
                                end = 16.dp,
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
                                modifier = Modifier.defaultMinSize(minWidth = 64.dp),
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
        MainTab.entries.forEachIndexed { index, tab ->
            NavigationBarItem(
                selected = selectedTab == index,
                onClick = { onSelected(index) },
                icon = tab.icon,
                label = tab.title,
            )
        }
    }
}

@Composable
private fun OverviewPage(contentPadding: PaddingValues) {
    val context = androidx.compose.ui.platform.LocalContext.current
    var snapshot by remember { mutableStateOf(HookStatusSnapshot.loading()) }

    LaunchedEffect(Unit) {
        while (true) {
            snapshot = withContext(Dispatchers.IO) { collectHookStatusSnapshot(context) }
            delay(1500)
        }
    }

    val dark = isSystemInDarkTheme()
    val statusBackground = when (snapshot.state) {
        HookUiState.Active -> if (dark) Color(0xFF183D28) else Color(0xFFD9F7E2)
        HookUiState.Repairing -> if (dark) Color(0xFF4B3B12) else Color(0xFFFFF1BF)
        HookUiState.Error -> if (dark) Color(0xFF4A2424) else Color(0xFFFFE0E0)
        else -> MiuixTheme.colorScheme.surfaceContainer
    }
    val statusContent = when (snapshot.state) {
        HookUiState.Active -> if (dark) Color(0xFFB9F6CA) else Color(0xFF102E1A)
        HookUiState.Repairing -> if (dark) Color(0xFFFFE08A) else Color(0xFF4B3700)
        HookUiState.Error -> if (dark) Color(0xFFFFB4AB) else Color(0xFF5A1010)
        else -> MiuixTheme.colorScheme.onSurfaceContainer
    }
    val primaryLabel = when (snapshot.state) {
        HookUiState.Active -> "已激活"
        HookUiState.Repairing -> "正在更新"
        HookUiState.Error -> "异常"
        HookUiState.Inactive -> "未激活"
        HookUiState.Unknown -> "状态未知"
        HookUiState.Loading -> "检测中"
    }
    val moduleLabel = when (snapshot.state) {
        HookUiState.Active -> "模块已加载"
        HookUiState.Repairing -> "模块更新中"
        HookUiState.Error -> "模块异常"
        HookUiState.Inactive -> "模块未加载"
        HookUiState.Unknown -> "LSPosed 未连接"
        HookUiState.Loading -> "正在检测"
    }

    LazyColumn(
        modifier = Modifier.fillMaxSize(),
        contentPadding = pagePadding(contentPadding),
        verticalArrangement = Arrangement.spacedBy(14.dp),
    ) {
        item {
            Card(
                modifier = Modifier.fillMaxWidth(),
                cornerRadius = 20.dp,
                colors = CardDefaults.defaultColors(color = statusBackground, contentColor = statusContent),
            ) {
                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(horizontal = 24.dp, vertical = 20.dp),
                    horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Column(modifier = Modifier.weight(1f)) {
                        Text(primaryLabel, fontSize = 28.sp, fontWeight = FontWeight.Bold)
                        Spacer(modifier = Modifier.height(6.dp))
                        Text(moduleLabel, fontSize = 16.sp)
                    }
                    if (snapshot.state == HookUiState.Active) {
                        Text("✓", fontSize = 46.sp, fontWeight = FontWeight.Bold)
                    }
                }
            }
        }

        item {
            Card(modifier = Modifier.fillMaxWidth(), cornerRadius = 20.dp) {
                OverviewInfoRow(
                    "当前门槛",
                    snapshot.thresholdDp?.let { thresholdLabel(it) } ?: "读取中",
                )
                OverviewInfoRow(
                    "Launcher",
                    snapshot.launcherVersion.ifBlank { "读取中" },
                )
            }
        }

        if (snapshot.detail.isNotBlank() && snapshot.state != HookUiState.Active) {
            item {
                SmallTitle("状态说明")
                Card(modifier = Modifier.fillMaxWidth(), cornerRadius = 20.dp) {
                    Text(
                        text = snapshot.detail,
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
            .padding(horizontal = 24.dp, vertical = 13.dp),
    ) {
        Text(label, fontSize = 17.sp)
        Spacer(modifier = Modifier.height(3.dp))
        Text(
            value,
            fontSize = 14.sp,
            color = MiuixTheme.colorScheme.onSurfaceVariantSummary,
            maxLines = 2,
            overflow = TextOverflow.Ellipsis,
        )
    }
}

private fun collectHookStatusSnapshot(context: Context): HookStatusSnapshot {
    val serviceSnapshot = XposedServiceBridge.snapshot(context)
    val launcherVersion = DiagnosticsCollector.readLauncherVersion(context)
    val threshold = serviceSnapshot.thresholdDp()

    if (!serviceSnapshot.serviceConnected()) {
        return HookStatusSnapshot(
            HookUiState.Unknown,
            launcherVersion,
            threshold,
            serviceSnapshot.error().ifBlank { "LSPosed 服务未连接。" },
        )
    }

    if (!serviceSnapshot.launcherLoaded()) {
        return HookStatusSnapshot(
            HookUiState.Inactive,
            launcherVersion,
            threshold,
            "系统桌面正在运行，但 LSPosed 尚未报告本模块已加载。请检查作用域后重启系统桌面。",
        )
    }

    return when (serviceSnapshot.targetState()) {
        "RELOADING" -> HookStatusSnapshot(
            HookUiState.Repairing,
            launcherVersion,
            threshold,
            "LSPosed 正在更新模块代码。",
        )
        "FAILED" -> HookStatusSnapshot(
            HookUiState.Error,
            launcherVersion,
            threshold,
            "LSPosed 报告目标进程的模块更新失败。",
        )
        "STALE" -> HookStatusSnapshot(
            HookUiState.Active,
            launcherVersion,
            threshold,
            "系统桌面仍加载上一版本模块，重启系统桌面后可更新。",
        )
        else -> HookStatusSnapshot(
            HookUiState.Active,
            launcherVersion,
            threshold,
            "",
        )
    }
}

@Composable
private fun SettingsPage(contentPadding: PaddingValues) {
    val context = androidx.compose.ui.platform.LocalContext.current
    val prefs = remember { ConfigBridge.localPreferences(context) }
    var thresholdDp by remember { mutableFloatStateOf(loadAndMigrateThresholdDp(context).toFloat()) }
    var applyStatus by remember { mutableStateOf("") }

    LazyColumn(
        modifier = Modifier.fillMaxSize(),
        contentPadding = pagePadding(contentPadding),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        item { SmallTitle("手势") }
        item {
            Card(modifier = Modifier.fillMaxWidth()) {
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
                    summary = thresholdLabel(value),
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
            Card(modifier = Modifier.fillMaxWidth()) {
                Text(
                    "超过 88 dp 后才会延后侧边栏，返回手势不变。配置通过 LSPosed API 102 传递，不需要 Root。",
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
    var logs by remember { mutableStateOf("读取诊断…") }
    var loading by remember { mutableStateOf(true) }

    fun refresh() {
        loading = true
        scope.launch {
            logs = withContext(Dispatchers.IO) { DiagnosticsCollector.collect(context) }
            loading = false
        }
    }

    LaunchedEffect(Unit) { refresh() }

    LazyColumn(
        modifier = Modifier.fillMaxSize(),
        contentPadding = pagePadding(contentPadding),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        item {
            Card(modifier = Modifier.fillMaxWidth()) {
                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(16.dp),
                    horizontalArrangement = Arrangement.spacedBy(10.dp),
                ) {
                    Button(
                        onClick = { refresh() },
                        enabled = !loading,
                        modifier = Modifier.weight(1f),
                    ) {
                        Text(if (loading) "读取中" else "刷新")
                    }
                    Button(
                        onClick = {
                            val clipboard = context.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
                            clipboard.setPrimaryClip(ClipData.newPlainText("HyperOS4 SwipeGate diagnostics", logs))
                            Toast.makeText(context, "诊断已复制", Toast.LENGTH_SHORT).show()
                        },
                        enabled = logs.isNotBlank() && !loading,
                        modifier = Modifier.weight(1f),
                    ) {
                        Text("复制")
                    }
                }
            }
        }
        item { SmallTitle("诊断") }
        item {
            Card(modifier = Modifier.fillMaxWidth()) {
                Text(
                    text = logs,
                    modifier = Modifier
                        .fillMaxWidth()
                        .horizontalScroll(rememberScrollState())
                        .padding(18.dp),
                    fontFamily = FontFamily.Monospace,
                    fontSize = 11.sp,
                    softWrap = false,
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
        contentPadding = pagePadding(contentPadding),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        item { SmallTitle("界面") }
        item {
            Card(modifier = Modifier.fillMaxWidth()) {
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
                    summary = if (blurSupported) "启用模糊与折射效果" else "当前设备不支持",
                    enabled = floatingBar && blurSupported,
                )
            }
        }

        item { SmallTitle("模块") }
        item {
            Card(modifier = Modifier.fillMaxWidth()) {
                Column(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(horizontal = 24.dp, vertical = 20.dp),
                ) {
                    Text("HyperOS4 SwipeGate")
                    Spacer(modifier = Modifier.height(6.dp))
                    Text(
                        "${BuildConfig.VERSION_NAME} (${BuildConfig.VERSION_CODE})",
                        color = MiuixTheme.colorScheme.onSurfaceVariantSummary,
                    )
                    Spacer(modifier = Modifier.height(14.dp))
                    Text("控制 HyperOS 4 侧滑停顿触发距离")
                    Spacer(modifier = Modifier.height(8.dp))
                    Text(
                        "Launcher 8.0+ · 已测试 RELEASE-8.01.02.5459",
                        color = MiuixTheme.colorScheme.onSurfaceVariantSummary,
                    )
                }
            }
        }

        item {
            Card(modifier = Modifier.fillMaxWidth()) {
                Button(
                    onClick = {
                        context.startActivity(
                            Intent(
                                Intent.ACTION_VIEW,
                                Uri.parse("https://github.com/PzHown/HyperOS4SwipeGate"),
                            ),
                        )
                    },
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(16.dp),
                ) {
                    Text("GitHub 项目")
                }
            }
        }
    }
}

private fun pagePadding(contentPadding: PaddingValues): PaddingValues = PaddingValues(
    start = 20.dp,
    top = contentPadding.calculateTopPadding() + 4.dp,
    end = 20.dp,
    bottom = contentPadding.calculateBottomPadding() + 18.dp,
)

private fun thresholdLabel(value: Int): String = when {
    value == 0 -> "默认（88 dp）"
    value <= ConfigBridge.STOCK_THRESHOLD_DP -> "$value dp（实际 88 dp）"
    else -> "$value dp"
}

private fun loadAndMigrateThresholdDp(context: Context): Int {
    val prefs = ConfigBridge.localPreferences(context)
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
