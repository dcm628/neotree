package com.neotree.app.ui

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.gestures.drag
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.unit.Density
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import kotlin.math.abs
import kotlin.math.atan2
import kotlin.math.cos
import kotlin.math.hypot
import kotlin.math.min
import kotlin.math.sin

/**
 * Hue ring with preset colors built in.
 *
 * - Touch/drag the outer ring to pick a hue by angle (0° = red at 3 o'clock,
 *   increasing clockwise, matching the sweep gradient).
 * - Each saturated preset is a short arc segment just inside the ring, at its
 *   own hue angle - tap it to pick exactly that color.
 * - Unsaturated presets (white) sit as a small dot in the very centre.
 * - The disk between shows the currently picked color.
 */
@Composable
fun HueWheel(
    hue: Float,
    centerColor: Color,
    onHueChange: (Float) -> Unit,
    modifier: Modifier = Modifier,
    ringWidth: Dp = 36.dp,
    presets: List<Color> = emptyList(),
    onPreset: (Color) -> Unit = {},
) {
    val onHue by rememberUpdatedState(onHueChange)
    val onPresetLatest by rememberUpdatedState(onPreset)
    val tabs = remember(presets) { presets.map { it to hsvOf(it) }.filter { it.second[1] >= 0.1f } }
    val centerPreset = remember(presets) { presets.firstOrNull { hsvOf(it)[1] < 0.1f } }

    Canvas(
        modifier
            .aspectRatio(1f)
            .pointerInput(tabs, centerPreset, ringWidth) {
                awaitEachGesture {
                    val down = awaitFirstDown()
                    val g = WheelGeometry(Size(size.width.toFloat(), size.height.toFloat()), ringWidth, this)
                    val dx = down.position.x - g.center.x
                    val dy = down.position.y - g.center.y
                    val dist = hypot(dx, dy)
                    val angle = angleOf(dx, dy)
                    when {
                        dist >= g.ringInner - g.slop -> {
                            // Ring: drag to set hue.
                            onHue(angle)
                            down.consume()
                            drag(down.id) { change ->
                                onHue(angleOf(change.position.x - g.center.x, change.position.y - g.center.y))
                                change.consume()
                            }
                        }
                        dist >= g.tabInner - g.slop -> {
                            // Tab track: nearest tab within reach.
                            tabs.minByOrNull { angleDistance(it.second[0], angle) }
                                ?.takeIf { angleDistance(it.second[0], angle) <= TAB_HIT_DEG }
                                ?.let { onPresetLatest(it.first); down.consume() }
                        }
                        centerPreset != null && dist <= g.centerDot + g.slop -> {
                            onPresetLatest(centerPreset)
                            down.consume()
                        }
                    }
                }
            }
    ) {
        val g = WheelGeometry(size, ringWidth, this)

        // Hue ring.
        drawCircle(brush = Brush.sweepGradient(HUE_SWEEP, center), radius = g.ringMid, style = Stroke(g.ringPx))

        // Preset tabs: arc segments on the inner track, with a thin outline.
        val outlinePx = 1.dp.toPx()
        tabs.forEach { (color, hsv) ->
            drawTab(hsv[0], TAB_SPAN_DEG + 1.5f, g, Color.Black.copy(alpha = 0.3f), Stroke(g.tabThickness + 2 * outlinePx))
            drawTab(hsv[0], TAB_SPAN_DEG, g, color, Stroke(g.tabThickness))
        }

        // Picked color, then the white preset dot in the very centre.
        drawCircle(centerColor, radius = g.disk)
        if (centerPreset != null) {
            drawCircle(centerPreset, radius = g.centerDot)
            drawCircle(Color.Black.copy(alpha = 0.35f), radius = g.centerDot, style = Stroke(outlinePx))
        }

        // Thumb on the ring at the current hue.
        val rad = Math.toRadians(hue.toDouble())
        val thumb = Offset(center.x + g.ringMid * cos(rad).toFloat(), center.y + g.ringMid * sin(rad).toFloat())
        drawCircle(Color.White, radius = g.ringPx / 2f + 2.dp.toPx(), center = thumb)
        drawCircle(Color.hsv(hue, 1f, 1f), radius = g.ringPx / 2f - 3.dp.toPx(), center = thumb)
        drawCircle(Color.Black.copy(alpha = 0.4f), radius = g.ringPx / 2f + 2.dp.toPx(), center = thumb, style = Stroke(outlinePx))
    }
}

/** Radii for the wheel's layers, outermost first. Shared by drawing and hit-testing. */
private class WheelGeometry(size: Size, ringWidth: Dp, density: Density) {
    val center = Offset(size.width / 2f, size.height / 2f)
    val ringPx = with(density) { ringWidth.toPx() }
    val outer = min(size.width, size.height) / 2f
    val ringMid = outer - ringPx / 2f
    val ringInner = outer - ringPx
    val tabThickness = with(density) { 12.dp.toPx() }
    val tabMid = ringInner - with(density) { 4.dp.toPx() } - tabThickness / 2f
    val tabInner = tabMid - tabThickness / 2f
    val disk = tabInner - with(density) { 6.dp.toPx() }
    val centerDot = with(density) { 11.dp.toPx() }
    val slop = with(density) { 4.dp.toPx() }
}

private fun DrawScope.drawTab(hueDeg: Float, spanDeg: Float, g: WheelGeometry, color: Color, style: Stroke) {
    drawArc(
        color = color,
        startAngle = hueDeg - spanDeg / 2f,
        sweepAngle = spanDeg,
        useCenter = false,
        topLeft = Offset(g.center.x - g.tabMid, g.center.y - g.tabMid),
        size = Size(g.tabMid * 2f, g.tabMid * 2f),
        style = style,
    )
}

private fun angleOf(dx: Float, dy: Float): Float =
    (Math.toDegrees(atan2(dy.toDouble(), dx.toDouble())).toFloat() + 360f) % 360f

private fun angleDistance(a: Float, b: Float): Float {
    val d = abs(a - b) % 360f
    return if (d > 180f) 360f - d else d
}

private fun hsvOf(c: Color): FloatArray {
    val hsv = FloatArray(3)
    android.graphics.Color.colorToHSV(
        android.graphics.Color.rgb((c.red * 255).toInt(), (c.green * 255).toInt(), (c.blue * 255).toInt()),
        hsv,
    )
    return hsv
}

// Sweep gradients run clockwise from 3 o'clock, matching atan2 in screen
// coordinates (y down), so hue = angle.
private val HUE_SWEEP = (0..6).map { Color.hsv(it * 60f % 360f, 1f, 1f) }

// Tab arc length in degrees. The closest presets are orange (25°) and amber
// (42°), 17° apart, so 12° leaves a visible gap between them. Taps pick the
// nearest tab within TAB_HIT_DEG of the touch angle.
private const val TAB_SPAN_DEG = 12f
private const val TAB_HIT_DEG = 12f
