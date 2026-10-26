package com.example.hwoverlay;

import android.Manifest;
import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.provider.Settings;
import android.view.View;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

/** Màn hình cấu hình: cấp quyền, bật/tắt overlay, xem nguồn dữ liệu. */
public class MainActivity extends Activity {
    private static final String STATD_HELP =
            "Máy không root thường chặn /proc/stat và GPU sysfs với app thường.\n"
            + "Chạy daemon statd bằng quyền shell để có tải CPU thật:\n\n"
            + "  ./tools/statd.sh\n\n"
            + "hoặc thủ công:\n"
            + "  adb push statd /data/local/tmp/\n"
            + "  adb shell /data/local/tmp/statd\n\n"
            + "Máy đã root (đọc được cả tải GPU):\n"
            + "  adb shell su -c /data/local/tmp/statd\n\n"
            + "Kéo overlay để di chuyển, chạm đúp để đổi chế độ gọn.";

    private final Handler handler = new Handler(Looper.getMainLooper());
    private Button permButton;
    private Button toggleButton;
    private TextView statusView;
    private SharedPreferences prefs;

    private final Runnable refresh = new Runnable() {
        @Override
        public void run() {
            updateUi();
            handler.postDelayed(this, 1000);
        }
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        prefs = getSharedPreferences(OverlayService.PREFS, MODE_PRIVATE);
        int pad = (int) (16 * getResources().getDisplayMetrics().density);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(pad, pad, pad, pad);

        TextView title = new TextView(this);
        title.setText("HW Overlay — CPU / GPU / RAM");
        title.setTextSize(20);
        root.addView(title);

        permButton = new Button(this);
        permButton.setText("Cấp quyền hiển thị trên ứng dụng khác");
        permButton.setOnClickListener(v -> startActivity(new Intent(
                Settings.ACTION_MANAGE_OVERLAY_PERMISSION, Uri.parse("package:" + getPackageName()))));
        root.addView(permButton);

        toggleButton = new Button(this);
        toggleButton.setOnClickListener(v -> toggleOverlay());
        root.addView(toggleButton);

        root.addView(prefCheckBox("Khóa vị trí + chạm xuyên qua overlay", OverlayService.KEY_LOCKED));
        root.addView(prefCheckBox("Chế độ gọn (chỉ chữ, không đồ thị)", OverlayService.KEY_COMPACT));

        TextView detailTitle = new TextView(this);
        detailTitle.setText("Hiển thị chi tiết");
        detailTitle.setTextSize(16);
        detailTitle.setPadding(0, pad, 0, 0);
        root.addView(detailTitle);
        root.addView(detailCheckBox("Từng nhân CPU (tải + xung)", OverlayService.DETAIL_CORES));
        root.addView(detailCheckBox("Nhiệt độ CPU / GPU / pin", OverlayService.DETAIL_TEMPS));
        root.addView(detailCheckBox("Pin: %, công suất sạc/xả, điện áp", OverlayService.DETAIL_BATTERY));
        root.addView(detailCheckBox("Swap / ZRAM", OverlayService.DETAIL_SWAP));
        root.addView(detailCheckBox("Top 3 tiến trình tốn CPU (cần statd)", OverlayService.DETAIL_TOP_PROCS));

        statusView = new TextView(this);
        statusView.setTypeface(Typeface.MONOSPACE);
        statusView.setPadding(0, pad, 0, pad);
        root.addView(statusView);

        TextView help = new TextView(this);
        help.setTypeface(Typeface.MONOSPACE);
        help.setTextIsSelectable(true);
        help.setText(STATD_HELP);
        root.addView(help);

        ScrollView scroll = new ScrollView(this);
        scroll.addView(root);
        scroll.setFitsSystemWindows(true);
        setContentView(scroll);

        if (Build.VERSION.SDK_INT >= 33) {
            requestPermissions(new String[] {Manifest.permission.POST_NOTIFICATIONS}, 0);
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        handler.post(refresh);
    }

    @Override
    protected void onPause() {
        handler.removeCallbacks(refresh);
        super.onPause();
    }

    private CheckBox prefCheckBox(String label, String key) {
        CheckBox cb = new CheckBox(this);
        cb.setText(label);
        cb.setChecked(prefs.getBoolean(key, false));
        cb.setOnCheckedChangeListener((b, checked) -> {
            prefs.edit().putBoolean(key, checked).apply();
            reloadOverlay();
        });
        return cb;
    }

    private CheckBox detailCheckBox(String label, int flag) {
        CheckBox cb = new CheckBox(this);
        cb.setText(label);
        cb.setChecked((prefs.getInt(OverlayService.KEY_DETAILS, 0) & flag) != 0);
        cb.setOnCheckedChangeListener((b, checked) -> {
            int details = prefs.getInt(OverlayService.KEY_DETAILS, 0);
            details = checked ? details | flag : details & ~flag;
            prefs.edit().putInt(OverlayService.KEY_DETAILS, details).apply();
            reloadOverlay();
        });
        return cb;
    }

    private void reloadOverlay() {
        if (OverlayService.running) {
            startService(new Intent(this, OverlayService.class).setAction(OverlayService.ACTION_RELOAD));
        }
    }

    private void toggleOverlay() {
        Intent intent = new Intent(this, OverlayService.class);
        if (OverlayService.running) {
            stopService(intent);
        } else if (Settings.canDrawOverlays(this)) {
            startForegroundService(intent);
        }
        handler.postDelayed(this::updateUi, 300);
    }

    private void updateUi() {
        boolean canDraw = Settings.canDrawOverlays(this);
        permButton.setVisibility(canDraw ? View.GONE : View.VISIBLE);
        toggleButton.setEnabled(canDraw);
        toggleButton.setText(OverlayService.running ? "Tắt overlay" : "Bật overlay");
        statusView.setText(NativeBridge.status());
    }
}
