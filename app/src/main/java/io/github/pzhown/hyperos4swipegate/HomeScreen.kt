package io.github.pzhown.hyperos4swipegate

import android.content.Context
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.isSystemInDarkTheme
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
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyListState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.nestedscroll.nestedScroll
import androidx.compose.ui.text.TextRange
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.TextFieldValue
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.withContext
import top.yukonga.miuix.kmp.basic.Button
import top.yukonga.miuix.kmp.basic.Card
import top.yukonga.miuix.kmp.basic.CardDefaults
import top.yukonga.miuix.kmp.basic.ScrollBehavior
import top.yukonga.miuix.kmp.basic.SmallTitle
import top.yukonga.miuix.kmp.basic.Text
import top.yukonga.miuix.kmp.basic.TextField
import top.yukonga.miuix.kmp.overlay.OverlayDialog
import top.yukonga.miuix.kmp.preference.SliderPreference
import top.yukonga.miuix.kmp.theme.MiuixTheme
import kotlin.math.roundToInt

private const val PREF_DP_MIGRATED = "threshold_dp_migrated_v1"

private enum class HomeHookState { Loading, Active, Repairing, Error, Inactive, Unknown }

private data class HomeStatusSnapshot(
    val state: HomeHookState,
    val detail: String = "",
) {
    companion object {
        fun loading() = HomeStatusSnapshot(HomeHookState.Loading)
    }
}

@Composable
internal fun HomeScreen(
    contentPadding: PaddingValues,
    listState: LazyListState,
    scrollBehavior: ScrollBehavior,
    onOpenDiagnostics: () -> Unit,
) {
    val context = androidx.compose.ui.platform.LocalContext.current
    val prefs = remember { ConfigBridge.localPreferences(context) }
    var snapshot by remember { mutableStateOf(HomeStatusSnapshot.loading()) }
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
            snapshot = withContext(Dispatchers.IO) { collectHomeStatusSnapshot(context) }
            delay(1500)
        }
    }

    val dark = isSystemInDarkTheme()
    val statusBackground = when (snapshot.state) {
        HomeHookState.Active -> if (dark) Color(0xFF183D28) else Color(0xFFD9F7E2)
        HomeHookState.Repairing -> if (dark) Color(0xFF4B3B12) else Color(0xFFFFF1BF)
        HomeHookState.Error -> if (dark) Color(0xFF4A2424) else Color(0xFFFFE0E0)
        else -> MiuixTheme.colorScheme.surfaceContainer
    }
    val statusContent = when (snapshot.state) {
        HomeHookState.Active -> if (dark) Color(0xFFB9F6CA) else Color(0xFF102E1A)
        HomeHookState.Repairing -> if (dark) Color(0xFFFFE08A) else Color(0xFF4B3700)
        HomeHookState.Error -> if (dark) Color(0xFFFFB4AB) else Color(0xFF5A1010)
        else -> MiuixTheme.colorScheme.onSurfaceContainer
    }
    val statusIconBackground = statusContent.copy(alpha = 0.12f)
    val statusIcon = when (snapshot.state) {
        HomeHookState.Active -> "✓"
        HomeHookState.Repairing -> "↻"
        HomeHookState.Error -> "!"
        HomeHookState.Inactive -> "–"
        HomeHookState.Unknown -> "?"
        HomeHookState.Loading -> "…"
    }
    val primaryLabel = when (snapshot.state) {
        HomeHookState.Active -> "运行正常"
        HomeHookState.Repairing -> "正在更新"
        HomeHookState.Error -> "异常"
        HomeHookState.Inactive -> "未激活"
        HomeHookState.Unknown -> "状态未知"
        HomeHookState.Loading -> "检测中"
    }
    val secondaryLabel = when (snapshot.state) {
        HomeHookState.Active -> "系统桌面已加载"
        HomeHookState.Repairing -> "模块更新中"
        HomeHookState.Error -> "模块需要检查"
        HomeHookState.Inactive -> "模块未加载"
        HomeHookState.Unknown -> "等待运行状态"
        HomeHookState.Loading -> "正在检测"
    }

    LazyColumn(
        state = listState,
        modifier = Modifier
            .fillMaxSize()
            .nestedScroll(scrollBehavior.nestedScrollConnection),
        contentPadding = homePagePadding(contentPadding),
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
                        Text(secondaryLabel, fontSize = 15.sp)
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

        if (snapshot.detail.isNotBlank() && snapshot.state != HomeHookState.Active) {
            item {
                Card(modifier = Modifier.fillMaxWidth()) {
                    Column(
                        modifier = Modifier.padding(horizontal = 22.dp, vertical = 16.dp),
                        verticalArrangement = Arrangement.spacedBy(12.dp),
                    ) {
                        Text(
                            text = snapshot.detail,
                            color = MiuixTheme.colorScheme.onSurfaceVariantSummary,
                        )
                        Row(
                            modifier = Modifier.fillMaxWidth(),
                            horizontalArrangement = Arrangement.End,
                        ) {
                            Button(onClick = onOpenDiagnostics) {
                                Text("查看诊断")
                            }
                        }
                    }
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
                    onValueChangeFinished = { applyThreshold(thresholdDp.roundToInt()) },
                    title = "侧边栏触发距离",
                    summary = "",
                    endActions = {
                        Text(
                            text = "$value dp ›",
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
                    text = "系统默认 88 dp",
                    modifier = Modifier.padding(
                        start = 24.dp,
                        end = 24.dp,
                        bottom = if (applyStatus.isBlank()) 18.dp else 6.dp,
                    ),
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

private fun collectHomeStatusSnapshot(context: Context): HomeStatusSnapshot {
    val service = XposedServiceBridge.snapshot(context)
    val runtime = RuntimeRequirementsBridge.snapshot(context)

    fun status(state: HomeHookState, detail: String = "") = HomeStatusSnapshot(state, detail)

    if (!service.serviceConnected() || !runtime.serviceConnected()) {
        return status(
            HomeHookState.Unknown,
            service.error().ifBlank { runtime.error().ifBlank { "LSPosed 服务未连接。" } },
        )
    }
    if (!runtime.lsposedSupported()) {
        return status(HomeHookState.Error, "LSPosed 版本不满足要求。")
    }
    if (!runtime.hyosSpawnerPresent()) {
        return status(HomeHookState.Error, "未检测到 HyOS Runtime。")
    }
    if (!runtime.launcherInScope()) {
        return status(HomeHookState.Inactive, "系统桌面未加入模块作用域。")
    }
    if (!runtime.zygiskNextSupported()) {
        return status(HomeHookState.Error, "当前运行环境不支持 HyOS Runtime。")
    }
    if (!service.launcherLoaded()) {
        return status(HomeHookState.Inactive, "系统桌面尚未加载模块。")
    }

    return when (service.targetState()) {
        "RELOADING" -> status(HomeHookState.Repairing, "模块代码正在重新加载。")
        "FAILED" -> status(HomeHookState.Error, "目标进程模块更新失败。")
        else -> status(HomeHookState.Active)
    }
}

private fun homePagePadding(contentPadding: PaddingValues): PaddingValues = PaddingValues(
    start = 16.dp,
    top = contentPadding.calculateTopPadding() + 4.dp,
    end = 16.dp,
    bottom = contentPadding.calculateBottomPadding() + 20.dp,
)

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
            migrated = if (legacyPx <= 0) {
                ConfigBridge.STOCK_THRESHOLD_DP
            } else {
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
