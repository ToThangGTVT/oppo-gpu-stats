package com.example.hwoverlay;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.graphics.drawable.Drawable;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.util.ArrayList;
import java.util.Collections;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import java.util.TreeSet;

/** Chọn app bị chặn chạy nền. Việc chặn do statd làm (cần quyền shell). */
public class BlockActivity extends Activity {
    static final String KEY_BLOCKED = "blocked";

    private final Handler handler = new Handler(Looper.getMainLooper());
    private SharedPreferences prefs;
    private TextView statusView;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        prefs = getSharedPreferences(OverlayService.PREFS, MODE_PRIVATE);
        float density = getResources().getDisplayMetrics().density;
        int pad = (int) (16 * density);
        int iconSize = (int) (32 * density);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(pad, pad, pad, pad);

        TextView help = new TextView(this);
        help.setText("App được chọn sẽ bị dừng hẳn (force-stop) sau "
                + "10 giây kể từ khi rời màn hình, và bị xếp vào nhóm hạn chế của hệ thống. "
                + "App sẽ không chạy nền, không nhận thông báo cho tới khi bạn tự mở lại.\n"
                + "Cần statd đang chạy (./tools/statd.sh). Danh sách được statd lưu lại.");
        root.addView(help);

        statusView = new TextView(this);
        statusView.setPadding(0, pad / 2, 0, pad / 2);
        root.addView(statusView);

        Set<String> blocked = blockedSet(this);
        for (ResolveInfo ri : launchableApps()) {
            String pkg = ri.activityInfo.packageName;
            CheckBox cb = new CheckBox(this);
            cb.setText(ri.loadLabel(getPackageManager()) + "\n" + pkg);
            Drawable icon = ri.loadIcon(getPackageManager());
            icon.setBounds(0, 0, iconSize, iconSize);
            cb.setCompoundDrawablesRelative(null, null, icon, null);
            cb.setChecked(blocked.contains(pkg));
            cb.setOnCheckedChangeListener((b, checked) -> {
                Set<String> set = new TreeSet<>(blockedSet(this));
                if (checked) set.add(pkg); else set.remove(pkg);
                prefs.edit().putStringSet(KEY_BLOCKED, set).apply();
                sync();
            });
            root.addView(cb);
        }

        ScrollView scroll = new ScrollView(this);
        scroll.addView(root);
        scroll.setFitsSystemWindows(true);
        setContentView(scroll);
        setTitle("Chặn chạy nền");
    }

    @Override
    protected void onResume() {
        super.onResume();
        sync();
    }

    /** App có biểu tượng trên launcher, trừ chính app này, sắp theo tên. */
    private List<ResolveInfo> launchableApps() {
        PackageManager pm = getPackageManager();
        Intent main = new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER);
        List<ResolveInfo> all = pm.queryIntentActivities(main, 0);
        List<ResolveInfo> out = new ArrayList<>();
        Set<String> seen = new HashSet<>();
        for (ResolveInfo ri : all) {
            String pkg = ri.activityInfo.packageName;
            if (pkg.equals(getPackageName()) || pkg.startsWith("com.android.") || !seen.add(pkg)) continue;
            out.add(ri);
        }
        Collections.sort(out, (a, b) -> a.loadLabel(pm).toString()
                .compareToIgnoreCase(b.loadLabel(pm).toString()));
        return out;
    }

    /** Gửi danh sách hiện tại cho statd và hiển thị kết quả. */
    private void sync() {
        String line = "block " + String.join(",", new TreeSet<>(blockedSet(this)));
        statusView.setText("Đang gửi cho statd…");
        new Thread(() -> {
            String reply = StatdClient.request(line.trim());
            String status = StatdClient.request("blockstatus");
            handler.post(() -> statusView.setText(describe(reply, status)));
        }).start();
    }

    private static String describe(String reply, String status) {
        if (reply == null) {
            return "statd chưa chạy: danh sách đã lưu trong app, sẽ áp dụng khi statd chạy "
                    + "và bạn mở lại màn hình này.";
        }
        if (reply.startsWith("err")) return "statd từ chối: " + reply.substring(3).trim();
        String s = "Đã áp dụng: " + reply.substring(2).trim() + " app bị chặn.";
        if (status != null && status.startsWith("ok")) {
            for (String part : status.substring(2).trim().split(" ")) {
                if (part.startsWith("kills=")) s += "\nĐã dừng " + part.substring(6) + " lần";
                else if (part.startsWith("last=") && !part.equals("last=-")) s += ", gần nhất: " + part.substring(5);
            }
        }
        return s;
    }

    static Set<String> blockedSet(Context c) {
        return c.getSharedPreferences(OverlayService.PREFS, MODE_PRIVATE)
                .getStringSet(KEY_BLOCKED, Collections.emptySet());
    }
}
