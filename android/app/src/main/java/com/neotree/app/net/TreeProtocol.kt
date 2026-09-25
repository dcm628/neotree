package com.neotree.app.net

import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * Encoders for the tree's command messages - byte-for-byte the same messages
 * mapping/neotree_serial.py sends over USB serial (firmware:
 * include/neo_tree_protocol.hpp, include/dcm_rgb.hpp). Over the network each
 * one is sent with a uint16 little-endian length prefix (TreeConnection).
 *
 * Color semantics (firmware): every LED shows its overlay color if one is
 * set, otherwise its base color.
 */
object TreeProtocol {
    const val DEFAULT_PORT = 7777
    const val PROTOCOL_VERSION = 1
    const val SERVICE_TYPE = "_neotree._tcp"

    const val REPLY_ACK = 0x80
    const val REPLY_HELLO = 0x81
    /** [0x82][JSON] - reply to STATUS_REQUEST, sent just before its ACK. */
    const val REPLY_STATUS = 0x82
    /** [0x83][JSON] - reply to DESCRIBE (ModeCatalog), sent just before its ACK. */
    const val REPLY_DESCRIBE = 0x83

    private const val NOOP = 0
    private const val COLOR_GROUP_RGB_UPDATE = 2
    private const val STATUS_REQUEST = 18
    private const val REBOOT = 19
    private const val WIFI_RECONNECT = 20
    private const val DEMO = 22
    private const val DESCRIBE = 23
    private const val SLOT_SET = 24
    private const val PARAM_SET = 25
    private const val SLOT_END = 26
    private const val PRESET = 29

    /** Round-trip check - the tree just ACKs it. */
    fun noop(): ByteArray = byteArrayOf(NOOP.toByte())
    /** Asks for a JSON snapshot of the tree's internals (firmware neo_tree_status.hpp). */
    fun statusRequest(): ByteArray = byteArrayOf(STATUS_REQUEST.toByte())
    /** Watchdog reboot ~250ms after the ACK. */
    fun reboot(): ByteArray = byteArrayOf(REBOOT.toByte())
    /** Tree leaves the WiFi network and rejoins (drops this connection). */
    fun wifiReconnect(): ByteArray = byteArrayOf(WIFI_RECONNECT.toByte())

    /**
     * The old DEMO command: runs one of the modes in slot 1, over the Canvas
     * (firmware neo_tree_engine.cpp); id 0 empties the slot.
     */
    val DEMOS = listOf(
        "Off", "Layers", "Lighthouse", "Sweep", "Drop", "Launch", "Bounce", "Snow", "Orbit",
        "Fireworks", "Chain", "Mixer",
    )
    fun demo(id: Int): ByteArray = byteArrayOf(DEMO.toByte(), id.toByte())

    // ---- modes (firmware engine/include/neotree/director.hpp) ----

    /** Asks for the modes and presets (REPLY_DESCRIBE). */
    fun describe(): ByteArray = byteArrayOf(DESCRIBE.toByte())

    /** Starts a mode (ModeInfo.index) in a slot, or empties it (mode -1); fade eases the change. */
    fun slotSet(slot: Int, mode: Int, fade: Boolean): ByteArray =
        byteArrayOf(SLOT_SET.toByte(), slot.toByte(), (if (mode < 0) 0xFF else mode).toByte(), (if (fade) 1 else 0).toByte())

    /** Sets a parameter of the mode running in a slot: numbers use value, colors use color. */
    fun paramSet(slot: Int, param: Int, value: Float, color: Rgb): ByteArray = message(10) {
        put(PARAM_SET.toByte()); put(slot.toByte()); put(param.toByte())
        putFloat(value)
        putRgb(color)
    }

    /** SLOT_END operations. */
    enum class SlotEnd(val code: Int) { END(0), REVERT(1), REMOVE(2), RESTART(3) }

    fun slotEnd(slot: Int, op: SlotEnd): ByteArray = byteArrayOf(SLOT_END.toByte(), slot.toByte(), op.code.toByte())

    /** Back to the base scene (the Canvas with your colors, as the tree boots). */
    fun revertScene(): ByteArray = byteArrayOf(SLOT_END.toByte(), 0xFF.toByte(), SlotEnd.REVERT.code.toByte())

    /** Applies a preset scene (ModeCatalog.presets index). */
    fun preset(index: Int): ByteArray = byteArrayOf(PRESET.toByte(), index.toByte())

    /** LEDs per string, in order - strings 1-4 are LEDs 0-299, 300-599, 600-799, 800-999. */
    val STRING_LENGTHS = intArrayOf(300, 300, 200, 200)

