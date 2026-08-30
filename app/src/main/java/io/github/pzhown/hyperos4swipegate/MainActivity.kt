package io.github.pzhown.hyperos4swipegate

import android.os.Bundle
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
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.asPaddingValues
import androidx.compose.foundation.layout.defaultMinSize
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.navigationBars
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import io.github.pzhown.hyperos4swipegate.ui.liquid.LiquidFloatingBottomBar
import io.github.pzhown.hyperos4swipegate.ui.liquid.LiquidFloatingBottomBarItem
import top.yukonga.miuix.kmp.basic.Icon
import top.yukonga.miuix.kmp.basic.MiuixScrollBehavior
import top.yukonga.miuix.kmp.basic.NavigationBar
import top.yukonga.miuix.kmp.basic.NavigationBarDisplayMode
import top.yukonga.miuix.kmp.basic.NavigationBarItem
import top.yukonga.miuix.kmp.basic.Scaffold
import top.yukonga.miuix.kmp.basic.Text
import top.yukonga.miuix.kmp.basic.TopAppBar
import top.yukonga.miuix.kmp.blur.isRuntimeShaderSupported
import top.yukonga.miuix.kmp.blur.layerBackdrop
import top.yukonga.miuix.kmp.blur.rememberLayerBackdrop
import top.yukonga.miuix.kmp.icon.MiuixIcons
import top.yukonga.miuix.kmp.icon.extended.Home
import top.yukonga.miuix.kmp.icon.extended.Info
import top.yukonga.miuix.kmp.icon.extended.ListView
import top.yukonga.miuix.kmp.theme.ColorSchemeMode
import top.yukonga.miuix.kmp.theme.MiuixTheme
import top.yukonga.miuix.kmp.theme.ThemeController

private const val PREF_UI_FLOATING_BAR = "ui_floating_bottom_bar"
private const val PREF_UI_LIQUID_GLASS = "ui_liquid_glass"

private enum class MainTab(val title: String, val icon: ImageVector) {
    Home("主页", MiuixIcons.Home),
    Diagnostics("诊断", MiuixIcons.ListView),
    Settings("设置", MiuixIcons.Info),
}

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
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
                                Text(text = tab.title, fontSize = 11.sp, lineHeight = 14.sp)
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
                    MainTab.Home -> HomeScreen(
                        contentPadding = innerPadding,
                        listState = homeListState,
                        scrollBehavior = homeScrollBehavior,
                        onOpenDiagnostics = { selectedTab = MainTab.Diagnostics.ordinal },
                    )
                    MainTab.Diagnostics -> DiagnosticsScreen(
                        contentPadding = innerPadding,
                        listState = diagnosticsListState,
                        scrollBehavior = diagnosticsScrollBehavior,
                    )
                    MainTab.Settings -> SettingsScreen(
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
