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
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.RadioButton
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.produceState
import androidx.compose.ui.text.input.KeyboardType
import com.neotree.app.net.TreeProtocol
import kotlinx.coroutines.delay

/**
 * Modes page: shows, scenes (presets, saving what's running, the base
 * scene), then the scene's slots (bottom to top), each with a mode picker,
 * controls built from the mode's parameters as the tree describes them, and
 * what happens when it ends. The tree pushes every change, so nothing here
 * polls; ages and time left count on locally between pushes.
 */
@Composable
fun ModesScreen(vm: TreeViewModel, contentPadding: PaddingValues) {
    val state by vm.connection.state.collectAsState()
    val catalog by vm.catalog.collectAsState()
    val live by vm.scene.collectAsState()
    val library by vm.library.collectAsState()
    val status by vm.status.collectAsState()
    // Read so the page recomposes when this phone edits a parameter.
    vm.paramEdits.collectAsState().value
    val connected = state is TreeConnection.State.Connected
    val now by produceState(System.currentTimeMillis()) {
        while (true) {
            delay(1000)
            value = System.currentTimeMillis()
        }
    }
    val elapsedSec = live?.let { ((now - it.atMs) / 1000).coerceAtLeast(0) } ?: 0L

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
        val lib = library
        val scene = live?.scene
        when {
            !connected -> Hint("Not connected to the tree.")
            c == null || lib == null -> Hint("Asking the tree for its modes…")
            scene == null -> Hint("Waiting for the tree's scene…")
            else -> {
                ShowsCard(vm, lib, scene.show, elapsedSec)
                ScenesCard(vm, c, lib, scene)
                scene.slots.forEachIndexed { i, slot -> SlotCard(vm, c, i, scene.slots.size, slot, elapsedSec) }
            }
        }
    }
}

@Composable
internal fun Hint(text: String) {
    Text(text, color = MaterialTheme.colorScheme.onSurfaceVariant)
}

@Composable
internal fun ModesCard(content: @Composable () -> Unit) {
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) { content() }
    }
}

@Composable
private fun SlotCard(vm: TreeViewModel, catalog: ModeCatalog, index: Int, count: Int, slot: SlotState, elapsedSec: Long) {
    val mode = if (slot.empty) null else catalog.modes.getOrNull(slot.modeIndex)
    var editLife by remember { mutableStateOf(false) }
    ModesCard {
        val where = when (index) {
            0 -> " · bottom"
            count - 1 -> " · top"
            else -> ""
        }
        Row(verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f)) {
                Text("Slot ${index + 1}$where", style = MaterialTheme.typography.titleSmall, fontWeight = FontWeight.Bold)
                Text(slotSummary(slot, elapsedSec), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
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
            FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedButton(onClick = { editLife = true }) { Text("When it ends…") }
                OutlinedButton(onClick = { vm.endSlot(index) }) { Text("End now") }
                OutlinedButton(onClick = { vm.setSlotMode(index, -1) }) { Text("Clear") }
            }
        }
    }
    if (editLife && mode != null) {
        LifecycleDialog(
            catalog = catalog,
            slot = slot,
            onDismiss = { editLife = false },
            onSave = { duration, cycles, policy, next ->
                editLife = false
                vm.setLifecycle(index, duration, cycles, policy, next)
            },
        )
    }
}

private fun slotSummary(slot: SlotState, elapsedSec: Long): String {
    if (slot.empty) return "Empty"
    // Ages count on between pushes while the mode runs.
    val moving = slot.state == "running" || slot.state == "entering"
    val age = slot.ageSec + if (moving) elapsedSec else 0
    val parts = mutableListOf("${slot.state} ${formatDuration(age)}")
    if (slot.durationSec > 0) parts += "of ${formatDuration(slot.durationSec)}"
    if (slot.cycleLimit > 0) parts += "${slot.cycles} of ${slot.cycleLimit} cycles"
    else if (slot.cycles > 0) parts += "${slot.cycles} cycles"
    if (slot.loops > 0) parts += "loop ${slot.loops + 1}"
    if (slot.policy.isNotEmpty() && slot.policy != "hold") parts += "then ${slot.policy}"
    return parts.joinToString(" · ")
}

internal fun formatDuration(s: Long) = when {
    s < 60 -> "${s}s"
    s % 60 == 0L -> "${s / 60} min"
    else -> "${s / 60}m ${s % 60}s"
}

/** What a slot does when its mode ends: after a time and/or a number of cycles, a policy. */
@Composable
private fun LifecycleDialog(
    catalog: ModeCatalog,
    slot: SlotState,
    onDismiss: () -> Unit,
    onSave: (durationSec: Int, cycles: Int, policy: TreeProtocol.EndPolicy, nextMode: Int) -> Unit,
) {
    var minutes by remember { mutableStateOf(if (slot.durationSec > 0) formatMinutes(slot.durationSec) else "") }
    var cycles by remember { mutableStateOf(if (slot.cycleLimit > 0) slot.cycleLimit.toString() else "") }
    var policy by remember { mutableStateOf(TreeProtocol.EndPolicy.from(slot.policy)) }
    var next by remember { mutableIntStateOf(-1) }
    val durationSec = ((minutes.replace(',', '.').toFloatOrNull() ?: 0f) * 60).roundToInt().coerceIn(0, 0xFFFF)
    val cycleCount = cycles.toIntOrNull()?.coerceIn(0, 0xFFFF) ?: 0
    val never = durationSec == 0 && cycleCount == 0
    val chainOk = policy != TreeProtocol.EndPolicy.CHAIN || next >= 0
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("When it ends") },
        text = {
            Column(Modifier.verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("Ends after (either one; leave both empty to run until changed):", style = MaterialTheme.typography.bodySmall)
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    OutlinedTextField(
                        value = minutes, onValueChange = { minutes = it }, label = { Text("Minutes") }, singleLine = true,
                        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Decimal), modifier = Modifier.weight(1f),
                    )
                    OutlinedTextField(
                        value = cycles, onValueChange = { cycles = it.filter(Char::isDigit) }, label = { Text("Cycles") },
                        singleLine = true, keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                        modifier = Modifier.weight(1f),
                    )
                }
                Text("Then:", style = MaterialTheme.typography.bodySmall)
                TreeProtocol.EndPolicy.entries.forEach { p ->
                    Row(Modifier.fillMaxWidth().clickable { policy = p }, verticalAlignment = Alignment.CenterVertically) {
                        RadioButton(selected = policy == p, onClick = { policy = p })
                        Text(p.label)
                    }
                }
                if (policy == TreeProtocol.EndPolicy.CHAIN) {
                    ModePicker(catalog, catalog.modes.getOrNull(next)) { next = it }
                }
                if (never && policy != TreeProtocol.EndPolicy.HOLD) {
                    Text("Set a time or a number of cycles for this to happen.", style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.error)
                }
            }
        },
        confirmButton = {
            TextButton(onClick = { onSave(durationSec, cycleCount, policy, next) }, enabled = chainOk) { Text("Save") }
        },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
    )
}

private fun formatMinutes(sec: Long): String =
    if (sec % 60 == 0L) (sec / 60).toString() else "%.1f".format(sec / 60f)

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
internal fun ParamControl(p: ParamInfo, value: ParamValue, onChange: (ParamValue) -> Unit) {
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