    /**
     * Group updates (12 LEDs each) lighting each string in its own dim color:
     * 1 red, 2 green, 3 blue, 4 white - checks string order and shows glitches.
     */
    fun stringTestPattern(): List<ByteArray> {
        val colors = listOf(Rgb(40, 0, 0), Rgb(0, 40, 0), Rgb(0, 0, 40), Rgb(25, 25, 25))
        val messages = mutableListOf<ByteArray>()
        var first = 0
        STRING_LENGTHS.forEachIndexed { s, length ->
            for (start in first until first + length step 12) {
                val n = minOf(12, first + length - start)
                messages += message(2 + 5 * n) {
                    put(COLOR_GROUP_RGB_UPDATE.toByte()); put(n.toByte())
                    for (i in start until start + n) { putShort(i.toShort()); putRgb(colors[s]) }
                }
            }
            first += length
        }
        return messages
    }

    /** Human-readable name of a command type, for the debug message log. */
    fun typeName(type: Int): String = when (type) {
        0 -> "NOOP"
        1 -> "SINGLE_LED"
        2 -> "GROUP"
        3 -> "FILL"
        9 -> "VOLUME_CART"
        10 -> "VOLUME_CYL"
        12 -> "BASE"
        15 -> "LIGHTS"
        16 -> "OUTPUT_MODE"
        17 -> "OUTPUT_TUNING"
        18 -> "STATUS"
        19 -> "REBOOT"
        20 -> "WIFI_RECONNECT"
        21 -> "BOOTSEL"
        22 -> "DEMO"
        23 -> "DESCRIBE"
        24 -> "SLOT_SET"
        25 -> "PARAM_SET"
        26 -> "SLOT_END"
        27 -> "SLOT_LIFE"
        28 -> "INPUT"
        29 -> "PRESET"
        else -> "TYPE_$type"
    }

    private const val SINGLE_LED_UPDATE = 1
    private const val ALL_LED_UPDATE = 3
    private const val SET_VOLUME_CYLINDRICAL = 10
    private const val ALL_LED_UPDATE_BASE = 12
    private const val TREE_OUTPUT = 15

    /** HELLO flags byte (optional third byte): bit 0 = lights on. */
    const val HELLO_FLAG_OUTPUT_ON = 0x01

    /**
     * Global lights on/off. Off blanks every LED without touching any colors,
     * so on restores exactly what was showing.
     */
    fun treeOutput(on: Boolean): ByteArray = message(2) { put(TREE_OUTPUT.toByte()); put(if (on) 1 else 0) }

    /** Overlay color on one LED. Black clears its overlay, showing its base color. */
    fun singleLed(index: Int, c: Rgb): ByteArray =
        message(6) { put(SINGLE_LED_UPDATE.toByte()); putShort(index.toShort()); putRgb(c) }

    /** Overlay color on every LED - black here is a true "off". */
    fun fillAll(c: Rgb): ByteArray = message(4) { put(ALL_LED_UPDATE.toByte()); putRgb(c) }

    /** Base color on every LED - shown wherever no overlay is set. */
    fun baseAll(c: Rgb): ByteArray = message(4) { put(ALL_LED_UPDATE_BASE.toByte()); putRgb(c) }

    /**
     * Overlay color on every LED whose mapped position is inside the band.
     * Heights in mm, angles in centidegrees (0-36000). With clearOutside, LEDs
     * outside the band drop their overlay and show their base color.
     */
    fun volumeCylindrical(
        zMinMm: Int, zMaxMm: Int,
        radiusMinMm: Int, radiusMaxMm: Int,
        angleMinCdeg: Int, angleMaxCdeg: Int,
        c: Rgb, clearOutside: Boolean,
    ): ByteArray = message(17) {
        put(SET_VOLUME_CYLINDRICAL.toByte())
        putShort(zMinMm.toShort()); putShort(zMaxMm.toShort())
        putShort(radiusMinMm.toShort()); putShort(radiusMaxMm.toShort())
        putShort(angleMinCdeg.toShort()); putShort(angleMaxCdeg.toShort())
        putRgb(c)
        put(if (clearOutside) 1 else 0)
    }

    private fun ByteBuffer.putRgb(c: Rgb) {
        put(c.r.toByte()); put(c.g.toByte()); put(c.b.toByte())
    }

    private inline fun message(size: Int, fill: ByteBuffer.() -> Unit): ByteArray {
        val buf = ByteBuffer.allocate(size).order(ByteOrder.LITTLE_ENDIAN)
        buf.fill()
        check(buf.position() == size) { "message packed ${buf.position()} bytes, expected $size" }
        return buf.array()
    }
}

data class Rgb(val r: Int, val g: Int, val b: Int) {
    fun scaled(factor: Float): Rgb = Rgb(
        (r * factor).toInt().coerceIn(0, 255),
        (g * factor).toInt().coerceIn(0, 255),
        (b * factor).toInt().coerceIn(0, 255),
    )

    companion object {
        val BLACK = Rgb(0, 0, 0)
    }
}

enum class AckStatus(val code: Int) {
    QUEUED(0), QUEUE_FULL(1), UNKNOWN_TYPE(2), BAD_LENGTH(3), NOT_ALLOWED(4);

    companion object {
        fun from(code: Int) = entries.firstOrNull { it.code == code }
    }
}
