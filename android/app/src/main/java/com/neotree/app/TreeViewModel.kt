package com.neotree.app

import android.app.Application
import android.content.Context
import androidx.compose.ui.graphics.Color
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.neotree.app.net.AckStatus
import com.neotree.app.net.Rgb
import com.neotree.app.net.TreeConnection
import com.neotree.app.net.TreeDiscovery
import com.neotree.app.net.TreeProtocol
import com.neotree.app.net.TreeStatus
import kotlinx.coroutines.Job
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.firstOrNull
import kotlinx.coroutines.launch
import kotlinx.coroutines.withTimeoutOrNull

/** What the Paint picker applies to. */
enum class PaintTarget { NONE, FILL, REGION }

/**
 * A picker's color: hue 0-360, saturation 0-1, brightness 0-1. No power
 * limit on brightness: the tree's supply measured 221W at the wall with all
 * 1000 LEDs full white (2026-09-24).
 */
data class PickerColor(val hue: Float, val saturation: Float, val brightness: Float) {
    /** Hue + saturation at full value - what the UI shows as "the color". */
    fun display(): Color = Color.hsv(hue, saturation, 1f)

    fun toRgb(): Rgb {
        val c = display()
        return Rgb((c.red * 255).toInt(), (c.green * 255).toInt(), (c.blue * 255).toInt()).scaled(brightness)
    }

    /**
     * Same brightness, hue/saturation taken from a preset. White keeps the
     * current hue (its own is meaningless) so the wheel thumb doesn't jump.
     */
    fun withPreset(preset: Color): PickerColor {
        val hsv = FloatArray(3)
        android.graphics.Color.colorToHSV(
            android.graphics.Color.rgb((preset.red * 255).toInt(), (preset.green * 255).toInt(), (preset.blue * 255).toInt()),
            hsv,
        )
        return if (hsv[1] < 0.1f) copy(saturation = 0f) else copy(hue = hsv[0], saturation = hsv[1])
    }
}

data class RegionSettings(
    val heightMinMm: Float = 0f,
    val heightMaxMm: Float = 2000f,
    val angleMinDeg: Float = 0f,
    val angleMaxDeg: Float = 360f,
    val clearOutside: Boolean = true,
)

class TreeViewModel(app: Application) : AndroidViewModel(app) {
    private val prefs = app.getSharedPreferences("neotree", Context.MODE_PRIVATE)
    private val discovery = TreeDiscovery(app)
    val connection = TreeConnection(viewModelScope)

    // Two independent pickers: Background always sets every LED's base color;
    // Paint sets the overlay, on the whole tree (FILL) or a region (REGION).
    private val _background = MutableStateFlow(loadColor("bg", PickerColor(30f, 0.9f, 1f)))
    val background: StateFlow<PickerColor> = _background.asStateFlow()
    private val _paint = MutableStateFlow(loadColor("paint", PickerColor(120f, 1f, 1f)))
    val paint: StateFlow<PickerColor> = _paint.asStateFlow()
    private val _paintTarget = MutableStateFlow(PaintTarget.NONE)
    val paintTarget: StateFlow<PaintTarget> = _paintTarget.asStateFlow()
    private val _region = MutableStateFlow(RegionSettings())
    val region: StateFlow<RegionSettings> = _region.asStateFlow()
    private val _status = MutableStateFlow("")
    val status: StateFlow<String> = _status.asStateFlow()
    private val _manualHost = MutableStateFlow(prefs.getString("manual_host", "") ?: "")
    val manualHost: StateFlow<String> = _manualHost.asStateFlow()

    // Global lights on/off as last known: from the tree's greeting on
    // connect, then from this phone's own toggles.
    private val _lightsOn = MutableStateFlow(true)
    val lightsOn: StateFlow<Boolean> = _lightsOn.asStateFlow()

    // Picker drags produce far more updates than the tree needs; conflated
    // channels keep only the newest pending one per picker, so the tree
    // always catches up to where the finger is instead of replaying every step.
    private val backgroundLive = Channel<ByteArray>(Channel.CONFLATED)
    private val paintLive = Channel<ByteArray>(Channel.CONFLATED)
    private var connectJob: Job? = null
    private var foreground = false

    init {
        // Lights re-checked at send time: an update still pending when the
        // lights are switched off must not change the colors being preserved.
        for (channel in listOf(backgroundLive, paintLive)) {
            viewModelScope.launch {
                for (message in channel) if (_lightsOn.value) connection.send(message)
            }
        }
        viewModelScope.launch {
            connection.state.collect { s ->
                if (s is TreeConnection.State.Connected && s.lightsOn != null) _lightsOn.value = s.lightsOn
            }
        }
        viewModelScope.launch {
            connection.status.collect { trackRates(it) }
        }
    }

    // ---- connection ----

