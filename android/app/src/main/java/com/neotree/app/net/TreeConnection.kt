package com.neotree.app.net

import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeoutOrNull
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.net.InetSocketAddress
import java.net.Socket
import java.nio.ByteBuffer
import java.nio.ByteOrder
import org.json.JSONObject

/** A STATUS reply: the tree's JSON snapshot (firmware neo_tree_status.hpp). */
data class TreeStatus(val json: JSONObject, val receivedAtMs: Long)

/** One line of the app-side debug log: a command sent, or a connection event. */
data class LogEntry(
    val timeMs: Long,
    val text: String,
    val ok: Boolean,
)

/**
 * One TCP connection to the tree's command server
 * (firmware/include/neo_tree_net_server.hpp). Frames are
 * [uint16 LE length][payload] both ways: the tree greets with HELLO, then
 * answers every command with an ACK carrying an AckStatus. A STATUS_REQUEST
 * also gets a STATUS frame, and a DESCRIBE a DESCRIBE frame, just before the
 * ACK. After SUBSCRIBE the tree also pushes SCENE and LIBRARY frames whenever
 * they change - at any time, including between a command and its ACK.
 */
class TreeConnection(private val scope: CoroutineScope) {

    sealed interface State {
        data object Disconnected : State
        data class Connecting(val host: String) : State
        /**
         * lightsOn is null if the tree's firmware doesn't report it; so is
         * streamToken (this connection's key on the UDP stream, TreeStream).
         */
        data class Connected(val host: String, val port: Int, val lightsOn: Boolean?, val streamToken: Int? = null) : State
        data class Failed(val host: String, val reason: String) : State
    }

    private val _state = MutableStateFlow<State>(State.Disconnected)
    val state: StateFlow<State> = _state.asStateFlow()

    private val _status = MutableStateFlow<TreeStatus?>(null)
    /** Latest STATUS reply (null until one arrives). */
    val status: StateFlow<TreeStatus?> = _status.asStateFlow()

    private val _catalog = MutableStateFlow<ModeCatalog?>(null)
    /** The tree's modes and presets, from its latest DESCRIBE reply (null until one arrives). */
    val catalog: StateFlow<ModeCatalog?> = _catalog.asStateFlow()

    /** The scene as last pushed (after SUBSCRIBE), with when it arrived. */
    data class PushedScene(val scene: SceneState, val receivedAtMs: Long)
    private val _scene = MutableStateFlow<PushedScene?>(null)
    val scene: StateFlow<PushedScene?> = _scene.asStateFlow()

    private val _library = MutableStateFlow<TreeLibrary?>(null)
    /** The tree's library, from its latest LIBRARY reply or push. */
    val library: StateFlow<TreeLibrary?> = _library.asStateFlow()

    private val _schedule = MutableStateFlow<TreeSchedule?>(null)
    /** The schedule, sent after every LIBRARY frame (so pushed with it). */
    val schedule: StateFlow<TreeSchedule?> = _schedule.asStateFlow()

    private val _fxSchema = MutableStateFlow<Map<EffectSection, FxSectionSchema>>(emptyMap())
    /** The custom-effect schema, by section, as FX_SCHEMA replies arrive. */
    val fxSchema: StateFlow<Map<EffectSection, FxSectionSchema>> = _fxSchema.asStateFlow()

    private val _fxDraft = MutableStateFlow<Map<EffectSection, FxSectionData>>(emptyMap())
    /** The draft effect, by section, as FX_GET replies arrive. */
    val fxDraft: StateFlow<Map<EffectSection, FxSectionData>> = _fxDraft.asStateFlow()

    /** Shows an edit here before the tree's next reply confirms it. */
    fun updateFxDraft(section: EffectSection, change: (FxSectionData) -> FxSectionData) {
        _fxDraft.update { m -> m[section]?.let { m + (section to change(it)) } ?: m }
    }

    private val _log = MutableStateFlow<List<LogEntry>>(emptyList())
    /** Recent commands and connection events, newest first. */
    val log: StateFlow<List<LogEntry>> = _log.asStateFlow()

    private fun log(text: String, ok: Boolean = true) {
        // update{} is atomic - sends, the reader and connects all log from different threads.
        _log.update { (listOf(LogEntry(System.currentTimeMillis(), text, ok)) + it).take(LOG_MAX) }
    }

