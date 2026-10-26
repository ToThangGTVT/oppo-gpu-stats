package com.example.hwoverlay;

import android.annotation.SuppressLint;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.ServiceInfo;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.PixelFormat;
import android.graphics.SurfaceTexture;
import android.graphics.Typeface;
import android.hardware.input.InputManager;
import android.os.Build;
import android.os.IBinder;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.TextureView;
import android.view.WindowManager;

/** Foreground service giữ cửa sổ overlay nổi trên mọi ứng dụng. */
public class OverlayService extends Service {
    static final String ACTION_STOP = "com.example.hwoverlay.STOP";
    static final String ACTION_RELOAD = "com.example.hwoverlay.RELOAD";
    static final String PREFS = "overlay";
    static final String KEY_LOCKED = "locked";
    static final String KEY_COMPACT = "compact";
    private static final String KEY_X = "x";
    private static final String KEY_Y = "y";
    private static final String CHANNEL = "overlay";
    private static final int INTERVAL_MS = 500;

    static volatile boolean running;

    private WindowManager wm;
    private WindowManager.LayoutParams lp;
    private TextureView view;
    private Surface surface;

    @Override
    public void onCreate() {
        super.onCreate();
        wm = (WindowManager) getSystemService(WINDOW_SERVICE);
        startInForeground();
        addOverlay();
        running = true;
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        String action = intent != null ? intent.getAction() : null;
        if (ACTION_STOP.equals(action)) {
            stopSelf();
        } else if (ACTION_RELOAD.equals(action)) {
            removeOverlay();
            addOverlay();
        }
        return START_NOT_STICKY;
    }