    /** Called when the app comes to the foreground: (re)connect and stay connected. */
    fun onForeground() {
        foreground = true
        if (connectJob?.isActive != true) {
            connectJob = viewModelScope.launch { connectLoop() }
        }
        updatePolling()
    }

    /** Called when the app is backgrounded: free this phone's slot on the tree. */
    fun onBackground() {
        foreground = false
        updatePolling()
        connectJob?.cancel()
        connection.close()
    }

    fun reconnectNow() {
        connectJob?.cancel()
        connection.close()
        onForeground()
    }

    fun setManualHost(host: String) {
        _manualHost.value = host.trim()
        prefs.edit().putString("manual_host", host.trim()).apply()
    }

    private suspend fun connectLoop() {
        while (foreground) {
            if (connection.state.value !is TreeConnection.State.Connected) {
                connectOnce()
            }
            // Reconnect promptly after a drop; poll gently otherwise.
            delay(if (connection.state.value is TreeConnection.State.Connected) 1000 else 3000)
        }
    }

    // Order: a typed-in address wins; else service discovery; else the last
    // address that worked; else the mDNS hostname.
    private suspend fun connectOnce() {
        val manual = _manualHost.value
        if (manual.isNotEmpty()) {
            connection.connect(manual)
            return
        }
        _status.value = "Looking for the tree…"
        val found = withTimeoutOrNull(DISCOVERY_TIMEOUT_MS) {
            runCatching { discovery.discover().firstOrNull() }.getOrNull()
        }
        if (found != null && connection.connect(found.host, found.port)) {
            prefs.edit().putString("last_host", found.host).apply()
            _status.value = ""
            return
        }
        val candidates = listOfNotNull(prefs.getString("last_host", null), "neotree.local").distinct()
        for (host in candidates) {
            if (connection.connect(host)) {
                prefs.edit().putString("last_host", host).apply()
                _status.value = ""
                return
            }
        }
        _status.value = "Couldn't find the tree on this WiFi"
    }

    // ---- lights ----

    fun setLights(on: Boolean) {
        // Flip immediately (not on ACK) so live updates stop the moment
        // "off" is tapped; revert if the tree didn't take it.
        val previous = _lightsOn.value
        _lightsOn.value = on
        viewModelScope.launch {
            val status = connection.send(TreeProtocol.treeOutput(on))
            if (status != AckStatus.QUEUED) _lightsOn.value = previous
            _status.value = when (status) {
                AckStatus.QUEUED -> if (on) "Lights on ✓" else "Lights off ✓"
                null -> "Not connected to the tree"
                else -> "Lights: tree rejected the command (older firmware?)"
            }
        }
    }

    // While the lights are off, nothing on this phone changes the tree's
    // colors, so turning them back on restores exactly what was showing.
    // The pickers still move (to choose a color in advance); they just
    // don't send.
    private fun lightsOffBlocks(): Boolean {
        if (_lightsOn.value) return false
        _status.value = "Lights are off – turn them on to change colors"
        return true
    }

    // ---- background picker: always the base color, applied live ----

    fun setBackgroundColor(value: PickerColor) {
        _background.value = value
        saveColor("bg", value)
        if (_lightsOn.value) backgroundLive.trySend(TreeProtocol.baseAll(value.toRgb()))
    }

    fun applyBackground() {
        if (lightsOffBlocks()) return
        sendNow("Background set", TreeProtocol.baseAll(_background.value.toRgb()))
    }

    // ---- paint picker: the overlay, on the whole tree or a region ----

    fun setPaintColor(value: PickerColor) {
        _paint.value = value
        saveColor("paint", value)
        reapplyPaint()
    }

    fun setRegion(value: RegionSettings) {
        _region.value = value
        if (_paintTarget.value == PaintTarget.REGION) reapplyPaint()
    }

    fun paintFill() = applyPaint(PaintTarget.FILL, "Tree filled")
    fun paintRegion() = applyPaint(PaintTarget.REGION, "Region painted")

    private fun applyPaint(target: PaintTarget, label: String) {
        if (lightsOffBlocks()) return
        _paintTarget.value = target
        sendNow(label, paintMessage(target) ?: return)
    }

    private fun reapplyPaint() {
        if (!_lightsOn.value) return
        paintLive.trySend(paintMessage(_paintTarget.value) ?: return)
    }

    private fun paintMessage(target: PaintTarget): ByteArray? {
        val c = _paint.value.toRgb()
        return when (target) {
            PaintTarget.FILL -> TreeProtocol.fillAll(c)
            PaintTarget.REGION -> {
                val r = _region.value
                TreeProtocol.volumeCylindrical(
                    zMinMm = r.heightMinMm.toInt(), zMaxMm = r.heightMaxMm.toInt(),
                    radiusMinMm = 0, radiusMaxMm = UNLIMITED_RADIUS_MM,
                    angleMinCdeg = (r.angleMinDeg * 100).toInt(), angleMaxCdeg = (r.angleMaxDeg * 100).toInt(),
                    c = c, clearOutside = r.clearOutside,
                )
            }
            PaintTarget.NONE -> null
        }
    }

