package com.neotree.app.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
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
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.Checkbox
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.RangeSlider
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Slider
import androidx.compose.material3.SliderDefaults
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
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import com.neotree.app.ColorTarget
import com.neotree.app.TreeViewModel
import com.neotree.app.net.TreeConnection
import kotlin.math.roundToInt

private val PRESETS = listOf(
    Color(0xFFFF0000), Color(0xFFFF6A00), Color(0xFFFFB300), Color(0xFF00FF00),
    Color(0xFF00FFD0), Color(0xFF0040FF), Color(0xFF8000FF), Color(0xFFFF00A0),
    Color(0xFFFFFFFF),
)

// Slider ranges for the region band. Heights cover the mapped tree
// (roughly -120 to 2000mm) with margin.
private val HEIGHT_RANGE = -200f..2100f
private val ANGLE_RANGE = 0f..360f

@Composable
fun MainScreen(vm: TreeViewModel) {
    Scaffold(modifier = Modifier.fillMaxSize()) { padding ->
        Column(
            modifier = Modifier
                .padding(padding)
                .verticalScroll(rememberScrollState())
                .padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(16.dp),
        ) {
            Text("NeoTree", style = MaterialTheme.typography.headlineMedium, fontWeight = FontWeight.Bold)
            ConnectionCard(vm)
            LightsCard(vm)
            ColorCard(vm)
            RegionCard(vm)
            AdvancedCard(vm)
        }
    }
}

@Composable
private fun ConnectionCard(vm: TreeViewModel) {
    val state by vm.connection.state.collectAsState()
    val status by vm.status.collectAsState()
    val manualHost by vm.manualHost.collectAsState()
    var editingHost by rememberSaveable { mutableStateOf(false) }
    var hostText by rememberSaveable(manualHost) { mutableStateOf(manualHost) }

    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            val (dot, text) = when (val s = state) {
                is TreeConnection.State.Connected -> Color(0xFF2E7D32) to "Connected to ${s.host}"
                is TreeConnection.State.Connecting -> Color(0xFFF9A825) to "Connecting to ${s.host}…"
                is TreeConnection.State.Failed -> Color(0xFFC62828) to "${s.host}: ${s.reason}"
                TreeConnection.State.Disconnected -> Color.Gray to "Not connected"
            }
            Row(verticalAlignment = Alignment.CenterVertically) {
                Box(Modifier.size(12.dp).clip(CircleShape).background(dot))
                Spacer(Modifier.width(8.dp))
                Text(text, style = MaterialTheme.typography.bodyLarge)
            }
            if (status.isNotEmpty()) {
                Text(status, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                TextButton(onClick = { vm.reconnectNow() }) { Text("Reconnect") }
                TextButton(onClick = { editingHost = !editingHost }) {
                    Text(if (manualHost.isEmpty()) "Enter address" else "Address: $manualHost")
                }
            }
            if (editingHost) {
                OutlinedTextField(
                    value = hostText,
                    onValueChange = { hostText = it },
                    label = { Text("Tree address (blank = find automatically)") },
                    singleLine = true,
                    modifier = Modifier.fillMaxWidth(),
                )
                Button(onClick = {
                    vm.setManualHost(hostText)
                    editingHost = false
                    vm.reconnectNow()
                }) { Text("Save & connect") }
            }
        }
    }
}

