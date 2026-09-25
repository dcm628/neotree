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
    /** The stream channel: UDP, newest-wins brush samples (TreeStream). */
    const val STREAM_PORT = 7778
    const val PROTOCOL_VERSION = 1
    const val SERVICE_TYPE = "_neotree._tcp"

    const val REPLY_ACK = 0x80
    const val REPLY_HELLO = 0x81
    /** [0x82][JSON] - reply to STATUS_REQUEST, sent just before its ACK. */
    const val REPLY_STATUS = 0x82
    /** [0x83][JSON] - reply to DESCRIBE (ModeCatalog), sent just before its ACK. */
    const val REPLY_DESCRIBE = 0x83
    /** [0x84][JSON] - the scene (SceneState), pushed after SUBSCRIBE whenever it changes. */
    const val REPLY_SCENE = 0x84
    /** [0x85][JSON] - the library (TreeLibrary): reply to LIBRARY, and pushed after SUBSCRIBE. */
    const val REPLY_LIBRARY = 0x85
    /** [0x86][JSON]: a section of the custom-effect schema (reply to FX_SCHEMA). */
    const val REPLY_FX_SCHEMA = 0x86
    /** [0x87][JSON]: a section of the draft effect (reply to FX_GET, a frame after its ACK). */
    const val REPLY_FX = 0x87

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
    private const val SLOT_LIFE = 27
    private const val PRESET = 29
    private const val LIBRARY = 30
    private const val SCENE_SAVE = 31
    private const val LIBRARY_DELETE = 32
    private const val SHOW_SET = 33
    private const val SHOW_PLAY = 34
    private const val SHOW_BOOT = 35
    private const val SUBSCRIBE = 36
    private const val ENTITY_SPAWN = 38
    private const val ENTITY_KILL = 39
    private const val BRUSH = 40
    private const val FX_SCHEMA = 41
    private const val FX_EDIT = 42
    private const val FX_SET = 43
    private const val FX_ITEM = 44
    private const val FX_GET = 45
    private const val FX_SAVE = 46
    private const val TIME_SET = 47
    private const val TIME_ZONE = 48
    const val BRUSH_LEN = 14

    /** Names on the wire: 20 bytes, NUL-padded (at most 19 used). */
    private const val NAME_LEN = 20
    // The tree's library limits (firmware engine/include/neotree/library.hpp).
    const val MAX_SHOW_ENTRIES = 16
    const val MAX_USER_PRESETS = 8
    const val MAX_USER_SHOWS = 4
    const val MAX_EFFECTS = 6

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

    /** Applies a preset scene (TreeLibrary.presets index). */
    fun preset(index: Int): ByteArray = byteArrayOf(PRESET.toByte(), index.toByte())

    // ---- the library (firmware engine/include/neotree/library.hpp) ----

    /** Asks for the library (REPLY_LIBRARY). */
    fun library(): ByteArray = byteArrayOf(LIBRARY.toByte())

    /** Has the tree push the scene and the library on every change (and once now). */
    fun subscribe(scene: Boolean = true, library: Boolean = true): ByteArray =
        byteArrayOf(SUBSCRIBE.toByte(), ((if (scene) 1 else 0) or (if (library) 2 else 0)).toByte())

    /** Saves the live scene as a preset (replacing the user preset of that name). */
    fun savePreset(name: String): ByteArray = byteArrayOf(SCENE_SAVE.toByte(), 1) + nameBytes(name)

    /** Makes the live scene the base scene - what the tree boots into and reverts to. */
    fun saveAsBase(): ByteArray = byteArrayOf(SCENE_SAVE.toByte(), 0) + ByteArray(NAME_LEN)

    fun resetBase(): ByteArray = byteArrayOf(LIBRARY_DELETE.toByte(), 0, 0)
    fun deletePreset(index: Int): ByteArray = byteArrayOf(LIBRARY_DELETE.toByte(), 1, index.toByte())
    fun deleteShow(index: Int): ByteArray = byteArrayOf(LIBRARY_DELETE.toByte(), 2, index.toByte())
    /** A saved custom effect, by its position (EffectInfo.position). */
    fun deleteEffect(position: Int): ByteArray = byteArrayOf(LIBRARY_DELETE.toByte(), 3, position.toByte())

    // ---- the clock (firmware neo_tree_clock.hpp) ----

    /** The phone's time; the tree takes it only if SNTP hasn't synced for 2 hours. */
    fun timeSet(unixMs: Long): ByteArray = message(9) {
        put(TIME_SET.toByte())
        putLong(unixMs)
    }

    /** A POSIX TZ rule (PosixTimeZone), stored on the tree; it keeps local time with it. */
    fun timeZone(rule: String): ByteArray {
        val raw = rule.toByteArray(Charsets.US_ASCII).take(63).toByteArray()
        return byteArrayOf(TIME_ZONE.toByte(), raw.size.toByte()) + raw
    }

    // ---- custom effects (engine/include/neotree/effect.hpp) ----
    // One draft effect is edited at a time, running in a slot; edits show there at once.

    /** The fields of a section of the effect schema (EffectSection.code). */
    fun fxSchema(section: Int): ByteArray = byteArrayOf(FX_SCHEMA.toByte(), section.toByte())

    /** Starts editing in slot: a new effect (mode -1), a copy of a built-in mode, or a saved effect (its mode index). */
    fun fxEdit(slot: Int, mode: Int): ByteArray =
        byteArrayOf(FX_EDIT.toByte(), slot.toByte(), (if (mode < 0) 0xFF else mode).toByte())

    /** One field of the draft: numbers, choices and toggles in value, colors in color. An action's item is rule * 4 + action. */
    fun fxSet(section: Int, item: Int, field: Int, value: Float, color: Rgb): ByteArray = message(11) {
        put(FX_SET.toByte()); put(section.toByte()); put(item.toByte()); put(field.toByte())
        putFloat(value)
        putRgb(color)
    }

    enum class FxOp(val code: Int) { ADD(0), REMOVE(1), DUPLICATE(2) }

    /** Adds (actions: to rule `item`), removes or duplicates an item of the draft. */
    fun fxItem(op: FxOp, section: Int, item: Int): ByteArray =
        byteArrayOf(FX_ITEM.toByte(), op.code.toByte(), section.toByte(), item.toByte())

    /** Asks for a section of the draft (answered with a REPLY_FX frame). */
    fun fxGet(section: Int): ByteArray = byteArrayOf(FX_GET.toByte(), section.toByte())

    /** Stores the draft as an effect called name (replacing the one of that name). */
    fun fxSave(name: String): ByteArray = byteArrayOf(FX_SAVE.toByte()) + nameBytes(name)

    /** Saves a show (replacing the user show of that name): entries are (preset index, seconds). */
    fun saveShow(name: String, entries: List<Pair<Int, Int>>, loop: Boolean, shuffle: Boolean): ByteArray {
        require(entries.size in 1..MAX_SHOW_ENTRIES)
        return message(3 + NAME_LEN + 3 * entries.size) {
            put(SHOW_SET.toByte())
            put(((if (loop) 1 else 0) or (if (shuffle) 2 else 0)).toByte())
            put(entries.size.toByte())
            put(nameBytes(name))
            for ((preset, seconds) in entries) {
                put(preset.toByte())
                putShort(seconds.coerceIn(0, 0xFFFF).toShort())
            }
        }
    }

    /** Plays a show (TreeLibrary.shows index); -1 stops it, leaving the scene as it is. */
    fun playShow(index: Int): ByteArray = byteArrayOf(SHOW_PLAY.toByte(), (if (index < 0) 0xFF else index).toByte())

    /** The show to play at power-up; -1 for none (the base scene). */
    fun bootShow(index: Int): ByteArray = byteArrayOf(SHOW_BOOT.toByte(), (if (index < 0) 0xFF else index).toByte())

    /**
     * What a slot does when its mode ends (SLOT_LIFE): after [durationSec]
     * and/or [cycles] (0 = never), [policy]; chain goes to mode [nextMode].
     */
    fun slotLife(slot: Int, durationSec: Int, cycles: Int, policy: EndPolicy, nextMode: Int = -1, repeats: Int = 0): ByteArray =
        message(11) {
            put(SLOT_LIFE.toByte()); put(slot.toByte())
            putShort(durationSec.coerceIn(0, 0xFFFF).toShort())
            putShort(cycles.coerceIn(0, 0xFFFF).toShort())
            put(policy.ordinal.toByte())
            put(repeats.coerceIn(0, 255).toByte())
            put((if (nextMode < 0) 0xFF else nextMode).toByte())
            put(1)    // fade
            put(15)   // over 1.5 s
        }

    // ---- direct control (firmware engine/include/neotree/direct.hpp) ----
    // In a slot running the "play" mode. What this connection creates is its
    // own, removed when it disconnects. Positions in mm, engine space (z up,
    // trunk on the z axis); velocities in mm/s.

    /** Kinds of entity a phone can make. */
    enum class DirectKind(val code: Int) { BALL(0), BRUSH(1) }

    /** A ball at [pos], moving at [vel]; [id] is this phone's own (the same id again replaces it). */
    fun spawnBall(slot: Int, id: Int, pos: FloatArray, vel: FloatArray, color: Rgb, sizeMm: Int = 0): ByteArray =
        message(20) {
            put(ENTITY_SPAWN.toByte()); put(slot.toByte()); put(id.toByte()); put(DirectKind.BALL.code.toByte())
            putXyz(pos); putXyz(vel)
            putRgb(color)
            put(sizeMm.coerceIn(0, 255).toByte())
        }

    /** Removes one of this phone's entities; -1 all of them. */
    fun killEntity(id: Int): ByteArray = byteArrayOf(ENTITY_KILL.toByte(), (if (id < 0) 0xFF else id).toByte())

    /** A brush sample (see TreeStream for sending them live over UDP). */
    fun brush(slot: Int, id: Int, pos: FloatArray, color: Rgb, radiusMm: Int, penDown: Boolean): ByteArray =
        message(BRUSH_LEN) {
            put(BRUSH.toByte()); put(slot.toByte()); put(id.toByte()); put((if (penDown) 1 else 0).toByte())
            putXyz(pos)
            putRgb(color)
            put(radiusMm.coerceIn(1, 255).toByte())
        }

    /** A stream datagram: ['N'][1][token u32][seq u16][message]. */
    fun streamPacket(token: Int, seq: Int, message: ByteArray): ByteArray {
        val buf = ByteBuffer.allocate(8 + message.size).order(ByteOrder.LITTLE_ENDIAN)
        buf.put('N'.code.toByte()); buf.put(1)
        buf.putInt(token); buf.putShort(seq.toShort())
        buf.put(message)
        return buf.array()
    }

    private fun ByteBuffer.putXyz(v: FloatArray) {
        for (k in 0 until 3) putShort(v[k].toInt().coerceIn(-32768, 32767).toShort())
    }

    /** Lifecycle policies, in the firmware's order (EndPolicy). */
    enum class EndPolicy(val label: String) {
        LOOP("Start again"), CHAIN("Change to another mode"), REVERT("Back to the base scene"),
        REMOVE("Clear the slot"), HOLD("Keep going");

        companion object {
            fun from(name: String): EndPolicy = entries.firstOrNull { it.name.equals(name, ignoreCase = true) } ?: HOLD
        }
    }

    /** The name the tree will store for [name] (see nameBytes). */
    fun storedName(name: String): String {
        val bytes = nameBytes(name)
        val end = bytes.indexOf(0).let { if (it < 0) bytes.size else it }
        return String(bytes, 0, end, Charsets.UTF_8).trim()
    }

    /** A name as the tree stores it: UTF-8, at most 19 bytes (never splitting a character), NUL-padded. */
    fun nameBytes(name: String): ByteArray {
        val out = ByteArray(NAME_LEN)
        var used = 0
        for (ch in name.trim().codePoints()) {
            val bytes = String(Character.toChars(ch)).toByteArray(Charsets.UTF_8)
            if (used + bytes.size > NAME_LEN - 1) break
            bytes.copyInto(out, used)
            used += bytes.size
        }
        return out
    }

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
        30 -> "LIBRARY"
        31 -> "SCENE_SAVE"
        32 -> "LIBRARY_DELETE"
        33 -> "SHOW_SET"
        34 -> "SHOW_PLAY"
        35 -> "SHOW_BOOT"
        36 -> "SUBSCRIBE"
        37 -> "BENCH"
        38 -> "ENTITY_SPAWN"
        39 -> "ENTITY_KILL"
        40 -> "BRUSH"
        41 -> "FX_SCHEMA"
        42 -> "FX_EDIT"
        43 -> "FX_SET"
        44 -> "FX_ITEM"
        45 -> "FX_GET"
        46 -> "FX_SAVE"
        47 -> "TIME_SET"
        48 -> "TIME_ZONE"
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