    private var socket: Socket? = null
    private var output: OutputStream? = null
    private var readerJob: Job? = null
    private var acks = Channel<Pair<Int, AckStatus?>>(Channel.UNLIMITED)
    // One command in flight at a time, so ACKs pair up with their commands.
    private val sendLock = Mutex()

    suspend fun connect(host: String, port: Int = TreeProtocol.DEFAULT_PORT): Boolean =
        withContext(Dispatchers.IO) {
            close()
            _state.value = State.Connecting(host)
            try {
                val s = Socket()
                s.tcpNoDelay = true
                s.connect(InetSocketAddress(host, port), CONNECT_TIMEOUT_MS)
                s.soTimeout = HELLO_TIMEOUT_MS
                val input = s.getInputStream()
                val hello = readFrame(input)
                if (hello.size < 2 || (hello[0].toInt() and 0xFF) != TreeProtocol.REPLY_HELLO) {
                    s.close()
                    _state.value = State.Failed(host, "not a NeoTree (unexpected greeting)")
                    return@withContext false
                }
                val version = hello[1].toInt() and 0xFF
                if (version != TreeProtocol.PROTOCOL_VERSION) {
                    s.close()
                    _state.value = State.Failed(host, "tree speaks protocol v$version, app v${TreeProtocol.PROTOCOL_VERSION}")
                    return@withContext false
                }
                s.soTimeout = 0
                socket = s
                output = s.getOutputStream()
                // Another tree, or new firmware: ask again.
                _fxSchema.value = emptyMap()
                _fxDraft.value = emptyMap()
                acks = Channel(Channel.UNLIMITED)
                readerJob = scope.launch(Dispatchers.IO) { readLoop(s, input, host) }
                val lightsOn = if (hello.size >= 3) (hello[2].toInt() and TreeProtocol.HELLO_FLAG_OUTPUT_ON) != 0 else null
                val token = if (hello.size >= 7) {
                    ByteBuffer.wrap(hello, 3, 4).order(ByteOrder.LITTLE_ENDIAN).int
                } else {
                    null
                }
                _state.value = State.Connected(host, port, lightsOn, token)
                log("connected to $host:$port (protocol v$version)")
                true
            } catch (e: IOException) {
                val reason = e.message ?: e.javaClass.simpleName
                _state.value = State.Failed(host, reason)
                log("connect to $host failed: $reason", ok = false)
                false
            }
        }

    /**
     * Sends one command and waits for its ACK. QUEUE_FULL means the tree is
     * momentarily behind (it drains commands between ~30ms LED frames), so
     * back off and resend. Returns null if the connection failed.
     */
    suspend fun send(message: ByteArray): AckStatus? = sendLock.withLock {
        withContext(Dispatchers.IO) {
            val name = TreeProtocol.typeName(message.firstOrNull()?.toInt()?.and(0xFF) ?: -1)
            val started = System.nanoTime()
            fun rttMs() = (System.nanoTime() - started) / 1_000_000
            repeat(QUEUE_FULL_RETRIES + 1) { attempt ->
                val out = output ?: run {
                    log("$name (${message.size}B): not connected", ok = false)
                    return@withContext null
                }
                try {
                    val len = message.size
                    out.write(byteArrayOf((len and 0xFF).toByte(), (len shr 8).toByte()) + message)
                    out.flush()
                } catch (e: IOException) {
                    log("$name (${message.size}B): send failed", ok = false)
                    fail("send failed: ${e.message}")
                    return@withContext null
                }
                val ack = withTimeoutOrNull(ACK_TIMEOUT_MS) { acks.receive() }
                if (ack == null) {
                    log("$name (${message.size}B): no ACK in ${ACK_TIMEOUT_MS}ms", ok = false)
                    fail("tree stopped answering")
                    return@withContext null
                }
                val status = ack.second
                if (status != AckStatus.QUEUE_FULL) {
                    // The debug page polls STATUS every second - only log those when they fail.
                    val routinePoll = name == "STATUS" && status == AckStatus.QUEUED
                    if (!routinePoll) {
                        val retries = if (attempt > 0) ", $attempt queue-full retries" else ""
                        log("$name (${message.size}B): $status in ${rttMs()}ms$retries", ok = status == AckStatus.QUEUED)
                    }
                    return@withContext status
                }
                delay(5L * (attempt + 1))
            }
            log("$name (${message.size}B): QUEUE_FULL after $QUEUE_FULL_RETRIES retries", ok = false)
            AckStatus.QUEUE_FULL
        }
    }

    fun close() {
        readerJob?.cancel()
        readerJob = null
        runCatching { socket?.close() }
        socket = null
        output = null
        _state.value = State.Disconnected
    }

