package com.neotree.app.net

import org.json.JSONObject
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.nio.ByteBuffer
import java.nio.ByteOrder

class ModesTest {
    // Trimmed from a real DESCRIBE reply.
    private val describe = """
        {"modes":[{"id":"canvas","n":"Colors","s":"Your colors","p":[]},
        {"id":"solid","n":"Solid","s":"One color","p":[{"id":"color","l":"Color","t":"c","d":"#ff8c33"}]},
        {"id":"rainbow","n":"Rainbow","s":"Spinning","p":[{"id":"speed","l":"Spin (turns/s)","t":"n","min":-1,"max":1,"st":0.05,"d":0.2}]},
        {"id":"sweep","n":"Sweep","s":"A band","p":[{"id":"motion","l":"Motion","t":"ch","d":1,"ch":"up|down|around"},
        {"id":"backdrop","l":"Backdrop","t":"t","d":true}]}],
        "presets":["Colors","Snow on rainbow"]}
    """.trimIndent()

    @Test
    fun parsesTheCatalog() {
        val c = ModeCatalog.parse(JSONObject(describe))
        assertEquals(listOf("canvas", "solid", "rainbow", "sweep"), c.modes.map { it.id })
        assertEquals(2, c.byId("rainbow")!!.index)
        assertEquals(listOf("Colors", "Snow on rainbow"), c.presets)

        val color = c.byId("solid")!!.params.single()
        assertEquals(ParamType.COLOR, color.type)
        assertEquals(Rgb(0xff, 0x8c, 0x33), color.default.color)

        val speed = c.byId("rainbow")!!.params.single()
        assertEquals(ParamType.NUMBER, speed.type)
        assertEquals(-1f, speed.min)
        assertEquals(0.05f, speed.step)
        assertEquals(0.2f, speed.default.number)

        val (motion, backdrop) = c.byId("sweep")!!.params
        assertEquals(ParamType.CHOICE, motion.type)
        assertEquals(listOf("up", "down", "around"), motion.choices)
        assertEquals(1f, motion.default.number)
        assertEquals(ParamType.TOGGLE, backdrop.type)
        assertEquals(1f, backdrop.default.number)
        assertEquals(1, backdrop.index)
    }

    @Test
    fun parsesTheScene() {
        val status = JSONObject(
            """{"scene":{"scene":"Snow on rainbow","slots":[
            {"mode":"solid","i":1,"state":"running","age":12,"cycles":0,"loops":0,"params":["#102030"],
             "life":{"duration":30,"cycles":0,"policy":"chain","held":false}},
            {"mode":"","i":-1,"state":"empty","age":0,"cycles":0,"loops":0,"params":[],
             "life":{"duration":0,"cycles":0,"policy":"hold","held":false}}],
            "base":["canvas",""]}}""",
        )
        val scene = SceneState.parse(status.optJSONObject("scene"))!!
        assertEquals("Snow on rainbow", scene.name)
        assertEquals(2, scene.slots.size)
        val s = scene.slots[0]
        assertEquals(1, s.modeIndex)
        assertEquals(Rgb(0x10, 0x20, 0x30), s.params[0].color)
        assertEquals(30L, s.durationSec)
        assertEquals("chain", s.policy)
        assertTrue(scene.slots[1].empty)
        assertEquals(listOf("canvas", ""), scene.base)
    }

    @Test
    fun encodesModeCommands() {
        assertArrayEquals(byteArrayOf(24, 2, 10, 1), TreeProtocol.slotSet(2, 10, fade = true))
        assertArrayEquals(byteArrayOf(24, 1, 0xFF.toByte(), 0), TreeProtocol.slotSet(1, -1, fade = false))
        assertArrayEquals(byteArrayOf(26, 0xFF.toByte(), 1), TreeProtocol.revertScene())
        assertArrayEquals(byteArrayOf(29, 3), TreeProtocol.preset(3))

        val p = TreeProtocol.paramSet(1, 2, 0.75f, Rgb(1, 2, 3))
        assertEquals(10, p.size)
        assertEquals(25, p[0].toInt())
        assertEquals(0.75f, ByteBuffer.wrap(p, 3, 4).order(ByteOrder.LITTLE_ENDIAN).float)
        assertArrayEquals(byteArrayOf(1, 2, 3), p.copyOfRange(7, 10))
    }

