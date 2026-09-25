package com.neotree.app.ui

import androidx.activity.compose.BackHandler
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
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Slider
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import com.neotree.app.TreeViewModel
import com.neotree.app.net.ACTIONS_PER_RULE
import com.neotree.app.net.EffectInfo
import com.neotree.app.net.EffectModes
import com.neotree.app.net.EffectSection
import com.neotree.app.net.FxItem
import com.neotree.app.net.FxSectionData
import com.neotree.app.net.FxSectionSchema
import com.neotree.app.net.ModeCatalog
import com.neotree.app.net.ParamInfo
import com.neotree.app.net.ParamType
import com.neotree.app.net.ParamValue
import com.neotree.app.net.SceneState
import com.neotree.app.net.TreeConnection
import com.neotree.app.net.TreeLibrary
import com.neotree.app.net.TreeProtocol
import kotlin.math.roundToInt

/** The editor's sections, top to bottom (actions live inside their rules). */
private val SECTION_ORDER = listOf(
    EffectSection.LAYERS, EffectSection.THINGS, EffectSection.SOURCES, EffectSection.MEETS,
    EffectSection.RULES, EffectSection.STARTS, EffectSection.SETTINGS,
)

private fun sectionHint(s: EffectSection): String = when (s) {
    EffectSection.LAYERS -> "Painted bottom to top. Things are drawn on a \"things\" layer."
    EffectSection.THINGS -> "Shapes that move and light the LEDs they touch. Sources, rules and \"Start with\" make them."
    EffectSection.SOURCES -> "Make things all the time, somewhere in or on the tree."
    EffectSection.MEETS -> "Things in groups (set on each thing) can pass through, bounce, stick or vanish when they meet."
    EffectSection.RULES -> "When something happens, do something: things meet, a timer, a count..."
    EffectSection.STARTS -> "Things there from the start (and each time it starts over)."
    EffectSection.SETTINGS -> "The whole effect."
    EffectSection.ACTIONS -> ""
}

/**
 * The custom-effect editor: a full page, built entirely from the schema the
 * tree sends. Every change shows on the tree straight away (the draft runs in
 * a slot); Save stores it as an effect you can play like any mode.
 */
