package com.neotree.app.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.Checkbox
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.RangeSlider
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import com.neotree.app.PaintTarget
import com.neotree.app.TreeViewModel
import com.neotree.app.net.TreeConnection
import kotlin.math.roundToInt

// Slider ranges for the region band. Heights cover the mapped tree
// (roughly -120 to 2000mm) with margin.
private val HEIGHT_RANGE = -200f..2100f
private val ANGLE_RANGE = 0f..360f

// Compact buttons for the half-width picker cards.
private val SMALL_BUTTON_PADDING = PaddingValues(horizontal = 6.dp, vertical = 0.dp)

/** The home page: lights, color pickers, region, single LED. */
@Composable
fun HomeScreen(vm: TreeViewModel, contentPadding: PaddingValues, onOpenDebug: () -> Unit, onOpenSchedule: () -> Unit) {
    Column(
        modifier = Modifier
            .padding(contentPadding)
            .verticalScroll(rememberScrollState())
            .padding(horizontal = 12.dp, vertical = 8.dp),
        verticalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        Header(vm, onOpenDebug)
        TimerCard(vm, onOpenSchedule)
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            BackgroundCard(vm, Modifier.weight(1f))
            PaintCard(vm, Modifier.weight(1f))
        }
        RegionCard(vm)
        AdvancedCard(vm)
    }
}

/** NeoTree title, compact connection status (tap for the debug page), lights switch. */
@Composable
private fun Header(vm: TreeViewModel, onOpenDebug: () -> Unit) {
    val state by vm.connection.state.collectAsState()
    val lightsOn by vm.lightsOn.collectAsState()

    val (dot, label) = when (state) {
        is TreeConnection.State.Connected -> Color(0xFF2E7D32) to "Connected"
        is TreeConnection.State.Connecting -> Color(0xFFF9A825) to "Connecting"
        is TreeConnection.State.Failed -> Color(0xFFC62828) to "Offline"
        TreeConnection.State.Disconnected -> Color.Gray to "Offline"
    }
    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
        Text("NeoTree", style = MaterialTheme.typography.titleLarge, fontWeight = FontWeight.Bold)
        Spacer(Modifier.width(10.dp))
        Row(
            Modifier
                .clip(RoundedCornerShape(12.dp))
                .clickable(onClick = onOpenDebug)
                .background(MaterialTheme.colorScheme.surfaceVariant)
                .padding(horizontal = 8.dp, vertical = 4.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Box(Modifier.size(8.dp).clip(CircleShape).background(dot))
            Spacer(Modifier.width(5.dp))
            Text(label, style = MaterialTheme.typography.labelMedium)
        }
        Spacer(Modifier.weight(1f))
        Text("Lights", style = MaterialTheme.typography.labelLarge)
        Spacer(Modifier.width(6.dp))
        Switch(
            checked = lightsOn,
            onCheckedChange = vm::setLights,
            enabled = state is TreeConnection.State.Connected,
        )
    }
}

@Composable
private fun PickerCardTitle(title: String) {
    Text(title, style = MaterialTheme.typography.titleSmall, fontWeight = FontWeight.Bold, modifier = Modifier.padding(start = 5.dp))
}

