package com.neotree.app.net

import org.junit.Assert.assertEquals
import org.junit.Test
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.time.Instant
import java.time.ZoneId

class PosixTimeZoneTest {
    private val now = Instant.parse("2026-09-25T17:00:00Z")

    /** The rule without its zone names (those come from the platform's locale data). */
    private fun rules(zone: String) = PosixTimeZone.of(ZoneId.of(zone), now).replace(Regex("<[^>]*>|[A-Za-z]{3,6}(?=[-+0-9,]|$)"), "N")

    @Test
    fun northernAndSouthernDaylightSaving() {
        assertEquals("N8N,M3.2.0,M11.1.0", rules("America/Los_Angeles"))
        assertEquals("N5N,M3.2.0,M11.1.0", rules("America/New_York"))
        assertEquals("N0N,M3.5.0/1,M10.5.0", rules("Europe/London"))
        assertEquals("N-1N,M3.5.0,M10.5.0/3", rules("Europe/Berlin"))
        assertEquals("N-10N,M10.1.0,M4.1.0/3", rules("Australia/Sydney"))
    }

    @Test
    fun noDaylightSaving() {
        assertEquals("N-5:30", rules("Asia/Kolkata"))
        assertEquals("N7", rules("America/Phoenix"))
        assertEquals("N0", rules("UTC"))
    }

    @Test
    fun namesAreLettersOrQuotedOffsets() {
        assertEquals("PST", PosixTimeZone.name("PST", java.time.ZoneOffset.ofHours(-8)))
        assertEquals("<+0530>", PosixTimeZone.name("GMT+05:30", java.time.ZoneOffset.ofHoursMinutes(5, 30)))
        assertEquals("<-03>", PosixTimeZone.name(null, java.time.ZoneOffset.ofHours(-3)))
    }

    @Test
    fun encodesTheClockCommands() {
        val set = TreeProtocol.timeSet(1_790_356_213_800L)
        assertEquals(9, set.size)
        assertEquals(47, set[0].toInt())
        assertEquals(1_790_356_213_800L, ByteBuffer.wrap(set, 1, 8).order(ByteOrder.LITTLE_ENDIAN).long)
        val zone = TreeProtocol.timeZone("PST8PDT,M3.2.0,M11.1.0")
        assertEquals(48, zone[0].toInt())
        assertEquals(22, zone[1].toInt())
        assertEquals("PST8PDT,M3.2.0,M11.1.0", String(zone, 2, 22, Charsets.US_ASCII))
    }
}
