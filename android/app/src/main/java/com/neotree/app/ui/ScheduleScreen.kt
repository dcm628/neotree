package com.neotree.app.ui

import androidx.activity.compose.BackHandler
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.DatePicker
import androidx.compose.material3.DatePickerDialog
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilterChip
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TimePicker
import androidx.compose.material3.rememberDatePickerState
import androidx.compose.material3.rememberTimePickerState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import com.neotree.app.TreeViewModel
import com.neotree.app.net.EVERY_DAY
import com.neotree.app.net.EventAction
import com.neotree.app.net.EventInfo
import com.neotree.app.net.ModeCatalog
import com.neotree.app.net.Repeat
import com.neotree.app.net.TimerInfo
import com.neotree.app.net.TimerMath
import com.neotree.app.net.TreeConnection
import com.neotree.app.net.TreeLibrary
import com.neotree.app.net.TreeSchedule
import com.neotree.app.net.formatDays
import com.neotree.app.net.formatTimeOfDay
import com.neotree.app.net.formatWhen
import kotlinx.coroutines.delay
import java.time.Instant
import java.time.LocalDate
import java.time.LocalDateTime
import java.time.ZoneOffset
import kotlin.math.roundToLong

/** The Home page's timer: what the lights do next, a running event, and the way to the schedule. */
@Composable
fun TimerCard(vm: TreeViewModel, onOpen: () -> Unit) {
    val schedule by vm.schedule.collectAsState()
    val live by vm.scene.collectAsState()
    val lightsOn by vm.lightsOn.collectAsState()
    val now by produceState(LocalDateTime.now()) {
        while (true) {
            delay(1000)
            value = LocalDateTime.now()
        }
    }
    LaunchedEffect(Unit) { vm.loadSchedule() }
    val scene = live?.scene
    ModesCard {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f)) {
                Text("Timer", style = MaterialTheme.typography.titleSmall, fontWeight = FontWeight.Bold)
                Text(timerSummary(schedule, scene?.timer ?: -1, lightsOn, now), style = MaterialTheme.typography.bodySmall)
            }
            OutlinedButton(onClick = onOpen) { Text("Schedule…") }
        }
        scene?.event?.let { e ->
            val elapsed = ((System.currentTimeMillis() - live!!.atMs) / 1000).coerceAtLeast(0)
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text("Now: ${e.name} · ${formatDuration((e.leftSec - elapsed).coerceAtLeast(0))} left",
                    style = MaterialTheme.typography.bodyMedium, modifier = Modifier.weight(1f))
                TextButton(onClick = vm::stopEvent) { Text("Stop") }
            }
        }
    }
}

private fun timerSummary(schedule: TreeSchedule?, treeTimer: Int, lightsOn: Boolean, now: LocalDateTime): String {
    if (treeTimer == -2) return "The tree is getting the time…"
    val timers = schedule?.timers ?: return "…"
    val level = TimerMath.level(timers, now) ?: return "No timer: the lights stay as you set them."
    val next = TimerMath.next(timers, now)
    val byHand = if (level != lightsOn) " (${if (lightsOn) "on" else "off"} by hand)" else ""
    val until = next?.let {
        val day = when (it.at.toLocalDate()) {
            now.toLocalDate() -> ""
            now.toLocalDate().plusDays(1) -> " tomorrow"
            else -> " " + it.at.dayOfWeek.name.lowercase().replaceFirstChar { c -> c.uppercase() }
        }
        " until ${formatTimeOfDay(it.at.toLocalTime().toSecondOfDay())}$day"
    } ?: ""
    return "Timer says ${if (level) "on" else "off"}$until$byHand."
}

