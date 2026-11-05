package com.displayxr.mc3d;

import android.app.ActivityManager;
import android.app.ApplicationExitInfo;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.os.Build;
import android.os.Bundle;
import android.os.IBinder;
import android.os.Parcel;
import android.os.ParcelFileDescriptor;

import java.io.BufferedReader;
import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.List;

/**
 * Loaded into Minecraft's process by the displayxr3d mod (from bytes inside the
 * .so). The DisplayXR 3D Bridge app broadcasts its Binder to us every 2 s.
 * LeviLauncher cannot see the DisplayXR runtime, so the bridge (which can)
 * connects to the runtime service and hands us the connected socket plus the
 * runtime library path; the mod then runs DisplayXR in this process.
 */
public final class BridgeReceiver extends BroadcastReceiver {
    public static final String ACTION_HELLO = "com.displayxr.mc3d.BRIDGE_HELLO";
    private static final int TX_STATUS = 3, TX_INFO = 4, TX_CONNECT = 5;

    private static volatile IBinder sBinder;
    private static volatile int sGen;
    private static volatile String sLastError = "";
    private static volatile String sPath = "";

    // This process's DisplayXR log lines, for the bridge app's screen (no adb).
    private static final ArrayList<String> sKeyLog = new ArrayList<>();
    private static final ArrayDeque<String> sRecentLog = new ArrayDeque<>();

    public static void install(Context ctx) {
        IntentFilter f = new IntentFilter(ACTION_HELLO);
        if (Build.VERSION.SDK_INT >= 33) {
            ctx.registerReceiver(new BridgeReceiver(), f, 2 /* RECEIVER_EXPORTED */);
        } else {
            ctx.registerReceiver(new BridgeReceiver(), f);
        }
        startLogReader();
        sAm = (ActivityManager) ctx.getSystemService(Context.ACTIVITY_SERVICE);
        try {
            sExitReport = readLastExit(ctx);
        } catch (Throwable t) {
            sExitReport = "(could not read why the game last closed: " + t + ")";
        }
    }

    private static volatile ActivityManager sAm;
    private static volatile String sExitReport = "";

    /** Why this app last died abnormally, one panel line per \\n; empty if it didn't. */
    public static String lastExitReport() {
        return sExitReport;
    }

    public static String memoryLine() {
        ActivityManager am = sAm;
        if (am == null) return "";
        ActivityManager.MemoryInfo mi = new ActivityManager.MemoryInfo();
        am.getMemoryInfo(mi);
        return "Tablet memory free: " + (mi.availMem >> 20) + " MB of " + (mi.totalMem >> 20) + " MB"
                + (mi.lowMemory ? " (LOW)" : "");
    }

    private static String exitName(int r) {
        switch (r) {
            case ApplicationExitInfo.REASON_SIGNALED: return "KILLED BY SIGNAL";
            case ApplicationExitInfo.REASON_LOW_MEMORY: return "OUT OF MEMORY";
            case ApplicationExitInfo.REASON_CRASH: return "JAVA CRASH";
            case ApplicationExitInfo.REASON_CRASH_NATIVE: return "NATIVE CRASH";
            case ApplicationExitInfo.REASON_ANR: return "NOT RESPONDING";
            case ApplicationExitInfo.REASON_INITIALIZATION_FAILURE: return "START FAILED";
            case ApplicationExitInfo.REASON_EXCESSIVE_RESOURCE_USAGE: return "TOO MUCH RESOURCE USE";
            case ApplicationExitInfo.REASON_DEPENDENCY_DIED: return "DEPENDENCY DIED";
            case ApplicationExitInfo.REASON_OTHER: return "KILLED BY THE SYSTEM";
            default: return null;  // normal: the player, the app itself, or unknown
        }
    }

    private static String readLastExit(Context ctx) throws Exception {
        if (Build.VERSION.SDK_INT < 30 || sAm == null) return "";
        List<ApplicationExitInfo> list = sAm.getHistoricalProcessExitReasons(null, 0, 8);
        for (ApplicationExitInfo e : list) {
            String name = exitName(e.getReason());
            if (name == null) continue;
            long ageMin = (System.currentTimeMillis() - e.getTimestamp()) / 60000;
            if (ageMin > 360) return "";
            StringBuilder sb = new StringBuilder();
            sb.append("LAST CLOSE, ").append(ageMin).append(" min ago: ").append(name)
                    .append(", game memory ").append(e.getPss() >> 10).append(" MB, status ").append(e.getStatus());
            String d = e.getDescription();
            if (d != null && !d.isEmpty()) sb.append('\n').append(d);
            String trace = traceSummary(e);
            if (!trace.isEmpty()) sb.append('\n').append(trace);
            return wrap(sb.toString());
        }
        return "";
    }

    /** Readable pieces of the crash record: it can't be fully decoded here, and needn't be. */
    private static String traceSummary(ApplicationExitInfo e) throws Exception {
        final int reason = e.getReason();
        if (reason != ApplicationExitInfo.REASON_CRASH_NATIVE && reason != ApplicationExitInfo.REASON_ANR) return "";
        InputStream in = e.getTraceInputStream();
        if (in == null) return "";
        ByteArrayOutputStream bo = new ByteArrayOutputStream();
        byte[] buf = new byte[8192];
        int n;
        try {
            while (bo.size() < 512 * 1024 && (n = in.read(buf)) > 0) bo.write(buf, 0, n);
        } finally {
            in.close();
        }
        byte[] all = bo.toByteArray();
        ArrayList<String> out = new ArrayList<>();
        StringBuilder run = new StringBuilder();
        boolean inMain = false;
        for (int i = 0; i <= all.length && out.size() < 10; i++) {
            int c = i < all.length ? all[i] & 0xff : 0;
            if (c >= 0x20 && c < 0x7f) {
                run.append((char) c);
                continue;
            }
            String s = run.toString().trim();
            run.setLength(0);
            if (s.length() < 6) continue;
            if (reason == ApplicationExitInfo.REASON_ANR) {
                if (s.startsWith("\"main\"")) inMain = true;
                else if (inMain && s.startsWith("\"")) break;
                if (inMain && (s.startsWith("native:") || s.startsWith("at ") || s.contains("Waiting")))
                    if (!out.contains(s)) out.add(s);
            } else {
                String low = s.toLowerCase();
                if ((low.contains(".so") || s.startsWith("_Z") || low.contains("abort") || low.contains("sig"))
                        && !out.contains(s))
                    out.add(s);
            }
        }
        return String.join("\n", out);
    }

