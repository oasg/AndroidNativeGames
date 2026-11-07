package com.totk.android;

import android.app.Activity;
import android.content.Intent;
import android.content.res.Configuration;
import android.graphics.Color;
import android.os.Build;
import android.os.Bundle;
import android.os.Process;
import android.util.Log;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.TextView;

import java.io.File;

/**
 * Minimal TOTK Android app shell.
 *
 * It owns a SurfaceView and app-private data roots, then starts the native
 * loader on a worker thread.  Game data is side-loaded, never packed in the
 * APK:
 *
 *   <external files>/game/         rewritten/android-v0/*.patched.bin
 *   <external files>/romfs/        extracted RomFS tree
 *   <external files>/profile/      save/temp/cache (created on first start)
 *   <external files>/shader-cache/ offline SPIR-V cache
 *
 * Use the intent extras below to override paths or loader mode:
 *   adb shell am start -n com.totk.android/.MainActivity \
 *     --es mode boot-sdk --es work /sdcard/Android/data/com.totk.android/files/game
 *
 * The swapchain follows the real Surface extent; the game keeps rendering at
 * its own VI resolution (1080p docked / 720p handheld).
 */
public final class MainActivity extends Activity implements SurfaceHolder.Callback {
    private static final String TAG = "totk-app-java";

    static { System.loadLibrary("totk_android"); }

    private SurfaceView view;
    private TextView status;
    private File base;

    private String work;
    private String mode;
    private String romfs;
    private String profile;
    private String shaderCache;
    private String accountUid;
    private String operationMode;
    private boolean inits;
    private boolean runNnmain;

    private boolean surfaceStarted;
    private boolean loaderStarted;

    private native boolean nativeAttachSurface(SurfaceHolder holder, int width, int height);
    private native void nativeDetachSurface();
    private native boolean nativeStartLoader(String work, String mode, String romfs,
                                             String profile, String shaderCache,
                                             String accountUid, String operationMode,
                                             boolean inits, boolean runNnmain);
    private native boolean nativeLoaderRunning();

    @Override protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        Log.i(TAG, "onCreate");

