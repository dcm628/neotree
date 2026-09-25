package com.neotree.app.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.MoreVert
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.FilterChip
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import com.neotree.app.TreeViewModel
import com.neotree.app.net.ModeCatalog
import com.neotree.app.net.SceneState
import com.neotree.app.net.ShowInfo
import com.neotree.app.net.ShowState
import com.neotree.app.net.TreeLibrary
import com.neotree.app.net.TreeProtocol
import kotlin.math.roundToInt

// ---- shows ----

/** What's playing, and the shows to play: built-ins and the family's own. */
@Composable
internal fun ShowsCard(vm: TreeViewModel, library: TreeLibrary, playing: ShowState?, elapsedSec: Long) {
    // null = closed; a ShowDraft = editing (a new show, a copy, or an existing user show).
    var editing by remember { mutableStateOf<ShowDraft?>(null) }
    var deleting by remember { mutableStateOf<ShowInfo?>(null) }
    ModesCard {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text("Shows", style = MaterialTheme.typography.titleSmall, fontWeight = FontWeight.Bold, modifier = Modifier.weight(1f))
            TextButton(onClick = { editing = ShowDraft.new() }) { Text("New show") }
        }
        if (playing != null) {
            val left = (playing.leftSec - elapsedSec).coerceAtLeast(0)
            Text("Playing ${playing.name}", fontWeight = FontWeight.Bold)
            Text(
                "Now: ${playing.entry} (${playing.position + 1} of ${playing.count}) · next in ${formatDuration(left)}" +
                    if (playing.round > 0) " · round ${playing.round + 1}" else "",
                style = MaterialTheme.typography.bodySmall,
            )
            Button(onClick = { vm.stopShow() }) { Text("Stop the show") }
            HorizontalDivider()
        } else {
            Text(
                "A show plays scenes in turn, for as long as you like - set one to play at power-up to leave the tree running on its own.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        library.shows.forEach { show ->
            ShowRow(
                library = library,
                show = show,
                isPlaying = playing?.name == show.name,
                onPlay = { vm.playShow(show.index) },
                onBoot = { vm.setBootShow(if (library.bootShow == show.name) -1 else show.index) },
                onEdit = { editing = ShowDraft.of(show, library, copy = !show.user) },
                onDelete = { deleting = show },
            )
        }
    }
    editing?.let { draft ->
        ShowEditorDialog(
            vm = vm,
            library = library,
            draft = draft,
            onDismiss = { editing = null },
            onSave = { name, entries, loop, shuffle ->
                editing = null
                vm.saveShow(name, entries, loop, shuffle)
            },
        )
    }
    deleting?.let { show ->
        ConfirmDialog(
            title = "Delete \"${show.name}\"?",
            text = if (library.bootShow == show.name) "It won't play at power-up any more." else "The scenes in it are kept.",
            confirm = "Delete",
            onConfirm = {
                deleting = null
                vm.deleteShow(show.index)
            },
            onDismiss = { deleting = null },
        )
    }
}

@Composable
private fun ShowRow(
    library: TreeLibrary,
    show: ShowInfo,
    isPlaying: Boolean,
    onPlay: () -> Unit,
    onBoot: () -> Unit,
    onEdit: () -> Unit,
    onDelete: () -> Unit,
) {
    var menu by remember { mutableStateOf(false) }
    val boot = library.bootShow == show.name
    Row(verticalAlignment = Alignment.CenterVertically) {
        Column(Modifier.weight(1f)) {
            Text(show.name + if (show.user) "" else " (built in)")
            val total = show.entries.sumOf { it.seconds.toLong() }
            val details = buildList {
                add("${show.entries.size} scenes")
                if (total > 0) add("~${formatDuration(total)} a round")
                add(if (show.loop) "loops" else "once")
                if (show.shuffle) add("shuffled")
                if (show.entries.any { it.preset < 0 }) add("missing scenes are skipped")
            }
            Text(details.joinToString(" · "), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            if (boot) Text("Plays at power-up", style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.primary)
        }
        if (isPlaying) {
            Text("Playing", style = MaterialTheme.typography.labelMedium, color = MaterialTheme.colorScheme.primary)
        } else {
            TextButton(onClick = onPlay) { Text("Play") }
        }
        Box {
            IconButton(onClick = { menu = true }) { Icon(Icons.Filled.MoreVert, contentDescription = "More") }
            DropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
                DropdownMenuItem(
                    text = { Text(if (boot) "Don't play at power-up" else "Play at power-up") },
                    onClick = { menu = false; onBoot() },
                )
                DropdownMenuItem(
                    text = { Text(if (show.user) "Edit" else "Copy and edit") },
                    onClick = { menu = false; onEdit() },
                )
                if (show.user) {
                    DropdownMenuItem(text = { Text("Delete") }, onClick = { menu = false; onDelete() })
                }
            }
        }
    }
}

