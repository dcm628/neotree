package com.neotree.app.net

import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
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

/**
 * One TCP connection to the tree's command server
 * (firmware/include/neo_tree_net_server.hpp). Frames are
 * [uint16 LE length][payload] both ways: the tree greets with HELLO, then
 * answers every command with an ACK carrying an AckStatus.
 */
class TreeConnection(private val scope: CoroutineScope) {

    sealed interface State {
        data object Disconnected : State
        data class Connecting(val host: String) : State
        data class Connected(val host: String, val port: Int) : State
        data class Failed(val host: String, val reason: String) : State
    }

    private val _state = MutableStateFlow<State>(State.Disconnected)
    val state: StateFlow<State> = _state.asStateFlow()

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
                acks = Channel(Channel.UNLIMITED)
                readerJob = scope.launch(Dispatchers.IO) { readLoop(s, input, host) }
                _state.value = State.Connected(host, port)
                true
            } catch (e: IOException) {
                _state.value = State.Failed(host, e.message ?: e.javaClass.simpleName)
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
            repeat(QUEUE_FULL_RETRIES + 1) { attempt ->
                val out = output ?: return@withContext null
                try {
                    val len = message.size
                    out.write(byteArrayOf((len and 0xFF).toByte(), (len shr 8).toByte()) + message)
                    out.flush()
                } catch (e: IOException) {
                    fail("send failed: ${e.message}")
                    return@withContext null
                }
                val ack = withTimeoutOrNull(ACK_TIMEOUT_MS) { acks.receive() }
                if (ack == null) {
                    fail("tree stopped answering")
                    return@withContext null
                }
                val status = ack.second
                if (status != AckStatus.QUEUE_FULL) return@withContext status
                delay(5L * (attempt + 1))
            }
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
    }

    private suspend fun readLoop(s: Socket, input: InputStream, host: String) {
        try {
            while (true) {
                val frame = readFrame(input)
                if (frame.size == 3 && (frame[0].toInt() and 0xFF) == TreeProtocol.REPLY_ACK) {
                    acks.send((frame[1].toInt() and 0xFF) to AckStatus.from(frame[2].toInt() and 0xFF))
                }
            }
        } catch (e: IOException) {
            if (socket === s) {
                close()
                _state.value = State.Failed(host, "connection lost")
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
    }
}
