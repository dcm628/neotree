package com.neotree.app.render

import kotlin.math.cos
import kotlin.math.sin

/** Where an LED's position came from - the same three cases as the engine's PositionSource. */
enum class PositionSource { MAPPED, SYNTHETIC, NONE }

/**
 * The tree's LED positions, in the engine's space: z up, millimeters, the
 * trunk on the z axis. Built once from tree_positions.csv; the renderer only
 * reads it. Array index = LED number.
 */
class LedLayout(
    val xMm: FloatArray,
    val yMm: FloatArray,
    val zMm: FloatArray,
    val source: Array<PositionSource>,
) {
    val count: Int get() = source.size

    /** Indices of LEDs that have a position, in index order - the ones to draw. */
    val positioned: IntArray = source.indices.filter { source[it] != PositionSource.NONE }.toIntArray()
    val mappedCount: Int = source.count { it == PositionSource.MAPPED }
    val syntheticCount: Int = source.count { it == PositionSource.SYNTHETIC }

    /** Height range of the positioned LEDs (0 if there are none). */
    val minZMm: Float = positioned.minOfOrNull { zMm[it] } ?: 0f
    val maxZMm: Float = positioned.maxOfOrNull { zMm[it] } ?: 0f

    /** 0 = lowest positioned LED, 1 = highest (engine LedGeometry::height01). */
    fun height01(i: Int): Float {
        val span = maxZMm - minZMm
        return if (span > 0f) (zMm[i] - minZMm) / span else 0f
    }

    companion object {
        /** The engine's LedGeometry::max_leds. */
        const val MAX_LEDS = 1000
        const val ASSET_NAME = "tree_positions.csv"

        /**
         * Parses tree_positions.csv (mapping/generate_sim_positions.py): '#'
         * comment lines, a header `index,z_mm,radius_mm,angle_deg,source`, then
         * one LED per line with angle in degrees CCW from +x. Converted to
         * cartesian as the engine does (led_point_from_cylindrical). LEDs
         * missing from the file get no position. Same rules and errors as
         * sim/common/positions_csv.cpp; throws IllegalArgumentException.
         */
        fun parseCsv(text: String): LedLayout {
            data class Row(val index: Int, val x: Float, val y: Float, val z: Float, val source: PositionSource)

            val rows = ArrayList<Row>(MAX_LEDS)
            var headerSeen = false
            text.lineSequence().forEachIndexed { n, raw ->
                val line = raw.trimEnd('\r')
                if (line.isEmpty() || line[0] == '#') return@forEachIndexed
                if (!headerSeen) {
                    headerSeen = true   // index,z_mm,radius_mm,angle_deg,source
                    return@forEachIndexed
                }
                val f = line.split(',')
                val lineNo = n + 1
                val index = f[0].trim().toIntOrNull()
                require(index != null && index in 0 until MAX_LEDS) { "line $lineNo: bad LED index '${f[0]}'" }
                require(f.size >= 5) { "line $lineNo: expected 5 fields, got ${f.size}" }
                val source = when (f[4].trim()) {
                    "mapped" -> PositionSource.MAPPED
                    "synthetic" -> PositionSource.SYNTHETIC
                    "none" -> PositionSource.NONE
                    else -> throw IllegalArgumentException("line $lineNo: unknown source '${f[4]}'")
                }
                fun number(field: Int, name: String) =
                    f[field].trim().toFloatOrNull() ?: throw IllegalArgumentException("line $lineNo: bad $name '${f[field]}'")
                val z = number(1, "z_mm")
                val r = number(2, "radius_mm")
                val a = Math.toRadians(number(3, "angle_deg").toDouble())
                rows += Row(index, (r * cos(a)).toFloat(), (r * sin(a)).toFloat(), z, source)
            }
            require(rows.isNotEmpty()) { "no LEDs" }

            val count = rows.maxOf { it.index } + 1
            val x = FloatArray(count)
            val y = FloatArray(count)
            val z = FloatArray(count)
            val source = Array(count) { PositionSource.NONE }
            for (row in rows) {
                x[row.index] = row.x
                y[row.index] = row.y
                z[row.index] = row.z
                source[row.index] = row.source
            }
            return LedLayout(x, y, z, source)
        }
    }
}
