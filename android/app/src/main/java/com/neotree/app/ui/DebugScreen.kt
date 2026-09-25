package com.neotree.app.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import com.neotree.app.TreeViewModel
import com.neotree.app.net.TreeConnection
import com.neotree.app.net.TreeProtocol
import org.json.JSONArray
import org.json.JSONObject
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import kotlinx.coroutines.delay

private val ERROR_RED = Color(0xFFC62828)
private val WARN_AMBER = Color(0xFFE65100)

/**
 * Debug page: connection + controls, the tree's own status snapshot (polled
 * once a second while this page is showing), the tree's event log, and the
 * app's log of commands it sent.
 */
@Composable
fun DebugScreen(vm: TreeViewModel, contentPadding: PaddingValues) {
    val status by vm.connection.status.collectAsState()
    val rates by vm.debugRates.collectAsState()
    // Ticks once a second so "status age" goes stale visibly if polling stops.
    val now by produceState(System.currentTimeMillis()) {
        while (true) {
            delay(1000)
            value = System.currentTimeMillis()
        }
    }
    Column(
        modifier = Modifier
            .padding(contentPadding)
            .verticalScroll(rememberScrollState())
            .padding(horizontal = 12.dp, vertical = 8.dp),
        verticalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        Text("Debug", style = MaterialTheme.typography.titleLarge, fontWeight = FontWeight.Bold)
        ConnectionCard(vm)
        ControlsCard(vm)
        DemosCard(vm, status?.json?.optJSONObject("engine")?.optInt("demo", -1) ?: -1)
        val s = status
        if (s == null) {
            Text("Waiting for the tree's status…", color = MaterialTheme.colorScheme.onSurfaceVariant)
        } else {
            val ageSec = ((now - s.receivedAtMs) / 1000).coerceAtLeast(0)
            TreeCard(s.json, ageSec)
            ClockCard(s.json.optJSONObject("clock"))
            WifiCard(s.json.optJSONObject("wifi"))
            LedCard(s.json.optJSONObject("led"), rates?.fps)
            EngineCard(s.json)
            NetworkCard(s.json, rates?.core1LoopsPerSec)
            TreeEventsCard(s.json.optJSONArray("events"), s.json.optLong("events_total"))
        }
        AppLogCard(vm)
    }
}

// ---- building blocks ----

@Composable
private fun Section(title: String, content: @Composable () -> Unit) {
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(3.dp)) {
            Text(title, style = MaterialTheme.typography.titleSmall, fontWeight = FontWeight.Bold)
            content()
        }
    }
}

/** One label/value line, value in monospace so numbers line up. */
@Composable
private fun Kv(label: String, value: String, valueColor: Color = Color.Unspecified) {
    Row(Modifier.fillMaxWidth()) {
        Text(
            label,
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.weight(0.42f),
        )
        Text(
            value,
            style = MaterialTheme.typography.bodySmall,
            fontFamily = FontFamily.Monospace,
            color = valueColor,
            modifier = Modifier.weight(0.58f),
        )
    }
}

private fun formatUptime(ms: Long): String {
    val s = ms / 1000
    return when {
        s >= 3600 -> "%dh %02dm %02ds".format(s / 3600, s % 3600 / 60, s % 60)
        s >= 60 -> "%dm %02ds".format(s / 60, s % 60)
        else -> "%.1fs".format(ms / 1000.0)
    }
}

private fun rssiQuality(rssi: Int) = when {
    rssi >= -55 -> "excellent"
    rssi >= -67 -> "good"
    rssi >= -75 -> "fair"
    else -> "weak"
}

private fun JSONArray?.ints(): String =
    if (this == null) "-" else (0 until length()).joinToString(" ") { optInt(it).toString() }

private fun JSONArray?.sum(): Int = if (this == null) 0 else (0 until length()).sumOf { optInt(it) }

// ---- sections ----

