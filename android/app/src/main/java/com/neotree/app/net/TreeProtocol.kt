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
