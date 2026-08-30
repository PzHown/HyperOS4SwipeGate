package io.github.pzhown.hyperos4swipegate

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.os.Build
import android.widget.Toast
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyListState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.nestedscroll.nestedScroll
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import top.yukonga.miuix.kmp.basic.Button
import top.yukonga.miuix.kmp.basic.Card
import top.yukonga.miuix.kmp.basic.ScrollBehavior
import top.yukonga.miuix.kmp.basic.SmallTitle
import top.yukonga.miuix.kmp.basic.Text
import top.yukonga.miuix.kmp.preference.ArrowPreference
import top.yukonga.miuix.kmp.theme.MiuixTheme

private data class DiagnosticUiSnapshot(
    val serviceConnected: Boolean,
    val apiVersion: Int,
    val framework: String,
    val launcherLoaded: Boolean,
    val targetState: String,
    val thresholdDp: Int,
    val launcherVersion: String,
    val launcherInScope: Boolean,
    val hyosRuntimeDetected: Boolean,
    val hyosSpawnerPresent: Boolean,
    val lsposedSupported: Boolean,
    val nativeState: String,
    val nativeFresh: Boolean,
    val nativeHealthy: Boolean,
    val nativeProfile: String,
    val nativeDetail: String,
    val diagnostics: String,
    val nativeLogs: String,
)

private enum class DiagnosticTone { Good, Warning, Error, Neutral }

