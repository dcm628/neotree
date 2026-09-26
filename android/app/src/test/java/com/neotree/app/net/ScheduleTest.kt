package com.neotree.app.net

import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.time.LocalDateTime

class ScheduleTest {
    private val fivePmToElevenPm = TimerInfo(onSec = 17 * 3600, offSec = 23 * 3600)

    @Test
    fun timerLevelAndNextChange() {
        val fri4pm = LocalDateTime.of(2026, 9, 25, 16, 0)
        assertEquals(false, TimerMath.level(listOf(fivePmToElevenPm), fri4pm))
        assertEquals(TimerChange(true, LocalDateTime.of(2026, 9, 25, 17, 0)), TimerMath.next(listOf(fivePmToElevenPm), fri4pm))
        val fri8pm = LocalDateTime.of(2026, 9, 25, 20, 0)
        assertEquals(TimerChange(false, LocalDateTime.of(2026, 9, 25, 23, 0)), TimerMath.next(listOf(fivePmToElevenPm), fri8pm))
        // Weekdays only: Friday night's next "on" is Monday.
        val weekdays = fivePmToElevenPm.copy(days = 0b0111110)
        assertEquals(TimerChange(true, LocalDateTime.of(2026, 9, 28, 17, 0)),
            TimerMath.next(listOf(weekdays), LocalDateTime.of(2026, 9, 25, 23, 30)))
        // Overnight, and two windows that touch: no change at the seam.
        val night = TimerInfo(onSec = 23 * 3600, offSec = 2 * 3600)
        assertEquals(true, TimerMath.level(listOf(night), LocalDateTime.of(2026, 9, 26, 1, 0)))
        assertEquals(TimerChange(false, LocalDateTime.of(2026, 9, 26, 2, 0)), TimerMath.next(listOf(fivePmToElevenPm, night), fri8pm))
        assertNull(TimerMath.level(listOf(fivePmToElevenPm.copy(enabled = false)), fri8pm))
        assertNull(TimerMath.level(emptyList(), fri8pm))
    }

    @Test
    fun parsesTheSchedule() {
        val s = TreeSchedule.parse(JSONObject(
            """{"rev":3,"timers":[{"on":1,"d":62,"a":61200,"b":82800}],"events":[{"on":1,"n":"New Year","r":1,"y":2026,
            "m":12,"day":31,"d":127,"t":86340,"w":1,"x":"Holiday evening","len":900}]}""",
        ))
        assertEquals(listOf(TimerInfo(true, 62, 61200, 82800)), s.timers)
        val e = s.events.single()
        assertEquals(Repeat.YEARLY, e.repeat)
        assertEquals(EventAction.SHOW, e.action)
        assertEquals("Holiday evening", e.target)
        assertEquals(900L, e.durationSec)
        assertEquals("Every Dec 31 at 11:59 PM", formatWhen(e))
        assertEquals("Weekdays", formatDays(62))
        assertEquals("Mon, Wed, Fri", formatDays(0b0101010))
        assertEquals("11:59:30 PM", formatTimeOfDay(86370))
        assertEquals("12:00 AM", formatTimeOfDay(0))
    }

    @Test
    fun sceneCarriesLightsTimerAndEvent() {
        val scene = SceneState.parse(JSONObject(
            """{"rev":1,"scene":"","slots":[],"base":[],"show":null,"lights":false,"timer":0,"event":{"n":"New Year","i":2,"left":95}}""",
        ))!!
        assertEquals(false, scene.lights)
        assertEquals(0, scene.timer)
        assertEquals(RunningEvent("New Year", 2, 95), scene.event)
    }

    @Test
    fun encodesTheScheduleCommands() {
        val t = TreeProtocol.scheduleTimer(0, TimerInfo(true, 62, 61200, 82800))
        assertEquals(12, t.size)
        assertEquals(listOf(49, 0, 1, 62), t.slice(0..3).map { it.toInt() })
        val tb = ByteBuffer.wrap(t).order(ByteOrder.LITTLE_ENDIAN)
        assertEquals(61200, tb.getInt(4))
        assertEquals(82800, tb.getInt(8))

        val e = TreeProtocol.scheduleEvent(1, EventInfo(name = "New Year", repeat = Repeat.YEARLY, timeSec = 86340,
            action = EventAction.MODE, target = "fx:A long effect name", durationSec = 900))
        assertEquals(62, e.size)
        val b = ByteBuffer.wrap(e).order(ByteOrder.LITTLE_ENDIAN)
        assertEquals(listOf(50, 1, 1, 1, 127), e.slice(0..4).map { it.toInt() and 0xFF })
        assertEquals(2026, b.getShort(5).toInt())
        assertEquals(12, e[7].toInt())
        assertEquals(31, e[8].toInt())
        assertEquals(86340, b.getInt(9))
        assertEquals(2, e[13].toInt())
        assertEquals(900, b.getInt(14))
        assertEquals("New Year", String(e, 18, 8, Charsets.UTF_8))
        assertEquals("fx:A long effect name", String(e, 38, 21, Charsets.UTF_8))
        assertEquals(listOf(51, 1, 3), TreeProtocol.deleteEvent(3).map { it.toInt() })
        assertEquals(listOf(52, 0xFF), TreeProtocol.runEvent(-1).map { it.toInt() and 0xFF })
    }
}
