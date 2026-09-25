package com.neotree.app.net

import java.time.Instant
import java.time.LocalDate
import java.time.ZoneId
import java.time.ZoneOffset
import java.time.zone.ZoneOffsetTransitionRule
import java.time.zone.ZoneOffsetTransitionRule.TimeDefinition
import java.util.Locale
import java.util.TimeZone
import kotlin.math.abs

/**
 * The phone's time zone as a POSIX TZ rule (e.g. "PST8PDT,M3.2.0,M11.1.0"),
 * for the tree, which has no time zone database (firmware
 * engine/include/neotree/civil_time.hpp). Built from the zone's current
 * recurring daylight-saving rules, so the tree keeps local time through DST
 * changes on its own.
 */
object PosixTimeZone {
    fun of(zone: ZoneId = ZoneId.systemDefault(), now: Instant = Instant.now()): String {
        val rules = zone.rules
        val std = rules.getStandardOffset(now)
        val tz = TimeZone.getTimeZone(zone)
        val transitions = rules.transitionRules
        val start = transitions.firstOrNull { it.offsetAfter.totalSeconds > it.offsetBefore.totalSeconds }
        val end = transitions.firstOrNull { it.offsetAfter.totalSeconds < it.offsetBefore.totalSeconds }
        val stdPart = name(tz.getDisplayName(false, TimeZone.SHORT, Locale.US), std) + offset(std)
        if (start == null || end == null) return stdPart
        val dst = start.offsetAfter
        val dstPart = name(tz.getDisplayName(true, TimeZone.SHORT, Locale.US), dst) +
            (if (dst.totalSeconds == std.totalSeconds + 3600) "" else offset(dst))
        return "$stdPart$dstPart,${rule(start)},${rule(end)}"
    }

    /** A zone abbreviation if it's plain letters, else the offset quoted ("<+0530>"). */
    internal fun name(display: String?, offset: ZoneOffset): String {
        if (display != null && display.matches(Regex("[A-Za-z]{3,6}"))) return display
        val s = offset.totalSeconds
        val h = abs(s) / 3600
        val m = abs(s) % 3600 / 60
        return "<" + (if (s < 0) "-" else "+") + "%02d".format(h) + (if (m != 0) "%02d".format(m) else "") + ">"
    }

    /** POSIX offsets count west of UTC: UTC-8 is "8". */
    private fun offset(o: ZoneOffset): String = hms(-o.totalSeconds)

    private fun hms(seconds: Int): String {
        val a = abs(seconds)
        val h = a / 3600
        val m = a % 3600 / 60
        val s = a % 60
        return (if (seconds < 0) "-" else "") + h +
            (if (m != 0 || s != 0) ":%02d".format(m) else "") + (if (s != 0) ":%02d".format(s) else "")
    }

    /** "Mm.w.d[/time]" (or "Jn" for a fixed date), at the wall-clock time before the change. */
    internal fun rule(r: ZoneOffsetTransitionRule): String {
        val date = when (val dow = r.dayOfWeek) {
            null -> "J" + LocalDate.of(2001, r.month, r.dayOfMonthIndicator.coerceIn(1, r.month.length(false))).dayOfYear
            else -> {
                val dom = r.dayOfMonthIndicator
                val week = when {
                    dom < 0 -> 5                                  // counted back from the month's end
                    dom + 6 >= r.month.length(false) -> 5         // e.g. "Sunday on or after the 25th": the last
                    else -> (dom - 1) / 7 + 1
                }
                "M${r.month.value}.$week.${dow.value % 7}"
            }
        }
        var t = r.localTime.toSecondOfDay() + if (r.isMidnightEndOfDay) 86400 else 0
        t += when (r.timeDefinition) {
            TimeDefinition.UTC -> r.offsetBefore.totalSeconds
            TimeDefinition.STANDARD -> r.offsetBefore.totalSeconds - r.standardOffset.totalSeconds
            else -> 0
        }
        return if (t == 2 * 3600) date else "$date/${hms(t)}"
    }
}
