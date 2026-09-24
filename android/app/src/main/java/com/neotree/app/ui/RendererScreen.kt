package com.neotree.app.ui

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.gestures.calculateCentroidSize
import androidx.compose.foundation.gestures.calculatePan
import androidx.compose.foundation.gestures.calculateZoom
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.MutableState
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.Saver
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.input.pointer.positionChanged
import androidx.compose.ui.unit.dp
import com.neotree.app.TreeViewModel
import com.neotree.app.render.ColorView
import com.neotree.app.render.LedFrameSource
import com.neotree.app.render.LedLayout
import com.neotree.app.render.OrbitCamera
import com.neotree.app.render.TreePointCloud
import com.neotree.app.render.ViewerColors
import kotlin.math.abs

// The viewer's HUD colors (raylib RAYWHITE, LIGHTGRAY, GRAY) - the view is
// always dark, whatever the app theme.
private val HUD_TEXT = Color(0xFFF5F5F5)
private val HUD_DIM = Color(0xFFC8C8C8)
private val HUD_HINT = Color(0xFF828282)

// Orbit speed: the viewer's 0.008 rad per mouse pixel, per dp here.
private const val ORBIT_RAD_PER_DP = 0.008f

private val CameraSaver = Saver<OrbitCamera, FloatArray>(
    save = { floatArrayOf(it.yaw, it.pitch, it.distanceMm, it.targetZMm) },
    restore = { OrbitCamera(it[0], it[1], it[2], it[3]) },
)

/**
 * Renderer page: the tree's LEDs as a 3D point cloud, colored by engine
 * output (or position source / height) - the phone version of the desktop
 * viewer, sim/viewer/main.cpp.
 */
@Composable
fun RendererScreen(vm: TreeViewModel, contentPadding: PaddingValues) {
    val layout by vm.ledLayout.collectAsState()
    Box(
        modifier = Modifier
            .padding(contentPadding)
            .fillMaxSize()
            .background(Color(ViewerColors.BACKGROUND)),
    ) {
        val result = layout
        val loaded = result?.getOrNull()
        when {
            loaded != null -> TreeView(loaded, vm.rendererFrames)
            result == null -> Text("Loading LED positions…", color = HUD_DIM, modifier = Modifier.align(Alignment.Center))
            else -> Text(
                "Couldn't load LED positions: ${result.exceptionOrNull()?.message}",
                color = MaterialTheme.colorScheme.error,
                modifier = Modifier.align(Alignment.Center).padding(16.dp),
            )
        }
    }
}

