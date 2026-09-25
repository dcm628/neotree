package com.neotree.app.ui

import android.content.Context
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.gestures.calculatePan
import androidx.compose.foundation.gestures.calculateZoom
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.FilterChip
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Slider
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.MutableState
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.input.pointer.positionChange
import androidx.compose.ui.input.pointer.util.VelocityTracker
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import com.neotree.app.PickerColor
import com.neotree.app.PlayController
import com.neotree.app.TreeViewModel
import com.neotree.app.net.TreeConnection
import com.neotree.app.render.LedLayout
import com.neotree.app.render.OrbitCamera
import com.neotree.app.render.TreePointCloud
import com.neotree.app.render.TreeSurface
import com.neotree.app.render.ViewerColors
import kotlinx.coroutines.delay
import kotlin.math.asin
import kotlin.math.atan2
import kotlin.math.roundToInt
import kotlin.math.sqrt

private enum class PlayTool(val label: String, val hint: String) {
    PAINT("Paint", "Drag on the tree to paint · two fingers turn and zoom"),
    FLICK("Flick", "Swipe across the tree to throw a ball · two fingers turn and zoom"),
    AIM("Aim", "Point the top of the phone at the tree; hold the button to paint"),
}

// Strokes shown on the phone's view as they're sent (the tree has the real ones).
private class StrokeDot(val x: Float, val y: Float, val z: Float, val argb: Int, val atMs: Long)

private const val MAX_DOTS = 3000
private const val SAMPLE_MS = 16L                 // at most ~60 samples a second
private const val MIN_FLICK_DP_S = 250f
private const val FLICK_GAIN = 0.7f               // on-screen swipe speed -> ball speed
private const val MAX_BALL_SPEED = 9000f          // mm/s
private const val ORBIT_RAD_PER_DP = 0.008f

/**
 * Play page (docs/RENDERER.md 12, M7): paint on the tree and throw balls at
 * it - on the 3D view, or by pointing the phone at the tree. Strokes and
 * balls live in the tree's "play" mode, put in a free slot when this page
 * opens; everything this phone made goes when it disconnects.
 */
@Composable
fun PlayScreen(vm: TreeViewModel, contentPadding: PaddingValues) {
    val layout by vm.ledLayout.collectAsState()
    val state by vm.connection.state.collectAsState()
    val live by vm.scene.collectAsState()
    val catalog by vm.catalog.collectAsState()
    val status by vm.status.collectAsState()
    val play = vm.play
    val connected = state is TreeConnection.State.Connected
    val playSlot = play.playSlot(live?.scene)

    LaunchedEffect(connected, live?.scene != null, catalog != null) {
        if (connected) play.ensurePlay()
    }

    Column(Modifier.padding(contentPadding).fillMaxSize()) {
        if (!connected) {
            Text("Not connected to the tree.", modifier = Modifier.padding(12.dp))
        } else if (playSlot == null) {
            Text("Starting Play on the tree…", modifier = Modifier.padding(12.dp))
        }
        if (status.isNotEmpty()) {
            Text(status, style = MaterialTheme.typography.bodySmall, modifier = Modifier.padding(horizontal = 12.dp))
        }
        val loaded = layout?.getOrNull()
        if (loaded == null) {
            Text("Loading LED positions…", modifier = Modifier.padding(12.dp))
        } else {
            PlayArea(vm, loaded, enabled = connected && playSlot != null)
        }
    }
}