/** The schedule: lights on/off times by day, and events. */
@Composable
fun ScheduleScreen(vm: TreeViewModel, contentPadding: PaddingValues, onClose: () -> Unit) {
    val state by vm.connection.state.collectAsState()
    val schedule by vm.schedule.collectAsState()
    val library by vm.library.collectAsState()
    val catalog by vm.catalog.collectAsState()
    val status by vm.status.collectAsState()
    val live by vm.scene.collectAsState()
    var editingTimer by remember { mutableStateOf<Pair<Int, TimerInfo>?>(null) }
    var editingEvent by remember { mutableStateOf<Pair<Int, EventInfo>?>(null) }
    var deleting by remember { mutableStateOf<Pair<String, Int>?>(null) }
    BackHandler(onBack = onClose)
    LaunchedEffect(Unit) { vm.loadSchedule() }
    val connected = state is TreeConnection.State.Connected

    Column(
        modifier = Modifier
            .padding(contentPadding)
            .verticalScroll(rememberScrollState())
            .padding(horizontal = 12.dp, vertical = 8.dp),
        verticalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            IconButton(onClick = onClose) { Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Back") }
            Text("Schedule", style = MaterialTheme.typography.titleLarge, fontWeight = FontWeight.Bold)
        }
        if (status.isNotEmpty()) {
            Text(status, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
        val s = schedule
        when {
            !connected -> Hint("Not connected to the tree.")
            s == null -> Hint("Asking the tree for its schedule…")
            else -> {
                ModesCard {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Text("Lights on and off", style = MaterialTheme.typography.titleSmall, fontWeight = FontWeight.Bold,
                            modifier = Modifier.weight(1f))
                        if (s.timers.size < TreeSchedule.MAX_TIMERS) {
                            TextButton(onClick = { editingTimer = s.timers.size to TimerInfo() }) { Text("+ Add") }
                        }
                    }
                    Text("Like a plug-in timer. Switching the lights by hand lasts until the timer's next change.",
                        style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                    if (s.timers.isEmpty()) Hint("No timer: the lights stay as you set them.")
                    s.timers.forEachIndexed { i, t ->
                        ItemRow(onClick = { editingTimer = i to t }) {
                            Column(Modifier.weight(1f)) {
                                Text("On ${formatTimeOfDay(t.onSec)} · Off ${formatTimeOfDay(t.offSec)}" +
                                    if (t.offSec <= t.onSec) " (next day)" else "", style = MaterialTheme.typography.bodyMedium)
                                Text(formatDays(t.days), style = MaterialTheme.typography.bodySmall)
                            }
                            Switch(checked = t.enabled, onCheckedChange = { vm.setTimer(i, t.copy(enabled = it)) })
                            TextButton(onClick = { deleting = "timer" to i }) { Text("Delete") }
                        }
                    }
                }
                ModesCard {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Text("Events", style = MaterialTheme.typography.titleSmall, fontWeight = FontWeight.Bold,
                            modifier = Modifier.weight(1f))
                        if (s.events.size < TreeSchedule.MAX_EVENTS) {
                            TextButton(onClick = { editingEvent = s.events.size to EventInfo() }) { Text("+ Add") }
                        }
                    }
                    Text("At an exact moment, play a scene, show or mode (lights on), then go back to what was playing.",
                        style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                    if (s.events.isEmpty()) Hint("No events.")
                    val running = live?.scene?.event?.index ?: -1
                    s.events.forEachIndexed { i, e ->
                        ItemRow(onClick = { editingEvent = i to e }) {
                            Column(Modifier.weight(1f)) {
                                Text(e.name + if (i == running) " · playing now" else "", style = MaterialTheme.typography.bodyMedium,
                                    fontWeight = FontWeight.Bold)
                                Text(formatWhen(e), style = MaterialTheme.typography.bodySmall)
                                Text("${e.action.label}: ${targetName(e, catalog)}" +
                                    if (e.durationSec > 0) " for ${formatDuration(e.durationSec)}" else ", and it stays",
                                    style = MaterialTheme.typography.bodySmall)
                                Row {
                                    if (i == running) {
                                        TextButton(onClick = vm::stopEvent) { Text("Stop") }
                                    } else {
                                        TextButton(onClick = { vm.tryEvent(i) }) { Text("Try now") }
                                    }
                                    TextButton(onClick = { deleting = "event" to i }) { Text("Delete") }
                                }
                            }
                            Switch(checked = e.enabled, onCheckedChange = { vm.setEvent(i, e.copy(enabled = it)) })
                        }
                    }
                }
            }
        }
    }
    editingTimer?.let { (i, t) ->
        TimerDialog(t, onDismiss = { editingTimer = null }) {
            editingTimer = null
            vm.setTimer(i, it)
        }
    }
    editingEvent?.let { (i, e) ->
        EventDialog(e, library, catalog, onDismiss = { editingEvent = null }) {
            editingEvent = null
            vm.setEvent(i, it)
        }
    }
    deleting?.let { (what, i) ->
        ConfirmDialog(
            title = if (what == "timer") "Delete these on/off times?" else "Delete \"${schedule?.events?.getOrNull(i)?.name}\"?",
            text = "It's removed from the tree's schedule.",
            confirm = "Delete",
            onConfirm = {
                deleting = null
                if (what == "timer") vm.deleteTimer(i) else vm.deleteEvent(i)
            },
            onDismiss = { deleting = null },
        )
    }
}

private fun targetName(e: EventInfo, catalog: ModeCatalog?): String =
    if (e.action == EventAction.MODE) catalog?.byId(e.target)?.name ?: e.target else e.target

@Composable
private fun ItemRow(onClick: () -> Unit, content: @Composable () -> Unit) {
    Surface(
        shape = RoundedCornerShape(8.dp),
        color = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.5f),
        modifier = Modifier.fillMaxWidth().clickable(onClick = onClick),
    ) {
        Row(Modifier.padding(8.dp), verticalAlignment = Alignment.CenterVertically) { content() }
    }
}

/** Seven chips, Sunday first. */
@Composable
private fun DayChips(days: Int, onChange: (Int) -> Unit) {
    val letters = listOf("S", "M", "T", "W", "T", "F", "S")
    FlowRow(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
        letters.forEachIndexed { i, l ->
            FilterChip(selected = days shr i and 1 == 1, onClick = { onChange(days xor (1 shl i)) }, label = { Text(l) })
        }
    }
    Row(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
        TextButton(onClick = { onChange(EVERY_DAY) }) { Text("Every day") }
        TextButton(onClick = { onChange(0b0111110) }) { Text("Weekdays") }
        TextButton(onClick = { onChange(0b1000001) }) { Text("Weekends") }
    }
}

@Composable
private fun TimerDialog(initial: TimerInfo, onDismiss: () -> Unit, onSave: (TimerInfo) -> Unit) {
    var t by remember { mutableStateOf(initial) }
    var picking by remember { mutableStateOf<String?>(null) }
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("Lights on and off") },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    OutlinedButton(onClick = { picking = "on" }) { Text("On ${formatTimeOfDay(t.onSec)}") }
                    OutlinedButton(onClick = { picking = "off" }) { Text("Off ${formatTimeOfDay(t.offSec)}") }
                }
                if (t.offSec <= t.onSec) Text("Off is the next day.", style = MaterialTheme.typography.bodySmall)
                Text("Days it turns on:", style = MaterialTheme.typography.bodySmall)
                DayChips(t.days) { t = t.copy(days = it) }
            }
        },
        confirmButton = { TextButton(onClick = { onSave(t) }, enabled = t.days != 0) { Text("Save") } },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
    )
    picking?.let { which ->
        TimeDialog(if (which == "on") t.onSec else t.offSec, withSeconds = false, onDismiss = { picking = null }) {
            t = if (which == "on") t.copy(onSec = it) else t.copy(offSec = it)
            picking = null
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun TimeDialog(sec: Int, withSeconds: Boolean, onDismiss: () -> Unit, onSet: (Int) -> Unit) {
    val picker = rememberTimePickerState(initialHour = sec / 3600, initialMinute = sec / 60 % 60, is24Hour = false)
    var seconds by remember { mutableStateOf((sec % 60).toString()) }
    AlertDialog(
        onDismissRequest = onDismiss,
        text = {
            Column(horizontalAlignment = Alignment.CenterHorizontally) {
                TimePicker(state = picker)
                if (withSeconds) {
                    OutlinedTextField(
                        value = seconds, onValueChange = { seconds = it.filter(Char::isDigit).take(2) },
                        label = { Text("Seconds") }, singleLine = true, modifier = Modifier.width(120.dp),
                        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                    )
                }
            }
        },
        confirmButton = {
            TextButton(onClick = {
                onSet(picker.hour * 3600 + picker.minute * 60 + (seconds.toIntOrNull() ?: 0).coerceIn(0, 59))
            }) { Text("Set") }
        },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
    )
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun EventDialog(
    initial: EventInfo,
    library: TreeLibrary?,
    catalog: ModeCatalog?,
    onDismiss: () -> Unit,
    onSave: (EventInfo) -> Unit,
) {
    var e by remember { mutableStateOf(initial) }
    var minutes by remember { mutableStateOf(if (initial.durationSec == 0L) "0" else formatMinutes(initial.durationSec)) }
    var pickingTime by remember { mutableStateOf(false) }
    var pickingDate by remember { mutableStateOf(false) }
    val targets: List<Pair<String, String>> = when (e.action) {
        EventAction.PRESET -> library?.presets?.map { it.name to it.name } ?: emptyList()
        EventAction.SHOW -> library?.shows?.map { it.name to it.name } ?: emptyList()
        EventAction.MODE -> catalog?.modes?.filter { it.pickable }?.map { it.id to it.name } ?: emptyList()
    }
    val durationSec = ((minutes.replace(',', '.').toDoubleOrNull() ?: -1.0) * 60).roundToLong()
    val ok = e.name.isNotBlank() && targets.any { it.first == e.target } && durationSec >= 0 &&
        (e.repeat != Repeat.WEEKLY || e.days != 0)
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(if (initial.name.isEmpty()) "New event" else "Event") },
        text = {
            Column(Modifier.verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedTextField(value = e.name, onValueChange = { e = e.copy(name = it.take(19)) }, label = { Text("Name") },
                    singleLine = true)
                FlowRow(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                    Repeat.entries.forEach { r ->
                        FilterChip(selected = e.repeat == r, onClick = { e = e.copy(repeat = r) }, label = { Text(r.label) })
                    }
                }
                if (e.repeat == Repeat.WEEKLY) {
                    DayChips(e.days) { e = e.copy(days = it) }
                } else {
                    val date = if (e.repeat == Repeat.ONCE) "%d-%02d-%02d".format(e.year, e.month, e.day)
                    else "%s %d".format(java.time.Month.of(e.month).name.lowercase().replaceFirstChar { it.uppercase() }, e.day)
                    OutlinedButton(onClick = { pickingDate = true }) { Text(date) }
                }
                OutlinedButton(onClick = { pickingTime = true }) { Text("At ${formatTimeOfDay(e.timeSec)}") }
                FlowRow(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                    EventAction.entries.forEach { a ->
                        FilterChip(selected = e.action == a, onClick = { e = e.copy(action = a, target = "") }, label = { Text(a.label) })
                    }
                }
                TargetPicker(targets, e.target) { e = e.copy(target = it) }
                OutlinedTextField(
                    value = minutes, onValueChange = { minutes = it }, label = { Text("For (minutes, 0 = it stays)") },
                    singleLine = true, isError = durationSec < 0,
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Decimal),
                )
            }
        },
        confirmButton = { TextButton(onClick = { onSave(e.copy(durationSec = durationSec)) }, enabled = ok) { Text("Save") } },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
    )
    if (pickingTime) {
        TimeDialog(e.timeSec, withSeconds = true, onDismiss = { pickingTime = false }) {
            e = e.copy(timeSec = it)
            pickingTime = false
        }
    }
    if (pickingDate) {
        val year = if (e.repeat == Repeat.ONCE) e.year else LocalDate.now().year
        val start = runCatching { LocalDate.of(year, e.month, e.day) }.getOrDefault(LocalDate.now())
        val picker = rememberDatePickerState(initialSelectedDateMillis = start.atStartOfDay().toInstant(ZoneOffset.UTC).toEpochMilli())
        DatePickerDialog(
            onDismissRequest = { pickingDate = false },
            confirmButton = {
                TextButton(onClick = {
                    picker.selectedDateMillis?.let {
                        val d = Instant.ofEpochMilli(it).atZone(ZoneOffset.UTC).toLocalDate()
                        e = e.copy(year = d.year, month = d.monthValue, day = d.dayOfMonth)
                    }
                    pickingDate = false
                }) { Text("Set") }
            },
            dismissButton = { TextButton(onClick = { pickingDate = false }) { Text("Cancel") } },
        ) { DatePicker(state = picker) }
    }
}

@Composable
private fun TargetPicker(targets: List<Pair<String, String>>, current: String, onPick: (String) -> Unit) {
    var open by remember { mutableStateOf(false) }
    Box {
        OutlinedButton(onClick = { open = true }) { Text(targets.firstOrNull { it.first == current }?.second ?: "Choose…") }
        DropdownMenu(expanded = open, onDismissRequest = { open = false }) {
            targets.forEach { (id, name) ->
                DropdownMenuItem(text = { Text(name) }, onClick = { open = false; onPick(id) })
            }
        }
    }
}

private fun formatMinutes(sec: Long): String = if (sec % 60 == 0L) (sec / 60).toString() else "%.1f".format(sec / 60.0)
