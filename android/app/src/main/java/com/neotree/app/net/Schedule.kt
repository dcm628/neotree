package com.neotree.app.net

import org.json.JSONArray
import org.json.JSONObject
import java.time.DayOfWeek
import java.time.LocalDate
import java.time.LocalDateTime
import java.time.LocalTime

/**
 * The tree's schedule (firmware engine/include/neotree/schedule.hpp): lights
 * on/off timers - like a plug-in Christmas light timer, by day of the week -
 * and events at set moments. Times are in the tree's local time.
 */
data class TreeSchedule(val timers: List<TimerInfo>, val events: List<EventInfo>) {
    companion object {
        const val MAX_TIMERS = 8
        const val MAX_EVENTS = 12

        fun parse(json: JSONObject): TreeSchedule {
            val t = json.optJSONArray("timers") ?: JSONArray()
            val e = json.optJSONArray("events") ?: JSONArray()
            return TreeSchedule(
                timers = (0 until t.length()).map { i ->
                    val o = t.getJSONObject(i)
                    TimerInfo(o.optInt("on", 1) == 1, o.optInt("d", EVERY_DAY), o.optInt("a"), o.optInt("b"))
                },
                events = (0 until e.length()).map { i ->
                    val o = e.getJSONObject(i)
                    EventInfo(
                        enabled = o.optInt("on", 1) == 1,
                        name = o.optString("n"),
                        repeat = Repeat.entries.getOrElse(o.optInt("r")) { Repeat.ONCE },
                        year = o.optInt("y", 2026),
                        month = o.optInt("m", 12),
                        day = o.optInt("day", 31),
                        days = o.optInt("d", EVERY_DAY),
                        timeSec = o.optInt("t"),
                        action = EventAction.entries.getOrElse(o.optInt("w")) { EventAction.SHOW },
                        target = o.optString("x"),
                        durationSec = o.optLong("len"),
                    )
                },
            )
        }
    }
}

/** Days of the week as a mask: bit 0 = Sunday ... bit 6 = Saturday. */
const val EVERY_DAY = 0x7F

fun dayBit(d: DayOfWeek): Int = 1 shl (d.value % 7)

/** Lights on at [onSec], off at [offSec] (seconds into the day), starting on [days]; an off at or before the on is the next day. */
data class TimerInfo(val enabled: Boolean = true, val days: Int = EVERY_DAY, val onSec: Int = 17 * 3600, val offSec: Int = 23 * 3600)

enum class Repeat(val label: String) { ONCE("Once"), YEARLY("Every year"), WEEKLY("Every week") }

enum class EventAction(val label: String) { PRESET("Scene"), SHOW("Show"), MODE("Mode") }

data class EventInfo(
    val enabled: Boolean = true,
    val name: String = "",
    val repeat: Repeat = Repeat.YEARLY,
    val year: Int = 2026,
    val month: Int = 12,
    val day: Int = 31,
    val days: Int = EVERY_DAY,
    val timeSec: Int = 23 * 3600 + 59 * 60,
    val action: EventAction = EventAction.SHOW,
    /** A scene or show name, or a mode id. */
    val target: String = "",
    /** Then back to what was playing before; 0 = it stays. */
    val durationSec: Long = 600,
)

/** The running event, from the scene JSON's "event". */
data class RunningEvent(val name: String, val index: Int, val leftSec: Long)

/** What the timer does next: [on] at [at] (local time). */
data class TimerChange(val on: Boolean, val at: LocalDateTime)

object TimerMath {
    /** Whether the timers say on at [t]: true / false, null if none is enabled (the firmware's timer_level). */
    fun level(timers: List<TimerInfo>, t: LocalDateTime): Boolean? {
        val active = timers.filter { it.enabled && it.days and EVERY_DAY != 0 }
        if (active.isEmpty()) return null
        return active.any { r -> windows(r, t.toLocalDate().minusDays(1), t.toLocalDate()).any { (s, e) -> !t.isBefore(s) && t.isBefore(e) } }
    }

    /** The timer's next change after [now] within a week, or null. */
    fun next(timers: List<TimerInfo>, now: LocalDateTime): TimerChange? {
        val current = level(timers, now) ?: return null
        val edges = timers.filter { it.enabled }
            .flatMap { r -> windows(r, now.toLocalDate().minusDays(1), now.toLocalDate().plusDays(8)).flatMap { listOf(it.first, it.second) } }
            .filter { it.isAfter(now) }
            .sorted()
        for (edge in edges) {
            val after = level(timers, edge) ?: return null
            if (after != current) return TimerChange(after, edge)
        }
        return null
    }

    private fun windows(r: TimerInfo, from: LocalDate, to: LocalDate): List<Pair<LocalDateTime, LocalDateTime>> {
        val out = mutableListOf<Pair<LocalDateTime, LocalDateTime>>()
        var d = from
        while (!d.isAfter(to)) {
            if (r.days and dayBit(d.dayOfWeek) != 0) {
                val start = d.atStartOfDay().plusSeconds(r.onSec.toLong())
                val end = d.atStartOfDay().plusSeconds(r.offSec.toLong() + if (r.offSec <= r.onSec) 86400 else 0)
                out += start to end
            }
            d = d.plusDays(1)
        }
        return out
    }
}

/** "5:00 PM", or with seconds when it has them ("11:59:30 PM"). */
fun formatTimeOfDay(sec: Int): String {
    val t = LocalTime.ofSecondOfDay((sec % 86400).toLong())
    val h = if (t.hour % 12 == 0) 12 else t.hour % 12
    val ampm = if (t.hour < 12) "AM" else "PM"
    return if (t.second != 0) "%d:%02d:%02d %s".format(h, t.minute, t.second, ampm) else "%d:%02d %s".format(h, t.minute, ampm)
}

private val DAY_LETTERS = listOf("Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat")

/** "Every day", "Weekdays", "Weekends", or "Mon, Wed, Fri". */
fun formatDays(days: Int): String = when (days and EVERY_DAY) {
    EVERY_DAY -> "Every day"
    0b0111110 -> "Weekdays"
    0b1000001 -> "Weekends"
    0 -> "No days"
    else -> (0..6).filter { days shr it and 1 == 1 }.joinToString(", ") { DAY_LETTERS[it] }
}

private val MONTHS = listOf("Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec")

/** "Every year on Dec 31 at 11:59 PM", "Once on Dec 31 2026 at ...", "Fri, Sat at ...". */
fun formatWhen(e: EventInfo): String {
    val date = MONTHS.getOrElse(e.month - 1) { "?" } + " " + e.day
    val at = formatTimeOfDay(e.timeSec)
    return when (e.repeat) {
        Repeat.ONCE -> "$date ${e.year} at $at"
        Repeat.YEARLY -> "Every $date at $at"
        Repeat.WEEKLY -> "${formatDays(e.days)} at $at"
    }
}
