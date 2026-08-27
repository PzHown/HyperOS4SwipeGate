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
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RectangleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.rounded.Description
import androidx.compose.material.icons.rounded.Info
import androidx.compose.material.icons.rounded.Settings
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
import top.yukonga.miuix.kmp.preference.SliderPreference
import top.yukonga.miuix.kmp.preference.SwitchPreference
import top.yukonga.miuix.kmp.theme.ColorSchemeMode
import top.yukonga.miuix.kmp.theme.MiuixTheme
import top.yukonga.miuix.kmp.theme.ThemeController
import kotlin.math.roundToInt

private const val PREF_UI_FLOATING_BAR = "ui_floating_bottom_bar"
private const val PREF_UI_LIQUID_GLASS = "ui_liquid_glass"
private const val PREF_DP_MIGRATED = "threshold_dp_migrated_v1"

private enum class MainTab(val title: String) {
    Settings("设置"),
    Logs("日志"),
    About("关于"),
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
    var useFloatingBar by remember {
        mutableStateOf(prefs.getBoolean(PREF_UI_FLOATING_BAR, true))
    }
    var useLiquidGlass by remember {
        mutableStateOf(prefs.getBoolean(PREF_UI_LIQUID_GLASS, true))
    }
    val blurSupported = isRuntimeShaderSupported()

