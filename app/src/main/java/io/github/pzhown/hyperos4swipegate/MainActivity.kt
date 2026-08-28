package io.github.pzhown.hyperos4swipegate

import android.content.ClipData
import android.content.ClipboardManager
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Bundle
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.animation.AnimatedContent
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInHorizontally
import androidx.compose.animation.slideOutHorizontally
import androidx.compose.animation.togetherWith
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.asPaddingValues
import androidx.compose.foundation.layout.defaultMinSize
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.navigationBars
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyListState
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.text.KeyboardOptions
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
import androidx.compose.ui.input.nestedscroll.nestedScroll
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.TextRange
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.TextFieldValue
import androidx.compose.ui.text.style.TextAlign
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
import top.yukonga.miuix.kmp.basic.MiuixScrollBehavior
import top.yukonga.miuix.kmp.basic.NavigationBar
import top.yukonga.miuix.kmp.basic.NavigationBarDisplayMode
import top.yukonga.miuix.kmp.basic.NavigationBarItem
import top.yukonga.miuix.kmp.basic.Scaffold
import top.yukonga.miuix.kmp.basic.ScrollBehavior
import top.yukonga.miuix.kmp.basic.SmallTitle
import top.yukonga.miuix.kmp.basic.Text
import top.yukonga.miuix.kmp.basic.TextField
import top.yukonga.miuix.kmp.basic.TopAppBar
import top.yukonga.miuix.kmp.blur.isRuntimeShaderSupported
import top.yukonga.miuix.kmp.blur.layerBackdrop
import top.yukonga.miuix.kmp.blur.rememberLayerBackdrop
import top.yukonga.miuix.kmp.icon.MiuixIcons
import top.yukonga.miuix.kmp.icon.extended.Home
import top.yukonga.miuix.kmp.icon.extended.Info
import top.yukonga.miuix.kmp.icon.extended.ListView
import top.yukonga.miuix.kmp.overlay.OverlayDialog
import top.yukonga.miuix.kmp.preference.ArrowPreference
import top.yukonga.miuix.kmp.preference.SliderPreference
import top.yukonga.miuix.kmp.preference.SwitchPreference
import top.yukonga.miuix.kmp.theme.ColorSchemeMode
import top.yukonga.miuix.kmp.theme.MiuixTheme
import top.yukonga.miuix.kmp.theme.ThemeController
import kotlin.math.roundToInt

private const val PREF_UI_FLOATING_BAR = "ui_floating_bottom_bar"
private const val PREF_UI_LIQUID_GLASS = "ui_liquid_glass"
private const val PREF_DP_MIGRATED = "threshold_dp_migrated_v1"
private const val LAUNCHER_ALIAS = "io.github.pzhown.hyperos4swipegate.LauncherAlias"

private enum class MainTab(val title: String, val icon: ImageVector) {
    Home("主页", MiuixIcons.Home),
    Diagnostics("诊断", MiuixIcons.ListView),
    Settings("设置", MiuixIcons.Info),
}

private enum class HookUiState { Loading, Active, Repairing, Error, Inactive, Unknown }