        if (Build.VERSION.SDK_INT >= 27) {
            setShowWhenLocked(true);
            setTurnScreenOn(true);
        }
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON |
                             WindowManager.LayoutParams.FLAG_TURN_SCREEN_ON |
                             WindowManager.LayoutParams.FLAG_SHOW_WHEN_LOCKED |
                             WindowManager.LayoutParams.FLAG_FULLSCREEN);
        hideSystemBars();

        base = getExternalFilesDir(null);
        if (base == null) base = getFilesDir();
        resolvePaths(getIntent());

        FrameLayout root = new FrameLayout(this);
        view = new SurfaceView(this);
        view.getHolder().addCallback(this);
        root.addView(view, new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT));

        status = new TextView(this);
        status.setTextColor(Color.WHITE);
        status.setBackgroundColor(0x99000000);
        status.setTextSize(TypedValue.COMPLEX_UNIT_SP, 10);
        status.setTypeface(android.graphics.Typeface.MONOSPACE);
        status.setPadding(24, 16, 24, 16);
        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.WRAP_CONTENT);
        lp.gravity = Gravity.TOP;
        root.addView(status, lp);
        setContentView(root);
        updateStatus("starting");
    }

    @Override protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        if (loaderStarted) {
            Log.i(TAG, "onNewIntent ignored while loader runs");
            return;
        }
        resolvePaths(intent);
        updateStatus("reconfigured");
    }

    @Override public void onConfigurationChanged(Configuration c) {
        super.onConfigurationChanged(c);
        Log.i(TAG, "configuration " + c.orientation + " " + c.screenWidthDp + "x" + c.screenHeightDp);
    }

    private void resolvePaths(Intent intent) {
        File gameDir = new File(base, "game");
        File romfsDir = new File(base, "romfs");
        File profileDir = new File(base, "profile");
        File cacheDir = new File(base, "shader-cache");
        mkdirs(gameDir);
        mkdirs(romfsDir);
        mkdirs(profileDir);
        mkdirs(cacheDir);

        work = stringExtra(intent, "work", gameDir.getAbsolutePath());
        /* An installed app cannot mmap(PROT_EXEC) external/app-private files.
         * When the APK packs the images as native libraries, map them from the
         * app's native library directory instead. */
        File libDir = new File(getApplicationInfo().nativeLibraryDir);
        if (new File(libDir, "libmain_patched.so").isFile())
            work = libDir.getAbsolutePath();
        mode = stringExtra(intent, "mode", "boot-sdk");
        romfs = stringExtra(intent, "romfs", romfsDir.getAbsolutePath());
        profile = stringExtra(intent, "profile", profileDir.getAbsolutePath());
        shaderCache = stringExtra(intent, "shader_cache", cacheDir.getAbsolutePath());
        accountUid = stringExtra(intent, "account_uid", null);
        operationMode = stringExtra(intent, "operation_mode", null);
        inits = intent == null || intent.getBooleanExtra("inits", true);
        runNnmain = intent == null || intent.getBooleanExtra("run_nnmain", true);

        /* The extraction docs use romfs/totk-romfs-min; accept both layouts. */
        File nested = new File(romfs, "totk-romfs-min");
        if (new File(romfs).isDirectory() && nested.isDirectory())
            romfs = nested.getAbsolutePath();

        Log.i(TAG, "work=" + work);
        Log.i(TAG, "romfs=" + romfs);
        Log.i(TAG, "profile=" + profile);
        Log.i(TAG, "shader_cache=" + shaderCache);
        Log.i(TAG, "mode=" + mode + " inits=" + inits + " run_nnmain=" + runNnmain);
    }

    private static String stringExtra(Intent intent, String key, String fallback) {
        if (intent == null) return fallback;
        String v = intent.getStringExtra(key);
        return v == null || v.isEmpty() ? fallback : v;
    }

    private static void mkdirs(File dir) {
        if (!dir.isDirectory() && !dir.mkdirs())
            Log.w(TAG, "cannot create " + dir);
    }

    @Override public void surfaceCreated(SurfaceHolder h) {
        Log.i(TAG, "surfaceCreated");
        updateStatus("surface created");
    }

    @Override public void surfaceChanged(SurfaceHolder h, int format, int w, int hh) {
        Log.i(TAG, "surfaceChanged " + w + "x" + hh + " format=" + format);
        boolean attached = nativeAttachSurface(h, w, hh);
        updateStatus("surface " + w + "x" + hh + (attached ? " attached" : " ATTACH FAILED"));
        if (!attached) return;
        if (!surfaceStarted) {
            surfaceStarted = true;
            startLoader();
        }
    }

    @Override public void surfaceDestroyed(SurfaceHolder h) {
        Log.i(TAG, "surfaceDestroyed");
        nativeDetachSurface();
        surfaceStarted = false;
        updateStatus("surface destroyed (loader may continue offscreen)");
    }

    private void startLoader() {
        if (loaderStarted) return;
        loaderStarted = true;
        Log.i(TAG, "starting loader work=" + work + " mode=" + mode);
        boolean ok = nativeStartLoader(work, mode, romfs, profile, shaderCache,
                                       accountUid, operationMode, inits, runNnmain);
        if (!ok) {
            loaderStarted = false;
            updateStatus("loader start FAILED");
            Log.e(TAG, "nativeStartLoader failed");
            return;
        }
        updateStatus("loader running (" + mode + ")\n" + dataSummary());
    }

    private String dataSummary() {
        return "game=" + work + "\nromfs=" + romfs + "\nprofile=" + profile +
               "\nshader-cache=" + shaderCache;
    }

    private void updateStatus(String line) {
        if (status == null) return;
        status.setText("TOTK Android\n" + line + "\n\n" + dataSummary() +
                       "\n\nSide-load data with adb; see logcat tag " + TAG + " / totk-app.");
    }

    private void hideSystemBars() {
        View decor = getWindow().getDecorView();
        decor.setSystemUiVisibility(View.SYSTEM_UI_FLAG_LAYOUT_STABLE |
                                    View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION |
                                    View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN |
                                    View.SYSTEM_UI_FLAG_HIDE_NAVIGATION |
                                    View.SYSTEM_UI_FLAG_FULLSCREEN |
                                    View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
    }

    @Override public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) hideSystemBars();
    }

    @Override protected void onDestroy() {
        Log.i(TAG, "onDestroy loaderRunning=" + nativeLoaderRunning());
        nativeDetachSurface();
        if (nativeLoaderRunning()) {
            /* The loader has no cooperative stop; a finished Activity must not
             * leave guest threads presenting into a dead Surface. */
            Process.killProcess(Process.myPid());
        }
        super.onDestroy();
    }
}