    @Test
    fun parsesTheLibraryAndAPlayingShow() {
        val lib = TreeLibrary.parse(
            JSONObject(
                """{"rev":7,"presets":[{"n":"Colors"},{"n":"Mine","u":1}],
                "shows":[{"n":"Holiday evening","loop":1,"shuffle":0,"e":[[1,240],[-1,60]]},
                         {"n":"Ours","u":1,"loop":0,"shuffle":1,"e":[[0,0]]}],
                "boot":"Ours","base":{"custom":1,"slots":["rainbow","snow","",""]}}""",
            ),
        )
        assertEquals(listOf("Colors", "Mine"), lib.presets.map { it.name })
        assertTrue(lib.presets[1].user)
        assertEquals(2, lib.shows.size)
        assertEquals(ShowEntryInfo(-1, 60), lib.shows[0].entries[1])
        assertTrue(lib.shows[1].user && lib.shows[1].shuffle && !lib.shows[1].loop)
        assertEquals("Ours", lib.bootShow)
        assertTrue(lib.baseCustom)
        assertEquals("snow", lib.baseSlots[1])

        val scene = SceneState.parse(
            JSONObject(
                """{"rev":42,"scene":"Holiday show","slots":[],"base":[],
                "show":{"n":"Holiday evening","entry":"Holiday show","pos":1,"of":5,"round":2,"left":312}}""",
            ),
        )!!
        assertEquals(42L, scene.revision)
        assertEquals(ShowState("Holiday evening", "Holiday show", 1, 5, 2, 312), scene.show)
        assertEquals(null, SceneState.parse(JSONObject("""{"scene":"x","slots":[],"show":null}"""))!!.show)
    }

    @Test
    fun encodesLibraryCommands() {
        val save = TreeProtocol.savePreset("Snowy")
        assertEquals(22, save.size)
        assertArrayEquals(byteArrayOf(31, 1, 'S'.code.toByte()), save.copyOfRange(0, 3))
        assertEquals(0, save[7].toInt())   // NUL-padded after the name

        // Names stop at 19 bytes without splitting a character.
        assertEquals("abcdefghijklmnopqrs", TreeProtocol.storedName("abcdefghijklmnopqrstuvwxyz"))
        assertEquals("abcdefghijklmnopqé", TreeProtocol.storedName("abcdefghijklmnopqé!")) // é is 2 bytes: 17 + 2 = 19 fits
        assertEquals("abcdefghijklmnopqr", TreeProtocol.storedName("abcdefghijklmnopqré"))

        val show = TreeProtocol.saveShow("Night", listOf(1 to 240, 5 to 90), loop = true, shuffle = true)
        assertEquals(3 + 20 + 6, show.size)
        assertArrayEquals(byteArrayOf(33, 3, 2), show.copyOfRange(0, 3))
        assertArrayEquals(byteArrayOf(1, (240 and 0xFF).toByte(), 0, 5, 90, 0), show.copyOfRange(23, 29))

        assertArrayEquals(byteArrayOf(34, 0xFF.toByte()), TreeProtocol.playShow(-1))
        assertArrayEquals(byteArrayOf(35, 2), TreeProtocol.bootShow(2))
        assertArrayEquals(byteArrayOf(36, 3), TreeProtocol.subscribe())
        assertArrayEquals(byteArrayOf(32, 1, 6), TreeProtocol.deletePreset(6))

        val life = TreeProtocol.slotLife(1, 600, 0, TreeProtocol.EndPolicy.CHAIN, nextMode = 8)
        assertEquals(11, life.size)
        assertArrayEquals(byteArrayOf(27, 1, 0x58, 0x02, 0, 0, 1, 0, 8, 1, 15), life)
    }
}