/** A show being edited: entries are (preset index, minutes as typed). */
private data class ShowDraft(
    val name: String,
    val entries: List<Pair<Int, String>>,
    val loop: Boolean,
    val shuffle: Boolean,
) {
    companion object {
        fun new() = ShowDraft("", emptyList(), loop = true, shuffle = false)

        fun of(show: ShowInfo, library: TreeLibrary, copy: Boolean) = ShowDraft(
            name = if (copy) "${show.name} copy" else show.name,
            entries = show.entries.map { e ->
                // A deleted preset stays -1 until another is picked; 0 s ("its own length") shows as blank.
                (if (e.preset in library.presets.indices) e.preset else -1) to
                    (if (e.seconds > 0) formatMinutesText(e.seconds) else "")
            },
            loop = show.loop,
            shuffle = show.shuffle,
        )
    }
}

private fun formatMinutesText(seconds: Int): String =
    if (seconds % 60 == 0) (seconds / 60).toString() else "%.1f".format(seconds / 60f)

@Composable
private fun ShowEditorDialog(
    vm: TreeViewModel,
    library: TreeLibrary,
    draft: ShowDraft,
    onDismiss: () -> Unit,
    onSave: (name: String, entries: List<Pair<Int, Int>>, loop: Boolean, shuffle: Boolean) -> Unit,
) {
    var name by remember { mutableStateOf(draft.name) }
    val entries = remember { mutableStateListOf(*draft.entries.toTypedArray()) }
    var loop by remember { mutableStateOf(draft.loop) }
    var shuffle by remember { mutableStateOf(draft.shuffle) }
    // Editing a show keeps its name: saving under the same name replaces it.
    val nameProblem = vm.showNameProblem(name)
    val entriesProblem = when {
        entries.isEmpty() -> "Add at least one scene"
        entries.any { it.first < 0 } -> "Pick a scene for every entry"
        else -> null
    }
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(if (draft.name.isEmpty()) "New show" else "Edit show") },
        text = {
            Column(Modifier.verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedTextField(
                    value = name, onValueChange = { name = it.take(40) }, label = { Text("Name") }, singleLine = true,
                    isError = nameProblem != null && name.isNotEmpty(),
                    supportingText = { if (nameProblem != null && name.isNotEmpty()) Text(nameProblem) },
                    modifier = Modifier.fillMaxWidth(),
                )
                Text("Scenes, in order - minutes each (blank: the scene's own length, or 5 min):",
                    style = MaterialTheme.typography.bodySmall)
                entries.forEachIndexed { i, (preset, minutes) ->
                    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                        PresetPicker(library, preset, Modifier.weight(1f)) { entries[i] = it to minutes }
                        OutlinedTextField(
                            value = minutes, onValueChange = { entries[i] = preset to it }, singleLine = true,
                            placeholder = { Text("min") },
                            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Decimal),
                            modifier = Modifier.width(72.dp),
                        )
                        IconButton(onClick = { entries.removeAt(i) }) { Icon(Icons.Filled.Close, contentDescription = "Remove") }
                    }
                }
                if (entries.size < TreeProtocol.MAX_SHOW_ENTRIES) {
                    OutlinedButton(onClick = { entries.add(-1 to "5") }) { Text("Add a scene") }
                }
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text("Loop forever", Modifier.weight(1f))
                    Switch(checked = loop, onCheckedChange = { loop = it })
                }
                if (!loop) {
                    Text("Then back to the base scene.", style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text("Shuffle each round", Modifier.weight(1f))
                    Switch(checked = shuffle, onCheckedChange = { shuffle = it })
                }
                entriesProblem?.let { Text(it, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.error) }
            }
        },
        confirmButton = {
            TextButton(
                onClick = {
                    val seconds = entries.map { (preset, minutes) ->
                        preset to ((minutes.replace(',', '.').toFloatOrNull() ?: 0f) * 60).roundToInt().coerceIn(0, 0xFFFF)
                    }
                    onSave(name, seconds, loop, shuffle)
                },
                enabled = nameProblem == null && entriesProblem == null,
            ) { Text("Save") }
        },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
    )
}

@Composable
private fun PresetPicker(library: TreeLibrary, current: Int, modifier: Modifier, onPick: (Int) -> Unit) {
    var open by remember { mutableStateOf(false) }
    Box(modifier) {
        OutlinedButton(onClick = { open = true }, modifier = Modifier.fillMaxWidth()) {
            Text(library.presets.getOrNull(current)?.name ?: "Choose…", maxLines = 1)
        }
        DropdownMenu(expanded = open, onDismissRequest = { open = false }) {
            library.presets.forEach { p ->
                DropdownMenuItem(text = { Text(p.name) }, onClick = { open = false; onPick(p.index) })
            }
        }
    }
}