private data class HookStatusSnapshot(
    val state: HookUiState,
    val launcherVersion: String = "",
    val thresholdDp: Int? = null,
    val detail: String = "",
    val lsposedStatus: String = "检测中",
    val hyosRuntimeStatus: String = "检测中",
    val zygiskNextStatus: String = "检测中",
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
    val homeListState = rememberLazyListState()
    val diagnosticsListState = rememberLazyListState()
    val settingsListState = rememberLazyListState()
    val blurSupported = isRuntimeShaderSupported()

    MiuixTheme(controller = themeController) {
        val homeScrollBehavior = MiuixScrollBehavior()
        val diagnosticsScrollBehavior = MiuixScrollBehavior()
        val settingsScrollBehavior = MiuixScrollBehavior()
        val surfaceColor = MiuixTheme.colorScheme.background
        val backdrop = rememberLayerBackdrop {
            drawRect(surfaceColor)
            drawContent()
        }
        val currentTab = MainTab.entries[selectedTab]
        val currentScrollBehavior = when (currentTab) {
            MainTab.Home -> homeScrollBehavior
            MainTab.Diagnostics -> diagnosticsScrollBehavior
            MainTab.Settings -> settingsScrollBehavior
        }
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
                                modifier = Modifier.defaultMinSize(minWidth = 72.dp),
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
                    title = if (currentTab == MainTab.Home) "SwipeGate" else currentTab.title,
                    largeTitle = if (currentTab == MainTab.Home) "SwipeGate" else currentTab.title,
                    subtitle = "",
                    scrollBehavior = currentScrollBehavior,
                )
            },
            bottomBar = bottomBar,
        ) { innerPadding ->
            AnimatedContent(
                targetState = currentTab,
                modifier = Modifier
                    .fillMaxSize()
                    .then(if (captureForLiquid) Modifier.layerBackdrop(backdrop) else Modifier),
                transitionSpec = {
                    val direction = if (targetState.ordinal > initialState.ordinal) 1 else -1
                    (slideInHorizontally(animationSpec = tween(220)) { width -> direction * (width / 6) } +
                        fadeIn(animationSpec = tween(180)))
                        .togetherWith(
                            slideOutHorizontally(animationSpec = tween(180)) { width -> -direction * (width / 8) } +
                                fadeOut(animationSpec = tween(140)),
                        )
                },
                label = "main-tab-content",
            ) { tab ->
                when (tab) {
                    MainTab.Home -> HomePage(
                        contentPadding = innerPadding,
                        listState = homeListState,
                        scrollBehavior = homeScrollBehavior,
                    )
                    MainTab.Diagnostics -> DiagnosticsPage(
                        contentPadding = innerPadding,
                        listState = diagnosticsListState,
                        scrollBehavior = diagnosticsScrollBehavior,
                    )
                    MainTab.Settings -> SettingsPage(
                        contentPadding = innerPadding,
                        listState = settingsListState,
                        scrollBehavior = settingsScrollBehavior,
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
private fun HomePage(
    contentPadding: PaddingValues,
    listState: LazyListState,
    scrollBehavior: ScrollBehavior,
) {
    val context = androidx.compose.ui.platform.LocalContext.current
    val prefs = remember { ConfigBridge.localPreferences(context) }
    var snapshot by remember { mutableStateOf(HookStatusSnapshot.loading()) }
    var thresholdDp by remember {
        mutableFloatStateOf(
            loadAndMigrateThresholdDp(context)
                .coerceIn(ConfigBridge.STOCK_THRESHOLD_DP, ConfigBridge.MAX_THRESHOLD_DP)
                .toFloat(),
        )
    }
    var applyStatus by remember { mutableStateOf("") }
    var showThresholdInput by remember { mutableStateOf(false) }
    var thresholdInput by remember { mutableStateOf(TextFieldValue(thresholdDp.roundToInt().toString())) }
    var thresholdInputError by remember { mutableStateOf("") }

    fun applyThreshold(appliedValue: Int) {
        val applied = appliedValue.coerceIn(ConfigBridge.STOCK_THRESHOLD_DP, ConfigBridge.MAX_THRESHOLD_DP)
        thresholdDp = applied.toFloat()
        prefs.edit().putInt(ConfigBridge.PREF_KEY_THRESHOLD_DP, applied).apply()
        ConfigBridge.applyThresholdDpAsync(context, applied) { result ->
            applyStatus = if (!result.success()) "应用失败：${result.message()}" else ""
        }
    }

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
    val statusIconBackground = when (snapshot.state) {
        HookUiState.Active -> if (dark) Color(0xFF2C5F3E) else Color(0xFFB8EBC8)
        HookUiState.Repairing -> if (dark) Color(0xFF66511A) else Color(0xFFFFE08A)
        HookUiState.Error -> if (dark) Color(0xFF693535) else Color(0xFFFFC2C2)
        else -> MiuixTheme.colorScheme.surface
    }
    val statusIcon = when (snapshot.state) {
        HookUiState.Active -> "✓"
        HookUiState.Repairing -> "↻"
        HookUiState.Error -> "!"
        HookUiState.Inactive -> "–"
        HookUiState.Unknown -> "?"
        HookUiState.Loading -> "…"
    }
    val primaryLabel = when (snapshot.state) {
        HookUiState.Active -> "运行正常"
        HookUiState.Repairing -> "正在更新"
        HookUiState.Error -> "异常"
        HookUiState.Inactive -> "未激活"
        HookUiState.Unknown -> "状态未知"
        HookUiState.Loading -> "检测中"
    }
    val moduleLabel = when (snapshot.state) {
        HookUiState.Active -> "系统桌面已加载"
        HookUiState.Repairing -> "模块更新中"
        HookUiState.Error -> when {
            snapshot.detail.startsWith("LSPosed 版本不支持") -> "LSPosed 版本不支持"
            snapshot.detail.startsWith("Zygisk Next 版本不支持") -> "运行环境不满足"
            snapshot.detail.startsWith("未检测到 HyOSRuntime") -> "HyOS Runtime 不可用"
            else -> "模块异常"
        }
        HookUiState.Inactive -> "模块未加载"
        HookUiState.Unknown -> "LSPosed 未连接"
        HookUiState.Loading -> "正在检测"
    }

    LazyColumn(
        state = listState,
        modifier = Modifier
            .fillMaxSize()
            .nestedScroll(scrollBehavior.nestedScrollConnection),
        contentPadding = pagePadding(contentPadding),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        item {
            Card(
                modifier = Modifier.fillMaxWidth(),
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
                        Text(primaryLabel, fontSize = 26.sp, fontWeight = FontWeight.Bold)
                        Spacer(modifier = Modifier.height(4.dp))
                        Text(moduleLabel, fontSize = 15.sp)
                    }
                    Box(
                        modifier = Modifier
                            .size(36.dp)
                            .background(statusIconBackground, CircleShape),
                        contentAlignment = Alignment.Center,
                    ) {
                        Text(statusIcon, fontSize = 20.sp, fontWeight = FontWeight.Bold)
                    }
                }
            }
        }

        if (snapshot.detail.isNotBlank() && snapshot.state != HookUiState.Active) {
            item {
                Card(modifier = Modifier.fillMaxWidth()) {
                    Text(
                        text = snapshot.detail,
                        modifier = Modifier.padding(horizontal = 22.dp, vertical = 16.dp),
                        color = MiuixTheme.colorScheme.onSurfaceVariantSummary,
                    )
                }
            }
        }

        item { SmallTitle("手势") }
        item {
            Card(modifier = Modifier.fillMaxWidth()) {
                val value = thresholdDp.roundToInt()
                SliderPreference(
                    value = thresholdDp,
                    onValueChange = {
                        thresholdDp = it.roundToInt()
                            .coerceIn(ConfigBridge.STOCK_THRESHOLD_DP, ConfigBridge.MAX_THRESHOLD_DP)
                            .toFloat()
                        applyStatus = ""
                    },
                    onValueChangeFinished = {
                        applyThreshold(thresholdDp.roundToInt())
                    },
                    title = "侧边栏触发距离",
                    summary = "",
                    endActions = {
                        Text(
                            text = "${thresholdLabel(value)} ›",
                            modifier = Modifier
                                .clickable {
                                    val text = value.toString()
                                    thresholdInput = TextFieldValue(text, selection = TextRange(text.length))
                                    thresholdInputError = ""
                                    showThresholdInput = true
                                }
                                .padding(horizontal = 8.dp, vertical = 6.dp),
                            fontSize = 14.sp,
                            color = MiuixTheme.colorScheme.onSurfaceVariantActions,
                        )
                    },
                    valueRange = ConfigBridge.STOCK_THRESHOLD_DP.toFloat()..ConfigBridge.MAX_THRESHOLD_DP.toFloat(),
                    steps = ConfigBridge.MAX_THRESHOLD_DP - ConfigBridge.STOCK_THRESHOLD_DP - 1,
                    showKeyPoints = false,
                )
                Text(
                    text = "范围 88–300 dp · 88 dp 为系统默认",
                    modifier = Modifier.padding(start = 24.dp, end = 24.dp, bottom = if (applyStatus.isBlank()) 18.dp else 6.dp),
                    fontSize = 13.sp,
                    color = MiuixTheme.colorScheme.onSurfaceVariantSummary,
                )
                if (applyStatus.isNotBlank()) {
                    Text(
                        text = applyStatus,
                        modifier = Modifier.padding(start = 24.dp, end = 24.dp, bottom = 18.dp),
                        fontSize = 13.sp,
                        color = MiuixTheme.colorScheme.error,
                    )
                }
            }
        }

        item { SmallTitle("运行环境") }
        item {
            Card(modifier = Modifier.fillMaxWidth()) {
                OverviewInfoRow("Launcher", snapshot.launcherVersion.ifBlank { "读取中" })
                OverviewInfoRow("LSPosed", snapshot.lsposedStatus)
                OverviewInfoRow("HyOS Runtime", snapshot.hyosRuntimeStatus)
                OverviewInfoRow("Zygisk Next", snapshot.zygiskNextStatus)
            }
        }
    }

    OverlayDialog(
        title = "侧边栏触发距离",
        summary = "请输入 88–300 dp",
        show = showThresholdInput,
        onDismissRequest = { showThresholdInput = false },
    ) {
        TextField(
            value = thresholdInput,
            onValueChange = { newValue ->
                val digits = newValue.text.filter { it.isDigit() }.take(3)
                thresholdInput = TextFieldValue(
                    text = digits,
                    selection = TextRange(
                        newValue.selection.start.coerceIn(0, digits.length),
                        newValue.selection.end.coerceIn(0, digits.length),
                    ),
                )
                thresholdInputError = ""
            },
            modifier = Modifier.fillMaxWidth(),
            label = "dp",
            useLabelAsPlaceholder = true,
            singleLine = true,
            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
        )
        if (thresholdInputError.isNotBlank()) {
            Text(
                text = thresholdInputError,
                modifier = Modifier.padding(top = 8.dp),
                fontSize = 13.sp,
                color = MiuixTheme.colorScheme.error,
            )
        }
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(top = 16.dp),
            horizontalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            Button(
                onClick = { showThresholdInput = false },
                modifier = Modifier.weight(1f),
            ) {
                Text("取消")
            }
            Button(
                onClick = {
                    val entered = thresholdInput.text.toIntOrNull()
                    if (entered == null || entered !in ConfigBridge.STOCK_THRESHOLD_DP..ConfigBridge.MAX_THRESHOLD_DP) {
                        thresholdInputError = "请输入 88–300 之间的整数"
                    } else {
                        showThresholdInput = false
                        applyStatus = ""
                        applyThreshold(entered)
                    }
                },
                modifier = Modifier.weight(1f),
            ) {
                Text("确定")
            }
        }
    }
}

@Composable
private fun OverviewInfoRow(label: String, value: String) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(horizontal = 24.dp, vertical = 14.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(16.dp),
    ) {
        Text(label, fontSize = 16.sp)
        Text(
            value,
            modifier = Modifier.weight(1f),
            fontSize = 13.sp,
            color = MiuixTheme.colorScheme.onSurfaceVariantSummary,
            maxLines = 2,
            overflow = TextOverflow.Ellipsis,
            textAlign = TextAlign.End,
        )
    }
}