/** The 3D view plus its overlay. Knows nothing about where [frameSource]'s frames come from. */
@Composable
private fun TreeView(layout: LedLayout, frameSource: LedFrameSource) {
    val camera = rememberSaveable(stateSaver = CameraSaver) { mutableStateOf(OrbitCamera.framing(layout)) }
    var colorView by rememberSaveable { mutableStateOf(ColorView.OUTPUT) }
    // Read only inside the Canvas, so camera moves and new frames redraw
    // without recomposing anything.
    val frame = frameSource.frames.collectAsState()
    val cloud = remember(layout) { TreePointCloud(layout) }

    Box(Modifier.fillMaxSize()) {
        Canvas(Modifier.fillMaxSize().treeGestures(camera, layout)) {
            cloud.update(camera.value, size.width, size.height)
            val projection = cloud.projection

            val lineWidth = 1.dp.toPx()
            val ends = cloud.lineScreen
            for (l in 0 until cloud.lineCount) {
                if (!cloud.lineVisible[l]) continue
                val o = l * 4
                drawLine(Color(cloud.lineArgb[l]), Offset(ends[o], ends[o + 1]), Offset(ends[o + 2], ends[o + 3]), lineWidth)
            }

            val view = colorView
            val rgb = frame.value?.rgb
            val minRadius = 0.75.dp.toPx()
            val maxRadius = 24.dp.toPx()
            for (k in 0 until cloud.visibleCount) {
                val s = cloud.backToFront(k)
                var radiusMm = ViewerColors.DOT_MM
                val argb = when (view) {
                    ColorView.OUTPUT -> {
                        // Linear engine output shown directly, as the viewer does.
                        val led = cloud.ledIndex[s]
                        val c = if (rgb != null && led < rgb.size) rgb[led] and 0xFFFFFF else 0
                        if (c == 0) {
                            radiusMm = ViewerColors.UNLIT_DOT_MM
                            ViewerColors.UNLIT
                        } else {
                            c or (0xFF shl 24)
                        }
                    }
                    ColorView.SOURCE -> cloud.sourceArgb[s]
                    ColorView.HEIGHT -> cloud.heightArgb[s]
                }
                drawCircle(
                    color = Color(argb),
                    radius = projection.sizePx(radiusMm, cloud.depth[s]).coerceIn(minRadius, maxRadius),
                    center = Offset(cloud.screenX[s], cloud.screenY[s]),
                )
            }
        }

        Column(Modifier.align(Alignment.TopStart).padding(12.dp)) {
            Text(
                "LEDs ${layout.count}: ${layout.mappedCount} mapped, ${layout.syntheticCount} synthetic",
                style = MaterialTheme.typography.bodyMedium,
                color = HUD_TEXT,
            )
            Text("colors: ${colorView.description}", style = MaterialTheme.typography.bodySmall, color = HUD_DIM)
            if (colorView == ColorView.OUTPUT) {
                Text("frames: ${frameSource.label}", style = MaterialTheme.typography.bodySmall, color = HUD_DIM)
            }
        }
        Row(
            modifier = Modifier.align(Alignment.BottomStart).fillMaxWidth().padding(12.dp),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
            verticalAlignment = Alignment.Bottom,
        ) {
            Text(
                "Drag to orbit · pinch to zoom · two-finger drag to raise · double-tap to reset",
                style = MaterialTheme.typography.bodySmall,
                color = HUD_HINT,
                modifier = Modifier.weight(1f),
            )
            FilledTonalButton(onClick = { colorView = colorView.next() }) {
                Text("Colors: ${colorView.label}")
            }
        }
    }
}

/**
 * Touch version of the viewer's mouse controls: one finger orbits, two
 * fingers pinch to zoom and drag up/down to move the view up and down the
 * tree, double-tap resets. Once a second finger lands, the gesture stays a
 * two-finger one until every finger lifts, so lifting one finger after a
 * pinch doesn't spin the tree.
 */
private fun Modifier.treeGestures(camera: MutableState<OrbitCamera>, layout: LedLayout): Modifier = this
    .pointerInput(camera, layout) {
        detectTapGestures(onDoubleTap = { camera.value = OrbitCamera.framing(layout) })
    }
    .pointerInput(camera, layout) {
        awaitEachGesture {
            awaitFirstDown(requireUnconsumed = false)
            var moving = false
            var twoFinger = false
            var slopPan = Offset.Zero
            var slopZoom = 1f
            do {
                val event = awaitPointerEvent()
                if (event.changes.any { it.isConsumed }) break
                val fingers = event.changes.count { it.pressed }
                if (fingers >= 2) twoFinger = true
                val pan = event.calculatePan()
                val zoom = event.calculateZoom()
                if (!moving) {
                    // Nothing moves until past touch slop, so taps stay taps.
                    slopPan += pan
                    slopZoom *= zoom
                    val slop = viewConfiguration.touchSlop
                    moving = slopPan.getDistance() > slop || abs(1f - slopZoom) * event.calculateCentroidSize() > slop
                }
                if (moving) {
                    val c = camera.value
                    if (fingers >= 2) {
                        // The tree follows the fingers: pan.y pixels at the target's depth, in mm.
                        val mmPerPx = c.distanceMm / OrbitCamera.focalLengthPx(size.height.toFloat())
                        camera.value = c.zoom(zoom).raise(pan.y * mmPerPx, layout.minZMm, layout.maxZMm)
                    } else if (!twoFinger) {
                        camera.value = c.orbit(-pan.x / density * ORBIT_RAD_PER_DP, pan.y / density * ORBIT_RAD_PER_DP)
                    }
                    event.changes.forEach { if (it.positionChanged()) it.consume() }
                }
            } while (event.changes.any { it.pressed })
        }
    }