@Composable
private fun PlayArea(vm: TreeViewModel, layout: LedLayout, enabled: Boolean) {
    val play = vm.play
    val camera = remember(layout) { mutableStateOf(OrbitCamera.framing(layout)) }
    val cloud = remember(layout) { TreePointCloud(layout) }
    val surface = remember(layout) { TreeSurface(layout) }
    var tool by rememberSaveable { mutableStateOf(PlayTool.PAINT) }
    val brush by play.brush.collectAsState()
    val randomBalls by play.randomBalls.collectAsState()
    val dots = remember { ArrayDeque<StrokeDot>() }
    // Bumped to redraw when dots or the cursor change (they're plain objects).
    var redraw by remember { mutableLongStateOf(0L) }
    val cursor = remember { FloatArray(3) }
    var cursorShown by remember { mutableStateOf(false) }
    val live by vm.scene.collectAsState()
    val fadeS = play.fadeSeconds() ?: 4f

    fun sample(p: FloatArray, penDown: Boolean) {
        play.brushAt(p, penDown)
        cursor[0] = p[0]; cursor[1] = p[1]; cursor[2] = p[2]
        cursorShown = true
        if (penDown) {
            if (dots.size >= MAX_DOTS) dots.removeFirst()
            dots.addLast(StrokeDot(p[0], p[1], p[2], brush.color.toRgb().let { (0xFF shl 24) or (it.r shl 16) or (it.g shl 8) or it.b }, System.currentTimeMillis()))
        }
        redraw++
    }

    // Fading the preview along with the tree's trails.
    LaunchedEffect(Unit) {
        while (true) {
            delay(100)
            if (dots.isNotEmpty()) redraw++
        }
    }

    Column(Modifier.fillMaxSize()) {
        Box(Modifier.fillMaxWidth().weight(1f).background(Color(ViewerColors.BACKGROUND))) {
            Canvas(
                Modifier.fillMaxSize().playGestures(
                    tool = tool,
                    enabled = enabled,
                    camera = camera,
                    layout = layout,
                    cloud = cloud,
                    surface = surface,
                    onBrush = ::sample,
                    onBrushUp = {
                        if (cursorShown) play.brushAt(cursor, penDown = false)
                        cursorShown = false
                        redraw++
                    },
                    onFlick = { p, v -> play.flick(p, v) },
                ),
            ) {
                if (redraw < 0) return@Canvas   // read so new dots and cursor moves redraw
                cloud.update(camera.value, size.width, size.height)
                val projection = cloud.projection
                val minRadius = 0.75.dp.toPx()
                for (k in 0 until cloud.visibleCount) {
                    val s = cloud.backToFront(k)
                    drawCircle(
                        color = Color(ViewerColors.UNLIT),
                        radius = projection.sizePx(ViewerColors.UNLIT_DOT_MM, cloud.depth[s]).coerceAtLeast(minRadius),
                        center = Offset(cloud.screenX[s], cloud.screenY[s]),
                    )
                }
                val now = System.currentTimeMillis()
                val keep = fadeS <= 0f
                val scratch = FloatArray(3)
                while (!keep && dots.isNotEmpty() && now - dots.first().atMs > fadeS * 1000f) dots.removeFirst()
                for (d in dots) {
                    if (!projection.project(d.x, d.y, d.z, scratch)) continue
                    val alpha = if (keep) 1f else (1f - (now - d.atMs) / (fadeS * 1000f)).coerceIn(0f, 1f)
                    drawCircle(
                        color = Color(d.argb).copy(alpha = alpha),
                        radius = projection.sizePx(brush.radiusMm * 0.6f, scratch[2]).coerceAtLeast(2f),
                        center = Offset(scratch[0], scratch[1]),
                    )
                }
                if (cursorShown && projection.project(cursor[0], cursor[1], cursor[2], scratch)) {
                    drawCircle(
                        color = Color.White,
                        radius = projection.sizePx(brush.radiusMm.toFloat(), scratch[2]).coerceAtLeast(6f),
                        center = Offset(scratch[0], scratch[1]),
                        style = Stroke(2.dp.toPx()),
                    )
                }
            }
            Text(
                tool.hint,
                style = MaterialTheme.typography.bodySmall,
                color = Color(0xFFC8C8C8),
                modifier = Modifier.align(Alignment.TopStart).padding(10.dp),
            )
        }

        Column(
            Modifier.fillMaxWidth().heightIn(0.dp, 320.dp).verticalScroll(rememberScrollState()).padding(horizontal = 12.dp, vertical = 6.dp),
            verticalArrangement = Arrangement.spacedBy(4.dp),
        ) {
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                PlayTool.entries.forEach { t ->
                    FilterChip(selected = tool == t, onClick = { tool = t }, label = { Text(t.label) })
                }
            }
            if (tool == PlayTool.AIM) {
                AimPanel(play, surface, enabled) { p, pen -> sample(p, pen) }
            }
            if (tool != PlayTool.FLICK || !randomBalls) {
                Text("Color", style = MaterialTheme.typography.labelSmall)
                GradientBar(
                    value = brush.color.hue, range = 0f..360f,
                    gradient = (0..6).map { Color.hsv(it * 60f % 360f, 1f, 1f) },
                    onChange = { play.setBrush(brush.copy(color = PickerColor(it, 1f, 1f))) },
                )
            }
            if (tool != PlayTool.FLICK) {
                Text("Brush size ${brush.radiusMm} mm", style = MaterialTheme.typography.labelSmall)
                Slider(
                    value = brush.radiusMm.toFloat(), valueRange = 40f..200f,
                    onValueChange = { play.setBrush(brush.copy(radiusMm = it.roundToInt())) },
                )
                FadeRow(play, fadeS)
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    OutlinedButton(onClick = { play.clearTrails(); dots.clear(); redraw++ }) { Text("Clear trails") }
                }
            } else {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text("Random colors", Modifier.weight(1f))
                    Switch(checked = randomBalls, onCheckedChange = { play.setRandomBalls(it) })
                }
                OutlinedButton(onClick = { play.clearMine() }) { Text("Clear my balls") }
            }
            live?.scene?.let { s ->
                play.playSlot(s)?.let { Text("Play is in slot ${it + 1} - remove it from the Modes tab.", style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant) }
            }
        }
    }
}