@Composable
private fun ColorCard(vm: TreeViewModel) {
    val hue by vm.hue.collectAsState()
    val saturation by vm.saturation.collectAsState()
    val brightness by vm.brightness.collectAsState()
    val live by vm.live.collectAsState()
    val lastTarget by vm.lastTarget.collectAsState()
    val picked = Color.hsv(hue, saturation, 1f)

    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            Text("Color", style = MaterialTheme.typography.titleMedium)
            HueWheel(
                hue = hue,
                centerColor = picked,
                onHueChange = vm::setHue,
                modifier = Modifier.fillMaxWidth(0.75f).align(Alignment.CenterHorizontally),
            )
            Row(
                Modifier.horizontalScroll(rememberScrollState()),
                horizontalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                PRESETS.forEach { preset ->
                    Box(
                        Modifier.size(40.dp).clip(CircleShape).background(preset)
                            .border(1.dp, MaterialTheme.colorScheme.outline, CircleShape)
                            .clickable { vm.pickPreset(preset) }
                    )
                }
            }
            LabeledGradientSlider(
                label = "Saturation",
                value = saturation, range = 0f..1f, onChange = vm::setSaturation,
                gradient = listOf(Color.White, Color.hsv(hue, 1f, 1f)),
            )
            LabeledGradientSlider(
                label = "Brightness ${(brightness * 100).roundToInt()}%",
                value = brightness, range = 0.02f..1f, onChange = vm::setBrightness,
                gradient = listOf(Color.Black, picked),
            )
            Row(verticalAlignment = Alignment.CenterVertically) {
                Switch(checked = live, onCheckedChange = vm::setLive)
                Spacer(Modifier.width(8.dp))
                Text(
                    if (live && lastTarget != ColorTarget.NONE) "Live: changes update the ${targetName(lastTarget)}"
                    else "Live updates " + if (live) "on" else "off",
                    style = MaterialTheme.typography.bodyMedium,
                )
            }
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Button(onClick = vm::fillTree, modifier = Modifier.weight(1f)) { Text("Fill tree") }
                OutlinedButton(onClick = vm::setBackground, modifier = Modifier.weight(1f)) { Text("Background") }
            }
        }
    }
}

@Composable
private fun LightsCard(vm: TreeViewModel) {
    val lightsOn by vm.lightsOn.collectAsState()
    val state by vm.connection.state.collectAsState()
    val connected = state is TreeConnection.State.Connected
    Card(Modifier.fillMaxWidth()) {
        Row(
            Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 12.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Column(Modifier.weight(1f)) {
                Text("Lights", style = MaterialTheme.typography.titleMedium)
                Text(
                    if (lightsOn) "On" else "Off – colors are kept for when you turn them back on",
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            Switch(checked = lightsOn, onCheckedChange = vm::setLights, enabled = connected)
        }
    }
}

private fun targetName(t: ColorTarget) = when (t) {
    ColorTarget.FILL -> "whole tree"
    ColorTarget.BACKGROUND -> "background"
    ColorTarget.REGION -> "region"
    ColorTarget.NONE -> ""
}

@Composable
private fun LabeledGradientSlider(
    label: String,
    value: Float,
    range: ClosedFloatingPointRange<Float>,
    onChange: (Float) -> Unit,
    gradient: List<Color>,
) {
    Column {
        Text(label, style = MaterialTheme.typography.labelLarge)
        Box(Modifier.fillMaxWidth().height(40.dp), contentAlignment = Alignment.Center) {
            Box(
                Modifier.fillMaxWidth().padding(horizontal = 10.dp).height(12.dp)
                    .clip(RoundedCornerShape(6.dp))
                    .background(Brush.horizontalGradient(gradient))
            )
            Slider(
                value = value,
                onValueChange = onChange,
                valueRange = range,
                colors = SliderDefaults.colors(
                    activeTrackColor = Color.Transparent,
                    inactiveTrackColor = Color.Transparent,
                    activeTickColor = Color.Transparent,
                    inactiveTickColor = Color.Transparent,
                ),
            )
        }
    }
}

@Composable
private fun RegionCard(vm: TreeViewModel) {
    val region by vm.region.collectAsState()
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text("Region", style = MaterialTheme.typography.titleMedium)
            Text(
                "Paints the LEDs inside a height band and angle slice with the color above.",
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
            Button(onClick = vm::paintRegion, modifier = Modifier.fillMaxWidth()) { Text("Paint region") }
        }
    }
}

@Composable
private fun AdvancedCard(vm: TreeViewModel) {
    var expanded by rememberSaveable { mutableStateOf(false) }
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
                    Button(onClick = { index?.let(vm::setSingleLed) }, enabled = index != null) { Text("Set to color") }
                    OutlinedButton(onClick = { index?.let(vm::clearSingleLed) }, enabled = index != null) { Text("Clear") }
                }
            }
        }
    }
}
