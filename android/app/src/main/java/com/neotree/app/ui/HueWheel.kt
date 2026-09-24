package com.neotree.app.ui

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.gestures.drag
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.unit.dp
import kotlin.math.atan2
import kotlin.math.cos
import kotlin.math.min
import kotlin.math.sin

/**
 * Hue ring: touch or drag anywhere on it to pick a hue by angle (0° = red at
 * 3 o'clock, increasing clockwise). The centre shows the picked color.
 */
@Composable
fun HueWheel(
    hue: Float,
    centerColor: Color,
    onHueChange: (Float) -> Unit,
    modifier: Modifier = Modifier,
) {
    val onChange by rememberUpdatedState(onHueChange)
    // Sweep gradients run clockwise from 3 o'clock, matching atan2 in screen
    // coordinates (y down), so hue = angle.
    val sweep = HUE_SWEEP

    Canvas(
        modifier
            .aspectRatio(1f)
            .pointerInput(Unit) {
                fun pick(p: Offset) {
                    val angle = Math.toDegrees(atan2(p.y - size.height / 2.0, p.x - size.width / 2.0)).toFloat()
                    onChange((angle + 360f) % 360f)
                }
                awaitEachGesture {
                    val down = awaitFirstDown()
                    pick(down.position)
                    down.consume()
                    drag(down.id) { change ->
                        pick(change.position)
                        change.consume()
                    }
                }
            }
    ) {
        val ringWidth = 36.dp.toPx()
        val radius = min(size.width, size.height) / 2f - ringWidth / 2f
        drawCircle(brush = Brush.sweepGradient(sweep, center), radius = radius, style = Stroke(ringWidth))
        drawCircle(color = centerColor, radius = radius - ringWidth / 2f - 12.dp.toPx())

        // Thumb on the ring at the current hue.
        val rad = Math.toRadians(hue.toDouble())
        val thumb = Offset(center.x + radius * cos(rad).toFloat(), center.y + radius * sin(rad).toFloat())
        drawCircle(Color.White, radius = ringWidth / 2f + 2.dp.toPx(), center = thumb)
        drawCircle(Color.hsv(hue, 1f, 1f), radius = ringWidth / 2f - 3.dp.toPx(), center = thumb)
        drawCircle(Color.Black.copy(alpha = 0.4f), radius = ringWidth / 2f + 2.dp.toPx(), center = thumb, style = Stroke(1.dp.toPx()))
    }
}

private val HUE_SWEEP = (0..6).map { Color.hsv(it * 60f % 360f, 1f, 1f) }
