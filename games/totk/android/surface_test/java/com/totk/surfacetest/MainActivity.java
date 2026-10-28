package com.totk.surfacetest;

import android.app.Activity;
import android.content.Intent;
import android.os.Build;
import android.os.Bundle;
import android.util.Log;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.WindowManager;

/**
 * App-shell probe: owns a SurfaceView, hands its Surface to native code, which
 * attaches it to a host native window and presents an animated Vulkan clear.
 *
 * Optional loader hook (surface must be attached first):
 *   adb shell am start -n com.totk.surfacetest/.MainActivity \
 *     --es loader_work /data/local/tmp/totk --es loader_mode boot-sdk
 */
public final class MainActivity extends Activity implements SurfaceHolder.Callback {
    static { System.loadLibrary("totk_surface"); }

    private static final String TAG = "totk-shell-java";
    private SurfaceView view;
    private boolean started;
    private boolean loaderLaunched;
    private String pendingWork;
    private String pendingMode;

    private native void nativeStart(SurfaceHolder holder);
    private native void nativeStop();
    private native void nativeRunLoader(String work, String mode);
    private native void nativeStopLoader();
    private native long nativeHostWindow();
    private native int nativePresented();

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        Log.i(TAG, "onCreate");
        if (Build.VERSION.SDK_INT >= 27) {
            setShowWhenLocked(true);
            setTurnScreenOn(true);
        }
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON |
                             WindowManager.LayoutParams.FLAG_TURN_SCREEN_ON |
                             WindowManager.LayoutParams.FLAG_SHOW_WHEN_LOCKED);
        Intent intent = getIntent();
        if (intent != null) {
            pendingWork = intent.getStringExtra("loader_work");
            pendingMode = intent.getStringExtra("loader_mode");
        }
        view = new SurfaceView(this);
        view.getHolder().addCallback(this);
        setContentView(view);
    }

    @Override protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        if (intent != null) {
            pendingWork = intent.getStringExtra("loader_work");
            pendingMode = intent.getStringExtra("loader_mode");
            maybeRunLoader();
        }
    }

    @Override public void surfaceCreated(SurfaceHolder h) {
        Log.i(TAG, "surfaceCreated");
    }

    @Override public void surfaceChanged(SurfaceHolder h, int format, int w, int hh) {
        Log.i(TAG, "surfaceChanged " + w + "x" + hh + " format=" + format);
        if (!started) {
            started = true;
            nativeStart(h);
            Log.i(TAG, "hostWindow=0x" + Long.toHexString(nativeHostWindow()));
            maybeRunLoader();
        }
    }

    @Override public void surfaceDestroyed(SurfaceHolder h) {
        Log.i(TAG, "surfaceDestroyed presented=" + nativePresented());
        nativeStopLoader();
        nativeStop();
        started = false;
        loaderLaunched = false;
    }

    private void maybeRunLoader() {
        if (loaderLaunched || pendingWork == null || !started) return;
        loaderLaunched = true;
        String mode = pendingMode == null ? "boot-sdk" : pendingMode;
        Log.i(TAG, "nativeRunLoader work=" + pendingWork + " mode=" + mode);
        nativeRunLoader(pendingWork, mode);
    }
}
