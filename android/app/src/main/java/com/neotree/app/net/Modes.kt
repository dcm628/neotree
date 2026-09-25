package com.neotree.app.net

import org.json.JSONArray
import org.json.JSONObject

/**
 * The tree's modes and presets, from its DESCRIBE reply (firmware
 * engine/include/neotree/modes.hpp describe_modes): the built-ins, a mode's
 * index - what SLOT_SET takes - being its position. Custom effects come from
 * the library and the scene ([withEffects]), at their own indices. A
 * parameter's index - what PARAM_SET takes - is its position in
 * [ModeInfo.params].
 */
data class ModeCatalog(val modes: List<ModeInfo>, val presets: List<String>) {
    fun byId(id: String): ModeInfo? = modes.firstOrNull { it.id == id }

    /** The mode with this index (what SLOT_SET takes and the scene reports). */
    fun mode(index: Int): ModeInfo? = modes.firstOrNull { it.index == index }

    /** The built-ins with the saved effects after them, and the draft (which can't be picked for a slot). */
    fun withEffects(effects: List<EffectInfo>, draft: DraftInfo?): ModeCatalog {
        val builtIns = modes.filter { !it.effect }
        val saved = effects.map { ModeInfo(it.mode, "fx:${it.name}", it.name, "Made in the app", emptyList(), effect = true) }
        val editing = draft?.let {
            ModeInfo(EffectModes.DRAFT, EffectModes.DRAFT_ID, "Editing: ${it.name}", "The effect being edited",
                emptyList(), effect = true, pickable = false)
        }
        return copy(modes = builtIns + saved + listOfNotNull(editing))
    }

    companion object {
        fun parse(json: JSONObject): ModeCatalog {
            val modes = json.optJSONArray("modes") ?: JSONArray()
            return ModeCatalog(
                modes = (0 until modes.length()).map { i ->
                    val m = modes.getJSONObject(i)
                    val params = m.optJSONArray("p") ?: JSONArray()
                    ModeInfo(
                        index = i,
                        id = m.optString("id"),
                        name = m.optString("n", m.optString("id")),
                        summary = m.optString("s"),
                        params = (0 until params.length()).map { k -> ParamInfo.parse(k, params.getJSONObject(k)) },
                    )
                },
                presets = json.optJSONArray("presets").strings(),
            )
        }
    }
}

data class ModeInfo(
    val index: Int,
    val id: String,
    val name: String,
    val summary: String,
    val params: List<ParamInfo>,
    /** A custom effect (made in the app) rather than a built-in. */
    val effect: Boolean = false,
    /** Offered when choosing a slot's mode (not the draft: that's the editor's). */
    val pickable: Boolean = true,
)

enum class ParamType { NUMBER, COLOR, CHOICE, TOGGLE }

data class ParamInfo(
    val index: Int,
    val id: String,
    val label: String,
    val type: ParamType,
    val min: Float = 0f,
    val max: Float = 1f,
    /** 0 = continuous. */
    val step: Float = 0f,
    val default: ParamValue = ParamValue(),
    val choices: List<String> = emptyList(),
) {
    companion object {
        fun parse(index: Int, p: JSONObject): ParamInfo {
            val type = when (p.optString("t")) {
                "c" -> ParamType.COLOR
                "ch" -> ParamType.CHOICE
                "t" -> ParamType.TOGGLE
                else -> ParamType.NUMBER
            }
            val default = when (type) {
                ParamType.COLOR -> ParamValue(color = parseHexColor(p.optString("d")) ?: Rgb.BLACK)
                ParamType.TOGGLE -> ParamValue(number = if (p.optBoolean("d")) 1f else 0f)
                else -> ParamValue(number = p.optDouble("d", 0.0).toFloat())
            }
            return ParamInfo(
                index = index,
                id = p.optString("id"),
                label = p.optString("l", p.optString("id")),
                type = type,
                min = p.optDouble("min", 0.0).toFloat(),
                max = p.optDouble("max", if (type == ParamType.NUMBER) 1.0 else 0.0).toFloat(),
                step = p.optDouble("st", 0.0).toFloat(),
                default = default,
                choices = p.optString("ch").split('|').filter { it.isNotEmpty() },
            )
        }
    }
}

/** A parameter's value: [number] for numbers, choices (index) and toggles (0/1); [color] for colors. */
data class ParamValue(val number: Float = 0f, val color: Rgb = Rgb.BLACK)

/** One slot of the running scene, from the status JSON's "scene" (firmware Director::describe_state). */
data class SlotState(
    /** Mode id, "" when empty. */
    val mode: String,
    /** Mode index, -1 when empty. */
    val modeIndex: Int,
    /** entering / running / ending / leaving / empty. */
    val state: String,
    val ageSec: Long,
    val cycles: Int,
    val loops: Int,
    val params: List<ParamValue>,
    val durationSec: Long,
    /** Ends after this many cycles (0 = not counted). */
    val cycleLimit: Int,
    val policy: String,
) {
    val empty: Boolean get() = modeIndex < 0 || state == "empty"
}

/** A show playing on the tree ("show" in the scene JSON). */
data class ShowState(
    val name: String,
    /** The preset playing now. */
    val entry: String,
    val position: Int,
    val count: Int,
    val round: Int,
    /** Seconds left in this entry, when the scene was reported. */
    val leftSec: Long,
)