@Composable
internal fun DiagnosticsScreen(
    contentPadding: PaddingValues,
    listState: LazyListState,
    scrollBehavior: ScrollBehavior,
) {
    val context = androidx.compose.ui.platform.LocalContext.current
    val scope = rememberCoroutineScope()
    var snapshot by remember { mutableStateOf<DiagnosticUiSnapshot?>(null) }
    var loading by remember { mutableStateOf(true) }
    var environmentExpanded by remember { mutableStateOf(false) }
    var logsExpanded by remember { mutableStateOf(false) }

    fun refresh() {
        loading = true
        scope.launch {
            snapshot = withContext(Dispatchers.IO) { collectDiagnosticUiSnapshot(context) }
            loading = false
        }
    }

    fun copyToClipboard(label: String, text: String, toast: String) {
        val clipboard = context.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
        clipboard.setPrimaryClip(ClipData.newPlainText(label, text))
        Toast.makeText(context, toast, Toast.LENGTH_SHORT).show()
    }

    LaunchedEffect(Unit) { refresh() }

    LazyColumn(
        state = listState,
        modifier = Modifier
            .fillMaxSize()
            .nestedScroll(scrollBehavior.nestedScrollConnection),
        contentPadding = diagnosticPagePadding(contentPadding),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        val current = snapshot
        if (current == null) {
            item {
                Card(modifier = Modifier.fillMaxWidth()) {
                    Text(
                        text = if (loading) "正在读取运行状态…" else "暂时无法读取诊断状态",
                        modifier = Modifier.padding(horizontal = 22.dp, vertical = 20.dp),
                        color = MiuixTheme.colorScheme.onSurfaceVariantSummary,
                    )
                }
            }
        } else {
            item { SmallTitle("运行链路") }
            item {
                Card(modifier = Modifier.fillMaxWidth()) {
                    DiagnosticStatusRow(
                        label = "LSPosed",
                        value = when {
                            !current.serviceConnected -> "未连接"
                            !current.lsposedSupported -> "版本不支持"
                            else -> "已连接 · API ${current.apiVersion}"
                        },
                        tone = if (current.serviceConnected && current.lsposedSupported) {
                            DiagnosticTone.Good
                        } else {
                            DiagnosticTone.Error
                        },
                    )
                    DiagnosticStatusRow(
                        label = "HyOS Runtime",
                        value = if (current.hyosRuntimeDetected) "可用" else "未检测到",
                        tone = if (current.hyosRuntimeDetected) DiagnosticTone.Good else DiagnosticTone.Error,
                    )
                    DiagnosticStatusRow(
                        label = "Native Hook",
                        value = nativeHookLabel(current),
                        tone = nativeHookTone(current),
                    )
                }
            }

            item { SmallTitle("目标与 Hook") }
            item {
                Card(modifier = Modifier.fillMaxWidth()) {
                    DiagnosticValueRow("目标", if (current.launcherLoaded) "系统桌面 · 已加载" else "系统桌面 · 未加载")
                    DiagnosticValueRow("Launcher", conciseLauncherVersion(current.launcherVersion))
                    DiagnosticValueRow("作用域", if (current.launcherInScope) "已包含" else "未包含")
                    DiagnosticValueRow("Hook 匹配", current.nativeProfile.ifBlank { "未确认" })
                    if (!current.nativeHealthy && current.nativeDetail.isNotBlank()) {
                        DiagnosticValueRow("Hook 详情", current.nativeDetail)
                    }
                }
            }

            item { SmallTitle("配置") }
            item {
                Card(modifier = Modifier.fillMaxWidth()) {
                    DiagnosticValueRow("触发距离", "${current.thresholdDp} dp")
                }
            }

            item {
                Card(modifier = Modifier.fillMaxWidth()) {
                    ArrowPreference(
                        title = "系统与环境",
                        summary = buildString {
                            append(Build.MANUFACTURER).append(' ').append(Build.MODEL)
                            append(" · Android ").append(Build.VERSION.RELEASE)
                        },
                        onClick = { environmentExpanded = !environmentExpanded },
                    )
                    if (environmentExpanded) {
                        DiagnosticValueRow("Android", "${Build.VERSION.RELEASE} · SDK ${Build.VERSION.SDK_INT}")
                        DiagnosticValueRow("框架", current.framework.ifBlank { "未知" })
                        DiagnosticValueRow("HyOS spawner", if (current.hyosSpawnerPresent) "存在" else "不存在")
                        DiagnosticValueRow("目标状态", targetStateLabel(current.targetState, current.launcherLoaded))
                    }
                }
            }

            item { SmallTitle("运行日志") }
            item {
                Card(modifier = Modifier.fillMaxWidth()) {
                    ArrowPreference(
                        title = "Native 日志",
                        summary = nativeLogSummary(current.nativeLogs),
                        onClick = { logsExpanded = !logsExpanded },
                    )
                    if (logsExpanded) {
                        Row(
                            modifier = Modifier
                                .fillMaxWidth()
                                .padding(start = 18.dp, end = 18.dp, bottom = 8.dp),
                            horizontalArrangement = Arrangement.End,
                        ) {
                            Button(
                                onClick = {
                                    copyToClipboard("SwipeGate native log", current.nativeLogs, "日志已复制")
                                },
                                enabled = current.nativeLogs.isNotBlank(),
                            ) {
                                Text("复制")
                            }
                        }
                        SelectionContainer {
                            Text(
                                text = current.nativeLogs.ifBlank { "暂无日志" },
                                modifier = Modifier
                                    .fillMaxWidth()
                                    .padding(start = 18.dp, end = 18.dp, bottom = 18.dp),
                                fontFamily = FontFamily.Monospace,
                                fontSize = 11.sp,
                                lineHeight = 16.sp,
                                softWrap = true,
                            )
                        }
                    }
                }
            }

            item { SmallTitle("问题排查") }
            item {
                Card(modifier = Modifier.fillMaxWidth()) {
                    Row(
                        modifier = Modifier
                            .fillMaxWidth()
                            .padding(horizontal = 18.dp, vertical = 16.dp),
                        horizontalArrangement = Arrangement.spacedBy(12.dp),
                    ) {
                        Button(
                            onClick = { refresh() },
                            enabled = !loading,
                            modifier = Modifier.weight(1f),
                        ) {
                            Text(if (loading) "刷新中" else "刷新")
                        }
                        Button(
                            onClick = {
                                val payload = buildString {
                                    append(current.diagnostics.trimEnd())
                                    append("\n\n[Native log]\n")
                                    append(current.nativeLogs.trim())
                                }
                                copyToClipboard("SwipeGate diagnostics", payload, "完整诊断已复制")
                            },
                            enabled = !loading,
                            modifier = Modifier.weight(1f),
                        ) {
                            Text("复制诊断")
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun DiagnosticStatusRow(
    label: String,
    value: String,
    tone: DiagnosticTone,
) {
    val color = when (tone) {
        DiagnosticTone.Good -> MiuixTheme.colorScheme.primary
        DiagnosticTone.Warning -> Color(0xFFD59A00)
        DiagnosticTone.Error -> MiuixTheme.colorScheme.error
        DiagnosticTone.Neutral -> MiuixTheme.colorScheme.onSurfaceVariantSummary
    }
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(horizontal = 22.dp, vertical = 14.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Box(
            modifier = Modifier
                .size(8.dp)
                .background(color, CircleShape),
        )
        Text(label, modifier = Modifier.weight(1f), fontSize = 16.sp)
        Text(
            text = value,
            fontSize = 13.sp,
            color = color,
            maxLines = 2,
            overflow = TextOverflow.Ellipsis,
            textAlign = TextAlign.End,
        )
    }
}

@Composable
private fun DiagnosticValueRow(label: String, value: String) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(horizontal = 22.dp, vertical = 13.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(16.dp),
    ) {
        Text(label, fontSize = 15.sp)
        Text(
            text = value,
            modifier = Modifier.weight(1f),
            fontSize = 13.sp,
            color = MiuixTheme.colorScheme.onSurfaceVariantSummary,
            maxLines = 2,
            overflow = TextOverflow.Ellipsis,
            textAlign = TextAlign.End,
        )
    }
}

private fun collectDiagnosticUiSnapshot(context: Context): DiagnosticUiSnapshot {
    val service = XposedServiceBridge.snapshot(context)
    val runtime = RuntimeRequirementsBridge.snapshot(context)
    val native = DiagnosticsStreamBridge.nativeHookStatus()
    val framework = listOf(runtime.frameworkName(), runtime.frameworkVersion())
        .filter { it.isNotBlank() }
        .joinToString(" ")

    return DiagnosticUiSnapshot(
        serviceConnected = service.serviceConnected(),
        apiVersion = service.apiVersion(),
        framework = framework,
        launcherLoaded = service.launcherLoaded(),
        targetState = service.targetState(),
        thresholdDp = service.thresholdDp(),
        launcherVersion = DiagnosticsCollector.readLauncherVersion(context),
        launcherInScope = runtime.launcherInScope(),
        hyosRuntimeDetected = runtime.hyosRuntimeDetected(),
        hyosSpawnerPresent = runtime.hyosSpawnerPresent(),
        lsposedSupported = runtime.lsposedSupported(),
        nativeState = native.state(),
        nativeFresh = native.fresh(),
        nativeHealthy = native.healthy(),
        nativeProfile = native.pattern(),
        nativeDetail = native.detail(),
        diagnostics = DiagnosticsCollector.collect(context),
        nativeLogs = XposedServiceBridge.readNativeRuntimeLog(context),
    )
}

private fun nativeHookLabel(snapshot: DiagnosticUiSnapshot): String = when {
    snapshot.nativeHealthy -> "健康"
    snapshot.nativeState == "HEALTHY" && !snapshot.nativeFresh -> "状态过期"
    snapshot.nativeState == "FAILED" -> "失败"
    snapshot.nativeState == "UNKNOWN" -> "待确认"
    snapshot.nativeState.isBlank() -> "待确认"
    else -> snapshot.nativeState
}

private fun nativeHookTone(snapshot: DiagnosticUiSnapshot): DiagnosticTone = when {
    snapshot.nativeHealthy -> DiagnosticTone.Good
    snapshot.nativeState == "FAILED" -> DiagnosticTone.Error
    snapshot.nativeState == "HEALTHY" && !snapshot.nativeFresh -> DiagnosticTone.Warning
    snapshot.nativeState == "UNKNOWN" || snapshot.nativeState.isBlank() -> DiagnosticTone.Neutral
    else -> DiagnosticTone.Warning
}

private fun targetStateLabel(state: String, launcherLoaded: Boolean): String = when (state) {
    "RELOADING" -> "正在更新"
    "FAILED" -> "更新失败"
    "STALE" -> "待重载"
    "UP_TO_DATE" -> "最新"
    else -> if (launcherLoaded) "已加载" else "未加载"
}

private fun nativeLogSummary(log: String): String {
    val trimmed = log.trim()
    if (trimmed.isBlank()) return "暂无日志"
    if (trimmed == "日志记录已关闭。") return "已关闭"
    val lines = trimmed.lineSequence().count()
    return "$lines 行"
}

private fun conciseLauncherVersion(version: String): String {
    if (version.isBlank()) return "未知"
    return Regex("\\d+\\.\\d+\\.\\d+\\.\\d+").find(version)?.value ?: version
}

private fun diagnosticPagePadding(contentPadding: PaddingValues): PaddingValues = PaddingValues(
    start = 16.dp,
    top = contentPadding.calculateTopPadding() + 4.dp,
    end = 16.dp,
    bottom = contentPadding.calculateBottomPadding() + 20.dp,
)
