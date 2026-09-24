package com.neotree.app.render

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import kotlin.math.cos
import kotlin.math.sin
import kotlin.math.tan

class ProjectionTest {
    private val width = 1000f
    private val height = 2000f
    private val out = FloatArray(3)

    private fun projection(camera: OrbitCamera) = Projection().apply { set(camera, width, height) }

    @Test
    fun targetIsAtScreenCenterAtCameraDistance() {
        val p = projection(OrbitCamera(yaw = 0.6f, pitch = 0.25f, distanceMm = 4500f, targetZMm = 900f))
        assertTrue(p.project(0f, 0f, 900f, out))
        assertEquals(width / 2, out[0], 1e-2f)
        assertEquals(height / 2, out[1], 1e-2f)
        assertEquals(4500f, out[2], 1e-2f)
    }

    /** Same eye position as the viewer: (x, z, -y) of its y-up camera position. */
    @Test
    fun eyeMatchesTheViewer() {
        val c = OrbitCamera(yaw = 0.6f, pitch = 0.25f, distanceMm = 4500f, targetZMm = 900f)
        val p = projection(c)
        // Continue the eye -> target line as far again past the target: dead center, twice as deep.
        val eyeX = c.distanceMm * cos(c.pitch) * sin(c.yaw)
        val eyeY = -c.distanceMm * cos(c.pitch) * cos(c.yaw)
        val eyeZ = c.targetZMm + c.distanceMm * sin(c.pitch)
        assertTrue(p.project(-eyeX, -eyeY, 2 * c.targetZMm - eyeZ, out))
        assertEquals(width / 2, out[0], 1e-2f)
        assertEquals(height / 2, out[1], 1e-2f)
        assertEquals(9000f, out[2], 1e-1f)
    }

    @Test
    fun screenDirectionsAndPerspective() {
        // yaw 0, level: the camera is on -y looking toward +y, so +x is right and +z is up.
        val p = projection(OrbitCamera(yaw = 0f, pitch = 0f, distanceMm = 1000f, targetZMm = 0f))
        assertTrue(p.project(100f, 0f, 0f, out))
        val right = out[0] - width / 2
        assertTrue(right > 0f)
        assertTrue(p.project(0f, 0f, 100f, out))
        assertTrue(out[1] < height / 2)
        // Twice as far away, half the offset.
        assertTrue(p.project(100f, 1000f, 0f, out))
        assertEquals(right / 2, out[0] - width / 2, 1e-2f)
        assertEquals(p.sizePx(12f, 1000f) / 2, p.sizePx(12f, 2000f), 1e-4f)
    }

    @Test
    fun verticalFieldOfViewIs45Degrees() {
        val p = projection(OrbitCamera(yaw = 0f, pitch = 0f, distanceMm = 1000f, targetZMm = 0f))
        val edge = 1000f * tan(Math.toRadians(22.5)).toFloat()
        assertTrue(p.project(0f, 0f, edge, out))
        assertEquals(0f, out[1], 1e-2f)   // top edge of the view
    }

    @Test
    fun pointsBehindTheCameraAreSkipped() {
        val p = projection(OrbitCamera(yaw = 0f, pitch = 0f, distanceMm = 1000f, targetZMm = 0f))
        assertFalse(p.project(0f, -1500f, 0f, out))
        assertFalse(p.project(0f, -1000f + Projection.NEAR_MM / 2, 0f, out))
    }

    @Test
    fun segmentsAreClippedAtTheNearPlane() {
        val p = projection(OrbitCamera(yaw = 0f, pitch = 0f, distanceMm = 1000f, targetZMm = 0f))
        val seg = FloatArray(4)
        // Entirely behind the camera.
        assertFalse(p.projectSegment(0f, -2000f, 0f, 100f, -1500f, 0f, seg))
        // From in front to behind: the far end is exact, the near end lands on the near plane.
        assertTrue(p.projectSegment(100f, 0f, 100f, 100f, -3000f, 100f, seg))
        assertTrue(p.project(100f, 0f, 100f, out))
        assertEquals(out[0], seg[0], 1e-2f)
        assertEquals(out[1], seg[1], 1e-2f)
        assertTrue(p.project(100f, -1000f + Projection.NEAR_MM, 100f, out))
        assertEquals(out[0], seg[2], 1e-1f)
        assertEquals(out[1], seg[3], 1e-1f)
    }

    @Test
    fun cameraControlsClamp() {
        val c = OrbitCamera(yaw = 0f, pitch = 0f, distanceMm = 4500f, targetZMm = 900f)
        assertEquals(OrbitCamera.MAX_PITCH, c.orbit(0f, 10f).pitch, 0f)
        assertEquals(-OrbitCamera.MAX_PITCH, c.orbit(0f, -10f).pitch, 0f)
        assertEquals(OrbitCamera.MIN_DISTANCE_MM, c.zoom(100f).distanceMm, 0f)
        assertEquals(OrbitCamera.MAX_DISTANCE_MM, c.zoom(0.01f).distanceMm, 0f)
        assertEquals(2250f, c.zoom(2f).distanceMm, 1e-3f)
        assertEquals(1800f, c.raise(5000f, 0f, 1800f).targetZMm, 0f)
        assertEquals(0f, c.raise(-5000f, 0f, 1800f).targetZMm, 0f)
        assertEquals(1000f, c.raise(100f, 0f, 1800f).targetZMm, 1e-3f)
    }

    @Test
    fun framingAimsAtMiddleOfTheTree() {
        val layout = LedLayout.parseCsv("index,z_mm,radius_mm,angle_deg,source\n0,-100,10,0,mapped\n1,1900,10,0,mapped")
        val c = OrbitCamera.framing(layout)
        assertEquals(900f, c.targetZMm, 1e-3f)
        assertEquals(4500f, c.distanceMm, 0f)
    }

    @Test
    fun pointCloudDrawsBackToFrontAndDropsHiddenPoints() {
        val layout = LedLayout.parseCsv(
            """
            index,z_mm,radius_mm,angle_deg,source
            0,0,500,90,mapped
            1,0,500,270,synthetic
            2,0,0,0,mapped
            3,0,3000,270,mapped
            """.trimIndent(),
        )
        // Camera on -y at 1000mm: LED 0 (+y, far), 2 (trunk), 1 (-y, near); 3 is behind the camera.
        val cloud = TreePointCloud(layout)
        cloud.update(OrbitCamera(yaw = 0f, pitch = 0f, distanceMm = 1000f, targetZMm = 0f), width, height)
        assertEquals(3, cloud.visibleCount)
        val order = (0 until cloud.visibleCount).map { cloud.ledIndex[cloud.backToFront(it)] }
        assertEquals(listOf(0, 2, 1), order)
        assertEquals(ViewerColors.MAPPED, cloud.sourceArgb[0])
        assertEquals(ViewerColors.SYNTHETIC, cloud.sourceArgb[1])
    }

    @Test
    fun heightColorsMatchRaylib() {
        // ColorFromHSV(240, 0.85, 1) and (0, 0.85, 1).
        assertEquals(0xFF2626FF.toInt(), ViewerColors.height(0f))
        assertEquals(0xFFFF2626.toInt(), ViewerColors.height(1f))
    }

    @Test
    fun colorViewsCycle() {
        assertEquals(ColorView.SOURCE, ColorView.OUTPUT.next())
        assertEquals(ColorView.HEIGHT, ColorView.SOURCE.next())
        assertEquals(ColorView.OUTPUT, ColorView.HEIGHT.next())
    }
}