@Composable
private fun ConnectionCard(vm: TreeViewModel) {
    val state by vm.connection.state.collectAsState()
    val message by vm.status.collectAsState()
    val manualHost by vm.manualHost.collectAsState()
    var hostText by rememberSaveable(manualHost) { mutableStateOf(manualHost) }
    // Rarely needed (discovery finds the tree), so tucked away by default.
    var editingAddress by rememberSaveable { mutableStateOf(false) }
    Section("Connection") {
        val (text, color) = when (val s = state) {
            is TreeConnection.State.Connected -> "Connected to ${s.host}:${s.port}" to Color.Unspecified
            is TreeConnection.State.Connecting -> "Connecting to ${s.host}…" to Color.Unspecified
            is TreeConnection.State.Failed -> "${s.host}: ${s.reason}" to ERROR_RED
            TreeConnection.State.Disconnected -> "Not connected" to ERROR_RED
        }
        Text(text, style = MaterialTheme.typography.bodyMedium, color = color)
        if (message.isNotEmpty()) {
            Text(message, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
        if (manualHost.isNotEmpty() && !editingAddress) {
            Text(
                "Using fixed address $manualHost (discovery off)",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        TextButton(onClick = { editingAddress = !editingAddress }) {
            Text(if (editingAddress) "Hide address" else "Set address manually")
        }
        if (editingAddress) {
            OutlinedTextField(
                value = hostText,
                onValueChange = { hostText = it },
                label = { Text("Address (blank = find automatically)") },
                singleLine = true,
                modifier = Modifier.fillMaxWidth(),
            )
            TextButton(onClick = {
                vm.setManualHost(hostText)
                vm.reconnectNow()
                editingAddress = false
            }) { Text("Save & reconnect") }
        }
    }
}

@Composable
private fun ControlsCard(vm: TreeViewModel) {
    val state by vm.connection.state.collectAsState()
    val connected = state is TreeConnection.State.Connected
    var confirm by rememberSaveable { mutableStateOf<String?>(null) }
    Section("Controls") {
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            OutlinedButton(onClick = vm::ping, enabled = connected, modifier = Modifier.weight(1f)) { Text("Ping") }
            OutlinedButton(onClick = vm::runStringTest, enabled = connected, modifier = Modifier.weight(1f)) {
                Text("String test")
            }
        }
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            OutlinedButton(onClick = { confirm = "wifi" }, enabled = connected, modifier = Modifier.weight(1f)) {
                Text("Rejoin WiFi")
            }
            Button(onClick = { confirm = "reboot" }, enabled = connected, modifier = Modifier.weight(1f)) {
                Text("Reboot tree")
            }
        }
    }
    when (confirm) {
        "reboot" -> ConfirmDialog(
            title = "Reboot the tree?",
            text = "The Pico restarts: the lights go to their boot colors and it's back online in about 5-10 seconds.",
            onConfirm = { vm.rebootTree(); confirm = null },
            onDismiss = { confirm = null },
        )
        "wifi" -> ConfirmDialog(
            title = "Rejoin WiFi?",
            text = "The tree leaves the network and reconnects. Every phone disconnects for a few seconds.",
            onConfirm = { vm.reconnectTreeWifi(); confirm = null },
            onDismiss = { confirm = null },
        )
    }
}

/** Runs the engine's built-in demos over the Canvas (stand-in for modes). */
@Composable
private fun DemosCard(vm: TreeViewModel, running: Int) {
    val state by vm.connection.state.collectAsState()
    val connected = state is TreeConnection.State.Connected
    Section("Engine demos") {
        TreeProtocol.DEMOS.indices.chunked(3).forEach { row ->
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                row.forEach { id ->
                    val label: @Composable () -> Unit = { Text(TreeProtocol.DEMOS[id]) }
                    if (id == running) {
                        Button(onClick = { vm.runDemo(id) }, enabled = connected, modifier = Modifier.weight(1f)) {
                            label()
                        }
                    } else {
                        OutlinedButton(onClick = { vm.runDemo(id) }, enabled = connected, modifier = Modifier.weight(1f)) {
                            label()
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun ConfirmDialog(title: String, text: String, onConfirm: () -> Unit, onDismiss: () -> Unit) {
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(title) },
        text = { Text(text) },
        confirmButton = { TextButton(onClick = onConfirm) { Text("Yes") } },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
    )
}

@Composable
private fun TreeCard(json: JSONObject, ageSec: Long) {
    Section("Tree") {
        val fw = json.optJSONObject("fw")
        Kv("Firmware", "${fw?.optString("board")} built ${fw?.optString("build")}")
        Kv("Uptime", formatUptime(json.optLong("uptime_ms")))
        Kv("Last reset", json.optString("reset"))
        Kv("Lights", if (json.optBoolean("lights_on")) "on" else "off")
        Kv("Status age", if (ageSec <= 2) "live" else "${ageSec}s old", if (ageSec > 5) ERROR_RED else Color.Unspecified)
    }
}

@Composable
private fun ClockCard(c: JSONObject?) {
    Section("Clock") {
        if (c == null) {
            Text("-"); return@Section
        }
        val set = c.optBoolean("set")
        Kv("Time", if (set) c.optString("local") else "not set yet", if (set) Color.Unspecified else ERROR_RED)
        Kv("Time zone", c.optString("tz"))
        val source = when (c.optString("source")) {
            "sntp" -> "internet time (SNTP)"
            "app" -> "a phone (no internet time)"
            else -> "-"
        }
        Kv("Set by", if (set) "$source, ${formatUptime(c.optLong("sync_age_s") * 1000)} ago" else "-")
        Kv("SNTP syncs", c.optLong("sntp_syncs").toString())
        Kv("Last correction", "${c.optLong("last_step_ms")} ms")
    }
}

@Composable
private fun WifiCard(w: JSONObject?) {
    Section("WiFi") {
        if (w == null) {
            Text("-"); return@Section
        }
        val up = w.optInt("link_code") == 3
        Kv("Link", w.optString("link"), if (up) Color.Unspecified else ERROR_RED)
        Kv("Network", w.optString("ssid"))
        if (up) {
            val rssi = w.optInt("rssi")
            Kv("Signal", "$rssi dBm (${rssiQuality(rssi)})")
            Kv("Channel", w.optInt("channel").toString())
            Kv("IP", w.optString("ip"))
            Kv("Gateway / DNS", "${w.optString("gateway")} / ${w.optString("dns")}")
            Kv("Access point", w.optString("bssid"))
            Kv("Up for", formatUptime(w.optLong("up_for_s") * 1000))
        }
        Kv("Tree MAC", w.optString("mac"))
        Kv("Connects since boot", w.optInt("connects").toString())
        Kv("Failed attempts", w.optInt("attempts").toString())
        Text("Connection trace (ms since boot):", style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant)
        Text(w.optString("trace"), style = MaterialTheme.typography.bodySmall, fontFamily = FontFamily.Monospace)
    }
}

@Composable
private fun LedCard(led: JSONObject?, measuredFps: Double?) {
    Section("LED output") {
        if (led == null) {
            Text("-"); return@Section
        }
        Kv("Frame rate", "${measuredFps?.let { "%.1f".format(it) } ?: "…"} fps (target ${led.optInt("target_fps")})")
        Kv("Frames sent", led.optLong("frames").toString())
        Kv("Pack / output", "${led.optInt("prepare_us")} / ${led.optInt("output_us")} µs")
        Kv("Worst output", "${led.optInt("max_output_us")} µs")
        Kv("Mode / phases", "${led.optInt("mode")} / ${led.optJSONArray("phases").ints()}")
        Kv("Stagger", "${led.optInt("stagger_ns")} ns")
        val faults = led.optJSONArray("underflows").sum() + led.optJSONArray("overflows").sum()
        Kv(
            "FIFO faults",
            "under ${led.optJSONArray("underflows").ints()}  over ${led.optJSONArray("overflows").ints()}",
            if (faults > 0) ERROR_RED else Color.Unspecified,
        )
    }
}

/** The rendering engine's per-frame cost, core0 stalls, and crash reports. */
@Composable
private fun EngineCard(json: JSONObject) {
    Section("Engine") {
        val e = json.optJSONObject("engine")
        if (e == null) {
            Text("-"); return@Section
        }
        Kv("Frame time", "${e.optInt("advance_us")} + ${e.optInt("render_us")} µs (sim + render)")
        Kv("Worst", "${e.optInt("max_advance_us")} + ${e.optInt("max_render_us")} µs")
        Kv(
            "Slow frames (>2 ms)",
            e.optInt("slow_frames").toString(),
            if (e.optInt("slow_frames") > 0) WARN_AMBER else Color.Unspecified,
        )
        Kv("Work per frame", "${e.optInt("led_evals")} evaluations, ${e.optInt("entities")} entities")
        Kv("Positioned LEDs", "${e.optInt("positioned")} of ${e.optInt("leds")}")
        Kv("Ticks dropped", e.optLong("ticks_dropped").toString())
        Kv("Rule fires", "${e.optLong("rule_fires")}  (peak ${e.optInt("peak_entities")} entities)")
        val limited = e.optLong("events_dropped") + e.optLong("actions_dropped")
        Kv(
            "Runaway limits hit",
            "quota ${e.optLong("spawns_over_quota")}  events ${e.optLong("events_dropped")}  actions ${e.optLong("actions_dropped")}",
            if (limited > 0) WARN_AMBER else Color.Unspecified,
        )
        Kv(
            "Rejected edits",
            e.optInt("rejected_edits").toString(),
            if (e.optInt("rejected_edits") > 0) ERROR_RED else Color.Unspecified,
        )
        json.optJSONObject("loop")?.let { l ->
            Kv(
                "core0 stalls",
                "${l.optInt("stalls_500us")} ≥0.5ms  ${l.optInt("stalls_2ms")} ≥2ms  ${l.optInt("stalls_10ms")} ≥10ms",
                if (l.optInt("stalls_10ms") > 0) WARN_AMBER else Color.Unspecified,
            )
            Kv("Worst stall", "${l.optInt("max_gap_us")} µs at ${formatUptime(l.optLong("max_gap_at_ms"))}")
        }
        json.optJSONObject("safety")?.let { sf ->
            val core = sf.optInt("fault_core", -1)
            Kv(
                "Crash reboots",
                sf.optInt("crash_count").toString(),
                if (sf.optBoolean("crash_reboot")) ERROR_RED else Color.Unspecified,
            )
            if (core >= 0) {
                Kv("Last fault", "core$core at ${sf.optString("fault_pc")}", ERROR_RED)
            }
        }
    }
}

@Composable
private fun NetworkCard(json: JSONObject, core1LoopsPerSec: Double?) {
    Section("Network & cores") {
        val net = json.optJSONObject("net")
        val lwip = json.optJSONObject("lwip")
        val queue = json.optJSONObject("queue")
        if (net != null) {
            Kv("Phones connected", net.optInt("clients").toString())
            Kv("Commands received", net.optLong("commands").toString())
            Kv("Connections accepted", "${net.optInt("accepts")} (${net.optInt("accept_errors")} errors)")
            val problems = net.optInt("write_failures") + net.optInt("overflow_closes")
            Kv(
                "Replies deferred / failed",
                "${net.optInt("writes_deferred")} / ${net.optInt("write_failures")}",
                if (problems > 0) ERROR_RED else Color.Unspecified,
            )
            Kv("Overflow closes", net.optInt("overflow_closes").toString())
            Kv("Status replies dropped", net.optInt("status_dropped").toString())
        }
        if (lwip != null) {
            Kv(
                "Network heap",
                "${lwip.optInt("heap_used")} used, peak ${lwip.optInt("heap_max")} of ${lwip.optInt("heap_size")}",
            )
            Kv(
                "Heap alloc errors",
                lwip.optInt("heap_errors").toString(),
                if (lwip.optInt("heap_errors") > 0) ERROR_RED else Color.Unspecified,
            )
        }
        if (queue != null) {
            Kv("Command queue", "${queue.optInt("level")} waiting, ${queue.optInt("dropped")} dropped")
        }
        Kv("core1 loop rate", core1LoopsPerSec?.let { "%,.0f /s".format(it) } ?: "…")
    }
}

@Composable
private fun TreeEventsCard(events: JSONArray?, total: Long) {
    Section("Tree event log") {
        if (events == null || events.length() == 0) {
            Text("No events", style = MaterialTheme.typography.bodySmall); return@Section
        }
        Text(
            "Newest first - showing ${events.length()} of $total since boot",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        for (i in 0 until events.length()) {
            val e = events.optJSONArray(i) ?: continue
            Row(Modifier.fillMaxWidth()) {
                Text(
                    formatUptime(e.optLong(0)),
                    style = MaterialTheme.typography.bodySmall,
                    fontFamily = FontFamily.Monospace,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.weight(0.24f),
                )
                val text = e.optString(1)
                val bad = listOf("fail", "lost", "stuck", "timed out", "dropped", "evicted", "overflow")
                    .any { text.contains(it, ignoreCase = true) }
                Text(
                    text,
                    style = MaterialTheme.typography.bodySmall,
                    color = if (bad) ERROR_RED else Color.Unspecified,
                    modifier = Modifier.weight(0.76f),
                )
            }
        }
    }
}

@Composable
private fun AppLogCard(vm: TreeViewModel) {
    val log by vm.connection.log.collectAsState()
    val timeFormat = SimpleDateFormat("HH:mm:ss.SSS", Locale.US)
    Section("App message log") {
        Text(
            "Commands this phone sent and connection events, newest first (status polls not shown)",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        if (log.isEmpty()) {
            Text("Nothing yet", style = MaterialTheme.typography.bodySmall)
        }
        for (entry in log.take(60)) {
            Row(Modifier.fillMaxWidth()) {
                Text(
                    timeFormat.format(Date(entry.timeMs)),
                    style = MaterialTheme.typography.bodySmall,
                    fontFamily = FontFamily.Monospace,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.weight(0.3f),
                )
                Text(
                    entry.text,
                    style = MaterialTheme.typography.bodySmall,
                    color = if (entry.ok) Color.Unspecified else ERROR_RED,
                    modifier = Modifier.weight(0.7f),
                )
            }
        }
    }
}