private fun collectHookStatusSnapshot(context: Context): HookStatusSnapshot {
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
        runtimeSnapshot.hyosRuntimeDetected() -> "可用 ✓"
        else -> "未检测到"
    }
    val zygiskNextStatus = if (runtimeSnapshot.zygiskNextSupported()) {
        "1.5.0+ 要求满足 ✓"
    } else {
        "需要 1.5.0+"
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

    if (!runtimeSnapshot.lsposedSupported()) {
        return status(
            HookUiState.Error,
            "LSPosed 版本不支持：需要 versionCode ≥ 7846。",
        )
    }

    if (!runtimeSnapshot.hyosSpawnerPresent()) {
        return status(
            HookUiState.Error,
            "未检测到 HyOS Runtime。",
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
            "运行环境不满足：需要 Zygisk Next 1.5.0+ 的 HyOS Runtime 支持。",
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

@Composable
private fun DiagnosticsPage(
    contentPadding: PaddingValues,
    listState: LazyListState,
    scrollBehavior: ScrollBehavior,
) {
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
        state = listState,
        modifier = Modifier
            .fillMaxSize()
            .nestedScroll(scrollBehavior.nestedScrollConnection),
        contentPadding = pagePadding(contentPadding),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        item {
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(12.dp),
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
                        clipboard.setPrimaryClip(ClipData.newPlainText("SwipeGate diagnostics", logs))
                        Toast.makeText(context, "诊断已复制", Toast.LENGTH_SHORT).show()
                    },
                    enabled = logs.isNotBlank() && !loading,
                    modifier = Modifier.weight(1f),
                ) {
                    Text("复制诊断")
                }
            }
        }

        item { SmallTitle("运行日志") }
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

        item { SmallTitle("技术信息") }
        item {
            Card(modifier = Modifier.fillMaxWidth()) {
                OverviewInfoRow("版本", "${BuildConfig.VERSION_NAME} · ${BuildConfig.VERSION_CODE}")
                OverviewInfoRow("框架", "LSPosed Modern API 102 · native_init")
                OverviewInfoRow("Hook", "Dynamic Pattern Scan · fail-closed")
                OverviewInfoRow("构建", "Android 17 · arm64-v8a · 16 KB aligned")
                OverviewInfoRow("配置", "LSPosed API 102 · 无 Root")
            }
        }
    }
}

