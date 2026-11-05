package com.displayxr.mc3d.bridge

import android.Manifest
import android.app.Activity
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.Color
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.Gravity
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView

/** Setup screen: camera permission, an On/Off switch, and what is happening. */
class MainActivity : Activity() {
    private lateinit var status: TextView
    private lateinit var cameraBtn: Button
    private lateinit var toggleBtn: Button
    private val handler = Handler(Looper.getMainLooper())

    private val refresh = object : Runnable {
        override fun run() {
            update()
            handler.postDelayed(this, 500)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        wakeRuntime()
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(64, 48, 64, 48)
            setBackgroundColor(Color.rgb(18, 18, 18))
        }
        fun label(text: String, size: Float) = TextView(this).apply {
            this.text = text
            textSize = size
            setTextColor(Color.WHITE)
            setPadding(0, 12, 0, 12)
        }
        root.addView(label("DisplayXR 3D Bridge", 28f))
        root.addView(label("Connects Minecraft to DisplayXR. Turn it on here, then open " +
            "Minecraft through LeviLauncher with the DisplayXR 3D mod enabled. Leave it on while you play.", 16f))
        cameraBtn = Button(this).apply {
            setOnClickListener { requestPermissions(arrayOf(Manifest.permission.CAMERA), 1) }
        }
        toggleBtn = Button(this).apply {
            textSize = 20f
            setOnClickListener { toggle() }
        }
        root.addView(cameraBtn)
        root.addView(toggleBtn)
        status = label("", 14f).apply { gravity = Gravity.START }
        root.addView(ScrollView(this).apply { addView(status) })
        setContentView(root)
    }

    override fun onResume() {
        super.onResume()
        handler.post(refresh)
    }

    override fun onPause() {
        handler.removeCallbacks(refresh)
        super.onPause()
    }

    private fun toggle() {
        val svc = Intent(this, BridgeService::class.java)
        if (BridgeService.running) {
            stopService(svc)
        } else {
            wakeRuntime()
            startForegroundService(svc)
        }
        handler.postDelayed({ update() }, 300)
    }

    private fun update() {
        val cameraOk = checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED
        cameraBtn.text = if (cameraOk) "1. Camera: allowed ✓" else "1. Allow camera (eye tracking)"
        toggleBtn.text = if (BridgeService.running) "Turn 3D OFF" else "2. Turn 3D ON"
        status.text = if (BridgeService.running) BridgeService.statusText(this) else "3D is off."
    }

    // Wake the runtime out of Android's "stopped" state so it can be connected to.
    private fun wakeRuntime() {
        try {
            val intent = Intent("org.khronos.openxr.OpenXRRuntimeService").apply {
                `package` = "org.freedesktop.monado.openxr_runtime.out_of_process"
                addFlags(Intent.FLAG_INCLUDE_STOPPED_PACKAGES)
            }
            startService(intent)
        } catch (_: Throwable) {
        }
    }
}
