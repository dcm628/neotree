package com.neotree.app.render

import com.neotree.app.net.Rgb
import com.neotree.app.net.TreeProtocol
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import java.nio.ByteBuffer
import java.nio.ByteOrder
import kotlin.math.PI
import kotlin.math.sqrt

class TreeSurfaceTest {
    // A cone: 2 m tall, 600 mm radius at the bottom, 60 mm at the top.
    private val layout: LedLayout = run {
        val n = 800
        val x = FloatArray(n)
        val y = FloatArray(n)
        val z = FloatArray(n)
        for (i in 0 until n) {
            val h = 2000f * i / (n - 1)
            val r = 600f - 0.27f * h
            val a = Math.toRadians(137.5 * i)
            x[i] = (r * Math.cos(a)).toFloat()
            y[i] = (r * Math.sin(a)).toFloat()
            z[i] = h
        }
        LedLayout(x, y, z, Array(n) { PositionSource.MAPPED })
    }
    private val surface = TreeSurface(layout)

    @Test
    fun theEnvelopeFollowsTheCone() {
        assertEquals(600f - 0.27f * 1000f, surface.radiusAt(1000f), 40f)
        assertTrue(surface.radiusAt(200f) > surface.radiusAt(1800f))
    }

    @Test
    fun aRayHitsTheNearSideAndMissesPastTheEdge() {
        val hit = FloatArray(3)
        // From 3 m away on -y, level at 1 m, straight at the trunk.
        assertTrue(surface.hit(floatArrayOf(0f, -3000f, 1000f), floatArrayOf(0f, 1f, 0f), hit))
        assertEquals(0f, hit[0], 1f)
        assertEquals(-surface.radiusAt(1000f), hit[1], 10f)   // on the near surface, not the far one
        assertEquals(1000f, hit[2], 1f)
        // Well to the side of the tree: a miss.
        assertFalse(surface.hit(floatArrayOf(1500f, -3000f, 1000f), floatArrayOf(0f, 1f, 0f), hit))
        // Above the top: a miss.
        assertFalse(surface.hit(floatArrayOf(0f, -3000f, 2500f), floatArrayOf(0f, 1f, 0f), hit))
    }

    @Test
    fun theFrontFacesTheViewerAndRightIsTheirRight() {
        val p = FloatArray(3)
        val side = (-PI / 2).toFloat()   // standing on -y, looking at +y: their right is +x
        surface.front(side, 0f, 1000f, p)
        assertEquals(0f, p[0], 1f)
        assertEquals(-surface.radiusAt(1000f), p[1], 1f)
        surface.front(side, 150f, 1000f, p)
        assertTrue(p[0] > 140f)
        assertEquals(surface.radiusAt(1000f), sqrt(p[0] * p[0] + p[1] * p[1]), 1f)   // on the surface
        surface.front(side, 5000f, 5000f, p)   // clamped to the outline and the top
        assertEquals(surface.maxZMm, p[2], 1e-3f)
        assertEquals(surface.radiusAt(surface.maxZMm), sqrt(p[0] * p[0] + p[1] * p[1]), 1f)
    }

    @Test
    fun aimTurnsIntoOffsets() {
        val o = FloatArray(2)
        TreeSurface.aimOffsets(0.1f, -0.2f, 2500f, o)
        assertEquals(2500f * Math.tan(0.1).toFloat(), o[0], 0.5f)
        assertEquals(2500f * Math.tan(-0.2).toFloat(), o[1], 0.5f)
        assertEquals(-0.1f, TreeSurface.wrap((2 * PI).toFloat() - 0.1f), 1e-5f)
    }

    @Test
    fun aRayThroughAPixelProjectsBackToThatPixel() {
        val proj = Projection()
        proj.set(OrbitCamera(yaw = 0.9f, pitch = 0.3f, distanceMm = 4000f, targetZMm = 1000f), 1080f, 1600f)
        val o = FloatArray(3)
        val d = FloatArray(3)
        val out = FloatArray(3)
        for ((sx, sy) in listOf(540f to 800f, 100f to 300f, 1000f to 1500f)) {
            proj.ray(sx, sy, o, d)
            assertEquals(1f, sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]), 1e-4f)
            assertTrue(proj.project(o[0] + d[0] * 3000f, o[1] + d[1] * 3000f, o[2] + d[2] * 3000f, out))
            assertEquals(sx, out[0], 0.05f)
            assertEquals(sy, out[1], 0.05f)
        }
    }

    @Test
    fun directControlMessages() {
        val spawn = TreeProtocol.spawnBall(3, 7, floatArrayOf(10f, -20f, 1500f), floatArrayOf(-800f, 0f, 2000f), Rgb(1, 2, 3), 60)
        assertEquals(20, spawn.size)
        assertArrayEquals(byteArrayOf(38, 3, 7, 0), spawn.copyOfRange(0, 4))
        val b = ByteBuffer.wrap(spawn).order(ByteOrder.LITTLE_ENDIAN)
        assertEquals(-20, b.getShort(6).toInt())
        assertEquals(-800, b.getShort(10).toInt())
        assertArrayEquals(byteArrayOf(1, 2, 3, 60), spawn.copyOfRange(16, 20))

        val brush = TreeProtocol.brush(3, 200, floatArrayOf(0f, 0f, 900f), Rgb(255, 0, 0), 90, penDown = true)
        assertEquals(TreeProtocol.BRUSH_LEN, brush.size)
        assertArrayEquals(byteArrayOf(40, 3, 200.toByte(), 1), brush.copyOfRange(0, 4))

        val packet = TreeProtocol.streamPacket(0x12345678, 65535, brush)
        assertEquals(8 + TreeProtocol.BRUSH_LEN, packet.size)
        assertArrayEquals(byteArrayOf('N'.code.toByte(), 1, 0x78, 0x56, 0x34, 0x12, -1, -1), packet.copyOfRange(0, 8))
        assertArrayEquals(byteArrayOf(39, -1), TreeProtocol.killEntity(-1))
    }
}