@Composable
private fun SettingsPage(
    contentPadding: PaddingValues,
    listState: LazyListState,
    scrollBehavior: ScrollBehavior,
    floatingBar: Boolean,
    liquidGlass: Boolean,
    blurSupported: Boolean,
    onFloatingBarChanged: (Boolean) -> Unit,
    onLiquidGlassChanged: (Boolean) -> Unit,
) {
    val context = androidx.compose.ui.platform.LocalContext.current
    var launcherIconHidden by remember { mutableStateOf(isLauncherIconHidden(context)) }

    LazyColumn(
        state = listState,
        modifier = Modifier
            .fillMaxSize()
            .nestedScroll(scrollBehavior.nestedScrollConnection),
        contentPadding = pagePadding(contentPadding),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        item { SmallTitle("应用") }
        item {
            Card(modifier = Modifier.fillMaxWidth()) {
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
                        "已隐藏，可从 LSPosed 模块页打开"
                    } else {
                        "从系统桌面隐藏 SwipeGate"
                    },
                )
            }
        }

        item { SmallTitle("外观") }
        item {
            Card(modifier = Modifier.fillMaxWidth()) {
                SwitchPreference(
                    checked = floatingBar,
                    onCheckedChange = onFloatingBarChanged,
                    title = "悬浮底栏",
                    summary = "使用悬浮式底部导航",
                )
                SwitchPreference(
                    checked = liquidGlass && blurSupported,
                    onCheckedChange = onLiquidGlassChanged,
                    title = "液态玻璃",
                    summary = when {
                        !blurSupported -> "当前设备不支持"
                        !floatingBar -> "需先启用悬浮底栏"
                        else -> "启用模糊与折射效果"
                    },
                    enabled = floatingBar && blurSupported,
                )
            }
        }

        item { SmallTitle("关于") }
        item {
            Card(modifier = Modifier.fillMaxWidth()) {
                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(horizontal = 24.dp, vertical = 18.dp),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(16.dp),
                ) {
                    Image(
                        painter = painterResource(R.drawable.swipegate_logo),
                        contentDescription = "SwipeGate Logo",
                        modifier = Modifier.size(64.dp),
                        contentScale = ContentScale.Fit,
                    )
                    Column(modifier = Modifier.weight(1f)) {
                        Text("SwipeGate", fontSize = 22.sp, fontWeight = FontWeight.Bold)
                        Spacer(modifier = Modifier.height(4.dp))
                        Text(
                            "${BuildConfig.VERSION_NAME} · ${BuildConfig.VERSION_CODE}",
                            fontSize = 13.sp,
                            color = MiuixTheme.colorScheme.onSurfaceVariantSummary,
                        )
                    }
                }
                OverviewInfoRow("兼容", "HyperOS 4 · Launcher 8.0+ · arm64-v8a")
                ArrowPreference(
                    title = "GitHub 项目",
                    summary = "PzHown/HyperOS4SwipeGate",
                    onClick = {
                        context.startActivity(
                            Intent(
                                Intent.ACTION_VIEW,
                                Uri.parse("https://github.com/PzHown/HyperOS4SwipeGate"),
                            ),
                        )
                    },
                )
            }
        }

        item { SmallTitle("开发者") }
        item {
            Card(modifier = Modifier.fillMaxWidth()) {
                OverviewInfoRow("作者", "PzHown")
                ArrowPreference(
                    title = "GitHub",
                    summary = "@PzHown",
                    onClick = {
                        context.startActivity(
                            Intent(Intent.ACTION_VIEW, Uri.parse("https://github.com/PzHown")),
                        )
                    },
                )
                ArrowPreference(
                    title = "酷安",
                    summary = "PzHown",
                    onClick = {
                        context.startActivity(
                            Intent(Intent.ACTION_VIEW, Uri.parse("https://www.coolapk.com/u/464418")),
                        )
                    },
                )
            }
        }
    }
}

