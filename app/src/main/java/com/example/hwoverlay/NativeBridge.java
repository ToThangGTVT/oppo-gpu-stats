package com.example.hwoverlay;

import android.graphics.Bitmap;
import android.view.Surface;

/** Các hàm native trong libhwoverlay.so (app/src/main/cpp). */
final class NativeBridge {
    static {
        System.loadLibrary("hwoverlay");
    }

    private NativeBridge() {}

    /**
     * Atlas ALPHA_8 chứa ký tự ASCII 32..126 vẽ bằng font hệ thống, mỗi ký tự một ô cellW x cellH.
     * slot 0 = chữ nhỏ (nhãn, thông tin phụ), 1 = chữ lớn (giá trị %). Phải gọi trước measure()/start().
     */
    static native void setFont(int slot, Bitmap atlas, float[] advances, int cellW, int cellH,
            int pad, int ascent);

    /** Kích thước overlay (pixel) cho mật độ màn hình và chế độ hiển thị. */
    static native int[] measure(float density, boolean compact);

    /** Bắt đầu luồng native lấy mẫu và vẽ lên surface. */
    static native void start(Surface surface, float density, boolean compact, int intervalMs);

    static native void stop();

    /** Mô tả nguồn dữ liệu đang đọc được (CPU/GPU/RAM/statd). */
    static native String status();
}