    @Override
    public void onDestroy() {
        removeOverlay();
        running = false;
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    private void startInForeground() {
        NotificationManager nm = getSystemService(NotificationManager.class);
        nm.createNotificationChannel(
                new NotificationChannel(CHANNEL, "Overlay", NotificationManager.IMPORTANCE_LOW));
        PendingIntent stop = PendingIntent.getService(this, 0,
                new Intent(this, OverlayService.class).setAction(ACTION_STOP),
                PendingIntent.FLAG_IMMUTABLE);
        PendingIntent open = PendingIntent.getActivity(this, 0,
                new Intent(this, MainActivity.class), PendingIntent.FLAG_IMMUTABLE);
        Notification n = new Notification.Builder(this, CHANNEL)
                .setSmallIcon(android.R.drawable.ic_menu_info_details)
                .setContentTitle("Đang hiển thị CPU / GPU / RAM")
                .setContentIntent(open)
                .addAction(new Notification.Action.Builder(null, "Tắt", stop).build())
                .setOngoing(true)
                .build();
        if (Build.VERSION.SDK_INT >= 34) {
            startForeground(1, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE);
        } else {
            startForeground(1, n);
        }
    }

    @SuppressLint("ClickableViewAccessibility")
    private void addOverlay() {
        SharedPreferences prefs = getSharedPreferences(PREFS, MODE_PRIVATE);
        boolean locked = prefs.getBoolean(KEY_LOCKED, false);
        boolean compact = prefs.getBoolean(KEY_COMPACT, false);
        float density = getResources().getDisplayMetrics().density;
        loadSystemFont(density);
        int[] size = NativeBridge.measure(density, compact);

        int flags = WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                | WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS
                | WindowManager.LayoutParams.FLAG_HARDWARE_ACCELERATED;
        if (locked) flags |= WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE;
        lp = new WindowManager.LayoutParams(size[0], size[1],
                WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY, flags, PixelFormat.TRANSLUCENT);
        lp.gravity = Gravity.TOP | Gravity.START;
        lp.x = prefs.getInt(KEY_X, 24);
        lp.y = prefs.getInt(KEY_Y, 120);
        if (Build.VERSION.SDK_INT >= 28) {
            lp.layoutInDisplayCutoutMode =
                    WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        }
        if (locked && Build.VERSION.SDK_INT >= 31) {
            // Android 12+ chặn chạm xuyên qua overlay nếu độ mờ vượt ngưỡng (mặc định 0.8).
            InputManager im = (InputManager) getSystemService(Context.INPUT_SERVICE);
            lp.alpha = Math.min(1f, im.getMaximumObscuringOpacityForTouch());
        }

        view = new TextureView(this);
        view.setOpaque(false);
        view.setSurfaceTextureListener(new TextureView.SurfaceTextureListener() {
            @Override
            public void onSurfaceTextureAvailable(SurfaceTexture st, int w, int h) {
                st.setDefaultBufferSize(size[0], size[1]);
                surface = new Surface(st);
                NativeBridge.start(surface, density, compact, INTERVAL_MS);
            }

            @Override
            public boolean onSurfaceTextureDestroyed(SurfaceTexture st) {
                NativeBridge.stop();
                if (surface != null) surface.release();
                surface = null;
                return true;
            }

            @Override
            public void onSurfaceTextureSizeChanged(SurfaceTexture st, int w, int h) {}

            @Override
            public void onSurfaceTextureUpdated(SurfaceTexture st) {}
        });
        if (!locked) view.setOnTouchListener(new DragListener());
        wm.addView(view, lp);
    }

    /** Dựng atlas glyph bằng font hệ thống (font OEM nếu có) để phần native tự vẽ chữ. */
    private static void loadSystemFont(float density) {
        buildFontAtlas(0, 11 * density, 500, density);
        buildFontAtlas(1, 17 * density, 700, density);
    }

    private static void buildFontAtlas(int slot, float sizePx, int weight, float density) {
        Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        paint.setTypeface(Build.VERSION.SDK_INT >= 28
                ? Typeface.create(Typeface.DEFAULT, weight, false)
                : (weight >= 600 ? Typeface.DEFAULT_BOLD : Typeface.DEFAULT));
        paint.setTextSize(sizePx);
        Paint.FontMetricsInt fm = paint.getFontMetricsInt();
        int pad = (int) Math.ceil(2 * density);
        float[] advances = new float[95];
        float maxAdvance = 0;
        for (int i = 0; i < advances.length; i++) {
            advances[i] = paint.measureText(String.valueOf((char) (32 + i)));
            maxAdvance = Math.max(maxAdvance, advances[i]);
        }
        int cellW = (int) Math.ceil(maxAdvance) + 2 * pad;
        int cellH = fm.descent - fm.ascent + 2 * pad;
        Bitmap atlas = Bitmap.createBitmap(cellW * advances.length, cellH, Bitmap.Config.ALPHA_8);
        Canvas canvas = new Canvas(atlas);
        for (int i = 0; i < advances.length; i++) {
            canvas.drawText(String.valueOf((char) (32 + i)), i * cellW + pad, pad - fm.ascent, paint);
        }
        NativeBridge.setFont(slot, atlas, advances, cellW, cellH, pad, -fm.ascent);
        atlas.recycle();
    }

    private void removeOverlay() {
        NativeBridge.stop();
        if (view != null) {
            wm.removeView(view);
            view = null;
        }
    }

    /** Kéo để di chuyển, chạm đúp để đổi chế độ đầy đủ / gọn. */
    private class DragListener implements android.view.View.OnTouchListener {
        private float downX, downY;
        private int startX, startY;
        private boolean moved;
        private long lastTapUp;

        @Override
        public boolean onTouch(android.view.View v, MotionEvent e) {
            switch (e.getActionMasked()) {
                case MotionEvent.ACTION_DOWN:
                    downX = e.getRawX();
                    downY = e.getRawY();
                    startX = lp.x;
                    startY = lp.y;
                    moved = false;
                    return true;
                case MotionEvent.ACTION_MOVE:
                    float dx = e.getRawX() - downX, dy = e.getRawY() - downY;
                    if (Math.abs(dx) + Math.abs(dy) > 12) moved = true;
                    if (moved) {
                        lp.x = startX + (int) dx;
                        lp.y = startY + (int) dy;
                        wm.updateViewLayout(view, lp);
                    }
                    return true;
                case MotionEvent.ACTION_UP:
                    SharedPreferences prefs = getSharedPreferences(PREFS, MODE_PRIVATE);
                    if (moved) {
                        prefs.edit().putInt(KEY_X, lp.x).putInt(KEY_Y, lp.y).apply();
                    } else if (e.getEventTime() - lastTapUp < 300) {
                        prefs.edit().putBoolean(KEY_COMPACT, !prefs.getBoolean(KEY_COMPACT, false)).apply();
                        // đổi kích thước cửa sổ sau khi xử lý xong sự kiện chạm hiện tại
                        view.post(() -> { removeOverlay(); addOverlay(); });
                    }
                    lastTapUp = moved ? 0 : e.getEventTime();
                    return true;
                default:
                    return false;
            }
        }
    }
}