    private fun fail(reason: String) {
        val host = (state.value as? State.Connected)?.host ?: return
        close()
        _state.value = State.Failed(host, reason)
        log("connection to $host dropped: $reason", ok = false)
    }

    private suspend fun readLoop(s: Socket, input: InputStream, host: String) {
        try {
            while (true) {
                val frame = readFrame(input)
                val type = frame.firstOrNull()?.toInt()?.and(0xFF)
                if (frame.size == 3 && type == TreeProtocol.REPLY_ACK) {
                    acks.send((frame[1].toInt() and 0xFF) to AckStatus.from(frame[2].toInt() and 0xFF))
                } else if (type == TreeProtocol.REPLY_STATUS) {
                    runCatching { JSONObject(String(frame, 1, frame.size - 1, Charsets.UTF_8)) }
                        .onSuccess { _status.value = TreeStatus(it, System.currentTimeMillis()) }
                        .onFailure { log("STATUS reply wasn't valid JSON (${frame.size}B)", ok = false) }
                } else if (type == TreeProtocol.REPLY_DESCRIBE) {
                    runCatching { ModeCatalog.parse(JSONObject(String(frame, 1, frame.size - 1, Charsets.UTF_8))) }
                        .onSuccess { _catalog.value = it }
                        .onFailure { log("DESCRIBE reply wasn't valid JSON (${frame.size}B)", ok = false) }
                } else if (type == TreeProtocol.REPLY_SCENE) {
                    runCatching { SceneState.parse(JSONObject(String(frame, 1, frame.size - 1, Charsets.UTF_8))) }
                        .onSuccess { s -> if (s != null) _scene.value = PushedScene(s, System.currentTimeMillis()) }
                        .onFailure { log("SCENE push wasn't valid JSON (${frame.size}B)", ok = false) }
                } else if (type == TreeProtocol.REPLY_LIBRARY) {
                    runCatching { TreeLibrary.parse(JSONObject(String(frame, 1, frame.size - 1, Charsets.UTF_8))) }
                        .onSuccess { _library.value = it }
                        .onFailure { log("LIBRARY reply wasn't valid JSON (${frame.size}B)", ok = false) }
                } else if (type == TreeProtocol.REPLY_SCHEDULE) {
                    runCatching { TreeSchedule.parse(JSONObject(String(frame, 1, frame.size - 1, Charsets.UTF_8))) }
                        .onSuccess { _schedule.value = it }
                        .onFailure { log("SCHEDULE reply wasn't valid JSON (${frame.size}B)", ok = false) }
                } else if (type == TreeProtocol.REPLY_FX_SCHEMA) {
                    runCatching { FxSectionSchema.parse(JSONObject(String(frame, 1, frame.size - 1, Charsets.UTF_8))) }
                        .onSuccess { s -> if (s != null) _fxSchema.update { it + (s.section to s) } }
                        .onFailure { log("FX_SCHEMA reply wasn't valid JSON (${frame.size}B)", ok = false) }
                } else if (type == TreeProtocol.REPLY_FX) {
                    runCatching { FxSectionData.parse(JSONObject(String(frame, 1, frame.size - 1, Charsets.UTF_8))) }
                        .onSuccess { d -> if (d != null) _fxDraft.update { it + (d.section to d) } }
                        .onFailure { log("FX reply wasn't valid JSON (${frame.size}B)", ok = false) }
                }
            }
        } catch (e: IOException) {
            if (socket === s) {
                close()
                _state.value = State.Failed(host, "connection lost")
                log("connection to $host lost", ok = false)
            }
        }
    }

    private fun readFrame(input: InputStream): ByteArray {
        val header = readExactly(input, 2)
        val len = (header[0].toInt() and 0xFF) or ((header[1].toInt() and 0xFF) shl 8)
        return readExactly(input, len)
    }

    private fun readExactly(input: InputStream, n: Int): ByteArray {
        val buf = ByteArray(n)
        var have = 0
        while (have < n) {
            val got = input.read(buf, have, n - have)
            if (got < 0) throw IOException("closed by tree")
            have += got
        }
        return buf
    }

    private companion object {
        const val CONNECT_TIMEOUT_MS = 3000
        const val HELLO_TIMEOUT_MS = 3000
        const val ACK_TIMEOUT_MS = 2000L
        const val QUEUE_FULL_RETRIES = 20
        const val LOG_MAX = 150
    }
}
