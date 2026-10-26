package com.totk.surfacetest;

import android.app.Activity;
import android.os.Build;
import android.os.Bundle;
import android.util.Log;
import android.view.SurfaceHolder;
import android.view.WindowManager;
import android.view.SurfaceView;

public final class MainActivity extends Activity implements SurfaceHolder.Callback {
    static { System.loadLibrary("totk_surface"); }
    private SurfaceView view;
    private native void nativeStart(SurfaceHolder holder);
    private native void nativeStop();

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        Log.i("totk-surface-java", "onCreate");
        if (Build.VERSION.SDK_INT >= 27) {
            setShowWhenLocked(true);
            setTurnScreenOn(true);
        }
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON |
                             WindowManager.LayoutParams.FLAG_TURN_SCREEN_ON |
                             WindowManager.LayoutParams.FLAG_SHOW_WHEN_LOCKED);
        view = new SurfaceView(this);
        view.getHolder().addCallback(this);
        setContentView(view);
    }
    @Override public void surfaceCreated(SurfaceHolder h) { Log.i("totk-surface-java", "surfaceCreated"); nativeStart(h); }
    @Override public void surfaceChanged(SurfaceHolder h, int f, int w, int hh) { Log.i("totk-surface-java", "surfaceChanged " + w + "x" + hh); }
    @Override public void surfaceDestroyed(SurfaceHolder h) { Log.i("totk-surface-java", "surfaceDestroyed"); nativeStop(); }
}