/** Always the base color - shown wherever nothing is painted. Applies as you move it. */
@Composable
private fun BackgroundCard(vm: TreeViewModel, modifier: Modifier) {
    val color by vm.background.collectAsState()
    val lightsOn by vm.lightsOn.collectAsState()
    Card(modifier) {
        Column(Modifier.padding(horizontal = 5.dp, vertical = 8.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
            PickerCardTitle("Background")
            CompactColorPicker(color, vm::setBackgroundColor)
            Button(
                onClick = vm::applyBackground,
                enabled = lightsOn,
                contentPadding = SMALL_BUTTON_PADDING,
                modifier = Modifier.fillMaxWidth().height(34.dp),
            ) { Text("Apply", style = MaterialTheme.typography.labelMedium) }
        }
    }
}

/** The overlay color, on the whole tree or the region. Re-applies as you move it. */
@Composable
private fun PaintCard(vm: TreeViewModel, modifier: Modifier) {
    val color by vm.paint.collectAsState()
    val target by vm.paintTarget.collectAsState()
    val lightsOn by vm.lightsOn.collectAsState()
    Card(modifier) {
        Column(Modifier.padding(horizontal = 5.dp, vertical = 8.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
            PickerCardTitle("Paint")
            CompactColorPicker(color, vm::setPaintColor)
            Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                TargetButton("Fill", target == PaintTarget.FILL, lightsOn, vm::paintFill, Modifier.weight(1f))
                TargetButton("Region", target == PaintTarget.REGION, lightsOn, vm::paintRegion, Modifier.weight(1f))
            }
        }
    }
}

/** Filled when it's the current paint target (what the picker re-applies to), outlined otherwise. */
@Composable
private fun TargetButton(label: String, active: Boolean, enabled: Boolean, onClick: () -> Unit, modifier: Modifier) {
    val m = modifier.height(34.dp)
    val text: @Composable () -> Unit = { Text(label, style = MaterialTheme.typography.labelMedium) }
    if (active) {
        Button(onClick = onClick, enabled = enabled, contentPadding = SMALL_BUTTON_PADDING, modifier = m) { text() }
    } else {
        OutlinedButton(onClick = onClick, enabled = enabled, contentPadding = SMALL_BUTTON_PADDING, modifier = m) { text() }
    }
}

@Composable
private fun RegionCard(vm: TreeViewModel) {
    val region by vm.region.collectAsState()
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text("Region", style = MaterialTheme.typography.titleMedium)
            Text(
                "The height band and angle slice that Paint → Region colors.",
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            Text(
                "Height ${region.heightMinMm.roundToInt()} – ${region.heightMaxMm.roundToInt()} mm",
                style = MaterialTheme.typography.labelLarge,
            )
            RangeSlider(
                value = region.heightMinMm..region.heightMaxMm,
                onValueChange = { vm.setRegion(region.copy(heightMinMm = it.start, heightMaxMm = it.endInclusive)) },
                valueRange = HEIGHT_RANGE,
            )
            Text(
                "Angle ${region.angleMinDeg.roundToInt()}° – ${region.angleMaxDeg.roundToInt()}°",
                style = MaterialTheme.typography.labelLarge,
            )
            RangeSlider(
                value = region.angleMinDeg..region.angleMaxDeg,
                onValueChange = { vm.setRegion(region.copy(angleMinDeg = it.start, angleMaxDeg = it.endInclusive)) },
                valueRange = ANGLE_RANGE,
            )
            Row(verticalAlignment = Alignment.CenterVertically) {
                Checkbox(
                    checked = region.clearOutside,
                    onCheckedChange = { vm.setRegion(region.copy(clearOutside = it)) },
                )
                Text("Everything outside shows the background color", style = MaterialTheme.typography.bodyMedium)
            }
        }
    }
}

@Composable
private fun AdvancedCard(vm: TreeViewModel) {
    var expanded by rememberSaveable { mutableStateOf(false) }
    val lightsOn by vm.lightsOn.collectAsState()
    var indexText by remember { mutableStateOf("0") }
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            TextButton(onClick = { expanded = !expanded }) {
                Text(if (expanded) "Hide advanced" else "Advanced: single LED")
            }
            if (expanded) {
                OutlinedTextField(
                    value = indexText,
                    onValueChange = { indexText = it.filter(Char::isDigit).take(4) },
                    label = { Text("LED number (0–999)") },
                    singleLine = true,
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                    modifier = Modifier.fillMaxWidth(),
                )
                val index = indexText.toIntOrNull()?.takeIf { it in 0..999 }
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Button(onClick = { index?.let(vm::setSingleLed) }, enabled = index != null && lightsOn) {
                        Text("Set to paint color")
                    }
                    OutlinedButton(onClick = { index?.let(vm::clearSingleLed) }, enabled = index != null && lightsOn) {
                        Text("Clear")
                    }
                }
            }
        }
    }
}
