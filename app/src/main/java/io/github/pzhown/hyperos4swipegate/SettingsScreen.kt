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
    val prefs = remember { ConfigBridge.localPreferences(context) }
    var launcherIconHidden by remember { mutableStateOf(isLauncherIconHidden(context)) }
    var logLevel by remember {
        mutableIntStateOf(
            ConfigBridge.sanitizeLogLevel(
                prefs.getInt(ConfigBridge.PREF_KEY_LOG_LEVEL, ConfigBridge.DEFAULT_LOG_LEVEL),
            ),
        )
    }
    var showLogLevelDialog by remember { mutableStateOf(false) }
    var logLevelStatus by remember { mutableStateOf("") }

    LazyColumn(
        modifier = Modifier
            .fillMaxSize()
            .nestedScroll(scrollBehavior.nestedScrollConnection),
        state = listState,
        contentPadding = PaddingValues(
            start = 12.dp,
            top = contentPadding.calculateTopPadding() + 12.dp,
            end = 12.dp,
            bottom = contentPadding.calculateBottomPadding() + 108.dp,
        ),
        verticalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        item { SmallTitle("应用") }
        item {
            Card(modifier = Modifier.fillMaxWidth()) {
                SwitchPreference(
                    checked = !launcherIconHidden,
                    onCheckedChange = { visible ->
                        val hidden = !visible
                        setLauncherIconHidden(context, hidden)
                        launcherIconHidden = hidden
                    },
                    title = "桌面图标",
                    summary = if (launcherIconHidden) "已隐藏，可从 LSPosed 打开" else "显示应用入口",
                )
            }
        }

        item { SmallTitle("外观") }
        item {
            Card(modifier = Modifier.fillMaxWidth()) {
                SwitchPreference(
                    checked = floatingBar,
                    onCheckedChange = onFloatingBarChanged,
                    title = "浮动导航栏",
                    summary = if (floatingBar) "底部导航悬浮显示" else "底部导航贴合页面",
                )
                SwitchPreference(
                    checked = liquidGlass,
                    onCheckedChange = onLiquidGlassChanged,
                    title = "液态玻璃",
                    summary = when {
                        !blurSupported -> "当前设备不支持实时模糊，将使用透明替代效果"
                        liquidGlass -> "已启用实时模糊与玻璃效果"
                        else -> "关闭实时模糊效果"
                    },
                )
            }
        }

        item { SmallTitle("高级") }
        item {
            Card(modifier = Modifier.fillMaxWidth()) {
                ArrowPreference(
                    title = "日志级别",
                    summary = when (logLevel) {
                        ConfigBridge.LOG_LEVEL_OFF -> "关闭"
                        ConfigBridge.LOG_LEVEL_DETAILED -> "详细"
                        else -> "基础"
                    },
                    onClick = { showLogLevelDialog = true },
                )
            }
        }
        if (logLevelStatus.isNotBlank()) {
            item {
                Text(
                    text = logLevelStatus,
                    modifier = Modifier.padding(horizontal = 18.dp),
                    fontSize = 13.sp,
                    color = MiuixTheme.colorScheme.error,
                )
            }
        }
    }

    if (showLogLevelDialog) {
        OverlayDialog(
            onDismissRequest = { showLogLevelDialog = false },
        ) {
            Column(
                modifier = Modifier.padding(24.dp),
                verticalArrangement = Arrangement.spacedBy(14.dp),
            ) {
                Text(
                    text = "日志级别",
                    fontSize = 20.sp,
                    fontWeight = FontWeight.SemiBold,
                )
                listOf(
                    ConfigBridge.LOG_LEVEL_OFF to "关闭",
                    ConfigBridge.LOG_LEVEL_BASIC to "基础",
                    ConfigBridge.LOG_LEVEL_DETAILED to "详细",
                ).forEach { (level, label) ->
                    Row(
                        modifier = Modifier
                            .fillMaxWidth()
                            .clickable {
                                logLevel = level
                                prefs.edit().putInt(ConfigBridge.PREF_KEY_LOG_LEVEL, level).apply()
                                ConfigBridge.applyLogLevelAsync(context, level) { result ->
                                    logLevelStatus = if (result.success()) "" else "应用失败：${result.message()}"
                                    if (!result.success()) {
                                        Toast.makeText(context, "日志级别同步失败", Toast.LENGTH_SHORT).show()
                                    }
                                }
                                showLogLevelDialog = false
                            }
                            .padding(vertical = 10.dp),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Text(
                            text = if (level == logLevel) "✓" else "",
                            modifier = Modifier.padding(end = 12.dp),
                            color = if (level == logLevel) MiuixTheme.colorScheme.primary else Color.Transparent,
                        )
                        Text(text = label)
                    }
                }
                Spacer(modifier = Modifier.height(2.dp))
            }
        }
    }
}

private fun isLauncherIconHidden(context: Context): Boolean {
    val component = ComponentName(context, LAUNCHER_ALIAS)
    return context.packageManager.getComponentEnabledSetting(component) ==
        PackageManager.COMPONENT_ENABLED_STATE_DISABLED
}

private fun setLauncherIconHidden(context: Context, hidden: Boolean) {
    val component = ComponentName(context, LAUNCHER_ALIAS)
    context.packageManager.setComponentEnabledSetting(
        component,
        if (hidden) PackageManager.COMPONENT_ENABLED_STATE_DISABLED
        else PackageManager.COMPONENT_ENABLED_STATE_ENABLED,
        PackageManager.DONT_KILL_APP,
    )
}
