package com.neotree.app.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Card
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.FilterChip
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Slider
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import com.neotree.app.PickerColor
import com.neotree.app.TreeViewModel
import com.neotree.app.net.ModeCatalog
import com.neotree.app.net.ModeInfo
import com.neotree.app.net.ParamInfo
import com.neotree.app.net.ParamType
import com.neotree.app.net.ParamValue
import com.neotree.app.net.Rgb
import com.neotree.app.net.SlotState
import com.neotree.app.net.TreeConnection
import kotlin.math.roundToInt

/**
 * Modes page: preset scenes, then the scene's slots (bottom to top), each
 * with a mode picker and controls built from the mode's parameters as the
 * tree describes them - nothing about any mode is hard-coded here.
 */
@Composable
fun ModesScreen(vm: TreeViewModel, contentPadding: PaddingValues) {
    val state by vm.connection.state.collectAsState()
    val catalog by vm.catalog.collectAsState()
    val scene by vm.scene.collectAsState()
    val status by vm.status.collectAsState()
    // Read so the page recomposes when this phone edits a parameter.
    vm.paramEdits.collectAsState().value
    val connected = state is TreeConnection.State.Connected

    Column(
        modifier = Modifier
            .padding(contentPadding)
            .verticalScroll(rememberScrollState())
            .padding(horizontal = 12.dp, vertical = 8.dp),
        verticalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        Text("Modes", style = MaterialTheme.typography.titleLarge, fontWeight = FontWeight.Bold)
        if (status.isNotEmpty()) {
            Text(status, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
        val c = catalog
        when {
            !connected -> Hint("Not connected to the tree.")
            c == null -> Hint("Asking the tree for its modes…")
            else -> {
                PresetsCard(vm, c, scene?.name)
                val slots = scene?.slots
                if (slots == null) {
                    Hint("Waiting for the tree's scene…")
                } else {
                    slots.forEachIndexed { i, slot -> SlotCard(vm, c, i, slots.size, slot) }
                }
            }
        }
    }
}

@Composable
private fun Hint(text: String) {
    Text(text, color = MaterialTheme.colorScheme.onSurfaceVariant)
}

@Composable
private fun ModesCard(content: @Composable () -> Unit) {
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) { content() }
    }
}

@Composable
private fun PresetsCard(vm: TreeViewModel, catalog: ModeCatalog, current: String?) {
    ModesCard {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text("Scenes", style = MaterialTheme.typography.titleSmall, fontWeight = FontWeight.Bold, modifier = Modifier.weight(1f))
            TextButton(onClick = { vm.revertScene() }) { Text("Back to base") }
        }
        FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            catalog.presets.forEachIndexed { i, name ->
                FilterChip(selected = name == current, onClick = { vm.applyPreset(i) }, label = { Text(name) })
            }
        }
        if (!current.isNullOrEmpty() && current !in catalog.presets) {
            Text("Now: $current", style = MaterialTheme.typography.bodySmall)
        }
    }
}

@Composable
private fun SlotCard(vm: TreeViewModel, catalog: ModeCatalog, index: Int, count: Int, slot: SlotState) {
    val mode = if (slot.empty) null else catalog.modes.getOrNull(slot.modeIndex)
    ModesCard {
        val where = when (index) {
            0 -> " · bottom"
            count - 1 -> " · top"
            else -> ""
        }
        Row(verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f)) {
                Text("Slot ${index + 1}$where", style = MaterialTheme.typography.titleSmall, fontWeight = FontWeight.Bold)
                Text(slotSummary(slot), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            ModePicker(catalog, mode) { vm.setSlotMode(index, it) }
        }
        if (mode != null) {
            if (mode.summary.isNotEmpty()) Text(mode.summary, style = MaterialTheme.typography.bodySmall)
            mode.params.forEach { p ->
                val key = TreeViewModel.ParamKey(index, mode.index, p.index)
                val value = vm.paramValue(key, slot.params.getOrNull(p.index), p.default)
                ParamControl(p, value) { vm.setParam(key, it) }
            }
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedButton(onClick = { vm.endSlot(index) }) { Text("End") }
                OutlinedButton(onClick = { vm.setSlotMode(index, -1) }) { Text("Clear") }
            }
        }
    }
}

private fun slotSummary(slot: SlotState): String {
    if (slot.empty) return "Empty"
    val parts = mutableListOf("${slot.state} ${formatAge(slot.ageSec)}")
    if (slot.durationSec > 0) parts += "of ${formatAge(slot.durationSec)}"
    if (slot.cycles > 0) parts += "${slot.cycles} cycles"
    if (slot.loops > 0) parts += "loop ${slot.loops + 1}"
    if (slot.policy.isNotEmpty() && slot.policy != "hold") parts += "then ${slot.policy}"
    return parts.joinToString(" · ")
}

