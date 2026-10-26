package com.utc.hwoverlay;

import android.app.Activity;
import android.app.DownloadManager;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.net.Uri;
import android.os.Build;
import android.os.Environment;
import android.provider.Settings;
import android.widget.Toast;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;

/** Kiểm tra bản mới trên GitHub Releases, tải APK đúng ABI của máy và mở trình cài đặt. */
final class UpdateChecker {
    static final String REPO = "ToThangGTVT/oppo-gpu-stats";
    private static final String LATEST_API = "https://api.github.com/repos/" + REPO + "/releases/latest";
    private static final String APK_MIME = "application/vnd.android.package-archive";

    /** Bản phát hành mới nhất trên GitHub. */
    static final class Release {
        String version;   // "1.2.0" (đã bỏ chữ v)
        String apkName;   // null nếu release không có APK hợp với máy
        String apkUrl;
        String pageUrl;
    }

    private UpdateChecker() {}

    /** Gọi trên luồng nền. Trả về null nếu không gọi được API. */
    static Release fetchLatest() {
        HttpURLConnection c = null;
        try {
            c = (HttpURLConnection) new URL(LATEST_API).openConnection();
            c.setConnectTimeout(5000);
            c.setReadTimeout(5000);
            c.setRequestProperty("Accept", "application/vnd.github+json");
            c.setRequestProperty("X-GitHub-Api-Version", "2022-11-28");
            c.setRequestProperty("User-Agent", "oppo-gpu-stats");
            if (c.getResponseCode() != 200) return null;
            JSONObject json = new JSONObject(readAll(c.getInputStream()));
            Release r = new Release();
            r.version = stripV(json.getString("tag_name"));
            r.pageUrl = json.optString("html_url");
            pickApk(json.optJSONArray("assets"), r);
            return r;
        } catch (Exception e) {
            return null;
        } finally {
            if (c != null) c.disconnect();
        }
    }

    /** Ưu tiên APK theo ABI chính của máy (arm64-v8a, armeabi-v7a...), không có thì lấy universal. */
    private static void pickApk(JSONArray assets, Release r) throws Exception {
        if (assets == null) return;
        String[] wanted = new String[Build.SUPPORTED_ABIS.length + 1];
        System.arraycopy(Build.SUPPORTED_ABIS, 0, wanted, 0, Build.SUPPORTED_ABIS.length);
        wanted[wanted.length - 1] = "universal";
        for (String abi : wanted) {
            for (int i = 0; i < assets.length(); i++) {
                JSONObject a = assets.getJSONObject(i);
                String name = a.getString("name");
                if (name.endsWith("-" + abi + ".apk")) {
                    r.apkName = name;
                    r.apkUrl = a.getString("browser_download_url");
                    return;
                }
            }
        }
    }

    static String currentVersion(Context ctx) {
        try {
            return stripV(ctx.getPackageManager().getPackageInfo(ctx.getPackageName(), 0).versionName);
        } catch (Exception e) {
            return "0";
        }
    }

    /** So sánh "1.10.0" với "1.9.2" theo từng số; phần sau dấu '-' (bản build dev) bị bỏ qua. */
    static int compareVersions(String a, String b) {
        String[] x = a.split("-")[0].split("\\."), y = b.split("-")[0].split("\\.");
        for (int i = 0; i < Math.max(x.length, y.length); i++) {
            int d = Integer.compare(part(x, i), part(y, i));
            if (d != 0) return d;
        }
        return 0;
    }

    /** Tải APK bằng DownloadManager rồi mở trình cài đặt khi tải xong. */
    static void downloadAndInstall(Activity activity, Release r) {
        if (!activity.getPackageManager().canRequestPackageInstalls()) {
            Toast.makeText(activity, "Cho phép cài app từ nguồn này rồi bấm cập nhật lại",
                    Toast.LENGTH_LONG).show();
            activity.startActivity(new Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES,
                    Uri.parse("package:" + activity.getPackageName())));
            return;
        }
        Context app = activity.getApplicationContext();
        DownloadManager dm = app.getSystemService(DownloadManager.class);
        DownloadManager.Request req = new DownloadManager.Request(Uri.parse(r.apkUrl))
                .setTitle(r.apkName)
                .setMimeType(APK_MIME)
                .setNotificationVisibility(DownloadManager.Request.VISIBILITY_VISIBLE_NOTIFY_COMPLETED)
                .setDestinationInExternalFilesDir(app, Environment.DIRECTORY_DOWNLOADS, r.apkName);
        long id = dm.enqueue(req);
        Toast.makeText(activity, "Đang tải " + r.apkName, Toast.LENGTH_SHORT).show();

        BroadcastReceiver done = new BroadcastReceiver() {
            @Override
            public void onReceive(Context ctx, Intent intent) {
                if (intent.getLongExtra(DownloadManager.EXTRA_DOWNLOAD_ID, -1) != id) return;
                app.unregisterReceiver(this);
                Uri apk = dm.getUriForDownloadedFile(id);
                if (apk == null) {
                    Toast.makeText(app, "Tải bản cập nhật thất bại", Toast.LENGTH_LONG).show();
                    return;
                }
                app.startActivity(new Intent(Intent.ACTION_VIEW)
                        .setDataAndType(apk, APK_MIME)
                        .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_ACTIVITY_NEW_TASK));
            }
        };
        // Broadcast do DownloadManager (tiến trình hệ thống khác) gửi nên phải exported.
        // Cờ này chỉ có tác dụng từ Android 13, máy cũ bỏ qua.
        app.registerReceiver(done, new IntentFilter(DownloadManager.ACTION_DOWNLOAD_COMPLETE),
                Context.RECEIVER_EXPORTED);
    }

    private static int part(String[] parts, int i) {
        if (i >= parts.length) return 0;
        try {
            return Integer.parseInt(parts[i].replaceAll("[^0-9]", ""));
        } catch (NumberFormatException e) {
            return 0;
        }
    }

    private static String stripV(String v) {
        return v != null && (v.startsWith("v") || v.startsWith("V")) ? v.substring(1) : String.valueOf(v);
    }

    private static String readAll(InputStream in) throws Exception {
        try (InputStream is = in) {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buf = new byte[8192];
            for (int n; (n = is.read(buf)) > 0; ) out.write(buf, 0, n);
            return out.toString(StandardCharsets.UTF_8.name());
        }
    }
}
