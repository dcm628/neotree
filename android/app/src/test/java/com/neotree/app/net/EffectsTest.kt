package com.neotree.app.net

import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.nio.ByteBuffer
import java.nio.ByteOrder

class EffectsTest {
    // Trimmed from a real FX_SCHEMA reply (things).
    private val thingsSchema = """
        {"s":2,"id":"things","l":"Things","item":"Thing","max":8,"f":[
        {"id":"shape","l":"Shape","t":"ch","ch":"ball|slab|bubble|capsule|wedge"},
        {"id":"size","l":"Size (mm)","t":"n","min":5,"max":1000,"st":5,"by":0,"when":15},
        {"id":"width","l":"Width (deg)","t":"n","min":1,"max":180,"st":1,"by":0,"when":16},
        {"id":"thickness","l":"Thickness (mm)","t":"n","min":1,"max":300,"st":1,"by":0,"when":4},
        {"id":"color","l":"Color","t":"c"},
        {"id":"falloff","l":"Edge","t":"ch","ch":"hard|linear|smooth|glow"},
        {"id":"edge","l":"Edge width (mm)","t":"n","max":500,"st":5,"by":5,"when":14},
        {"id":"surface","l":"Stays on the surface","t":"t"}]}
    """.trimIndent()

    @Test
    fun parsesASchemaSection() {
        val s = FxSectionSchema.parse(JSONObject(thingsSchema))!!
        assertEquals(EffectSection.THINGS, s.section)
        assertEquals("Thing", s.item)
        assertEquals(8, s.maxItems)
        val size = s.field("size")!!
        assertEquals(ParamType.NUMBER, size.param.type)
        assertEquals(5f, size.param.min)
        assertEquals(1000f, size.param.max)
        assertEquals(0, size.shownBy)
        assertEquals(listOf("hard", "linear", "smooth", "glow"), s.field("falloff")!!.param.choices)
        assertEquals(ParamType.TOGGLE, s.field("surface")!!.param.type)
        assertEquals(-1, s.field("color")!!.shownBy)
    }

    @Test
    fun showsOnlyTheFieldsThatApply() {
        val s = FxSectionSchema.parse(JSONObject(thingsSchema))!!
        fun values(shape: Int, falloff: Int) = List(8) { k ->
            when (k) {
                0 -> ParamValue(number = shape.toFloat())
                5 -> ParamValue(number = falloff.toFloat())
                else -> ParamValue()
            }
        }
        val ball = values(shape = 0, falloff = 2)
        assertTrue(s.shown(s.field("size")!!, ball))
        assertFalse(s.shown(s.field("width")!!, ball))
        assertFalse(s.shown(s.field("thickness")!!, ball))
        assertTrue(s.shown(s.field("edge")!!, ball))
        val wedge = values(shape = 4, falloff = 0)
        assertFalse(s.shown(s.field("size")!!, wedge))
        assertTrue(s.shown(s.field("width")!!, wedge))
        assertFalse(s.shown(s.field("edge")!!, wedge))   // a hard edge has no width
        assertTrue(s.shown(s.field("thickness")!!, values(shape = 2, falloff = 0)))
    }

    @Test
    fun parsesDraftSectionsWithActionsKeyedByRule() {
        val things = FxSectionData.parse(JSONObject("""{"s":2,"n":"Popcorn","i":[[0,110,"#ff1aff"],[1,20,"#000000"]]}"""))!!
        assertEquals("Popcorn", things.name)
        assertEquals(listOf(0, 1), things.items.map { it.index })
        assertEquals(110f, things.items[0].values[1].number)
        assertEquals(Rgb(0xff, 0x1a, 0xff), things.items[0].values[2].color)

        val actions = FxSectionData.parse(JSONObject("""{"s":6,"n":"x","i":[[0,0,5,1],[2,1,7,0]]}"""))!!
        assertEquals(EffectSection.ACTIONS, actions.section)
        val (a, b) = actions.items
        assertEquals(0, a.rule)
        assertEquals(0, a.index)
        assertEquals(listOf(5f, 1f), a.values.map { it.number })
        assertEquals(2, b.rule)
        assertEquals(1, b.action)
        assertEquals(2 * ACTIONS_PER_RULE + 1, b.index)
        assertNull(FxSectionData.parse(JSONObject("""{"s":99,"i":[]}""")))
    }

    @Test
    fun effectsJoinTheCatalogAtTheirOwnIndices() {
        val lib = TreeLibrary.parse(JSONObject(
            """{"presets":[],"shows":[],"boot":"","base":{"custom":0,"slots":[]},"fx":[{"n":"Popcorn","k":0,"i":49},{"n":"Fizz","k":2,"i":51}]}""",
        ))
        assertEquals(listOf(EffectInfo(0, "Popcorn", 49), EffectInfo(2, "Fizz", 51)), lib.effects)

        val scene = SceneState.parse(JSONObject(
            """{"rev":3,"scene":"","slots":[{"mode":"fx.draft","i":48,"state":"running"}],"base":[],"show":null,"fx":{"n":"New effect","rev":7,"slot":0}}""",
        ))!!
        assertEquals(DraftInfo("New effect", 7, 0), scene.draft)

        val builtIns = ModeCatalog.parse(JSONObject("""{"modes":[{"id":"canvas","n":"Colors","p":[]},{"id":"snow","n":"Snow","p":[]}],"presets":[]}"""))
        val c = builtIns.withEffects(lib.effects, scene.draft)
        assertEquals("Popcorn", c.mode(49)!!.name)
        assertEquals("fx:Fizz", c.mode(51)!!.id)
        assertEquals("Snow", c.mode(1)!!.name)
        assertEquals("Editing: New effect", c.mode(EffectModes.DRAFT)!!.name)
        assertFalse(c.mode(EffectModes.DRAFT)!!.pickable)
        assertNull(c.mode(50))
        // Again with a newer library: no duplicates.
        assertEquals(c.modes.size, c.withEffects(lib.effects, scene.draft).modes.size)
    }

    @Test
    fun encodesTheEffectCommands() {
        val set = TreeProtocol.fxSet(EffectSection.THINGS.code, 3, 5, 1.5f, Rgb(10, 20, 30))
        assertEquals(11, set.size)
        assertEquals(43, set[0].toInt())
        assertEquals(listOf(2, 3, 5), set.slice(1..3).map { it.toInt() })
        assertEquals(1.5f, ByteBuffer.wrap(set, 4, 4).order(ByteOrder.LITTLE_ENDIAN).float)
        assertEquals(listOf(10, 20, 30), set.slice(8..10).map { it.toInt() })

        assertEquals(listOf(42, 1, 0xFF), TreeProtocol.fxEdit(1, -1).map { it.toInt() and 0xFF })
        assertEquals(listOf(44, 2, 6, 9), TreeProtocol.fxItem(TreeProtocol.FxOp.DUPLICATE, 6, 9).map { it.toInt() })
        assertEquals(listOf(45, 5), TreeProtocol.fxGet(5).map { it.toInt() })
        assertEquals(21, TreeProtocol.fxSave("Popcorn").size)
        assertEquals(listOf(32, 3, 2), TreeProtocol.deleteEffect(2).map { it.toInt() })
    }
}