    // ---- single LED (advanced), in the paint color ----

    fun setSingleLed(index: Int) {
        if (lightsOffBlocks()) return
        sendNow("LED $index", TreeProtocol.singleLed(index, _paint.value.toRgb()))
    }

    fun clearSingleLed(index: Int) {
        if (lightsOffBlocks()) return
        sendNow("LED $index cleared", TreeProtocol.singleLed(index, Rgb.BLACK))
    }

    // ---- debug page ----

    /** Rates worked out from the change in the tree's counters between two STATUS replies. */
    data class DebugRates(val fps: Double, val core1LoopsPerSec: Double)

    private val _debugRates = MutableStateFlow<DebugRates?>(null)
    val debugRates: StateFlow<DebugRates?> = _debugRates.asStateFlow()
    private var previousStatus: TreeStatus? = null
    private var debugVisible = false
    private var pollJob: Job? = null

    private fun trackRates(s: TreeStatus?) {
        val prev = previousStatus
        previousStatus = s
        if (s == null || prev == null) return
        val dtSec = (s.json.optLong("uptime_ms") - prev.json.optLong("uptime_ms")) / 1000.0
        // A reboot resets the counters (uptime goes backwards) - wait for the next pair.
        if (dtSec <= 0.0) return
        val frames = s.json.optJSONObject("led")?.optLong("frames") ?: return
        val prevFrames = prev.json.optJSONObject("led")?.optLong("frames") ?: return
        val loops = s.json.optLong("core1_loops") - prev.json.optLong("core1_loops")
        _debugRates.value = DebugRates((frames - prevFrames) / dtSec, loops / dtSec)
    }

    /** The debug page polls STATUS once a second while it's on screen (and the app is). */
    fun setDebugVisible(visible: Boolean) {
        debugVisible = visible
        updatePolling()
    }

    private fun updatePolling() {
        if (debugVisible && foreground) {
            if (pollJob?.isActive != true) {
                pollJob = viewModelScope.launch {
                    while (true) {
                        if (connection.state.value is TreeConnection.State.Connected) {
                            connection.send(TreeProtocol.statusRequest())
                        }
                        delay(STATUS_POLL_MS)
                    }
                }
            }
        } else {
            pollJob?.cancel()
            pollJob = null
        }
    }

    fun ping() {
        viewModelScope.launch {
            val started = System.nanoTime()
            val status = connection.send(TreeProtocol.noop())
            val ms = (System.nanoTime() - started) / 1_000_000
            _status.value = if (status == AckStatus.QUEUED) "Ping: ${ms}ms round trip" else "Ping failed"
        }
    }

    fun rebootTree() = sendNow("Reboot", TreeProtocol.reboot())
    fun reconnectTreeWifi() = sendNow("WiFi reconnect", TreeProtocol.wifiReconnect())

    /** Lights strings 1-4 dim red/green/blue/white - checks string order, shows glitches. */
    fun runStringTest() {
        if (lightsOffBlocks()) return
        viewModelScope.launch {
            val messages = TreeProtocol.stringTestPattern()
            var ok = 0
            for (m in messages) {
                if (connection.send(m) == AckStatus.QUEUED) ok++ else break
            }
            _status.value = "String test: $ok of ${messages.size} messages sent"
        }
    }

    // ---- helpers ----

    private fun sendNow(label: String, message: ByteArray) {
        viewModelScope.launch {
            _status.value = when (connection.send(message)) {
                AckStatus.QUEUED -> "$label ✓"
                null -> "Not connected to the tree"
                else -> "$label: tree rejected the command"
            }
        }
    }

    private fun loadColor(key: String, default: PickerColor) = PickerColor(
        prefs.getFloat("${key}_hue", default.hue),
        prefs.getFloat("${key}_sat", default.saturation),
        prefs.getFloat("${key}_bright", default.brightness),
    )

    private fun saveColor(key: String, c: PickerColor) {
        prefs.edit()
            .putFloat("${key}_hue", c.hue)
            .putFloat("${key}_sat", c.saturation)
            .putFloat("${key}_bright", c.brightness)
            .apply()
    }

    override fun onCleared() {
        connection.close()
    }

    private companion object {
        const val DISCOVERY_TIMEOUT_MS = 4000L
        const val STATUS_POLL_MS = 1000L
        // Radius is a uint16 on the wire; putShort keeps the bit pattern, so
        // 0xFFFF arrives as 65535 = "any radius".
        const val UNLIMITED_RADIUS_MM = 0xFFFF
    }
}