@Composable
private fun FadeRow(play: PlayController, fadeS: Float) {
    // The slider moves freely; the tree hears about it when it settles.
    var local by remember { mutableFloatStateOf(fadeS) }
    var dragging by remember { mutableStateOf(false) }
    val shown = if (dragging) local else fadeS
    val keep = shown <= 0f
    Row(verticalAlignment = Alignment.CenterVertically) {
        Text(
            if (keep) "Strokes stay (on Colors)" else "Trails fade in ${"%.1f".format(shown)} s",
            style = MaterialTheme.typography.labelSmall,
            modifier = Modifier.weight(1f),
        )
        Text("Keep", style = MaterialTheme.typography.labelSmall)
        Switch(checked = keep, onCheckedChange = { play.setFade(if (it) 0f else 4f) })
    }
    if (!keep) {
        Slider(
            value = shown.coerceIn(0.5f, 30f), valueRange = 0.5f..30f,
            onValueChange = { dragging = true; local = it },
            onValueChangeFinished = { dragging = false; play.setFade((local * 2).roundToInt() / 2f) },
        )
    }
}

/**
 * Pointing the phone: the rotation sensor gives where its top edge points;
 * calibrating (pointing at the tree's centre) makes that the middle of the
 * tree. Standing distance and side place the aim on the tree's front.
 */
