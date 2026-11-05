package com.displayxr.mc3d.bridge

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.os.Binder
import android.os.Bundle
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.Parcel
import android.os.ParcelFileDescriptor
import android.os.SystemClock
import android.util.Log
import java.io.File

/**
 * The broker. LeviLauncher has no package visibility to the DisplayXR runtime,
 * so this app does the runtime's Java connect and hands the mod inside Minecraft
 * the connected socket plus the runtime library path, over Binder. DisplayXR
 * then runs inside Minecraft and the woven picture goes into Minecraft's own
 * window. There is no overlay.
 *
 * This service must stay alive while Minecraft is in 3D: it owns the runtime
 * Client (the service slot's death link), so if it dies the session dies too.
 */
class BridgeService : Service() {
    companion object {
        const val TAG = "DXR3DBridge"
        const val ACTION_HELLO = "com.displayxr.mc3d.BRIDGE_HELLO"
        const val TX_STATUS = 3
        const val TX_INFO = 4
        const val TX_CONNECT = 5
        const val PROTOCOL_VERSION = 2
        val LAUNCHERS = arrayOf("org.levimc.launcher", "org.levimc.launcher.dxr3d")
        val RUNTIMES = arrayOf(
            "org.freedesktop.monado.openxr_runtime.out_of_process",
            "org.freedesktop.monado.openxr_runtime.in_process",
        )
        const val RUNTIME_LIB = "libopenxr_displayxr.so"
        const val CLIENT_CLASS = "org.freedesktop.monado.ipc.Client"

        @Volatile var running = false
        @Volatile var modInfo = ""
        @Volatile var modInfoTime = 0L
        @Volatile var connectInfo = "Waiting for Minecraft to ask for DisplayXR."

        fun statusText(ctx: Context): String {
            val sb = StringBuilder()
            sb.append("DisplayXR runtime: ").append(runtimeDescription(ctx)).append("\n")
            sb.append("Connection: ").append(connectInfo).append("\n")
            if (modInfo.isNotEmpty()) {
                val age = (SystemClock.uptimeMillis() - modInfoTime) / 1000
                sb.append("— Minecraft (").append(age).append(" s ago) —\n").append(modInfo).append("\n")
            } else {
                sb.append("Minecraft: not started yet.\n")
            }
            return sb.toString()
        }

        private fun runtimeDescription(ctx: Context): String {
            for (pkg in RUNTIMES) {
                try {
                    val pi = ctx.packageManager.getPackageInfo(pkg, 0)
                    return "found, version ${pi.versionName}"
                } catch (_: Throwable) {
                }
            }
            return "NOT INSTALLED"
        }
    }

    private val handler = Handler(Looper.getMainLooper())
    private val connectLock = Any()
    private var client: Any? = null  // keeps the service slot + binding alive

    private fun runtimeFound(): Boolean = RUNTIMES.any {
        try { packageManager.getApplicationInfo(it, 0); true } catch (_: Throwable) { false }
    }

    private fun callerAllowed(uid: Int): Boolean {
        if (uid == android.os.Process.myUid()) return true
        return LAUNCHERS.any {
            try { packageManager.getApplicationInfo(it, 0).uid == uid } catch (_: Throwable) { false }
        }
    }

    private fun closeClient() {
        val c = client ?: return
        client = null
        try { c.javaClass.getMethod("markAsDiscardedByNative").invoke(c) } catch (t: Throwable) { Log.w(TAG, "close: $t") }
    }

    /** Same reflective connect as the runtime's RuntimeFdConnector sample. */
    private fun connectTo(pkg: String): ParcelFileDescriptor? {
        val rc = createPackageContext(pkg, Context.CONTEXT_INCLUDE_CODE or Context.CONTEXT_IGNORE_SECURITY)
        val cls = rc.classLoader.loadClass(CLIENT_CLASS)
        // Client(long nativePointer): must be non-zero, is never dereferenced here.
        val instance = cls.getConstructor(java.lang.Long.TYPE).newInstance(1L)
        val fd = cls.getMethod("blockingConnect", Context::class.java, String::class.java)
            .invoke(instance, applicationContext, pkg) as Int
        if (fd < 0) return null
        val field = cls.getDeclaredField("fd").apply { isAccessible = true }
        val dup = (field.get(instance) as ParcelFileDescriptor).dup()
        client = instance
        return dup
    }

