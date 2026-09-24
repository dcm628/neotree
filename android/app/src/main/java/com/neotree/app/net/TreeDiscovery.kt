package com.neotree.app.net

import android.content.Context
import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
import android.os.Build
import kotlinx.coroutines.channels.awaitClose
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.callbackFlow
import java.net.Inet4Address
import java.util.concurrent.Executors

/** The tree as found on the network (mDNS/DNS-SD, advertised by the firmware). */
data class FoundTree(val name: String, val host: String, val port: Int)

/**
 * Finds the tree's _neotree._tcp service with Android's built-in network
 * service discovery. Emits each tree as it's resolved; collect with a
 * timeout, since "nothing found" just means nothing is emitted.
 */
class TreeDiscovery(context: Context) {
    private val nsd = context.getSystemService(NsdManager::class.java)
    private val executor = Executors.newSingleThreadExecutor()

    fun discover(): Flow<FoundTree> = callbackFlow {
        val listener = object : NsdManager.DiscoveryListener {
            override fun onServiceFound(info: NsdServiceInfo) {
                resolve(info) { found -> trySend(found) }
            }
            override fun onStartDiscoveryFailed(serviceType: String, errorCode: Int) {
                close(IllegalStateException("discovery failed to start ($errorCode)"))
            }
            override fun onDiscoveryStarted(serviceType: String) {}
            override fun onDiscoveryStopped(serviceType: String) {}
            override fun onServiceLost(info: NsdServiceInfo) {}
            override fun onStopDiscoveryFailed(serviceType: String, errorCode: Int) {}
        }
        nsd.discoverServices(TreeProtocol.SERVICE_TYPE, NsdManager.PROTOCOL_DNS_SD, listener)
        awaitClose { runCatching { nsd.stopServiceDiscovery(listener) } }
    }

    private fun resolve(info: NsdServiceInfo, onResolved: (FoundTree) -> Unit) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            val callback = object : NsdManager.ServiceInfoCallback {
                override fun onServiceUpdated(resolved: NsdServiceInfo) {
                    val address = resolved.hostAddresses.firstOrNull { it is Inet4Address }
                        ?: resolved.hostAddresses.firstOrNull()
                    if (address != null) {
                        onResolved(FoundTree(resolved.serviceName, address.hostAddress!!, resolved.port))
                        runCatching { nsd.unregisterServiceInfoCallback(this) }
                    }
                }
                override fun onServiceInfoCallbackRegistrationFailed(errorCode: Int) {}
                override fun onServiceLost() {}
                override fun onServiceInfoCallbackUnregistered() {}
            }
            nsd.registerServiceInfoCallback(info, executor, callback)
        } else {
            @Suppress("DEPRECATION")
            nsd.resolveService(info, object : NsdManager.ResolveListener {
                override fun onServiceResolved(resolved: NsdServiceInfo) {
                    val host = resolved.host?.hostAddress ?: return
                    onResolved(FoundTree(resolved.serviceName, host, resolved.port))
                }
                override fun onResolveFailed(info: NsdServiceInfo, errorCode: Int) {}
            })
        }
    }
}