    MiuixTheme(controller = themeController) {
        val backdrop = rememberLayerBackdrop {
            drawRect(MiuixTheme.colorScheme.background)
            drawContent()
        }
        val currentTab = MainTab.entries[selectedTab]

        Box(modifier = Modifier.fillMaxSize()) {
            Scaffold(
                topBar = {
                    TopAppBar(
                        title = currentTab.title,
                        largeTitle = currentTab.title,
                        subtitle = "HyperOS4 SwipeGate · ${BuildConfig.VERSION_NAME}",
                    )
                },
                bottomBar = {
                    if (!useFloatingBar) {
                        StandardBottomBar(
                            selectedTab = selectedTab,
                            onSelected = { selectedTab = it },
                            backdrop = backdrop,
                            liquidGlass = useLiquidGlass && blurSupported,
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
                        MainTab.Settings -> SettingsPage(
                            contentPadding = innerPadding,
                            floatingBar = useFloatingBar,
                            liquidGlass = useLiquidGlass,
                            blurSupported = blurSupported,
                            onFloatingBarChanged = {
                                useFloatingBar = it
                                prefs.edit().putBoolean(PREF_UI_FLOATING_BAR, it).apply()
                            },
                            onLiquidGlassChanged = {
                                useLiquidGlass = it
                                prefs.edit().putBoolean(PREF_UI_LIQUID_GLASS, it).apply()
                            },
                        )

                        MainTab.Logs -> LogsPage(innerPadding, useFloatingBar)
                        MainTab.About -> AboutPage(innerPadding, useFloatingBar)
                    }
                }
            }

            if (useFloatingBar) {
                FloatingBottomBar(
                    modifier = Modifier.align(Alignment.BottomCenter),
                    selectedTab = selectedTab,
                    onSelected = { selectedTab = it },
                    backdrop = backdrop,
                    liquidGlass = useLiquidGlass && blurSupported,
                )
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
        color = if (liquidGlass) Color.Transparent else MiuixTheme.colorScheme.surface,
        showDivider = !liquidGlass,
        mode = NavigationBarDisplayMode.IconAndText,
    ) {
        BottomItems(selectedTab, onSelected, floating = false)
    }
}

@Composable
private fun FloatingBottomBar(
    modifier: Modifier,
    selectedTab: Int,
    onSelected: (Int) -> Unit,
    backdrop: LayerBackdrop,
    liquidGlass: Boolean,
) {
    val shape = RoundedCornerShape(36.dp)
    val glassModifier = if (liquidGlass) {
        modifier.textureBlur(backdrop = backdrop, shape = shape)
    } else {
        modifier
    }
    FloatingNavigationBar(
        modifier = glassModifier,
        color = if (liquidGlass) Color.Transparent else MiuixTheme.colorScheme.surfaceContainer,
        cornerRadius = 36.dp,
        showDivider = false,
    ) {
        BottomItems(selectedTab, onSelected, floating = true)
    }
}

@Composable
private fun BottomItems(
    selectedTab: Int,
    onSelected: (Int) -> Unit,
    floating: Boolean,
) {
    val icons = listOf(Icons.Rounded.Settings, Icons.Rounded.Description, Icons.Rounded.Info)
    MainTab.entries.forEachIndexed { index, tab ->
        if (floating) {
            FloatingNavigationBarItem(
                selected = selectedTab == index,
                onClick = { onSelected(index) },
                icon = icons[index],
                label = tab.title,
            )
        } else {
            NavigationBarItem(
                selected = selectedTab == index,
                onClick = { onSelected(index) },
                icon = icons[index],
                label = tab.title,
            )
        }
    }
}

@Composable
private fun SettingsPage(
    contentPadding: PaddingValues,
    floatingBar: Boolean,
    liquidGlass: Boolean,
    blurSupported: Boolean,
    onFloatingBarChanged: (Boolean) -> Unit,
    onLiquidGlassChanged: (Boolean) -> Unit,
) {
    val context = androidx.compose.ui.platform.LocalContext.current
    val prefs = remember { PreferenceManager.getDefaultSharedPreferences(context) }
    var thresholdDp by remember {
        mutableFloatStateOf(loadAndMigrateThresholdDp(context).toFloat())
    }
    var appliedStatus by remember {
        mutableStateOf(
            if (thresholdDp.roundToInt() == 0) "当前：默认（88 dp）"
            else "当前：${thresholdDp.roundToInt()} dp"
        )
    }

    LazyColumn(
        modifier = Modifier.fillMaxSize(),
        contentPadding = PaddingValues(
            start = 12.dp,
            top = contentPadding.calculateTopPadding() + 8.dp,
            end = 12.dp,
            bottom = contentPadding.calculateBottomPadding() + if (floatingBar) 104.dp else 16.dp,
        ),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        item {
            SmallTitle("手势")
            Card {
                SliderPreference(
                    value = thresholdDp,
                    onValueChange = { thresholdDp = it.roundToInt().toFloat() },
                    onValueChangeFinished = {
                        val value = thresholdDp.roundToInt().coerceIn(0, ConfigBridge.MAX_THRESHOLD_DP)
                        prefs.edit().putInt(ConfigBridge.PREF_KEY_THRESHOLD_DP, value).apply()
                        appliedStatus = if (value == 0) "应用中：默认（88 dp）" else "应用中：$value dp"
                        ConfigBridge.applyThresholdDpAsync(context, value) { result ->
                            appliedStatus = when {
                                !result.success() -> "应用失败：${result.message()}"
                                result.value() == 0 -> "已应用：默认（88 dp）"
                                result.value() <= ConfigBridge.STOCK_THRESHOLD_DP ->
                                    "已应用：${result.value()} dp（原厂下限仍为 88 dp）"
                                else -> "已应用：${result.value()} dp"
                            }
                        }
                    },
                    title = "侧边栏触发距离",
                    summary = "0 = 默认（原厂 88 dp）；89–320 dp 可延后侧边栏触发",
                    valueText = if (thresholdDp.roundToInt() == 0) "默认" else "${thresholdDp.roundToInt()} dp",
                    valueRange = 0f..ConfigBridge.MAX_THRESHOLD_DP.toFloat(),
                    steps = ConfigBridge.MAX_THRESHOLD_DP - 1,
                    showKeyPoints = true,
                    keyPoints = listOf(0f, 88f, 160f, 220f, 320f),
                )
                Column(modifier = Modifier.padding(horizontal = 20.dp, vertical = 14.dp)) {
                    Text(
                        text = appliedStatus,
                        color = MiuixTheme.colorScheme.onSurfaceVariantSummary,
                    )
                }
            }
        }

        item {
            SmallTitle("界面")
            Card {
                SwitchPreference(
                    title = "悬浮底栏",
                    summary = "使用 Miuix 官方 FloatingNavigationBar",
                    checked = floatingBar,
                    onCheckedChange = onFloatingBarChanged,
                )
                SwitchPreference(
                    title = "液态玻璃",
                    summary = if (blurSupported) {
                        "使用 Miuix miuix-blur 背景模糊；切换立即生效"
                    } else {
                        "当前系统不支持 RuntimeShader，已自动回退为普通底栏"
                    },
                    checked = liquidGlass && blurSupported,
                    enabled = blurSupported,
                    onCheckedChange = onLiquidGlassChanged,
                )
                AnimatedVisibility(visible = floatingBar && liquidGlass && blurSupported) {
                    Column(modifier = Modifier.padding(horizontal = 20.dp, vertical = 12.dp)) {
                        Text(
                            text = "当前：悬浮底栏 + 液态玻璃",
                            color = MiuixTheme.colorScheme.primary,
                        )
                    }
                }
            }
        }

        item {
            SmallTitle("说明")
            Card {
                Column(modifier = Modifier.padding(20.dp)) {
                    Text("普通边缘滑动仍由系统执行返回。")
                    Spacer(modifier = Modifier.height(6.dp))
                    Text("只有达到设定距离后，才允许原厂侧边栏状态机跨过 88 dp 边界。")
                    Spacer(modifier = Modifier.height(6.dp))
                    Text("修改设置无需重启；更新 Native 版本后仍建议完整重启一次。")
                }
            }
        }
    }
}

@Composable
private fun LogsPage(contentPadding: PaddingValues, floatingBar: Boolean) {
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
            start = 12.dp,
            top = contentPadding.calculateTopPadding() + 8.dp,
            end = 12.dp,
            bottom = contentPadding.calculateBottomPadding() + if (floatingBar) 104.dp else 16.dp,
        ),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        item {
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(8.dp),
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
                        clipboard.setPrimaryClip(ClipData.newPlainText("HyperOS4 SwipeGate logs", logs))
                        Toast.makeText(context, "日志已复制", Toast.LENGTH_SHORT).show()
                    },
                    enabled = logs.isNotBlank() && !loading,
                    modifier = Modifier.weight(1f),
                ) {
                    Text("复制")
                }
            }
        }
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
private fun AboutPage(contentPadding: PaddingValues, floatingBar: Boolean) {
    val context = androidx.compose.ui.platform.LocalContext.current
    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(
                start = 12.dp,
                top = contentPadding.calculateTopPadding() + 8.dp,
                end = 12.dp,
                bottom = contentPadding.calculateBottomPadding() + if (floatingBar) 104.dp else 16.dp,
            ),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        Card {
            Column(modifier = Modifier.padding(20.dp)) {
                Text("HyperOS4 SwipeGate", style = MiuixTheme.textStyles.title2)
                Spacer(modifier = Modifier.height(6.dp))
                Text("${BuildConfig.VERSION_NAME} (${BuildConfig.VERSION_CODE})")
                Spacer(modifier = Modifier.height(12.dp))
                Text("HyperOS 4 Rust Launcher 侧滑暂停门槛控制模块。")
                Spacer(modifier = Modifier.height(6.dp))
                Text("当前已验证 Launcher：RELEASE-8.01.02.5459")
                Spacer(modifier = Modifier.height(6.dp))
                Text("LSPosed API 102 Native · Android 17")
            }
        }

        Card {
            Column(modifier = Modifier.padding(20.dp)) {
                Text("界面组件", style = MiuixTheme.textStyles.title3)
                Spacer(modifier = Modifier.height(8.dp))
                Text("Compose Miuix 0.9.3")
                Text("miuix-ui · miuix-preference · miuix-blur")
            }
        }

        Button(
            onClick = {
                context.startActivity(
                    Intent(Intent.ACTION_VIEW, Uri.parse("https://github.com/PzHown/HyperOS4SwipeGate"))
                )
            },
            modifier = Modifier.fillMaxWidth(),
        ) {
            Text("打开 GitHub 项目")
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
