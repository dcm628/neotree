package com.neotree.app.net

import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.launch
import java.io.IOException
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress

/**
 * The stream channel (firmware neo_tree_net_server.hpp): brush samples over
 * UDP, 30-60 a second. Nothing comes back and nothing is retried - a lost
 * sample only makes a stroke a little coarser - and only the newest pending
 * sample is ever sent, so a slow moment never builds a backlog.
 *
 * Tied to one TCP connection by its token: the tree drops samples once that
 * connection has gone.
 */
class TreeStream(scope: CoroutineScope, host: String, private val token: Int) {
    private val newest = Channel<ByteArray>(Channel.CONFLATED)
    private var seq = 0
    private val job = scope.launch(Dispatchers.IO) {
        val socket = try {
            DatagramSocket()
        } catch (e: IOException) {
            return@launch
        }
        val address = try {
            InetAddress.getByName(host)
        } catch (e: IOException) {
            socket.close()
            return@launch
        }
        socket.use {
            for (packet in newest) {
                try {
                    it.send(DatagramPacket(packet, packet.size, address, TreeProtocol.STREAM_PORT))
                } catch (e: IOException) {
                    // A dropped sample is fine; the next one follows.
                }
            }
        }
    }

    /** Queues a BRUSH message; replaces any not yet sent. */
    fun send(brushMessage: ByteArray) {
        seq = (seq + 1) and 0xFFFF
        newest.trySend(TreeProtocol.streamPacket(token, seq, brushMessage))
    }

    fun close() {
        newest.close()
        job.cancel()
    }
}