private fun isLauncherIconHidden(context: Context): Boolean {
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

private fun pagePadding(contentPadding: PaddingValues): PaddingValues = PaddingValues(
    start = 16.dp,
    top = contentPadding.calculateTopPadding() + 4.dp,
    end = 16.dp,
    bottom = contentPadding.calculateBottomPadding() + 20.dp,
)

private fun thresholdLabel(value: Int): String = "$value dp"

private fun loadAndMigrateThresholdDp(context: Context): Int {
    val prefs = ConfigBridge.localPreferences(context)
    if (prefs.getBoolean(PREF_DP_MIGRATED, false)) {
        return prefs.getInt(ConfigBridge.PREF_KEY_THRESHOLD_DP, ConfigBridge.DEFAULT_THRESHOLD_DP)
            .let { if (it <= 0) ConfigBridge.STOCK_THRESHOLD_DP else it }
            .coerceIn(ConfigBridge.STOCK_THRESHOLD_DP, ConfigBridge.MAX_THRESHOLD_DP)
    }

    var migrated = prefs.getInt(ConfigBridge.PREF_KEY_THRESHOLD_DP, ConfigBridge.DEFAULT_THRESHOLD_DP)
    when {
        prefs.contains(ConfigBridge.LEGACY_PREF_KEY_THRESHOLD_PX) -> {
            val legacyPx = prefs.getInt(ConfigBridge.LEGACY_PREF_KEY_THRESHOLD_PX, 0)
            migrated = if (legacyPx <= 0) ConfigBridge.STOCK_THRESHOLD_DP else {
                val density = context.resources.displayMetrics.density
                if (density > 0f) (legacyPx / density).roundToInt() else ConfigBridge.STOCK_THRESHOLD_DP
            }
        }
        prefs.contains(ConfigBridge.LEGACY_PREF_KEY_EXTRA_DP) -> {
            val extraDp = prefs.getInt(ConfigBridge.LEGACY_PREF_KEY_EXTRA_DP, 0)
            migrated = ConfigBridge.STOCK_THRESHOLD_DP + extraDp.coerceAtLeast(0)
        }
        migrated <= 0 -> migrated = ConfigBridge.STOCK_THRESHOLD_DP
    }

    migrated = migrated.coerceIn(ConfigBridge.STOCK_THRESHOLD_DP, ConfigBridge.MAX_THRESHOLD_DP)
    prefs.edit()
        .putInt(ConfigBridge.PREF_KEY_THRESHOLD_DP, migrated)
        .putBoolean(PREF_DP_MIGRATED, true)
        .apply()
    return migrated
}
