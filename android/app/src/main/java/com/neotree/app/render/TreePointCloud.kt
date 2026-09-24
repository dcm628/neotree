package com.neotree.app.render

import java.util.Arrays

/** The viewer's color views (its C key). */
enum class ColorView(val label: String, val description: String) {
    OUTPUT("Output", "engine output"),
    SOURCE("Source", "position source (orange mapped, blue synthetic)"),
    HEIGHT("Height", "height");

    fun next(): ColorView = entries[(ordinal + 1) % entries.size]
}

/**
 * The viewer's look, as ARGB: background, dots, floor grid and trunk
 * (sim/viewer/main.cpp; the grid is raylib's DrawGrid(12, 0.25)).
 */
object ViewerColors {
    const val BACKGROUND = 0xFF0C0C10.toInt()
    /** Unlit LEDs: dim and small, so the tree stays visible when it's dark. */
    const val UNLIT = 0xFF2D2D32.toInt()
    const val MAPPED = 0xFFFF9628.toInt()
    const val SYNTHETIC = 0xFF5A78C8.toInt()
    const val TRUNK = 0xFF463728.toInt()
    const val GRID_CENTER = 0xFF7F7F7F.toInt()
    const val GRID = 0xFFBFBFBF.toInt()

    /** Dot radii, mm: a lit LED, and an unlit one in the output view. */
    const val DOT_MM = 12f
    const val UNLIT_DOT_MM = 7f

    /** Blue at the bottom through green to red at the top (h = 0..1). */
    fun height(h: Float): Int = hsv(240f * (1f - h), 0.85f, 1f)

    /** raylib's ColorFromHSV: hue in degrees, saturation and value 0..1. */
    fun hsv(hue: Float, saturation: Float, value: Float): Int {
        fun channel(n: Float): Int {
            var k = (n + hue / 60f) % 6f
            k = minOf(k, 4f - k).coerceIn(0f, 1f)
            return ((value - value * saturation * k) * 255f).toInt()
        }
        return (0xFF shl 24) or (channel(5f) shl 16) or (channel(3f) shl 8) or channel(1f)
    }
}

/**
 * Everything the Renderer view draws, projected for the current camera.
 * Built once per layout: the positions, the fixed colors of the source and
 * height views, and the floor grid and trunk are all worked out here. Each
 * [update] then only projects and depth-sorts - no allocation.
 *
 * Points are numbered by "slot", 0 until [size], one per LED that has a
 * position; [ledIndex] maps a slot to its LED number.
 */
class TreePointCloud(layout: LedLayout) {
    val projection = Projection()
    val ledIndex: IntArray = layout.positioned
    val size = ledIndex.size

    private val x = FloatArray(size) { layout.xMm[ledIndex[it]] }
    private val y = FloatArray(size) { layout.yMm[ledIndex[it]] }
    private val z = FloatArray(size) { layout.zMm[ledIndex[it]] }

    /** Fixed dot colors per slot for the position-source and height views. */
    val sourceArgb = IntArray(size) {
        if (layout.source[ledIndex[it]] == PositionSource.MAPPED) ViewerColors.MAPPED else ViewerColors.SYNTHETIC
    }
    val heightArgb = IntArray(size) { ViewerColors.height(layout.height01(ledIndex[it])) }

    /** Projected screen position (px) and depth (mm) per slot; valid for visible slots only. */
    val screenX = FloatArray(size)
    val screenY = FloatArray(size)
    val depth = FloatArray(size)

    // Visible slots as (depth bits << 32 | slot), sorted near to far. Depths
    // are positive, so their raw float bits sort the same way the floats do.
    private val order = LongArray(size)
    var visibleCount = 0
        private set

    // Floor grid (1.5m each way, 250mm squares, at z = 0) and the trunk, as
    // world segments x0,y0,z0,x1,y1,z1.
    private val lineWorld: FloatArray
    val lineArgb: IntArray
    val lineCount: Int
    /** Projected segments (x0, y0, x1, y1 per line) and whether each is on screen. */
    val lineScreen: FloatArray
    val lineVisible: BooleanArray

    init {
        val world = ArrayList<Float>()
        val colors = ArrayList<Int>()
        val half = GRID_HALF_LINES * GRID_SPACING_MM
        for (i in -GRID_HALF_LINES..GRID_HALF_LINES) {
            val c = i * GRID_SPACING_MM
            world += listOf(c, -half, 0f, c, half, 0f)
            world += listOf(-half, c, 0f, half, c, 0f)
            val color = if (i == 0) ViewerColors.GRID_CENTER else ViewerColors.GRID
            colors += listOf(color, color)
        }
        world += listOf(0f, 0f, layout.minZMm, 0f, 0f, layout.maxZMm)
        colors += ViewerColors.TRUNK
        lineWorld = world.toFloatArray()
        lineArgb = colors.toIntArray()
        lineCount = lineArgb.size
        lineScreen = FloatArray(lineCount * 4)
        lineVisible = BooleanArray(lineCount)
    }

    private val scratch = FloatArray(4)
    private var lastCamera: OrbitCamera? = null
    private var lastWidth = 0f
    private var lastHeight = 0f

    /** Projects everything for [camera] on a [widthPx] x [heightPx] view; skipped if neither changed. */
    fun update(camera: OrbitCamera, widthPx: Float, heightPx: Float) {
        if (camera == lastCamera && widthPx == lastWidth && heightPx == lastHeight) return
        lastCamera = camera
        lastWidth = widthPx
        lastHeight = heightPx
        projection.set(camera, widthPx, heightPx)

        var n = 0
        for (s in 0 until size) {
            if (!projection.project(x[s], y[s], z[s], scratch)) continue
            screenX[s] = scratch[0]
            screenY[s] = scratch[1]
            depth[s] = scratch[2]
            order[n++] = (scratch[2].toRawBits().toLong() shl 32) or s.toLong()
        }
        Arrays.sort(order, 0, n)
        visibleCount = n

        for (l in 0 until lineCount) {
            val w = l * 6
            lineVisible[l] = projection.projectSegment(
                lineWorld[w], lineWorld[w + 1], lineWorld[w + 2],
                lineWorld[w + 3], lineWorld[w + 4], lineWorld[w + 5], scratch,
            )
            if (lineVisible[l]) scratch.copyInto(lineScreen, l * 4)
        }
    }

    /** The [k]th visible slot in drawing order, farthest first (k in 0 until [visibleCount]). */
    fun backToFront(k: Int): Int = (order[visibleCount - 1 - k] and 0xFFFFFFFFL).toInt()

    private companion object {
        const val GRID_HALF_LINES = 6
        const val GRID_SPACING_MM = 250f
    }
}
