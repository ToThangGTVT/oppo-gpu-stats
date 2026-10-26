// Vẽ bảng overlay (chữ + đồ thị) bằng CPU vào buffer RGBA_8888 premultiplied.
#pragma once

#include <cstdint>

#include "stats.h"

struct History {
    static constexpr int N = 120;   // số mẫu hiển thị trên đồ thị
    float cpu[N], gpu[N], ram[N];
    int count = 0;
    int head = 0;                   // vị trí ghi tiếp theo

    void push(const Sample& s);
    // k = 0 là mẫu cũ nhất trong cửa sổ N, k = N-1 là mới nhất. -1 nếu chưa có.
    float at(const float* series, int k) const;
};

// Các phần chi tiết bật/tắt trong cài đặt (bitmask, khớp với OverlayService.DETAIL_*).
enum DetailFlags {
    kDetailCores = 1,      // tải + xung từng nhân CPU
    kDetailTemps = 2,      // nhiệt độ CPU / GPU / pin
    kDetailBattery = 4,    // % pin, công suất, điện áp
    kDetailSwap = 8,       // swap / zram
    kDetailTopProcs = 16,  // top tiến trình tốn CPU (cần statd)
};

struct Layout {
    int cell;       // đơn vị khoảng cách, tính bằng pixel
    bool compact;   // true = chỉ chữ, không đồ thị
    int details;    // DetailFlags
    int width;
    int height;
};

enum FontSlot { kFontSmall = 0, kFontLarge = 1, kFontTiny = 2, kFontCount = 3 };

// Atlas glyph ASCII 32..126 và '°' do Java dựng bằng font hệ thống (mỗi glyph một ô cellW x cellH,
// kênh alpha 8 bit). `pad` là lề trái/trên trong ô, `ascent` là khoảng từ đỉnh dòng tới
// chân chữ, `advances` là độ rộng ký tự. Chỉ gọi khi luồng vẽ đang dừng.
void setFontAtlas(int slot, const uint8_t* alpha, int stride, int cellW, int cellH, int pad,
                  int ascent, const float* advances);

Layout computeLayout(float density, bool compact, int details);

void renderOverlay(uint32_t* pixels, int stride, int w, int h, const Layout& layout,
                   const History& hist, const Sample& s, bool daemonLinked);
