package com.neotree.app.render

import kotlin.math.ceil
import kotlin.math.cos
import kotlin.math.sin
import kotlin.math.sqrt
import kotlin.math.tan

/**
 * The tree's outer surface, for putting the brush and balls on it: per
 * height band the radius 85% of the LEDs are inside, like the engine's
 * envelope (LedGeometry::envelope_radius), from the app's LED layout.
 */
class TreeSurface(layout: LedLayout) {
    val minZMm = layout.minZMm
    val maxZMm = layout.maxZMm
    val centerZMm: Float get() = (minZMm + maxZMm) / 2f
    private val radius = FloatArray(BANDS)

    init {
        val height = (maxZMm - minZMm).coerceAtLeast(1f)
        val perBand = Array(BANDS) { ArrayList<Float>() }
        for (i in layout.positioned) {
            val b = (((layout.zMm[i] - minZMm) / height) * BANDS).toInt().coerceIn(0, BANDS - 1)
            perBand[b] += sqrt(layout.xMm[i] * layout.xMm[i] + layout.yMm[i] * layout.yMm[i])
        }
        val have = BooleanArray(BANDS)
        for (b in 0 until BANDS) {
            val rs = perBand[b].sorted()
            if (rs.isNotEmpty()) {
                radius[b] = rs[(ceil(0.85 * rs.size).toInt() - 1).coerceIn(0, rs.size - 1)]
                have[b] = true
            }
        }
        // Gaps from the nearest bands with LEDs.
        for (b in 0 until BANDS) {
            if (have[b]) continue
            val lo = (b downTo 0).firstOrNull { have[it] }
            val hi = (b until BANDS).firstOrNull { have[it] }
            radius[b] = when {
                lo != null && hi != null -> radius[lo] + (radius[hi] - radius[lo]) * (b - lo).toFloat() / (hi - lo)
                lo != null -> radius[lo]
                hi != null -> radius[hi]
                else -> 0f
            }
        }
    }

    /** The surface's radius at height [z] (mm); flat past the ends. */
    fun radiusAt(z: Float): Float {
        val height = maxZMm - minZMm
        if (height <= 0f) return radius[0]
        val f = (z - minZMm) / height * BANDS - 0.5f
        if (f <= 0f) return radius[0]
        if (f >= BANDS - 1) return radius[BANDS - 1]
        val b = f.toInt()
        val t = f - b
        return radius[b] + (radius[b + 1] - radius[b]) * t
    }

    private fun inside(x: Float, y: Float, z: Float): Boolean =
        z >= minZMm && z <= maxZMm && x * x + y * y <= radiusAt(z).let { it * it }

    /**
     * Where a ray from [origin] along unit [dir] first enters the tree, into
     * [out]; false if it misses. Marched in small steps, then refined.
     */
    fun hit(origin: FloatArray, dir: FloatArray, out: FloatArray): Boolean {
        val reach = sqrt(origin[0] * origin[0] + origin[1] * origin[1] + (origin[2] - centerZMm).let { it * it }) +
            (maxZMm - minZMm) + 2000f
        var t = 0f
        var prev = 0f
        while (t < reach) {
            if (inside(origin[0] + dir[0] * t, origin[1] + dir[1] * t, origin[2] + dir[2] * t)) {
                // Bisect between the last point outside and this one.
                var lo = prev
                var hi = t
                repeat(8) {
                    val mid = (lo + hi) / 2f
                    if (inside(origin[0] + dir[0] * mid, origin[1] + dir[1] * mid, origin[2] + dir[2] * mid)) hi = mid else lo = mid
                }
                for (k in 0 until 3) out[k] = origin[k] + dir[k] * hi
                return true
            }
            prev = t
            t += STEP_MM
        }
        return false
    }

    /**
     * Aiming (the phone as a pointer): the point on the side of the tree
     * facing someone standing at angle [sideRad] around the trunk, [rightMm]
     * to their right of the trunk and at height [zMm] - clamped to the tree's
     * outline and height.
     */
    fun front(sideRad: Float, rightMm: Float, zMm: Float, out: FloatArray) {
        val z = zMm.coerceIn(minZMm, maxZMm)
        val r = radiusAt(z)
        val x = rightMm.coerceIn(-r, r)
        // Toward the viewer, and their right (looking at the trunk).
        val tx = cos(sideRad)
        val ty = sin(sideRad)
        val depth = sqrt((r * r - x * x).coerceAtLeast(0f))
        out[0] = tx * depth - ty * x
        out[1] = ty * depth + tx * x
        out[2] = z
    }

    companion object {
        const val BANDS = 16
        const val STEP_MM = 15f

        /**
         * Aim angles to offsets on the tree: turning [dYawRad] right of the
         * calibrated centre and [dPitchRad] above it, from [distanceMm] away,
         * points [rightMm, upMm] from the centre.
         */
        fun aimOffsets(dYawRad: Float, dPitchRad: Float, distanceMm: Float, out: FloatArray) {
            val limit = 1.3f   // ~75 degrees: beyond, tan runs away
            out[0] = distanceMm * tan(dYawRad.coerceIn(-limit, limit))
            out[1] = distanceMm * tan(dPitchRad.coerceIn(-limit, limit))
        }

        /** An angle difference wrapped into [-pi, pi). */
        fun wrap(a: Float): Float {
            val twoPi = (2 * Math.PI).toFloat()
            var d = a % twoPi
            if (d >= Math.PI) d -= twoPi
            if (d < -Math.PI) d += twoPi
            return d
        }
    }
}