data class SceneState(
    val name: String,
    val slots: List<SlotState>,
    val base: List<String>,
    val show: ShowState? = null,
    /** Changes whenever what's running changes (-1 from older firmware). */
    val revision: Long = -1,
    /** The custom effect being edited (null from older firmware). */
    val draft: DraftInfo? = null,
) {
    companion object {
        /** Parses the scene JSON (pushed, or the status JSON's "scene"); null if there isn't one. */
        fun parse(scene: JSONObject?): SceneState? {
            if (scene == null) return null
            val slots = scene.optJSONArray("slots") ?: return null
            val show = scene.optJSONObject("show")
            val fx = scene.optJSONObject("fx")
            return SceneState(
                revision = scene.optLong("rev", -1),
                draft = fx?.let { DraftInfo(it.optString("n"), it.optLong("rev"), it.optInt("slot", -1)) },
                show = show?.let {
                    ShowState(
                        name = it.optString("n"),
                        entry = it.optString("entry"),
                        position = it.optInt("pos"),
                        count = it.optInt("of"),
                        round = it.optInt("round"),
                        leftSec = it.optLong("left"),
                    )
                },
                name = scene.optString("scene"),
                slots = (0 until slots.length()).map { i ->
                    val s = slots.getJSONObject(i)
                    val params = s.optJSONArray("params") ?: JSONArray()
                    val life = s.optJSONObject("life")
                    SlotState(
                        mode = s.optString("mode"),
                        modeIndex = s.optInt("i", -1),
                        state = s.optString("state"),
                        ageSec = s.optLong("age"),
                        cycles = s.optInt("cycles"),
                        loops = s.optInt("loops"),
                        params = (0 until params.length()).map { k ->
                            when (val v = params.get(k)) {
                                is String -> ParamValue(color = parseHexColor(v) ?: Rgb.BLACK)
                                is Number -> ParamValue(number = v.toFloat())
                                is Boolean -> ParamValue(number = if (v) 1f else 0f)
                                else -> ParamValue()
                            }
                        },
                        durationSec = life?.optLong("duration") ?: 0,
                        cycleLimit = life?.optInt("cycles") ?: 0,
                        policy = life?.optString("policy") ?: "",
                    )
                },
                base = scene.optJSONArray("base").strings(),
            )
        }
    }
}

/**
 * The tree's library (firmware Library::describe): presets and shows -
 * built-ins first, then the user's - the base scene and the startup show.
 * Indices are what PRESET, SHOW_PLAY and the delete commands take.
 */
data class TreeLibrary(
    val presets: List<PresetInfo>,
    val shows: List<ShowInfo>,
    /** The show played at power-up, "" for none. */
    val bootShow: String,
    val baseCustom: Boolean,
    val baseSlots: List<String>,
    /** Saved custom effects. */
    val effects: List<EffectInfo> = emptyList(),
) {
    companion object {
        fun parse(json: JSONObject): TreeLibrary {
            val presets = json.optJSONArray("presets") ?: JSONArray()
            val shows = json.optJSONArray("shows") ?: JSONArray()
            val base = json.optJSONObject("base")
            val fx = json.optJSONArray("fx") ?: JSONArray()
            return TreeLibrary(
                effects = (0 until fx.length()).map { i ->
                    val e = fx.getJSONObject(i)
                    EffectInfo(e.optInt("k"), e.optString("n"), e.optInt("i"))
                },
                presets = (0 until presets.length()).map { i ->
                    val p = presets.getJSONObject(i)
                    PresetInfo(i, p.optString("n"), p.optInt("u") == 1)
                },
                shows = (0 until shows.length()).map { i ->
                    val s = shows.getJSONObject(i)
                    val e = s.optJSONArray("e") ?: JSONArray()
                    ShowInfo(
                        index = i,
                        name = s.optString("n"),
                        user = s.optInt("u") == 1,
                        loop = s.optInt("loop", 1) == 1,
                        shuffle = s.optInt("shuffle") == 1,
                        entries = (0 until e.length()).map { k ->
                            val pair = e.getJSONArray(k)
                            ShowEntryInfo(preset = pair.optInt(0, -1), seconds = pair.optInt(1))
                        },
                    )
                },
                bootShow = json.optString("boot"),
                baseCustom = base?.optInt("custom") == 1,
                baseSlots = base?.optJSONArray("slots").strings(),
            )
        }
    }
}

data class PresetInfo(val index: Int, val name: String, val user: Boolean)

/** An entry of a show: a preset index (-1 if that preset has been deleted) and how long, 0 = its own length. */
data class ShowEntryInfo(val preset: Int, val seconds: Int)

data class ShowInfo(
    val index: Int,
    val name: String,
    val user: Boolean,
    val loop: Boolean,
    val shuffle: Boolean,
    val entries: List<ShowEntryInfo>,
)

/** "#rrggbb" -> Rgb, or null. */
fun parseHexColor(s: String): Rgb? {
    if (s.length != 7 || s[0] != '#') return null
    val v = s.substring(1).toIntOrNull(16) ?: return null
    return Rgb((v shr 16) and 0xFF, (v shr 8) and 0xFF, v and 0xFF)
}

private fun JSONArray?.strings(): List<String> =
    if (this == null) emptyList() else (0 until length()).map { optString(it) }