    /** Writes {1, socket, path} or {0, reason} to the reply. */
    private fun handleConnect(reply: Parcel) {
        synchronized(connectLock) {
            closeClient()  // Minecraft restarted: release the old slot first
            var why = "the DisplayXR runtime is not installed"
            for (pkg in RUNTIMES) {
                val info = try { packageManager.getApplicationInfo(pkg, 0) } catch (_: Throwable) { continue }
                val lib = File(info.nativeLibraryDir, RUNTIME_LIB)
                if (!lib.exists()) {
                    why = "${lib.path} does not exist"
                    continue
                }
                try {
                    val pfd = connectTo(pkg)
                    if (pfd != null) {
                        connectInfo = "connected, Minecraft has the socket ($pkg)"
                        reply.writeInt(1)
                        pfd.writeToParcel(reply, 0)
                        reply.writeString(lib.path)
                        pfd.close()
                        return
                    }
                    why = "the runtime service refused the connection"
                } catch (t: Throwable) {
                    val c = (t as? java.lang.reflect.InvocationTargetException)?.targetException ?: t
                    why = "${c.javaClass.simpleName}: ${c.message}"
                    Log.w(TAG, "connect $pkg failed", c)
                }
            }
            connectInfo = "FAILED: $why"
            reply.writeInt(0)
            reply.writeString(why)
        }
    }

    private val binder = object : Binder() {
        override fun onTransact(code: Int, data: Parcel, reply: Parcel?, flags: Int): Boolean {
            if (code == TX_STATUS || code == TX_INFO || code == TX_CONNECT) {
                if (!callerAllowed(getCallingUid())) {
                    Log.w(TAG, "refused uid ${getCallingUid()}")
                    return false
                }
            }
            return try {
                when (code) {
                    TX_STATUS -> {
                        reply?.writeInt(PROTOCOL_VERSION)
                        reply?.writeInt(if (runtimeFound()) 1 else 0)
                        reply?.writeInt(0)
                        reply?.writeInt(0)
                        reply?.writeInt(0)
                        true
                    }
                    TX_INFO -> {
                        modInfo = data.readString() ?: ""
                        modInfoTime = SystemClock.uptimeMillis()
                        true
                    }
                    TX_CONNECT -> {
                        if (reply != null) handleConnect(reply)
                        true
                    }
                    else -> super.onTransact(code, data, reply, flags)
                }
            } catch (t: Throwable) {
                Log.e(TAG, "transaction $code failed", t)
                false
            }
        }
    }

    private val hello = object : Runnable {
        override fun run() {
            for (pkg in LAUNCHERS) {
                try {
                    val i = Intent(ACTION_HELLO).setPackage(pkg)
                    val extras = Bundle()
                    extras.putBinder("binder", binder)
                    i.putExtras(extras)
                    sendBroadcast(i)
                } catch (t: Throwable) {
                    Log.w(TAG, "hello to $pkg failed: $t")
                }
            }
            handler.postDelayed(this, 2000)
        }
    }

    override fun onBind(intent: Intent?): IBinder = binder

    override fun onCreate() {
        super.onCreate()
        running = true
        val nm = getSystemService(NotificationManager::class.java)
        nm.createNotificationChannel(
            NotificationChannel("bridge", "3D bridge", NotificationManager.IMPORTANCE_LOW)
        )
        val n = Notification.Builder(this, "bridge")
            .setContentTitle("DisplayXR 3D is on")
            .setContentText("Open Minecraft through LeviLauncher")
            .setSmallIcon(android.R.drawable.ic_menu_view)
            .build()
        startForeground(1, n)
        handler.post(hello)
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int = START_NOT_STICKY

    override fun onDestroy() {
        running = false
        handler.removeCallbacksAndMessages(null)
        synchronized(connectLock) { closeClient() }
        super.onDestroy()
    }
}
