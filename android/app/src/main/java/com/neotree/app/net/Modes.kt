package com.neotree.app.net

import org.json.JSONArray
import org.json.JSONObject

/**
 * The tree's modes and presets, from its DESCRIBE reply (firmware
 * engine/include/neotree/modes.hpp describe_modes). A mode's index - what
 * SLOT_SET takes - is its position in [modes]; a parameter's index - what
 * PARAM_SET takes - is its position in [ModeInfo.params].
 */
data class ModeCatalog(val modes: List<ModeInfo>, val presets: List<String>) {
    fun byId(id: String): ModeInfo? = modes.firstOrNull { it.id == id }

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
    val policy: String,
) {
    val empty: Boolean get() = modeIndex < 0 || state == "empty"
}

data class SceneState(val name: String, val slots: List<SlotState>, val base: List<String>) {
    companion object {
        /** Parses the status JSON's "scene" object; null if the firmware doesn't report one. */
        fun parse(scene: JSONObject?): SceneState? {
            if (scene == null) return null
            val slots = scene.optJSONArray("slots") ?: return null
            return SceneState(
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
                        policy = life?.optString("policy") ?: "",
                    )
                },
                base = scene.optJSONArray("base").strings(),
            )
        }
    }
}

/** "#rrggbb" -> Rgb, or null. */
fun parseHexColor(s: String): Rgb? {
    if (s.length != 7 || s[0] != '#') return null
    val v = s.substring(1).toIntOrNull(16) ?: return null
    return Rgb((v shr 16) and 0xFF, (v shr 8) and 0xFF, v and 0xFF)
}

private fun JSONArray?.strings(): List<String> =
    if (this == null) emptyList() else (0 until length()).map { optString(it) }