@Composable
fun EffectEditorScreen(vm: TreeViewModel, contentPadding: PaddingValues, onClose: () -> Unit) {
    val state by vm.connection.state.collectAsState()
    val schema by vm.fxSchema.collectAsState()
    val draft by vm.fxDraft.collectAsState()
    val status by vm.status.collectAsState()
    val live by vm.scene.collectAsState()
    val connected = state is TreeConnection.State.Connected
    var saving by remember { mutableStateOf(false) }
    BackHandler(onBack = onClose)
    // After a reconnect (or coming back to the app) the draft is read again.
    LaunchedEffect(connected, draft.isEmpty()) {
        if (connected && draft.isEmpty()) vm.editEffect(EffectModes.DRAFT)
    }
    val name = draft[EffectSection.SETTINGS]?.name ?: live?.scene?.draft?.name ?: ""
    val ready = SECTION_ORDER.all { schema[it] != null && draft[it] != null } &&
        schema[EffectSection.ACTIONS] != null && draft[EffectSection.ACTIONS] != null

    Column(
        modifier = Modifier
            .padding(contentPadding)
            .verticalScroll(rememberScrollState())
            .padding(horizontal = 12.dp, vertical = 8.dp),
        verticalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            IconButton(onClick = onClose) { Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Close the editor") }
            Column(Modifier.weight(1f)) {
                Text(name.ifEmpty { "Effect" }, style = MaterialTheme.typography.titleLarge, fontWeight = FontWeight.Bold)
                Text("Changes show on the tree as you make them", style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            Button(onClick = { saving = true }, enabled = ready) { Text("Save…") }
        }
        if (status.isNotEmpty()) {
            Text(status, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
        when {
            !connected -> Hint("Not connected to the tree.")
            !ready -> Hint("Reading the effect from the tree…")
            else -> SECTION_ORDER.forEach { s -> SectionCard(vm, schema[s]!!, draft[s]!!, schema, draft) }
        }
    }
    if (saving) {
        NameDialog(
            title = "Save this effect",
            hint = "Saved on the tree as a mode: play it, put it in scenes and shows. The same name replaces it.",
            problem = vm::effectNameProblem,
            onDismiss = { saving = false },
            onSave = {
                saving = false
                vm.saveEffect(it)
            },
            initial = name,
        )
    }
}

@Composable
private fun SectionCard(
    vm: TreeViewModel,
    schema: FxSectionSchema,
    data: FxSectionData,
    allSchema: Map<EffectSection, FxSectionSchema>,
    allDraft: Map<EffectSection, FxSectionData>,
) {
    val section = schema.section
    ModesCard {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(schema.label, style = MaterialTheme.typography.titleSmall, fontWeight = FontWeight.Bold, modifier = Modifier.weight(1f))
            if (section != EffectSection.SETTINGS && data.items.size < schema.maxItems) {
                TextButton(onClick = { vm.fxItem(TreeProtocol.FxOp.ADD, section, 0) }) { Text("+ Add") }
            }
        }
        Text(sectionHint(section), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        if (section == EffectSection.SETTINGS) {
            data.items.firstOrNull()?.let { item -> Fields(vm, schema, item) }
        } else {
            if (data.items.isEmpty()) Hint("None.")
            data.items.forEachIndexed { k, item ->
                ItemCard(vm, schema, item, "${schema.item} ${k + 1}", canCopy = data.items.size < schema.maxItems) {
                    if (section == EffectSection.RULES) {
                        RuleActions(vm, allSchema[EffectSection.ACTIONS]!!, allDraft[EffectSection.ACTIONS]!!, rule = k)
                    }
                }
            }
        }
    }
}

/** A rule's actions ("Then"), inside its card. */
@Composable
private fun RuleActions(vm: TreeViewModel, schema: FxSectionSchema, data: FxSectionData, rule: Int) {
    val actions = data.items.filter { it.rule == rule }
    HorizontalDivider()
    Row(verticalAlignment = Alignment.CenterVertically) {
        Text("Then", style = MaterialTheme.typography.labelLarge, modifier = Modifier.weight(1f))
        if (actions.size < ACTIONS_PER_RULE) {
            TextButton(onClick = { vm.fxItem(TreeProtocol.FxOp.ADD, EffectSection.ACTIONS, rule) }) { Text("+ Add action") }
        }
    }
    if (actions.isEmpty()) Hint("Nothing yet - add what it does.")
    actions.forEachIndexed { k, a ->
        ItemCard(vm, schema, a, "${k + 1}.", canCopy = actions.size < ACTIONS_PER_RULE, startOpen = true)
    }
}

/** One item: a title with what it is, opened to show its fields (only those that apply). */
@Composable
private fun ItemCard(
    vm: TreeViewModel,
    schema: FxSectionSchema,
    item: FxItem,
    title: String,
    canCopy: Boolean,
    startOpen: Boolean = false,
    extra: @Composable () -> Unit = {},
) {
    var open by rememberSaveable(schema.section, item.index) { mutableStateOf(startOpen) }
    Surface(
        shape = RoundedCornerShape(8.dp),
        color = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.5f),
        modifier = Modifier.fillMaxWidth(),
    ) {
        Column(Modifier.padding(8.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Row(Modifier.fillMaxWidth().clickable { open = !open }, verticalAlignment = Alignment.CenterVertically) {
                Text(title, style = MaterialTheme.typography.labelLarge, fontWeight = FontWeight.Bold)
                Text("  " + summary(schema, item), style = MaterialTheme.typography.bodyMedium, modifier = Modifier.weight(1f))
                swatch(schema, item)?.let { c ->
                    Box(
                        Modifier
                            .size(width = 28.dp, height = 18.dp)
                            .background(c, RoundedCornerShape(4.dp))
                            .border(1.dp, MaterialTheme.colorScheme.outline, RoundedCornerShape(4.dp)),
                    )
                }
                Text(if (open) "  ▲" else "  ▼", style = MaterialTheme.typography.bodySmall)
            }
            if (open) {
                Fields(vm, schema, item)
                extra()
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    if (canCopy) {
                        OutlinedButton(onClick = { vm.fxItem(TreeProtocol.FxOp.DUPLICATE, schema.section, item.index) }) { Text("Copy") }
                    }
                    OutlinedButton(onClick = { vm.fxItem(TreeProtocol.FxOp.REMOVE, schema.section, item.index) }) { Text("Remove") }
                }
            }
        }
    }
}

@Composable
private fun Fields(vm: TreeViewModel, schema: FxSectionSchema, item: FxItem) {
    schema.fields.forEach { f ->
        if (!schema.shown(f, item.values)) return@forEach
        val value = item.values.getOrNull(f.index) ?: ParamValue()
        val onChange = { v: ParamValue -> vm.setFxField(schema.section, item.index, f.index, v) }
        when (f.param.type) {
            ParamType.NUMBER -> FxNumber(f.param, value.number) { onChange(ParamValue(number = it)) }
            ParamType.CHOICE -> FxChoice(f.param, value.number.roundToInt()) { onChange(ParamValue(number = it.toFloat())) }
            else -> ParamControl(f.param, value, onChange)
        }
    }
}

/** What an item is, in a few words: its first choice (a shape, a kind, what happens...). */
private fun summary(schema: FxSectionSchema, item: FxItem): String {
    if (schema.section == EffectSection.MEETS) {
        val g = schema.fields.getOrNull(0)?.param?.choices ?: return ""
        val what = schema.fields.getOrNull(2)?.param?.choices ?: return ""
        fun pick(list: List<String>, k: Int) = list.getOrNull(item.values.getOrNull(k)?.number?.roundToInt() ?: 0) ?: "?"
        return "${pick(g, 0)} meets ${pick(g, 1)}: ${pick(what, 2)}"
    }
    val f = schema.fields.firstOrNull { it.param.type == ParamType.CHOICE && schema.shown(it, item.values) } ?: return ""
    val k = item.values.getOrNull(f.index)?.number?.roundToInt() ?: 0
    return f.param.choices.getOrNull(k) ?: ""
}

/** The item's first shown color, if it has one. */
private fun swatch(schema: FxSectionSchema, item: FxItem): Color? {
    val f = schema.fields.firstOrNull { it.param.type == ParamType.COLOR && schema.shown(it, item.values) } ?: return null
    val c = item.values.getOrNull(f.index)?.color ?: return null
    return Color(c.r, c.g, c.b)
}

/** A choice: a menu when there are many options, chips when few. */
@Composable
private fun FxChoice(p: ParamInfo, value: Int, onChange: (Int) -> Unit) {
    if (p.choices.size <= 5) {
        ParamControl(p, ParamValue(number = value.toFloat())) { onChange(it.number.roundToInt()) }
        return
    }
    var open by remember { mutableStateOf(false) }
    Row(verticalAlignment = Alignment.CenterVertically) {
        Text(p.label, style = MaterialTheme.typography.bodyMedium, modifier = Modifier.weight(1f))
        Box {
            OutlinedButton(onClick = { open = true }) { Text(p.choices.getOrNull(value) ?: "Choose…") }
            DropdownMenu(expanded = open, onDismissRequest = { open = false }) {
                p.choices.forEachIndexed { i, c ->
                    DropdownMenuItem(text = { Text(c) }, onClick = { open = false; onChange(i) })
                }
            }
        }
    }
}

/** A number: a slider, and its value can be tapped to type one in. */
@Composable
private fun FxNumber(p: ParamInfo, value: Float, onChange: (Float) -> Unit) {
    var typing by remember { mutableStateOf(false) }
    val steps = if (p.step > 0f) ((p.max - p.min) / p.step).roundToInt() - 1 else 0
    fun snap(v: Float) = (if (p.step > 0f) p.min + ((v - p.min) / p.step).roundToInt() * p.step else v).coerceIn(p.min, p.max)
    Column {
        Row {
            Text(p.label, style = MaterialTheme.typography.bodyMedium, modifier = Modifier.weight(1f))
            Text(
                fxNumberText(value, p.step),
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.primary,
                modifier = Modifier.clickable { typing = true },
            )
        }
        Slider(
            value = value.coerceIn(p.min, p.max),
            onValueChange = { onChange(snap(it)) },
            valueRange = p.min..p.max,
            // Fine-grained ranges slide freely (snapped to the step); few steps show ticks.
            steps = if (steps in 1..20) steps else 0,
        )
    }
    if (typing) {
        var text by remember { mutableStateOf(fxNumberText(value, p.step)) }
        val parsed = text.replace(',', '.').toFloatOrNull()
        AlertDialog(
            onDismissRequest = { typing = false },
            title = { Text(p.label) },
            text = {
                OutlinedTextField(
                    value = text, onValueChange = { text = it }, singleLine = true,
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                    supportingText = { Text("${fxNumberText(p.min, p.step)} to ${fxNumberText(p.max, p.step)}") },
                    isError = parsed == null,
                )
            },
            confirmButton = {
                TextButton(onClick = { typing = false; parsed?.let { onChange(snap(it)) } }, enabled = parsed != null) { Text("Set") }
            },
            dismissButton = { TextButton(onClick = { typing = false }) { Text("Cancel") } },
        )
    }
}

private fun fxNumberText(v: Float, step: Float): String = when {
    step >= 1f || step == 0f && v == v.roundToInt().toFloat() -> v.roundToInt().toString()
    step >= 0.1f -> "%.1f".format(v)
    else -> "%.2f".format(v)
}

/**
 * The Modes page's effects: the ones saved on the tree (play, edit, delete),
 * a new one - blank or a copy of a built-in - and the one being edited.
 */
@Composable
internal fun EffectsCard(vm: TreeViewModel, catalog: ModeCatalog, library: TreeLibrary, scene: SceneState, onEdit: () -> Unit) {
    var deleting by remember { mutableStateOf<EffectInfo?>(null) }
    var newMenu by remember { mutableStateOf(false) }
    ModesCard {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text("Effects", style = MaterialTheme.typography.titleSmall, fontWeight = FontWeight.Bold, modifier = Modifier.weight(1f))
            Box {
                OutlinedButton(onClick = { newMenu = true }) { Text("New…") }
                DropdownMenu(expanded = newMenu, onDismissRequest = { newMenu = false }) {
                    DropdownMenuItem(text = { Text("Blank") }, onClick = { newMenu = false; vm.editEffect(-1); onEdit() })
                    HorizontalDivider()
                    Text("A copy of:", style = MaterialTheme.typography.labelMedium, modifier = Modifier.padding(horizontal = 12.dp, vertical = 4.dp))
                    catalog.modes.filter { it.pickable }.forEach { m ->
                        DropdownMenuItem(text = { Text(m.name) }, onClick = { newMenu = false; vm.editEffect(m.index); onEdit() })
                    }
                }
            }
        }
        Text("Make your own from building blocks: backgrounds, things that move, sources and rules.",
            style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        scene.draft?.let { d ->
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text("Last edited: ${d.name}", style = MaterialTheme.typography.bodySmall, modifier = Modifier.weight(1f))
                TextButton(onClick = { vm.editEffect(EffectModes.DRAFT); onEdit() }) { Text("Continue") }
            }
        }
        if (library.effects.isEmpty()) {
            Hint("None saved yet.")
        } else {
            HorizontalDivider()
            Text("Saved (${library.effects.size} of ${TreeProtocol.MAX_EFFECTS})", style = MaterialTheme.typography.labelMedium)
            library.effects.forEach { e ->
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(e.name, Modifier.weight(1f))
                    TextButton(onClick = { vm.playEffect(e) }) { Text("Play") }
                    TextButton(onClick = { vm.editEffect(e.mode); onEdit() }) { Text("Edit") }
                    TextButton(onClick = { deleting = e }) { Text("Delete") }
                }
            }
        }
    }
    deleting?.let { e ->
        ConfirmDialog(
            title = "Delete \"${e.name}\"?",
            text = "It's removed from the tree. Saved scenes that use it leave its slot empty.",
            confirm = "Delete",
            onConfirm = {
                deleting = null
                vm.deleteEffect(e)
            },
            onDismiss = { deleting = null },
        )
    }
}
