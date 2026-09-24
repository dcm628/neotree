package com.neotree.app.render

import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

/**
 * One frame of engine output for the Renderer page: 0xRRGGBB per LED (the
 * top byte is ignored), array index = LED number. LEDs past the end of [rgb]
 * are dark, so an empty frame is an all-dark tree.
 *
 * Don't change [rgb] after the frame is emitted - the page may be drawing
 * it. Emit a new LedFrame (a new object, so the StateFlow sees a change) for
 * every frame; recycling a few arrays in rotation is fine.
 */
class LedFrame(val rgb: IntArray) {
    companion object {
        fun dark(ledCount: Int) = LedFrame(IntArray(ledCount))
    }
}

/**
 * Where the Renderer page's "engine output" colors come from. The page only
 * reads [frames], and only collects it while it's on screen - so a source
 * that streams from the tree, built with
 * `stateIn(scope, SharingStarted.WhileSubscribed(...), null)`, streams only
 * while someone is looking at the page.
 */
interface LedFrameSource {
    /** Short description for the page's overlay, e.g. "live from the tree". */
    val label: String

    /** The newest frame; null when there's none (not streaming yet) - drawn as all dark. */
    val frames: StateFlow<LedFrame?>
}

/**
 * What the engine outputs today: every LED dark, because it has no scene
 * content yet (docs/RENDERER.md, M1). Stands in until frames are streamed
 * from the tree.
 */
class DarkFrameSource(ledCount: Int) : LedFrameSource {
    override val label = "all dark - the engine has no scenes yet"
    override val frames: StateFlow<LedFrame?> = MutableStateFlow(LedFrame.dark(ledCount)).asStateFlow()
}