// ---- scenes ----

/** Presets to apply, saving what's running as a scene or as the base scene, and the family's saved scenes. */
@Composable
internal fun ScenesCard(vm: TreeViewModel, catalog: ModeCatalog, library: TreeLibrary, scene: SceneState) {
    var saving by remember { mutableStateOf(false) }
    var makingBase by remember { mutableStateOf(false) }
    var deleting by remember { mutableStateOf<Int?>(null) }
    ModesCard {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text("Scenes", style = MaterialTheme.typography.titleSmall, fontWeight = FontWeight.Bold, modifier = Modifier.weight(1f))
            TextButton(onClick = { vm.revertScene() }) { Text("Back to base") }
        }
        FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            library.presets.forEach { p ->
                FilterChip(selected = p.name == scene.name, onClick = { vm.applyPreset(p.index) }, label = { Text(p.name) })
            }
        }
        if (scene.name.isNotEmpty() && library.presets.none { it.name == scene.name }) {
            Text("Now: ${scene.name}", style = MaterialTheme.typography.bodySmall)
        }
        FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            OutlinedButton(onClick = { saving = true }) { Text("Save as a scene…") }
            OutlinedButton(onClick = { makingBase = true }) { Text("Make this the base…") }
        }
        val baseModes = library.baseSlots.filter { it.isNotEmpty() }
            .map { id -> catalog.byId(id)?.name ?: id }
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(
                "Base scene (power-up and \"Back to base\"): " +
                    (if (library.baseCustom) baseModes.joinToString(" + ").ifEmpty { "nothing" } else "Colors"),
                style = MaterialTheme.typography.bodySmall,
                modifier = Modifier.weight(1f),
            )
            if (library.baseCustom) TextButton(onClick = { vm.resetBase() }) { Text("Reset") }
        }
        val mine = library.presets.filter { it.user }
        if (mine.isNotEmpty()) {
            HorizontalDivider()
            Text("Your scenes (${mine.size} of ${TreeProtocol.MAX_USER_PRESETS})", style = MaterialTheme.typography.labelMedium)
            mine.forEach { p ->
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(p.name, Modifier.weight(1f))
                    TextButton(onClick = { deleting = p.index }) { Text("Delete") }
                }
            }
        }
    }
    if (saving) {
        NameDialog(
            title = "Save what's playing",
            hint = "Saved on the tree: modes, settings and what happens when each ends. Same name replaces it.",
            problem = vm::presetNameProblem,
            onDismiss = { saving = false },
            onSave = {
                saving = false
                vm.savePreset(it)
            },
        )
    }
    if (makingBase) {
        ConfirmDialog(
            title = "Make this the base scene?",
            text = "The tree will power up into what's playing now, and \"Back to base\" will return to it. " +
                "The Home page's colors only work while Colors (the Canvas) is in the bottom slot.",
            confirm = "Make it the base",
            onConfirm = {
                makingBase = false
                vm.saveAsBase()
            },
            onDismiss = { makingBase = false },
        )
    }
    deleting?.let { index ->
        val name = library.presets.getOrNull(index)?.name ?: ""
        val inShows = library.shows.filter { s -> s.entries.any { it.preset == index } }.map { it.name }
        ConfirmDialog(
            title = "Delete \"$name\"?",
            text = if (inShows.isEmpty()) "It's removed from the tree." else "Shows using it skip it: ${inShows.joinToString()}.",
            confirm = "Delete",
            onConfirm = {
                deleting = null
                vm.deletePreset(index)
            },
            onDismiss = { deleting = null },
        )
    }
}

@Composable
internal fun NameDialog(
    title: String,
    hint: String,
    problem: (String) -> String?,
    onDismiss: () -> Unit,
    onSave: (String) -> Unit,
    initial: String = "",
) {
    var name by remember { mutableStateOf(initial) }
    val issue = problem(name)
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(title) },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(hint, style = MaterialTheme.typography.bodySmall)
                OutlinedTextField(
                    value = name, onValueChange = { name = it.take(40) }, label = { Text("Name") }, singleLine = true,
                    isError = issue != null && name.isNotEmpty(),
                    supportingText = { if (issue != null && name.isNotEmpty()) Text(issue) },
                )
            }
        },
        confirmButton = { TextButton(onClick = { onSave(name) }, enabled = issue == null) { Text("Save") } },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
    )
}

@Composable
internal fun ConfirmDialog(title: String, text: String, confirm: String, onConfirm: () -> Unit, onDismiss: () -> Unit) {
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(title) },
        text = { Text(text) },
        confirmButton = { TextButton(onClick = onConfirm) { Text(confirm) } },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
    )
}