@Composable
private fun AimPanel(play: PlayController, surface: TreeSurface, enabled: Boolean, onSample: (FloatArray, Boolean) -> Unit) {
    val context = LocalContext.current
    var yaw by remember { mutableFloatStateOf(0f) }
    var pitch by remember { mutableFloatStateOf(0f) }
    var calYaw by rememberSaveable { mutableStateOf<Float?>(null) }
    var calPitch by rememberSaveable { mutableFloatStateOf(0f) }
    var distanceM by rememberSaveable { mutableFloatStateOf(2.5f) }
    var sideDeg by rememberSaveable { mutableFloatStateOf(270f) }
    var penDown by remember { mutableStateOf(false) }
    var noSensor by remember { mutableStateOf(false) }

    DisposableEffect(Unit) {
        val sm = context.getSystemService(Context.SENSOR_SERVICE) as SensorManager
        val sensor = sm.getDefaultSensor(Sensor.TYPE_GAME_ROTATION_VECTOR) ?: sm.getDefaultSensor(Sensor.TYPE_ROTATION_VECTOR)
        val matrix = FloatArray(9)
        val listener = object : SensorEventListener {
            override fun onSensorChanged(event: SensorEvent) {
                SensorManager.getRotationMatrixFromVector(matrix, event.values)
                // The phone's top edge (+y) in the world: x east, y north, z up.
                val px = matrix[1]
                val py = matrix[4]
                val pz = matrix[7]
                yaw = atan2(px, py)                       // clockwise from north: turning right increases it
                pitch = asin(pz.coerceIn(-1f, 1f))
            }

            override fun onAccuracyChanged(sensor: Sensor, accuracy: Int) = Unit
        }
        if (sensor == null) noSensor = true else sm.registerListener(listener, sensor, SensorManager.SENSOR_DELAY_GAME)
        onDispose { sm.unregisterListener(listener) }
    }

    // Stream the aim ~30 times a second once calibrated.
    LaunchedEffect(calYaw, enabled) {
        val offsets = FloatArray(2)
        val p = FloatArray(3)
        while (calYaw != null && enabled) {
            TreeSurface.aimOffsets(TreeSurface.wrap(yaw - calYaw!!), pitch - calPitch, distanceM * 1000f, offsets)
            surface.front(Math.toRadians(sideDeg.toDouble()).toFloat(), offsets[0], surface.centerZMm + offsets[1], p)
            onSample(p, penDown)
            delay(33)
        }
    }

    if (noSensor) {
        Text("This phone has no rotation sensor.", color = MaterialTheme.colorScheme.error)
        return
    }
    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        Button(onClick = { calYaw = yaw; calPitch = pitch }) { Text(if (calYaw == null) "Point at the centre, tap" else "Recentre") }
        HoldButton("Hold to paint", enabled = calYaw != null) { penDown = it }
    }
    Text("Standing ${"%.1f".format(distanceM)} m away", style = MaterialTheme.typography.labelSmall)
    Slider(value = distanceM, valueRange = 1f..6f, onValueChange = { distanceM = it })
    Text("Where you stand around the tree: ${sideDeg.roundToInt()}° (move until the glow follows your aim)",
        style = MaterialTheme.typography.labelSmall)
    Slider(value = sideDeg, valueRange = 0f..359f, onValueChange = { sideDeg = it })
}

/** A button that's "down" while a finger is on it. */
@Composable
private fun HoldButton(label: String, enabled: Boolean, onHold: (Boolean) -> Unit) {
    var held by remember { mutableStateOf(false) }
    Box(
        Modifier
            .height(48.dp)
            .background(
                if (!enabled) Color.Gray else if (held) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.primaryContainer,
                MaterialTheme.shapes.medium,
            )
            .pointerInput(enabled) {
                if (!enabled) return@pointerInput
                awaitEachGesture {
                    awaitFirstDown()
                    held = true
                    onHold(true)
                    do {
                        val event = awaitPointerEvent()
                    } while (event.changes.any { it.pressed })
                    held = false
                    onHold(false)
                }
            }
            .padding(horizontal = 20.dp),
        contentAlignment = Alignment.Center,
    ) {
        Text(label, fontWeight = FontWeight.Bold, color = if (held) MaterialTheme.colorScheme.onPrimary else MaterialTheme.colorScheme.onPrimaryContainer)
    }
}

/**
 * One finger does the tool's thing (paint, or flick a ball); two fingers turn
 * and zoom the view. Aim uses the phone, so there one finger turns the view.
 */