    private static String wrap(String text) {
        StringBuilder sb = new StringBuilder();
        for (String line : text.split("\n")) {
            while (line.length() > 88) {
                sb.append(line, 0, 88).append('\n');
                line = "  " + line.substring(88);
            }
            sb.append(line).append('\n');
        }
        return sb.toString();
    }

    private static void startLogReader() {
        Thread t = new Thread(() -> {
            try {
                Process p = Runtime.getRuntime().exec(new String[] {"logcat", "-v", "tag",
                        "--pid=" + android.os.Process.myPid(), "*:W"});
                BufferedReader r = new BufferedReader(new InputStreamReader(p.getInputStream()));
                String raw;
                while ((raw = r.readLine()) != null) {
                    String low = raw.toLowerCase();
                    if (!(low.contains("monado") || low.contains("dxr") || low.contains("displayxr")
                            || low.contains("openxr") || low.contains("weave"))) continue;
                    String line = raw.length() > 170 ? raw.substring(0, 170) + "..." : raw;
                    synchronized (sKeyLog) {
                        if (sKeyLog.size() < 12 && !sKeyLog.contains(line)
                                && (raw.startsWith("E/") || raw.startsWith("F/")
                                    || low.contains("fail") || low.contains("error")
                                    || low.contains("display processor"))) {
                            sKeyLog.add(line);
                        }
                        if (!line.equals(sRecentLog.peekLast())) {
                            sRecentLog.addLast(line);
                            while (sRecentLog.size() > 6) sRecentLog.removeFirst();
                        }
                    }
                }
            } catch (Throwable e) {
                synchronized (sKeyLog) {
                    sKeyLog.add("(could not read the log: " + e + ")");
                }
            }
        }, "dxr3d-log");
        t.setDaemon(true);
        t.start();
    }

    @Override
    public void onReceive(Context c, Intent i) {
        Bundle b = i.getExtras();
        IBinder binder = b != null ? b.getBinder("binder") : null;
        if (binder != null && binder != sBinder) {
            sBinder = binder;
            sGen++;
        }
    }

    /** {binderGen, protocol, runtimeFound, 0, 0, 0} or null when not connected. */
    public static int[] queryStatus() {
        IBinder b = sBinder;
        if (b == null) return null;
        Parcel d = Parcel.obtain(), r = Parcel.obtain();
        try {
            if (!b.transact(TX_STATUS, d, r, 0)) return null;
            int[] s = new int[6];
            s[0] = sGen;
            for (int k = 1; k < 6; k++) s[k] = r.readInt();
            return s;
        } catch (Throwable t) {
            fail(b, t);
            return null;
        } finally {
            d.recycle();
            r.recycle();
        }
    }

    /**
     * Asks the bridge for a fresh connection to the DisplayXR service. Returns the
     * socket fd (ours to own) or -1; the runtime library path is then in
     * runtimePath(), the reason for a -1 in lastError().
     */
    public static int connect() {
        IBinder b = sBinder;
        if (b == null) {
            sLastError = "bridge app not found";
            return -1;
        }
        Parcel d = Parcel.obtain(), r = Parcel.obtain();
        try {
            if (!b.transact(TX_CONNECT, d, r, 0)) {
                sLastError = "the bridge app refused the call";
                return -1;
            }
            if (r.readInt() != 1) {
                sLastError = r.readString();
                return -1;
            }
            ParcelFileDescriptor pfd = ParcelFileDescriptor.CREATOR.createFromParcel(r);
            sPath = r.readString();
            return pfd.detachFd();
        } catch (Throwable t) {
            fail(b, t);
            return -1;
        } finally {
            d.recycle();
            r.recycle();
        }
    }

    public static String runtimePath() {
        return sPath;
    }

    /** Mod diagnostics plus our DisplayXR log lines, shown on the bridge app's screen. */
    public static boolean sendInfo(String text) {
        IBinder b = sBinder;
        if (b == null) return false;
        StringBuilder sb = new StringBuilder(text);
        synchronized (sKeyLog) {
            if (!sKeyLog.isEmpty()) {
                sb.append("-- DisplayXR key lines --\n");
                for (String l : sKeyLog) sb.append(l).append('\n');
            }
            if (!sRecentLog.isEmpty()) {
                sb.append("-- DisplayXR newest lines --\n");
                for (String l : sRecentLog) sb.append(l).append('\n');
            }
        }
        Parcel d = Parcel.obtain();
        try {
            d.writeString(sb.toString());
            return b.transact(TX_INFO, d, null, IBinder.FLAG_ONEWAY);
        } catch (Throwable t) {
            fail(b, t);
            return false;
        } finally {
            d.recycle();
        }
    }

    public static String lastError() {
        return sLastError;
    }

    private static void fail(IBinder b, Throwable t) {
        sLastError = t.getClass().getSimpleName() + ": " + t.getMessage();
        if (sBinder == b) sBinder = null;  // wait for the next hello
    }
}
