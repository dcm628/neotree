package com.neotree.app.render

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File

class LedLayoutTest {
    private val sample = """
        # a comment
        # another
        index,z_mm,radius_mm,angle_deg,source
        0,100.0,200.0,0.0,mapped
        1,500.0,100.0,90.0,synthetic
        3,1100.0,50.0,180.0,mapped
    """.trimIndent()

    @Test
    fun parsesAndConvertsLikeTheEngine() {
        val layout = LedLayout.parseCsv(sample)
        assertEquals(4, layout.count)   // highest index + 1
        assertEquals(200f, layout.xMm[0], 1e-3f)
        assertEquals(0f, layout.yMm[0], 1e-3f)
        assertEquals(100f, layout.zMm[0], 1e-3f)
        assertEquals(0f, layout.xMm[1], 1e-3f)   // 90 degrees: +y
        assertEquals(100f, layout.yMm[1], 1e-3f)
        assertEquals(-50f, layout.xMm[3], 1e-3f)  // 180 degrees: -x
        assertEquals(PositionSource.MAPPED, layout.source[0])
        assertEquals(PositionSource.SYNTHETIC, layout.source[1])
        assertEquals(PositionSource.NONE, layout.source[2])   // not in the file
        assertEquals(2, layout.mappedCount)
        assertEquals(1, layout.syntheticCount)
        assertEquals(listOf(0, 1, 3), layout.positioned.toList())
    }

    @Test
    fun heightRangeIgnoresUnpositionedLeds() {
        val layout = LedLayout.parseCsv(sample)
        assertEquals(100f, layout.minZMm, 0f)
        assertEquals(1100f, layout.maxZMm, 0f)
        assertEquals(0f, layout.height01(0), 1e-6f)
        assertEquals(0.4f, layout.height01(1), 1e-6f)
        assertEquals(1f, layout.height01(3), 1e-6f)
    }

    @Test
    fun acceptsWindowsLineEndings() {
        val layout = LedLayout.parseCsv(sample.replace("\n", "\r\n"))
        assertEquals(PositionSource.MAPPED, layout.source[3])
    }

    @Test(expected = IllegalArgumentException::class)
    fun rejectsIndexOutOfRange() {
        LedLayout.parseCsv("index,z_mm,radius_mm,angle_deg,source\n1000,0,0,0,mapped")
    }

    @Test(expected = IllegalArgumentException::class)
    fun rejectsUnknownSource() {
        LedLayout.parseCsv("index,z_mm,radius_mm,angle_deg,source\n0,0,0,0,guessed")
    }

    @Test(expected = IllegalArgumentException::class)
    fun rejectsBadNumber() {
        LedLayout.parseCsv("index,z_mm,radius_mm,angle_deg,source\n0,tall,0,0,mapped")
    }

    @Test(expected = IllegalArgumentException::class)
    fun rejectsEmptyFile() {
        LedLayout.parseCsv("# nothing here\nindex,z_mm,radius_mm,angle_deg,source\n")
    }

    /** The file the app bundles (copied from sim/data at build time) parses, and agrees with its own header comment. */
    @Test
    fun bundledPositionsFileParses() {
        // Unit tests run from the app module's directory.
        val file = File("../../sim/data/${LedLayout.ASSET_NAME}")
        assertTrue("missing ${file.absolutePath}", file.isFile)
        val text = file.readText()
        val layout = LedLayout.parseCsv(text)
        assertEquals(LedLayout.MAX_LEDS, layout.count)
        assertEquals(layout.positioned.size, layout.mappedCount + layout.syntheticCount)
        Regex("""# (\d+) mapped, (\d+) synthetic""").find(text)?.let {
            assertEquals(it.groupValues[1].toInt(), layout.mappedCount)
            assertEquals(it.groupValues[2].toInt(), layout.syntheticCount)
        }
    }
}
