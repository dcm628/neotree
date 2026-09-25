package com.neotree.app.net

import org.json.JSONArray
import org.json.JSONObject
import kotlin.math.roundToInt

/**
 * Custom effects (firmware engine/include/neotree/effect.hpp): an effect is
 * built from sections of items - backgrounds, things, sources, what happens
 * when things meet, rules and their actions, what it starts with - each item
 * a list of fields. The tree describes the fields (FX_SCHEMA) and sends the
 * draft being edited a section at a time (FX_GET); the editor is built from
 * both, so it needs nothing hard-coded about what the fields are.
 */
enum class EffectSection(val code: Int) {
    SETTINGS(0), LAYERS(1), THINGS(2), SOURCES(3), MEETS(4), RULES(5), ACTIONS(6), STARTS(7);

    companion object {
        fun from(code: Int): EffectSection? = entries.firstOrNull { it.code == code }
    }
}

/** How many actions a rule holds: an action's item index is rule * this + action. */
const val ACTIONS_PER_RULE = 4

/** One field of a section. It's shown only while field [shownBy]'s value has its bit set in [shownWhen]. */
data class FxField(
    val index: Int,
    val param: ParamInfo,
    val shownBy: Int = -1,
    val shownWhen: Int = 0,
)

data class FxSectionSchema(
    val section: EffectSection,
    val label: String,
    /** What one item is called, e.g. "Thing". */
    val item: String,
    val maxItems: Int,
    val fields: List<FxField>,
) {
    fun field(id: String): FxField? = fields.firstOrNull { it.param.id == id }

    /** Whether the editor shows field [f] of an item with these [values]. */
    fun shown(f: FxField, values: List<ParamValue>): Boolean {
        if (f.shownBy < 0) return true
        val by = fields.getOrNull(f.shownBy) ?: return false
        val k = values.getOrNull(f.shownBy)?.number?.roundToInt() ?: return false
        return k in 0..15 && (f.shownWhen shr k) and 1 == 1 && shown(by, values)
    }

    companion object {
        fun parse(json: JSONObject): FxSectionSchema? {
            val section = EffectSection.from(json.optInt("s", -1)) ?: return null
            val fields = json.optJSONArray("f") ?: JSONArray()
            return FxSectionSchema(
                section = section,
                label = json.optString("l"),
                item = json.optString("item"),
                maxItems = json.optInt("max", 1),
                fields = (0 until fields.length()).map { k ->
                    val f = fields.getJSONObject(k)
                    FxField(k, ParamInfo.parse(k, f), f.optInt("by", -1), f.optInt("when", 0))
                },
            )
        }
    }
}

/** One item of the draft: [index] is what FX_SET / FX_ITEM take ([rule] and [action] for actions). */
data class FxItem(
    val index: Int,
    val values: List<ParamValue>,
    val rule: Int = -1,
    val action: Int = -1,
)

/** A section of the draft effect, as the tree last sent it. */
data class FxSectionData(val section: EffectSection, val name: String, val items: List<FxItem>) {
    companion object {
        fun parse(json: JSONObject): FxSectionData? {
            val section = EffectSection.from(json.optInt("s", -1)) ?: return null
            val items = json.optJSONArray("i") ?: JSONArray()
            return FxSectionData(
                section = section,
                name = json.optString("n"),
                items = (0 until items.length()).map { k ->
                    val a = items.getJSONArray(k)
                    if (section == EffectSection.ACTIONS) {
                        val rule = a.optInt(0)
                        val action = a.optInt(1)
                        FxItem(rule * ACTIONS_PER_RULE + action, values(a, 2), rule, action)
                    } else {
                        FxItem(k, values(a, 0))
                    }
                },
            )
        }

        private fun values(a: JSONArray, from: Int): List<ParamValue> = (from until a.length()).map { k ->
            when (val v = a.get(k)) {
                is String -> ParamValue(color = parseHexColor(v) ?: Rgb.BLACK)
                is Number -> ParamValue(number = v.toFloat())
                is Boolean -> ParamValue(number = if (v) 1f else 0f)
                else -> ParamValue()
            }
        }
    }
}

/** A saved effect in the tree's library: its [position] (what deleting takes) and [mode] index (what SLOT_SET and FX_EDIT take). */
data class EffectInfo(val position: Int, val name: String, val mode: Int)

/** The draft effect, from the scene JSON's "fx": its name, revision, and the slot running it (-1 for none). */
data class DraftInfo(val name: String, val revision: Long, val slot: Int)

/** Mode indices of custom effects (firmware modes.hpp): the draft, then the library's by position. */
object EffectModes {
    const val BASE = 48
    const val DRAFT = BASE
    const val DRAFT_ID = "fx.draft"
}
