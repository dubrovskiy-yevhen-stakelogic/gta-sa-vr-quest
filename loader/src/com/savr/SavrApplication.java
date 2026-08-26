package com.savr;

import android.app.Activity;
import android.app.Application;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.SurfaceTexture;
import android.graphics.Typeface;
import android.os.Bundle;
import android.text.Layout;
import android.text.StaticLayout;
import android.text.TextPaint;
import android.text.TextUtils;
import android.view.Surface;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;

/**
 * Loads the VR layer before anything of the game runs.
 *
 * <p>The APK patch points {@code <application android:name>} here. Android
 * instantiates this class before any activity, which is comfortably earlier than
 * the game pulling in {@code libGame.so} through ReLinker — so the native side
 * gets to install itself first and then waits for the game library to show up.
 *
 * <p>Two things can only come from the Java side and are handed over here: the
 * app class loader, without which a native thread cannot look up the game's
 * classes, and the game Activity, which the OpenXR loader needs before it will
 * talk to the runtime at all.
 *
 * <p>A failure to load is logged and swallowed on purpose: a broken VR layer
 * should leave a playable flat game behind, not a boot loop.
 */
public final class SavrApplication extends Application {

    private static final String TAG = "SAVR";
    private static final String GAME_ACTIVITY = "com.rockstargames.gtasa.GameActivity";

    private static boolean nativeReady = false;
    private static final String[] DEFAULT_SETTINGS = {
            "vr_driving.ini",
            "vr_appearance.ini",
            "vr_calib.ini",
            "vr_graphics.ini",
            "vr_holsters.ini",
            "vr_hud.ini",
            "vr_locomotion.ini"
    };

    static {
        try {
            System.loadLibrary("savr");
            nativeReady = true;
        } catch (UnsatisfiedLinkError e) {
            android.util.Log.e(TAG, "failed to load libsavr.so", e);
        }
    }

    @Override
    public void onCreate() {
        super.onCreate();
        if (!nativeReady) {
            return;
        }

        installMissingDefaultSettings();
        nativeOnApplicationCreate(SavrApplication.class.getClassLoader());

        registerActivityLifecycleCallbacks(new ActivityLifecycleCallbacks() {
            @Override
            public void onActivityCreated(Activity activity, Bundle state) {
                // Only the game activity is of interest. The stock launcher
                // activity is a Play Asset Delivery downloader that this build
                // skips entirely, and handing it to the OpenXR loader would tie
                // the session to a window that is about to disappear.
                if (GAME_ACTIVITY.equals(activity.getClass().getName())) {
                    supplyDataPacks(activity);
                    nativeOnActivityCreated(activity);
                }
            }

            // Logged because two separate symptoms — the OpenXR session never
            // leaving IDLE and the engine never drawing — are both exactly what a
            // paused activity looks like, and guessing between them is expensive.
            @Override public void onActivityStarted(Activity activity) { log("started", activity); }
            @Override public void onActivityResumed(Activity activity) { log("resumed", activity); }
            @Override public void onActivityPaused(Activity activity) { log("paused", activity); }
            @Override public void onActivityStopped(Activity activity) { log("stopped", activity); }
            @Override public void onActivitySaveInstanceState(Activity activity, Bundle state) {}

            @Override
            public void onActivityDestroyed(Activity activity) {
                // Kill the process when the game activity is gone. The engine and
                // our render loop do not survive a stop/restart cleanly, so a
                // lingering frozen process would be what the next launch resumes
                // (the activity is singleTask). Exiting guarantees every launch
                // starts fresh instead of re-attaching to a hung instance.
                if (GAME_ACTIVITY.equals(activity.getClass().getName())) {
                    android.util.Log.i(TAG, "game activity destroyed - exiting for a clean next launch");
                    android.os.Process.killProcess(android.os.Process.myPid());
                }
            }
        });
    }

