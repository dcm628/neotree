package com.neotree.app

import com.neotree.app.net.AckStatus
import com.neotree.app.net.Rgb
import com.neotree.app.net.SceneState
import com.neotree.app.net.TreeConnection
import com.neotree.app.net.TreeProtocol
import com.neotree.app.net.TreeStream
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlin.random.Random

/**
 * The Play tab's side of the tree (docs/RENDERER.md 12, M7): the "play" mode
 * in a slot, this phone's brush and balls in it (direct control), and the
 * UDP stream that carries brush samples.
 *
 * Everything this phone makes is its own: the tree removes it when the
 * connection closes.
 */
class PlayController(
    private val scope: CoroutineScope,
    private val connection: TreeConnection,
    private val scene: StateFlow<TreeViewModel.LiveScene?>,
    private val setStatus: (String) -> Unit,
) {
    /** Brush settings; the fade belongs to the play mode (shared by everyone painting). */
    data class Brush(val color: PickerColor = PickerColor(40f, 1f, 1f), val radiusMm: Int = 90)

    private val _brush = MutableStateFlow(Brush())
    val brush: StateFlow<Brush> = _brush.asStateFlow()
    private val _randomBalls = MutableStateFlow(true)
    /** Each flicked ball a random bright color (else the brush color). */
    val randomBalls: StateFlow<Boolean> = _randomBalls.asStateFlow()

    /** This phone's own ball settings (how they look and move is the tree's, shared: the play mode's params). */
    data class Throw(val sizeMm: Int = 70, val strength: Float = 1f, val perFlick: Int = 1)

    private val _throw = MutableStateFlow(Throw())
    val throwSettings: StateFlow<Throw> = _throw.asStateFlow()

    fun setThrow(value: Throw) {
        _throw.value = value
    }

    private var stream: TreeStream? = null
    private var streamToken: Int? = null
    private var nextBall = 0

    init {
        scope.launch {
            connection.state.collect { s ->
                val token = (s as? TreeConnection.State.Connected)?.streamToken
                if (token != streamToken) {
                    stream?.close()
                    stream = if (s is TreeConnection.State.Connected && token != null) TreeStream(scope, s.host, token) else null
                    streamToken = token
                }
            }
        }
    }

    /** The slot running the play mode, or null. */
    fun playSlot(scene: SceneState?): Int? = scene?.slots?.indexOfFirst { it.mode == PLAY_MODE && !it.empty }?.takeIf { it >= 0 }

    /**
     * Puts the play mode in the topmost free slot (or the top slot) if it
     * isn't running - an ordinary scene edit, removable from the Modes tab.
     */
    fun ensurePlay() {
        val s = scene.value?.scene ?: return
        if (playSlot(s) != null) return
        val mode = connection.catalog.value?.byId(PLAY_MODE)?.index ?: run {
            setStatus("This tree's firmware has no Play mode - update it")
            return
        }
        val slot = (s.slots.indices.reversed().firstOrNull { it > 0 && s.slots[it].empty }) ?: (s.slots.size - 1)
        scope.launch { connection.send(TreeProtocol.slotSet(slot, mode, fade = false)) }
    }

    fun setBrush(value: Brush) {
        _brush.value = value
    }

    fun setRandomBalls(on: Boolean) {
        _randomBalls.value = on
    }

    /** The play mode's trail fade, seconds; 0 keeps strokes (painted on the Colors canvas). */
    fun fadeSeconds(): Float? {
        val s = scene.value?.scene ?: return null
        val slot = playSlot(s) ?: return null
        return s.slots[slot].params.getOrNull(FADE_PARAM)?.number
    }

    fun setFade(seconds: Float) {
        val slot = playSlot(scene.value?.scene) ?: return
        scope.launch { connection.send(TreeProtocol.paramSet(slot, FADE_PARAM, seconds, Rgb.BLACK)) }
    }

    /** A brush sample: over the stream when there is one (newest wins), else as a command. */
    fun brushAt(pos: FloatArray, penDown: Boolean) {
        val slot = playSlot(scene.value?.scene) ?: return
        val b = _brush.value
        val msg = TreeProtocol.brush(slot, BRUSH_ID, pos, b.color.toRgb(), b.radiusMm, penDown)
        val s = stream
        if (s != null) {
            s.send(msg)
        } else {
            scope.launch { connection.send(msg) }
        }
    }

    /**
     * Throws from [pos] at [vel] (mm, mm/s), scaled by this phone's throw
     * strength; several balls per flick fan out a little.
     */
    fun flick(pos: FloatArray, vel: FloatArray) {
        val slot = playSlot(scene.value?.scene) ?: return
        val t = _throw.value
        val speed = kotlin.math.sqrt(vel[0] * vel[0] + vel[1] * vel[1] + vel[2] * vel[2]) * t.strength
        val messages = (0 until t.perFlick).map { k ->
            val color = if (_randomBalls.value) {
                PickerColor(Random.nextFloat() * 360f, 0.9f + 0.1f * Random.nextFloat(), 1f).toRgb()
            } else {
                _brush.value.color.toRgb()
            }
            // The first ball goes where it was flicked; the rest scatter by up to ~20%.
            val scatter = if (k == 0) 0f else 0.2f * speed
            val v = FloatArray(3) { vel[it] * t.strength + scatter * (Random.nextFloat() * 2f - 1f) }
            val p = FloatArray(3) { pos[it] + if (k == 0) 0f else 40f * (Random.nextFloat() * 2f - 1f) }
            val id = nextBall
            nextBall = (nextBall + 1) % BALL_IDS
            TreeProtocol.spawnBall(slot, id, p, v, color, t.sizeMm)
        }
        scope.launch {
            for (m in messages) {
                if (connection.send(m) != AckStatus.QUEUED) {
                    setStatus("The tree didn't take the ball")
                    break
                }
            }
        }
    }

    /** Clears the trails (restarts the play mode - which also drops everyone's balls). */
    fun clearTrails() {
        val slot = playSlot(scene.value?.scene) ?: return
        scope.launch { connection.send(TreeProtocol.slotEnd(slot, TreeProtocol.SlotEnd.RESTART)) }
    }

    /** Removes this phone's balls. */
    fun clearMine() {
        scope.launch { connection.send(TreeProtocol.killEntity(-1)) }
    }

    fun close() {
        stream?.close()
        stream = null
    }

    companion object {
        const val PLAY_MODE = "play"
        const val FADE_PARAM = 0
        const val BRUSH_ID = 200
        const val BALL_IDS = 64
    }
}
