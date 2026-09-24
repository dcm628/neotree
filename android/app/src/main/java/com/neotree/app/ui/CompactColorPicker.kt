package com.neotree.app.ui

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.gestures.drag
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.offset
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.unit.dp
import com.neotree.app.PickerColor
import kotlin.math.roundToInt

val COLOR_PRESETS = listOf(
    Color(0xFFFF0000), Color(0xFFFF6A00), Color(0xFFFFB300), Color(0xFF00FF00), Color(0xFF00FFD0),
    Color(0xFF0040FF), Color(0xFF8000FF), Color(0xFFFF00A0), Color(0xFFFFFFFF),
)

/**
 * Hue ring with built-in presets + saturation and brightness bars, sized to fit
 * two side by side on a phone.
 */
@Composable
fun CompactColorPicker(color: PickerColor, onChange: (PickerColor) -> Unit, modifier: Modifier = Modifier) {
    Column(modifier, verticalArrangement = Arrangement.spacedBy(6.dp)) {
        HueWheel(
            hue = color.hue,
            centerColor = color.display(),
            onHueChange = { onChange(color.copy(hue = it)) },
            ringWidth = 16.dp,
            presets = COLOR_PRESETS,
            onPreset = { onChange(color.withPreset(it)) },
            modifier = Modifier.fillMaxWidth(),
        )
        LabeledBar("Saturation") {
            GradientBar(
                value = color.saturation, range = 0f..1f,
                gradient = listOf(Color.White, Color.hsv(color.hue, 1f, 1f)),
                onChange = { onChange(color.copy(saturation = it)) },
            )
        }
        LabeledBar("Brightness ${(color.brightness * 100).roundToInt()}%") {
            GradientBar(
                value = color.brightness, range = 0.02f..1f,
                gradient = listOf(Color.Black, color.display()),
                onChange = { onChange(color.copy(brightness = it)) },
            )
        }
    }
}

/** Label tucked right against its bar (the bar's touch area has built-in top padding). */
@Composable
private fun LabeledBar(label: String, bar: @Composable () -> Unit) {
    Column {
        Text(label, style = MaterialTheme.typography.labelSmall, modifier = Modifier.offset(y = 2.dp))
        bar()
    }
}

/**
 * A slim slider: gradient track + round thumb, 24dp tall. Material's Slider
 * enforces a much taller touch area than fits in half a phone width.
 */
@Composable
fun GradientBar(
    value: Float,
    range: ClosedFloatingPointRange<Float>,
    gradient: List<Color>,
    onChange: (Float) -> Unit,
    modifier: Modifier = Modifier,
) {
    val onChangeLatest by rememberUpdatedState(onChange)
    Canvas(
        modifier
            .fillMaxWidth()
            .height(24.dp)
            .pointerInput(range) {
                fun set(x: Float) {
                    val r = size.height / 2f
                    val frac = ((x - r) / (size.width - 2 * r)).coerceIn(0f, 1f)
                    onChangeLatest(range.start + frac * (range.endInclusive - range.start))
                }
                awaitEachGesture {
                    val down = awaitFirstDown()
                    set(down.position.x)
                    down.consume()
                    drag(down.id) { change ->
                        set(change.position.x)
                        change.consume()
                    }
                }
            }
    ) {
        val r = size.height / 2f
        val trackH = size.height * 0.5f
        drawRoundRect(
            brush = Brush.horizontalGradient(gradient, startX = r, endX = size.width - r),
            topLeft = Offset(0f, (size.height - trackH) / 2f),
            size = Size(size.width, trackH),
            cornerRadius = CornerRadius(trackH / 2f),
        )
        val frac = ((value - range.start) / (range.endInclusive - range.start)).coerceIn(0f, 1f)
        val thumb = Offset(r + frac * (size.width - 2 * r), size.height / 2f)
        drawCircle(Color.White, radius = r - 1.dp.toPx(), center = thumb)
        drawCircle(Color.Black.copy(alpha = 0.4f), radius = r - 1.dp.toPx(), center = thumb, style = Stroke(1.dp.toPx()))
    }
}