    /**
     * Seeds a clean installation with the project owner's verified Quest
     * settings. Existing files always win, so an update never destroys a
     * player's own calibration. Deleting the SAVR INI files and launching the
     * game again restores this exact shipping baseline.
     */
    private void installMissingDefaultSettings() {
        final File filesDirectory = getExternalFilesDir(null);
        if (filesDirectory == null) {
            android.util.Log.w(TAG, "default settings skipped: no writable app directory");
            return;
        }
        final File destinationDirectory = filesDirectory;

        for (String name : DEFAULT_SETTINGS) {
            final File destination = new File(destinationDirectory, name);
            if (destination.isFile()) {
                continue;
            }

            final File temporary = new File(destinationDirectory, name + ".shipping.tmp");
            try (InputStream input = getAssets().open("savr_defaults/" + name);
                 FileOutputStream output = new FileOutputStream(temporary, false)) {
                byte[] buffer = new byte[16 * 1024];
                int read;
                while ((read = input.read(buffer)) >= 0) {
                    if (read > 0) {
                        output.write(buffer, 0, read);
                    }
                }
                output.getFD().sync();
            } catch (IOException error) {
                if (temporary.exists() && !temporary.delete()) {
                    android.util.Log.w(TAG, "could not remove " + temporary);
                }
                android.util.Log.e(TAG, "could not seed " + name, error);
                continue;
            }

            if (destination.exists() || !temporary.renameTo(destination)) {
                if (temporary.exists() && !temporary.delete()) {
                    android.util.Log.w(TAG, "could not remove " + temporary);
                }
                if (!destination.exists()) {
                    android.util.Log.e(TAG, "could not publish shipping default " + name);
                }
                continue;
            }
            android.util.Log.i(TAG, "installed shipping default " + name);
        }
    }

    private static SurfaceTexture gameTexture;
    private static Surface gameSurface;
    private static final float[] textureTransform = new float[16];
    private static boolean attached = false;
    private static long updateCount = 0;
    private Typeface vrHudTypeface;
    private Typeface vrBigTypeface;
    private int vrTextRenders;

    /**
     * Rasterise one current game string, not a screenshot crop. Native calls
     * this only when the text changes and uploads the returned transparent
     * bitmap into the existing OpenXR HUD compositor.
     */
    public int[] renderVrText(String source, int width, int height, int style) {
        if (width <= 0 || height <= 0 || source == null || source.isEmpty()) {
            return new int[0];
        }
        String text = source.replace("~n~", "\n")
                .replaceAll("~[A-Za-z0-9_]+~", "");
        if (text.isEmpty()) {
            return new int[0];
        }

        try {
            if (vrHudTypeface == null) {
                vrHudTypeface = Typeface.createFromAsset(getAssets(),
                        "Fonts/HELVETICANEUELTCOM-MDCN.TTF");
            }
            if (vrBigTypeface == null) {
                vrBigTypeface = Typeface.createFromAsset(getAssets(),
                        "Fonts/PRICEDOWNGTAVINT.TTF");
            }
        } catch (RuntimeException error) {
            android.util.Log.w(TAG, "VR HUD font asset unavailable", error);
            vrHudTypeface = Typeface.DEFAULT_BOLD;
            vrBigTypeface = Typeface.DEFAULT_BOLD;
        }

        Bitmap bitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(bitmap);
        canvas.drawColor(Color.TRANSPARENT, android.graphics.PorterDuff.Mode.CLEAR);

        // The layout was tuned on a 1024-wide raster. Native now requests any
        // multiple of it for sharper glyphs, so every pixel metric scales with
        // the width and the on-screen composition stays identical.
        final float resolutionScale = width / 1024.0f;
        TextPaint paint = new TextPaint(TextPaint.ANTI_ALIAS_FLAG |
                TextPaint.SUBPIXEL_TEXT_FLAG);
        paint.setColor(Color.WHITE);
        paint.setTypeface(style == 1 ? vrBigTypeface : vrHudTypeface);
        paint.setTextSize((style == 1 ? 76.0f : 42.0f) * resolutionScale);
        paint.setShadowLayer((style == 1 ? 4.0f : 3.0f) * resolutionScale,
                2.0f * resolutionScale, 2.0f * resolutionScale, Color.BLACK);

        final int horizontalPadding = Math.round(32 * resolutionScale);
        final int layoutWidth = Math.max(1, width - horizontalPadding * 2);
        StaticLayout layout = StaticLayout.Builder.obtain(text, 0, text.length(),
                        paint, layoutWidth)
                .setAlignment(Layout.Alignment.ALIGN_CENTER)
                .setIncludePad(false)
                .setEllipsize(TextUtils.TruncateAt.END)
                .setMaxLines(style == 1 ? 2 : 5)
                .build();
        canvas.save();
        canvas.translate(horizontalPadding,
                Math.max(0.0f, (height - layout.getHeight()) * 0.5f));
        layout.draw(canvas);
        canvas.restore();

        int[] pixels = new int[width * height];
        bitmap.getPixels(pixels, 0, width, 0, 0, width, height);
        bitmap.recycle();
        if (++vrTextRenders <= 4) {
            android.util.Log.i(TAG, "VR HUD text rasterized style=" + style
                    + " chars=" + text.length() + " size=" + width + "x" + height);
        }
        return pixels;
    }