private fun formatAge(s: Long) = if (s < 60) "${s}s" else "${s / 60}m ${s % 60}s"

@Composable
private fun ModePicker(catalog: ModeCatalog, current: ModeInfo?, onPick: (Int) -> Unit) {
    var open by remember { mutableStateOf(false) }
    Box {
        OutlinedButton(onClick = { open = true }) { Text(current?.name ?: "Choose…") }
        DropdownMenu(expanded = open, onDismissRequest = { open = false }) {
            DropdownMenuItem(text = { Text("Empty") }, onClick = { open = false; onPick(-1) })
            catalog.modes.forEach { m ->
                DropdownMenuItem(text = { Text(m.name) }, onClick = { open = false; onPick(m.index) })
            }
        }
    }
}

@Composable
private fun ParamControl(p: ParamInfo, value: ParamValue, onChange: (ParamValue) -> Unit) {
    when (p.type) {
        ParamType.NUMBER -> NumberParam(p, value.number) { onChange(ParamValue(number = it)) }
        ParamType.TOGGLE -> Row(verticalAlignment = Alignment.CenterVertically) {
            Text(p.label, style = MaterialTheme.typography.bodyMedium, modifier = Modifier.weight(1f))
            Switch(checked = value.number >= 0.5f, onCheckedChange = { onChange(ParamValue(number = if (it) 1f else 0f)) })
        }
        ParamType.CHOICE -> Column {
            Text(p.label, style = MaterialTheme.typography.bodyMedium)
            FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                p.choices.forEachIndexed { i, name ->
                    FilterChip(
                        selected = value.number.roundToInt() == i,
                        onClick = { onChange(ParamValue(number = i.toFloat())) },
                        label = { Text(name) },
                    )
                }
            }
        }
        ParamType.COLOR -> ColorParam(p, value.color) { onChange(ParamValue(color = it)) }
    }
}

@Composable
private fun NumberParam(p: ParamInfo, value: Float, onChange: (Float) -> Unit) {
    val steps = if (p.step > 0f) ((p.max - p.min) / p.step).roundToInt() - 1 else 0
    Column {
        Row {
            Text(p.label, style = MaterialTheme.typography.bodyMedium, modifier = Modifier.weight(1f))
            Text(formatNumber(value, p.step), style = MaterialTheme.typography.bodyMedium)
        }
        Slider(
            value = value.coerceIn(p.min, p.max),
            onValueChange = { v ->
                val snapped = if (p.step > 0f) p.min + ((v - p.min) / p.step).roundToInt() * p.step else v
                onChange(snapped.coerceIn(p.min, p.max))
            },
            valueRange = p.min..p.max,
            steps = steps.coerceIn(0, 200),
        )
    }
}

private fun formatNumber(v: Float, step: Float): String = when {
    step >= 1f -> v.roundToInt().toString()
    step >= 0.1f -> "%.1f".format(v)
    else -> "%.2f".format(v)
}

@Composable
private fun ColorParam(p: ParamInfo, color: Rgb, onChange: (Rgb) -> Unit) {
    var open by remember { mutableStateOf(false) }
    // The picker works in hue/saturation/brightness; keep its own state while
    // open so a dark color doesn't lose its hue on the round trip through RGB.
    var picker by remember { mutableStateOf(color.toPicker()) }
    Column {
        Row(
            Modifier
                .fillMaxWidth()
                .clickable {
                    if (!open) picker = color.toPicker()
                    open = !open
                },
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text(p.label, style = MaterialTheme.typography.bodyMedium, modifier = Modifier.weight(1f))
            Box(
                Modifier
                    .size(width = 44.dp, height = 24.dp)
                    .background(Color(color.r, color.g, color.b), RoundedCornerShape(6.dp))
                    .border(1.dp, MaterialTheme.colorScheme.outline, RoundedCornerShape(6.dp)),
            )
        }
        if (open) {
            CompactColorPicker(
                color = picker,
                onChange = {
                    picker = it
                    onChange(it.toRgb())
                },
                modifier = Modifier
                    .padding(top = 6.dp)
                    .width(240.dp)
                    .align(Alignment.CenterHorizontally),
            )
        }
    }
}

private fun Rgb.toPicker(): PickerColor {
    val hsv = FloatArray(3)
    android.graphics.Color.RGBToHSV(r, g, b, hsv)
    return PickerColor(hsv[0], hsv[1], hsv[2].coerceAtLeast(0.02f))
}
