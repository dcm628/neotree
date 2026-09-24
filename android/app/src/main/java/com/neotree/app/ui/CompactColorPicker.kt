package com.neotree.app.ui

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.gestures.drag
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
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
 * Small hue ring + preset dots + saturation and brightness bars, sized to fit
 * two side by side on a phone.
 */
@OptIn(ExperimentalLayoutApi::class)
@Composable
fun CompactColorPicker(color: PickerColor, onChange: (PickerColor) -> Unit, modifier: Modifier = Modifier) {
    Column(modifier, verticalArrangement = Arrangement.spacedBy(6.dp)) {
        HueWheel(
            hue = color.hue,
            centerColor = color.display(),
            onHueChange = { onChange(color.copy(hue = it)) },
            ringWidth = 16.dp,
            modifier = Modifier.fillMaxWidth(0.85f).align(Alignment.CenterHorizontally),
        )
        FlowRow(
            Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(5.dp, Alignment.CenterHorizontally),
            verticalArrangement = Arrangement.spacedBy(5.dp),
        ) {
            COLOR_PRESETS.forEach { preset ->
                Box(
                    Modifier.size(20.dp).clip(CircleShape).background(preset)
                        .border(1.dp, MaterialTheme.colorScheme.outline, CircleShape)
                        .clickable { onChange(color.withPreset(preset)) }
                )
            }
        }
        Text("Saturation", style = MaterialTheme.typography.labelSmall)
        GradientBar(
            value = color.saturation, range = 0f..1f,
            gradient = listOf(Color.White, Color.hsv(color.hue, 1f, 1f)),
            onChange = { onChange(color.copy(saturation = it)) },
        )
        Text("Brightness ${(color.brightness * 100).roundToInt()}%", style = MaterialTheme.typography.labelSmall)
        GradientBar(
            value = color.brightness, range = 0.02f..1f,
            gradient = listOf(Color.Black, color.display()),
            onChange = { onChange(color.copy(brightness = it)) },
        )
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
