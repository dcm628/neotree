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
import kotlinx.coroutines.Job
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.firstOrNull
import kotlinx.coroutines.launch
import kotlinx.coroutines.withTimeoutOrNull

/** What the "live" controls re-apply as they move. */
enum class ColorTarget { NONE, FILL, BACKGROUND, REGION }

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

    // Picker state: hue 0-360, saturation 0-1. Brightness 0-1 scales every
    // color sent - also the family-friendly guard against full-white power draw.
    private val _hue = MutableStateFlow(prefs.getFloat("hue", 120f))
    val hue: StateFlow<Float> = _hue.asStateFlow()
    private val _saturation = MutableStateFlow(prefs.getFloat("saturation", 1f))
    val saturation: StateFlow<Float> = _saturation.asStateFlow()
    private val _brightness = MutableStateFlow(prefs.getFloat("brightness", 0.5f))
    val brightness: StateFlow<Float> = _brightness.asStateFlow()
    private val _region = MutableStateFlow(RegionSettings())
    val region: StateFlow<RegionSettings> = _region.asStateFlow()
    private val _live = MutableStateFlow(true)
    val live: StateFlow<Boolean> = _live.asStateFlow()
    private val _lastTarget = MutableStateFlow(ColorTarget.NONE)
    val lastTarget: StateFlow<ColorTarget> = _lastTarget.asStateFlow()
    private val _status = MutableStateFlow("")
    val status: StateFlow<String> = _status.asStateFlow()
    private val _manualHost = MutableStateFlow(prefs.getString("manual_host", "") ?: "")
    val manualHost: StateFlow<String> = _manualHost.asStateFlow()

    // Live drags produce far more updates than the tree needs; a conflated
    // channel keeps only the newest pending one, so the tree always catches
    // up to where the finger is instead of replaying every step.
    private val liveUpdates = Channel<ByteArray>(Channel.CONFLATED)
    private var connectJob: Job? = null
    private var foreground = false

    // Global lights on/off as last known: from the tree's greeting on
    // connect, then from this phone's own toggles.
    private val _lightsOn = MutableStateFlow(true)
    val lightsOn: StateFlow<Boolean> = _lightsOn.asStateFlow()

    init {
        viewModelScope.launch {
            for (message in liveUpdates) connection.send(message)
        }
        viewModelScope.launch {
            connection.state.collect { s ->
                if (s is TreeConnection.State.Connected && s.lightsOn != null) _lightsOn.value = s.lightsOn
            }
        }
    }

    /** Called when the app comes to the foreground: (re)connect and stay connected. */
    fun onForeground() {
        foreground = true
        if (connectJob?.isActive != true) {
            connectJob = viewModelScope.launch { connectLoop() }
        }
    }

    /** Called when the app is backgrounded: free this phone's slot on the tree. */
    fun onBackground() {
        foreground = false
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

    // ---- controls ----

    fun setHue(value: Float) { _hue.value = value; prefs.edit().putFloat("hue", value).apply(); reapplyLive() }
    fun setSaturation(value: Float) { _saturation.value = value; prefs.edit().putFloat("saturation", value).apply(); reapplyLive() }
    fun setBrightness(value: Float) { _brightness.value = value; prefs.edit().putFloat("brightness", value).apply(); reapplyLive() }
    fun setRegion(value: RegionSettings) { _region.value = value; if (_lastTarget.value == ColorTarget.REGION) reapplyLive() }
    fun setLive(value: Boolean) { _live.value = value }

    fun pickPreset(color: Color) {
        val hsv = FloatArray(3)
        android.graphics.Color.colorToHSV(
            android.graphics.Color.rgb((color.red * 255).toInt(), (color.green * 255).toInt(), (color.blue * 255).toInt()),
            hsv,
        )
        _hue.value = hsv[0]
        _saturation.value = hsv[1]
        prefs.edit().putFloat("hue", hsv[0]).putFloat("saturation", hsv[1]).apply()
        reapplyLive()
    }

    /** The picked color at full value - what the UI swatch shows. */
    fun pickedColor(): Color = Color.hsv(_hue.value, _saturation.value, 1f)

    private fun outputColor(): Rgb {
        val c = pickedColor()
        return Rgb((c.red * 255).toInt(), (c.green * 255).toInt(), (c.blue * 255).toInt()).scaled(_brightness.value)
    }

    fun fillTree() = apply(ColorTarget.FILL)
    fun setBackground() = apply(ColorTarget.BACKGROUND)
    fun paintRegion() = apply(ColorTarget.REGION)

    fun setLights(on: Boolean) {
        viewModelScope.launch {
            val status = connection.send(TreeProtocol.treeOutput(on))
            if (status == AckStatus.QUEUED) _lightsOn.value = on
            _status.value = when (status) {
                AckStatus.QUEUED -> if (on) "Lights on ✓" else "Lights off ✓"
                null -> "Not connected to the tree"
                else -> "Lights: tree rejected the command (older firmware?)"
            }
        }
    }

    fun setSingleLed(index: Int) = sendNow("LED $index", TreeProtocol.singleLed(index, outputColor()))
    fun clearSingleLed(index: Int) = sendNow("LED $index cleared", TreeProtocol.singleLed(index, Rgb.BLACK))

    private fun apply(target: ColorTarget) {
        _lastTarget.value = target
        val label = when (target) {
            ColorTarget.FILL -> "Tree filled"
            ColorTarget.BACKGROUND -> "Background set"
            ColorTarget.REGION -> "Region painted"
            ColorTarget.NONE -> return
        }
        sendNow(label, messageFor(target) ?: return)
    }

    private fun messageFor(target: ColorTarget): ByteArray? {
        val c = outputColor()
        return when (target) {
            ColorTarget.FILL -> TreeProtocol.fillAll(c)
            ColorTarget.BACKGROUND -> TreeProtocol.baseAll(c)
            ColorTarget.REGION -> {
                val r = _region.value
                TreeProtocol.volumeCylindrical(
                    zMinMm = r.heightMinMm.toInt(), zMaxMm = r.heightMaxMm.toInt(),
                    radiusMinMm = 0, radiusMaxMm = UNLIMITED_RADIUS_MM,
                    angleMinCdeg = (r.angleMinDeg * 100).toInt(), angleMaxCdeg = (r.angleMaxDeg * 100).toInt(),
                    c = c, clearOutside = r.clearOutside,
                )
            }
            ColorTarget.NONE -> null
        }
    }

    private fun reapplyLive() {
        if (!_live.value) return
        val message = messageFor(_lastTarget.value) ?: return
        liveUpdates.trySend(message)
    }

    private fun sendNow(label: String, message: ByteArray) {
        viewModelScope.launch {
            _status.value = when (connection.send(message)) {
                AckStatus.QUEUED -> "$label ✓"
                null -> "Not connected to the tree"
                else -> "$label: tree rejected the command"
            }
        }
    }

    override fun onCleared() {
        connection.close()
    }

    private companion object {
        const val DISCOVERY_TIMEOUT_MS = 4000L
        // Radius is a uint16 on the wire; putShort keeps the bit pattern, so
        // 0xFFFF arrives as 65535 = "any radius".
        const val UNLIMITED_RADIUS_MM = 0xFFFF
    }
}
