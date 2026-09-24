package com.neotree.app.render

import kotlin.math.PI
import kotlin.math.cos
import kotlin.math.sin
import kotlin.math.tan

/**
 * The desktop viewer's orbit camera (sim/viewer/main.cpp), in engine space
 * (z up, mm). It circles the point (0, 0, [targetZMm]) on the trunk at
 * [distanceMm]; [yaw] turns it around the trunk, [pitch] raises it above
 * (positive) or below the target. Same numbers as the viewer, which works in
 * meters with y up.
 */
data class OrbitCamera(val yaw: Float, val pitch: Float, val distanceMm: Float, val targetZMm: Float) {
    fun orbit(dYaw: Float, dPitch: Float) = copy(
        yaw = (yaw + dYaw) % TWO_PI,
        pitch = (pitch + dPitch).coerceIn(-MAX_PITCH, MAX_PITCH),
    )

    /** [factor] > 1 moves in (pinch apart), < 1 backs off. */
    fun zoom(factor: Float) =
        if (factor > 0f) copy(distanceMm = (distanceMm / factor).coerceIn(MIN_DISTANCE_MM, MAX_DISTANCE_MM)) else this

    /** Moves the target up the trunk, kept within the tree's height ([minZMm] to [maxZMm]). */
    fun raise(dzMm: Float, minZMm: Float, maxZMm: Float) =
        copy(targetZMm = (targetZMm + dzMm).coerceIn(minOf(minZMm, maxZMm), maxOf(minZMm, maxZMm)))

    companion object {
        /** Vertical field of view, as the viewer's Camera3D.fovy. */
        const val FOV_Y_DEG = 45f
        const val MAX_PITCH = 1.4f
        const val MIN_DISTANCE_MM = 500f
        const val MAX_DISTANCE_MM = 20000f
        private const val TWO_PI = (2 * PI).toFloat()

        /** The viewer's starting view: aimed at the middle of the tree's height. */
        fun framing(layout: LedLayout) = OrbitCamera(
            yaw = 0.6f,
            pitch = 0.25f,
            distanceMm = 4500f,
            targetZMm = (layout.minZMm + layout.maxZMm) / 2f,
        )

        /** Pixels per unit of (sideways offset / depth) for a view [viewHeightPx] tall. */
        fun focalLengthPx(viewHeightPx: Float): Float =
            viewHeightPx / 2f / tan(Math.toRadians(FOV_Y_DEG / 2.0)).toFloat()
    }
}

/**
 * Perspective projection for one camera and viewport: [set] once per frame,
 * then [project] each point. Screen coordinates are pixels, origin top-left,
 * y down; depth is mm along the view direction. Allocates nothing after
 * construction.
 */
class Projection {
    private var eyeX = 0f
    private var eyeY = 0f
    private var eyeZ = 0f
    // Camera basis in engine space. Right is always level (the camera never rolls).
    private var rightX = 0f
    private var rightY = 0f
    private var upX = 0f
    private var upY = 0f
    private var upZ = 0f
    private var fwdX = 0f
    private var fwdY = 0f
    private var fwdZ = 0f
    private var centerX = 0f
    private var centerY = 0f

    var focalPx = 0f
        private set

    // Camera-space scratch for projectSegment.
    private val seg = FloatArray(6)

    fun set(camera: OrbitCamera, widthPx: Float, heightPx: Float) {
        val sy = sin(camera.yaw)
        val cy = cos(camera.yaw)
        val sp = sin(camera.pitch)
        val cp = cos(camera.pitch)
        val d = camera.distanceMm
        // The viewer's position = target + d*(cp*sin(yaw), sp, cp*cos(yaw)) in
        // its y-up space, which is (x, z, -y) of engine space.
        eyeX = d * cp * sy
        eyeY = -d * cp * cy
        eyeZ = camera.targetZMm + d * sp
        fwdX = -cp * sy
        fwdY = cp * cy
        fwdZ = -sp
        rightX = cy          // normalize(forward x z-up); cp > 0 since |pitch| < pi/2
        rightY = sy
        upX = -sp * sy       // right x forward
        upY = sp * cy
        upZ = cp
        focalPx = OrbitCamera.focalLengthPx(heightPx)
        centerX = widthPx / 2f
        centerY = heightPx / 2f
    }

    /**
     * Writes (screen x, screen y, depth) into [out]. Returns false - and
     * leaves [out] alone - for points nearer than [NEAR_MM] or behind the camera.
     */
    fun project(x: Float, y: Float, z: Float, out: FloatArray): Boolean {
        val rx = x - eyeX
        val ry = y - eyeY
        val rz = z - eyeZ
        val depth = rx * fwdX + ry * fwdY + rz * fwdZ
        if (depth < NEAR_MM) return false
        val s = focalPx / depth
        out[0] = centerX + (rx * rightX + ry * rightY) * s
        out[1] = centerY - (rx * upX + ry * upY + rz * upZ) * s
        out[2] = depth
        return true
    }

    /** On-screen size, px, of something [sizeMm] across at [depth]. */
    fun sizePx(sizeMm: Float, depth: Float): Float = sizeMm * focalPx / depth

    /**
     * Projects a line segment, clipped to the near plane. Writes (x0, y0, x1,
     * y1) in pixels into [out]; false if it's entirely behind the near plane.
     */
    fun projectSegment(x0: Float, y0: Float, z0: Float, x1: Float, y1: Float, z1: Float, out: FloatArray): Boolean {
        toCamera(x0, y0, z0, 0)
        toCamera(x1, y1, z1, 3)
        val d0 = seg[2]
        val d1 = seg[5]
        if (d0 < NEAR_MM && d1 < NEAR_MM) return false
        if (d0 < NEAR_MM || d1 < NEAR_MM) {
            // Move the near end to where the segment crosses the near plane.
            val t = (NEAR_MM - d0) / (d1 - d0)
            val at = if (d0 < NEAR_MM) 0 else 3
            for (k in 0 until 3) seg[at + k] = seg[k] + (seg[3 + k] - seg[k]) * t
            seg[at + 2] = NEAR_MM
        }
        for (end in 0..1) {
            val s = focalPx / seg[end * 3 + 2]
            out[end * 2] = centerX + seg[end * 3] * s
            out[end * 2 + 1] = centerY - seg[end * 3 + 1] * s
        }
        return true
    }

    private fun toCamera(x: Float, y: Float, z: Float, at: Int) {
        val rx = x - eyeX
        val ry = y - eyeY
        val rz = z - eyeZ
        seg[at] = rx * rightX + ry * rightY
        seg[at + 1] = rx * upX + ry * upY + rz * upZ
        seg[at + 2] = rx * fwdX + ry * fwdY + rz * fwdZ
    }

    companion object {
        /** Nearest drawable depth - also caps how huge a dot can get when zoomed right in. */
        const val NEAR_MM = 100f
    }
}
