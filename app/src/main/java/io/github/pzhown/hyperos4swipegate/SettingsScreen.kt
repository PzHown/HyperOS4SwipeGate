package io.github.pzhown.hyperos4swipegate

import android.content.ComponentName
import android.content.Context
import android.content.pm.PackageManager
import android.widget.Toast
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyListState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.nestedscroll.nestedScroll
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import top.yukonga.miuix.kmp.basic.Card
import top.yukonga.miuix.kmp.basic.ScrollBehavior
import top.yukonga.miuix.kmp.basic.SmallTitle
import top.yukonga.miuix.kmp.basic.Text
import top.yukonga.miuix.kmp.overlay.OverlayDialog
import top.yukonga.miuix.kmp.preference.ArrowPreference
import top.yukonga.miuix.kmp.preference.SwitchPreference
import top.yukonga.miuix.kmp.theme.MiuixTheme

private const val LAUNCHER_ALIAS = "io.github.pzhown.hyperos4swipegate.LauncherAlias"

@Composable
internal fun SettingsScreen(
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
    var logLevel by remember {
        mutableIntStateOf(
            ConfigBridge.sanitizeLogLevel(
                prefs.getInt(ConfigBridge.PREF_KEY_LOG_LEVEL, ConfigBridge.DEFAULT_LOG_LEVEL),
            ),
        )
    }
    var showLogLevelDialog by remember { mutableStateOf(false) }

    fun applyLogLevel(level: Int) {
        val safeLevel = ConfigBridge.sanitizeLogLevel(level)
        logLevel = safeLevel
        showLogLevelDialog = false
        ConfigBridge.applyLogLevelAsync(context, safeLevel) { result ->
            if (!result.success()) {
                Toast.makeText(context, "日志设置同步失败：${result.message()}", Toast.LENGTH_SHORT).show()
            }
        }
    }

    LazyColumn(
        state = listState,
        modifier = Modifier
            .fillMaxSize()
            .nestedScroll(scrollBehavior.nestedScrollConnection),
        contentPadding = settingsPagePadding(contentPadding),
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

        item { SmallTitle("高级") }
        item {
            Card(modifier = Modifier.fillMaxWidth()) {
                ArrowPreference(
                    title = "日志记录",
                    summary = when (logLevel) {
                        ConfigBridge.LOG_LEVEL_COMPACT -> "精简"
                        ConfigBridge.LOG_LEVEL_DETAILED -> "详细"
                        else -> "关闭"
                    },
                    onClick = { showLogLevelDialog = true },
                )
            }
        }
    }

    OverlayDialog(
        title = "日志记录",
        summary = "选择 Native 日志详细程度",
        show = showLogLevelDialog,
        onDismissRequest = { showLogLevelDialog = false },
    ) {
        Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
            LogLevelOption(
                title = "关闭",
                summary = "不保留运行日志",
                selected = logLevel == ConfigBridge.LOG_LEVEL_OFF,
                onClick = { applyLogLevel(ConfigBridge.LOG_LEVEL_OFF) },
            )
            LogLevelOption(
                title = "精简",
                summary = "记录加载、Hook、配置变化与异常",
                selected = logLevel == ConfigBridge.LOG_LEVEL_COMPACT,
                onClick = { applyLogLevel(ConfigBridge.LOG_LEVEL_COMPACT) },
            )
            LogLevelOption(
                title = "详细",
                summary = "额外记录侧滑过程与周期健康状态",
                selected = logLevel == ConfigBridge.LOG_LEVEL_DETAILED,
                onClick = { applyLogLevel(ConfigBridge.LOG_LEVEL_DETAILED) },
            )
        }
    }
}

@Composable
private fun LogLevelOption(
    title: String,
    summary: String,
    selected: Boolean,
    enabled: Boolean = true,
    onClick: () -> Unit,
) {
    val titleColor = if (enabled) Color.Unspecified else MiuixTheme.colorScheme.onSurfaceVariantSummary
    val summaryColor = if (enabled) {
        MiuixTheme.colorScheme.onSurfaceVariantSummary
    } else {
        MiuixTheme.colorScheme.onSurfaceVariantSummary.copy(alpha = 0.55f)
    }
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .clickable(enabled = enabled, onClick = onClick)
            .padding(horizontal = 8.dp, vertical = 12.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Column(modifier = Modifier.weight(1f)) {
            Text(title, fontSize = 16.sp, color = titleColor)
            Spacer(modifier = Modifier.height(2.dp))
            Text(summary, fontSize = 13.sp, color = summaryColor)
        }
        if (selected && enabled) {
            Text(
                "✓",
                fontSize = 18.sp,
                fontWeight = FontWeight.Bold,
                color = MiuixTheme.colorScheme.primary,
            )
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
        if (hidden) PackageManager.COMPONENT_ENABLED_STATE_DISABLED else PackageManager.COMPONENT_ENABLED_STATE_ENABLED,
        PackageManager.DONT_KILL_APP,
    )
    true
}.getOrDefault(false)

private fun settingsPagePadding(contentPadding: PaddingValues): PaddingValues = PaddingValues(
    start = 16.dp,
    top = contentPadding.calculateTopPadding() + 4.dp,
    end = 16.dp,
    bottom = contentPadding.calculateBottomPadding() + 20.dp,
)