private fun Modifier.playGestures(
    tool: PlayTool,
    enabled: Boolean,
    camera: MutableState<OrbitCamera>,
    layout: LedLayout,
    cloud: TreePointCloud,
    surface: TreeSurface,
    onBrush: (FloatArray, Boolean) -> Unit,
    onBrushUp: () -> Unit,
    onFlick: (FloatArray, FloatArray) -> Unit,
): Modifier = pointerInput(tool, enabled, layout) {
    val origin = FloatArray(3)
    val dir = FloatArray(3)
    val hit = FloatArray(3)
    fun pick(at: Offset): Boolean {
        cloud.projection.ray(at.x, at.y, origin, dir)
        return surface.hit(origin, dir, hit)
    }
    awaitEachGesture {
        val down = awaitFirstDown(requireUnconsumed = false)
        cloud.update(camera.value, size.width.toFloat(), size.height.toFloat())
        val tracker = VelocityTracker()
        tracker.addPosition(down.uptimeMillis, down.position)
        var twoFinger = false
        var lastSample = 0L
        var flickFrom: FloatArray? = null
        val oneFingerTool = enabled && tool != PlayTool.AIM
        if (oneFingerTool && pick(down.position)) {
            if (tool == PlayTool.PAINT) {
                onBrush(hit.copyOf(), true)
                lastSample = down.uptimeMillis
            } else {
                flickFrom = hit.copyOf()
            }
        }
        do {
            val event = awaitPointerEvent()
            val fingers = event.changes.count { it.pressed }
            if (fingers >= 2 && !twoFinger) {
                twoFinger = true
                if (tool == PlayTool.PAINT) onBrushUp()
                flickFrom = null
            }
            if (twoFinger || !oneFingerTool) {
                val pan = event.calculatePan()
                camera.value = camera.value.orbit(-pan.x / density * ORBIT_RAD_PER_DP, pan.y / density * ORBIT_RAD_PER_DP)
                    .zoom(event.calculateZoom())
                cloud.update(camera.value, size.width.toFloat(), size.height.toFloat())
            } else {
                val change = event.changes.first()
                tracker.addPosition(change.uptimeMillis, change.position)
                if (tool == PlayTool.PAINT && change.pressed && change.uptimeMillis - lastSample >= SAMPLE_MS) {
                    if (pick(change.position)) {
                        onBrush(hit.copyOf(), true)
                    } else {
                        onBrushUp()   // off the tree: lift the pen
                    }
                    lastSample = change.uptimeMillis
                }
            }
            event.changes.forEach { if (it.positionChange() != Offset.Zero) it.consume() }
        } while (event.changes.any { it.pressed })

        if (!twoFinger && oneFingerTool) {
            if (tool == PlayTool.PAINT) {
                onBrushUp()
            } else {
                val from = flickFrom
                val v = tracker.calculateVelocity()
                val speedDp = sqrt(v.x * v.x + v.y * v.y) / density
                if (from != null && speedDp >= MIN_FLICK_DP_S) {
                    // The swipe's direction and speed on screen, at the ball's
                    // depth, plus a push into the tree.
                    val depth = sqrt((from[0] - origin[0]).let { it * it } + (from[1] - origin[1]).let { it * it } +
                        (from[2] - origin[2]).let { it * it })
                    val vel = FloatArray(3)
                    cloud.projection.screenToWorldVelocity(v.x * FLICK_GAIN, v.y * FLICK_GAIN, depth, vel)
                    val fwd = FloatArray(3)
                    cloud.projection.forward(fwd)
                    val speed = sqrt(vel[0] * vel[0] + vel[1] * vel[1] + vel[2] * vel[2])
                    for (k in 0 until 3) vel[k] += fwd[k] * 0.3f * speed
                    val total = sqrt(vel[0] * vel[0] + vel[1] * vel[1] + vel[2] * vel[2])
                    if (total > MAX_BALL_SPEED) for (k in 0 until 3) vel[k] *= MAX_BALL_SPEED / total
                    onFlick(from, vel)
                }
            }
        }
    }
}
