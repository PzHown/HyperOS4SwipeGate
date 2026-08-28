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
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.RectangleShape
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.preference.PreferenceManager
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import top.yukonga.miuix.kmp.basic.Button
import top.yukonga.miuix.kmp.basic.Card
import top.yukonga.miuix.kmp.basic.FloatingNavigationBar
import top.yukonga.miuix.kmp.basic.FloatingNavigationBarItem
import top.yukonga.miuix.kmp.basic.NavigationBar
import top.yukonga.miuix.kmp.basic.NavigationBarDisplayMode
import top.yukonga.miuix.kmp.basic.NavigationBarItem
import top.yukonga.miuix.kmp.basic.Scaffold
import top.yukonga.miuix.kmp.basic.SmallTitle
import top.yukonga.miuix.kmp.basic.Text
import top.yukonga.miuix.kmp.basic.TopAppBar
import top.yukonga.miuix.kmp.blur.LayerBackdrop
import top.yukonga.miuix.kmp.blur.isRuntimeShaderSupported
import top.yukonga.miuix.kmp.blur.layerBackdrop
import top.yukonga.miuix.kmp.blur.rememberLayerBackdrop
import top.yukonga.miuix.kmp.blur.textureBlur
import top.yukonga.miuix.kmp.icon.MiuixIcons
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
    Settings("设置", MiuixIcons.Settings),
    Logs("日志", MiuixIcons.ListView),
    About("关于", MiuixIcons.Info),
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
        val backgroundColor = MiuixTheme.colorScheme.background
        val backdrop = rememberLayerBackdrop {
            drawRect(backgroundColor)
            drawContent()
        }
        val currentTab = MainTab.entries[selectedTab]

        Scaffold(
            topBar = {
                TopAppBar(
                    title = currentTab.title,
                    largeTitle = currentTab.title,
                    subtitle = when (currentTab) {
                        MainTab.Settings -> "${BuildConfig.VERSION_NAME} (${BuildConfig.VERSION_CODE})"
                        MainTab.Logs -> "Native Hook 运行诊断"
                        MainTab.About -> "HyperOS4 SwipeGate"
                    },
                )
            },
            bottomBar = {
                if (floatingBar) {
                    FloatingBottomBar(
                        selectedTab = selectedTab,
                        onSelected = { selectedTab = it },
                        backdrop = backdrop,
                        liquidGlass = liquidGlass && blurSupported,
                    )
                } else {
                    StandardBottomBar(
                        selectedTab = selectedTab,
                        onSelected = { selectedTab = it },
                        backdrop = backdrop,
                        liquidGlass = liquidGlass && blurSupported,
                    )
                }
            },
        ) { innerPadding ->
            Box(
                modifier = Modifier
                    .fillMaxSize()
                    .layerBackdrop(backdrop),
            ) {
                when (currentTab) {
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
    selectedTab: Int,
    onSelected: (Int) -> Unit,
    backdrop: LayerBackdrop,
    liquidGlass: Boolean,
) {
    val modifier = if (liquidGlass) {
        Modifier.textureBlur(backdrop = backdrop, shape = RectangleShape)
    } else {
        Modifier
    }
    NavigationBar(
        modifier = modifier,
        color = if (liquidGlass) {
            MiuixTheme.colorScheme.surface.copy(alpha = 0.78f)
        } else {
            MiuixTheme.colorScheme.surface
        },
        showDivider = !liquidGlass,
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
private fun FloatingBottomBar(
    selectedTab: Int,
    onSelected: (Int) -> Unit,
    backdrop: LayerBackdrop,
    liquidGlass: Boolean,
) {
    val shape = RoundedCornerShape(40.dp)
    val modifier = if (liquidGlass) {
        Modifier.textureBlur(backdrop = backdrop, shape = shape)
    } else {
        Modifier
    }
    FloatingNavigationBar(
        modifier = modifier,
        color = if (liquidGlass) {
            MiuixTheme.colorScheme.surfaceContainer.copy(alpha = 0.68f)
        } else {
            MiuixTheme.colorScheme.surfaceContainer
        },
        cornerRadius = 40.dp,
        horizontalOutSidePadding = 44.dp,
        shadowElevation = 8.dp,
        showDivider = false,
        defaultWindowInsetsPadding = true,
    ) {
        MainTab.entries.forEachIndexed { index, tab ->
            FloatingNavigationBarItem(
                selected = selectedTab == index,
                onClick = { onSelected(index) },
                icon = tab.icon,
                label = tab.title,
            )
        }
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
                                result.value() <= ConfigBridge.STOCK_THRESHOLD_DP ->
                                    "已应用：${result.value()} dp（等同原厂 88 dp）"
                                else -> "已应用：${result.value()} dp"
                            }
                        }
                    },
                    title = "侧边栏触发距离",
                    summary = if (value == 0) {
                        "默认 · 原厂边界 88 dp"
                    } else {
                        "当前 $value dp · 原厂边界 88 dp"
                    },
                    valueRange = 0f..ConfigBridge.MAX_THRESHOLD_DP.toFloat(),
                    steps = ConfigBridge.MAX_THRESHOLD_DP - 1,
                    showKeyPoints = false,
                )
                if (applyStatus.isNotBlank()) {
                    Text(
                        text = applyStatus,
                        modifier = Modifier.padding(start = 24.dp, end = 24.dp, bottom = 16.dp),
                        color = if (applyStatus.startsWith("应用失败")) {
                            MiuixTheme.colorScheme.error
                        } else {
                            MiuixTheme.colorScheme.onSurfaceVariantSummary
                        },
                    )
                }
            }
        }

        item { SmallTitle("说明") }
        item {
            Card {
                Column(modifier = Modifier.padding(horizontal = 24.dp, vertical = 18.dp)) {
                    Text("普通边缘滑动仍由系统执行返回。")
                    Spacer(modifier = Modifier.height(8.dp))
                    Text("0 为默认；设置大于 88 dp 后，只有达到设定距离才允许原厂侧边栏状态机跨过 88 dp 边界。")
                    Spacer(modifier = Modifier.height(8.dp))
                    Text("距离修改即时写入系统属性；Native Hook 自愈无需重启桌面。")
                }
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
        item { SmallTitle("操作") }
        item {
            Card {
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
                    ) { Text(if (loading) "读取中" else "刷新") }
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
                    summary = "使用 Miuix FloatingNavigationBar",
                )
                SwitchPreference(
                    checked = liquidGlass && blurSupported,
                    onCheckedChange = onLiquidGlassChanged,
                    title = "液态玻璃",
                    summary = if (blurSupported) {
                        "为悬浮底栏启用 Miuix 模糊与半透明效果"
                    } else {
                        "当前设备不支持 RuntimeShader，已自动回退"
                    },
                    enabled = blurSupported,
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
                    Text("HyperOS 4 Rust Launcher 侧滑暂停门槛控制模块")
                    Spacer(modifier = Modifier.height(8.dp))
                    Text(
                        "已验证 Launcher：RELEASE-8.01.02.5459",
                        color = MiuixTheme.colorScheme.onSurfaceVariantSummary,
                    )
                    Spacer(modifier = Modifier.height(4.dp))
                    Text(
                        "LSPosed API 102 · Android 17 · Compose Miuix 0.9.3",
                        color = MiuixTheme.colorScheme.onSurfaceVariantSummary,
                    )
                }
            }
        }

        item {
            Card {
                Button(
                    onClick = {
                        context.startActivity(
                            Intent(Intent.ACTION_VIEW, Uri.parse("https://github.com/PzHown/HyperOS4SwipeGate"))
                        )
                    },
                    modifier = Modifier
                        .padding(16.dp)
                        .fillMaxWidth(),
                ) { Text("打开 GitHub 项目") }
            }
        }
    }
}

private fun loadAndMigrateThresholdDp(context: Context): Int {
    val prefs = PreferenceManager.getDefaultSharedPreferences(context)
    if (prefs.getBoolean(PREF_DP_MIGRATED, false)) {
        return prefs.getInt(
            ConfigBridge.PREF_KEY_THRESHOLD_DP,
            ConfigBridge.DEFAULT_THRESHOLD_DP,
        ).coerceIn(0, ConfigBridge.MAX_THRESHOLD_DP)
    }

    var migrated = prefs.getInt(
        ConfigBridge.PREF_KEY_THRESHOLD_DP,
        ConfigBridge.DEFAULT_THRESHOLD_DP,
    )
    when {
        prefs.contains(ConfigBridge.LEGACY_PREF_KEY_THRESHOLD_PX) -> {
            val legacyPx = prefs.getInt(ConfigBridge.LEGACY_PREF_KEY_THRESHOLD_PX, 0)
            migrated = if (legacyPx <= 0) {
                0
            } else {
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