    /**
     * The surface the game renders into, in place of the one Horizon OS takes away.
     *
     * <p>An immersive app on Horizon OS has no 2D panel, so the SurfaceView the
     * game normally draws to is destroyed the moment the session starts, and with
     * it goes the game's EGL surface and its whole render loop. Handing the engine
     * a buffer of our own keeps that loop alive and completely independent of the
     * shell's window lifecycle.
     *
     * <p>It is a {@link SurfaceTexture} rather than a plain buffer because the
     * finished frame then arrives back as an external GL texture in the game's own
     * context — no readback, no copy, nothing crossing the CPU. From there it is
     * just a textured quad into each eye's swapchain.
     *
     * <p>Called from native code, on the render thread, with {@code texName} owned
     * by the context that will consume it.
     */
    public static Surface createGameSurface(int width, int height) {
        if (gameSurface == null) {
            // Detached on purpose. The surface has to exist before the engine is
            // asked to render, but the engine is what creates the GL context, so
            // at this point there is no context to own a texture yet. Binding one
            // here silently produced texture 0 and a black headset.
            gameTexture = new SurfaceTexture(false);
            gameTexture.setDefaultBufferSize(width, height);
            gameSurface = new Surface(gameTexture);
            android.util.Log.i(TAG, "game surface " + width + "x" + height);
        }
        return gameSurface;
    }

    /**
     * Bind the game surface to a texture, once the engine's GL context exists.
     *
     * <p>Called from native code, on the render thread, after the engine has made
     * its context current.
     */
    public static void attachGameTexture(int texName) {
        if (gameTexture == null || attached) {
            return;
        }
        gameTexture.attachToGLContext(texName);
        attached = true;
        android.util.Log.i(TAG, "game surface attached to texture " + texName);
    }

    /**
     * Pull the newest finished frame into the texture and report how it is laid
     * out in memory. The transform is not decoration: the producer is free to hand
     * back a rotated or flipped buffer, and guessing at the orientation is exactly
     * the kind of thing that looks almost right until it isn't.
     *
     * <p>Called from native code once per frame, on the render thread.
     *
     * @return the 4x4 texture transform, or null before the surface exists
     */
    public static float[] updateGameTexture() {
        if (gameTexture == null || !attached) {
            return null;
        }
        gameTexture.updateTexImage();
        gameTexture.getTransformMatrix(textureTransform);

        // A black headset looks identical whether the engine is drawing nothing
        // or nothing is reaching us at all. The buffer timestamp separates the
        // two: it only advances when a finished frame is actually queued.
        ++updateCount;
        if (updateCount <= 5 || updateCount % 60 == 0) {
            android.util.Log.i(TAG, "game frames: " + updateCount
                    + ", buffer timestamp " + gameTexture.getTimestamp());
        }
        return textureTransform;
    }

    private static void log(String state, Activity activity) {
        if (GAME_ACTIVITY.equals(activity.getClass().getName())) {
            android.util.Log.i(TAG, "activity " + state);
        }
    }

    // Where the game's data pack is unpacked on the headset. GameThread reads
    // the pack path from the launch intent's extras and passes it to the engine;
    // launching from the Play downloader would fill these in from Play Asset
    // Delivery, but a library launch on a headset has no Play Store to ask, so
    // onInitialSetup receives data_main=null and the engine loads nothing and
    // draws black. Setting the extras here, before GameThread reads them, gives
    // the engine its data without the downloader.
    private static final String DATA_PACK_NAME = "data_main";
    private static final String DATA_PACK_PATH =
            "/storage/emulated/0/savr/data_main/assets";

    private static void supplyDataPacks(Activity activity) {
        android.content.Intent intent = activity.getIntent();
        if (intent == null) {
            return;
        }
        if (intent.getStringArrayExtra("packPaths") != null) {
            return; // a real downloader already provided them
        }
        intent.putExtra("packNames", new String[] { DATA_PACK_NAME });
        intent.putExtra("packPaths", new String[] { DATA_PACK_PATH });
        android.util.Log.i(TAG, "supplied data pack path " + DATA_PACK_PATH);
    }

    private static native void nativeOnApplicationCreate(ClassLoader loader);

    private static native void nativeOnActivityCreated(Object activity);
}
