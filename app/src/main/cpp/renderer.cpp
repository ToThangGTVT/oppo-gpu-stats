#include "renderer.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

// ---------- Màu (premultiplied, thứ tự byte R G B A trong bộ nhớ) ----------

constexpr uint32_t rgba(uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    return (a << 24) | ((b * a / 255) << 16) | ((g * a / 255) << 8) | (r * a / 255);
}

constexpr uint32_t kBg       = rgba(14, 16, 22, 235);
constexpr uint32_t kBorder   = rgba(255, 255, 255, 28);
constexpr uint32_t kGraphBg  = rgba(255, 255, 255, 12);
constexpr uint32_t kGrid     = rgba(255, 255, 255, 22);
constexpr uint32_t kWhite    = rgba(255, 255, 255, 255);
constexpr uint32_t kGrey     = rgba(160, 168, 180, 255);
constexpr uint32_t kCpuColor = rgba(105, 220, 120, 255);
constexpr uint32_t kGpuColor = rgba(255, 176, 50, 255);
constexpr uint32_t kRamColor = rgba(80, 175, 255, 255);

uint32_t withAlpha(uint32_t premul, uint32_t a) {
    uint32_t r = (premul & 0xff) * a / 255, g = ((premul >> 8) & 0xff) * a / 255,
             b = ((premul >> 16) & 0xff) * a / 255, sa = (premul >> 24) * a / 255;
    return (sa << 24) | (b << 16) | (g << 8) | r;
}

struct Canvas {
    uint32_t* px;
    int stride, w, h;

    void blend(int x, int y, uint32_t c) {
        if ((unsigned)x >= (unsigned)w || (unsigned)y >= (unsigned)h) return;
        uint32_t& d = px[y * stride + x];
        uint32_t sa = c >> 24;
        if (sa == 255) { d = c; return; }
        uint32_t inv = 255 - sa, out = 0;
        for (int sh = 0; sh < 32; sh += 8) {
            uint32_t v = ((c >> sh) & 0xff) + ((d >> sh) & 0xff) * inv / 255;
            out |= (v > 255 ? 255 : v) << sh;
        }
        d = out;
    }

    void fillRect(int x, int y, int rw, int rh, uint32_t c) {
        int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
        int x1 = x + rw > w ? w : x + rw, y1 = y + rh > h ? h : y + rh;
        for (int yy = y0; yy < y1; yy++)
            for (int xx = x0; xx < x1; xx++) blend(xx, yy, c);
    }

    // Độ thụt vào của hàng yy ở góc bo tròn bán kính r.
    static int cornerInset(int yy, int rh, int r) {
        int dy = yy < r ? r - yy : (yy >= rh - r ? yy - (rh - r - 1) : 0);
        if (dy <= 0) return 0;
        return r - (int)std::sqrt((float)(r * r - (dy - 1) * (dy - 1)) + 0.5f);
    }

    void fillRoundRect(int x, int y, int rw, int rh, int r, uint32_t c) {
        for (int yy = 0; yy < rh; yy++) {
            int inset = cornerInset(yy, rh, r);
            fillRect(x + inset, y + yy, rw - 2 * inset, 1, c);
        }
    }

    // Viền 1px của hình chữ nhật bo góc (chỉ cạnh trái/phải + trên/dưới, đủ cho nền overlay).
    void strokeRoundRect(int x, int y, int rw, int rh, int r, uint32_t c) {
        for (int yy = 0; yy < rh; yy++) {
            int inset = cornerInset(yy, rh, r);
            if (yy == 0 || yy == rh - 1) {
                fillRect(x + inset, y + yy, rw - 2 * inset, 1, c);
            } else {
                int next = cornerInset(yy + (yy < rh / 2 ? -1 : 1), rh, r);
                int span = next > inset ? next - inset : 1;
                fillRect(x + inset, y + yy, span, 1, c);
                fillRect(x + rw - inset - span, y + yy, span, 1, c);
            }
        }
    }
};

// ---------- Font hệ thống (atlas từ Java) ----------

constexpr int kFirstChar = 32, kNumChars = 95;

struct FontAtlas {
    std::vector<uint8_t> alpha;   // kNumChars ô xếp ngang
    int cellW = 0, cellH = 0, pad = 0, ascent = 0;
    float adv[kNumChars] = {};
    int lineH() const { return cellH - 2 * pad; }
} g_fonts[kFontCount];

float textWidth(const FontAtlas& f, const char* s) {
    float w = 0;
    for (; *s; s++) {
        int i = (unsigned char)*s - kFirstChar;
        if (i >= 0 && i < kNumChars) w += f.adv[i];
    }
    return w;
}

// (x, baseline) là điểm bắt đầu trên đường chân chữ.
void drawText(Canvas& cv, const FontAtlas& f, float x, int baseline, const char* s, uint32_t color) {
    if (f.alpha.empty()) return;
    const int rowStride = f.cellW * kNumChars;
    const int gy = baseline - f.ascent - f.pad;
    for (; *s; s++) {
        int i = (unsigned char)*s - kFirstChar;
        if (i < 0 || i >= kNumChars) continue;
        int gx = (int)(x + 0.5f) - f.pad;
        const uint8_t* src = f.alpha.data() + i * f.cellW;
        for (int row = 0; row < f.cellH; row++)
            for (int col = 0; col < f.cellW; col++) {
                uint8_t a = src[row * rowStride + col];
                if (a) cv.blend(gx + col, gy + row, withAlpha(color, a));
            }
        x += f.adv[i];
    }
}

// ---------- Layout (đơn vị: cell) ----------

constexpr int kPad = 4;
constexpr int kInnerW = 120;
constexpr int kGraphH = 16;
constexpr int kTextGap = 2;     // chữ -> đồ thị
constexpr int kRowGap = 4;      // giữa các hàng

void drawGraph(Canvas& cv, int gx, int gy, int gw, int gh, const History& hist,
               const float* series, uint32_t color, int cell) {
    cv.fillRect(gx, gy, gw, gh, kGraphBg);
    for (int q = 1; q < 4; q++) cv.fillRect(gx, gy + gh * q / 4, gw, 1, kGrid);
    int thick = cell * 3 / 4 < 2 ? 2 : cell * 3 / 4;
    int prevY = -1;
    for (int x = 0; x < gw; x++) {
        float f = (float)x * (History::N - 1) / (float)(gw - 1);
        int i0 = (int)f;
        int i1 = i0 + 1 < History::N ? i0 + 1 : i0;
        float a = hist.at(series, i0), b = hist.at(series, i1);
        if (a < 0 || b < 0) { prevY = -1; continue; }
        float v = a + (b - a) * (f - (float)i0);
        int y = gy + gh - 1 - (int)(v / 100.f * (float)(gh - 1) + 0.5f);
        // vùng tô mờ dần từ đường xuống đáy
        for (int yy = y; yy < gy + gh; yy++) {
            uint32_t alpha = 110 - 85 * (uint32_t)(yy - y) / (uint32_t)(gy + gh - y);
            cv.blend(gx + x, yy, withAlpha(color, alpha));
        }
        int top = prevY < 0 ? y : (prevY < y ? prevY : y);
        int bot = prevY < 0 ? y : (prevY > y ? prevY : y);
        cv.fillRect(gx + x, top - thick / 2, 1, bot - top + thick, color);
        prevY = y;
    }
}

void fmtPct(char* buf, size_t n, float v, bool approx) {
    if (v < 0) snprintf(buf, n, "N/A");
    else snprintf(buf, n, approx ? "~%d%%" : "%d%%", (int)(v + 0.5f));
}

void fmtMhz(char* buf, size_t n, int mhz) {
    if (mhz <= 0) buf[0] = 0;
    else if (mhz >= 1000) snprintf(buf, n, "%.2f GHz", (float)mhz / 1000.f);
    else snprintf(buf, n, "%d MHz", mhz);
}

struct RowText { const char* label; uint32_t color; char value[16]; char extra[24]; };

void buildRows(const Sample& s, RowText rows[3]) {
    rows[0] = {"CPU", kCpuColor, {}, {}};
    rows[1] = {"GPU", kGpuColor, {}, {}};
    rows[2] = {"RAM", kRamColor, {}, {}};

    fmtPct(rows[0].value, sizeof rows[0].value, s.cpu, s.cpuFromFreq);
    int maxMhz = 0;
    for (int i = 0; i < s.nCores; i++) if (s.coreMhz[i] > maxMhz) maxMhz = s.coreMhz[i];
    fmtMhz(rows[0].extra, sizeof rows[0].extra, maxMhz);

    fmtPct(rows[1].value, sizeof rows[1].value, s.gpu, false);
    if (s.gpuMhz > 0) fmtMhz(rows[1].extra, sizeof rows[1].extra, s.gpuMhz);
    else if (s.gpuMemMb >= 0) snprintf(rows[1].extra, sizeof rows[1].extra, "MEM %.1f GB", s.gpuMemMb / 1024.f);

    fmtPct(rows[2].value, sizeof rows[2].value, s.ram, false);
    if (s.ramTotalMb > 0)
        snprintf(rows[2].extra, sizeof rows[2].extra, "%.1f / %.1f GB", s.ramUsedMb / 1024.f, s.ramTotalMb / 1024.f);
}

}  // namespace

void setFontAtlas(int slot, const uint8_t* alpha, int stride, int cellW, int cellH, int pad,
                  int ascent, const float* advances) {
    if (slot < 0 || slot >= kFontCount) return;
    FontAtlas& f = g_fonts[slot];
    const int rowBytes = cellW * kNumChars;
    f.alpha.resize((size_t)rowBytes * cellH);
    for (int y = 0; y < cellH; y++) memcpy(&f.alpha[(size_t)y * rowBytes], alpha + y * stride, rowBytes);
    f.cellW = cellW;
    f.cellH = cellH;
    f.pad = pad;
    f.ascent = ascent;
    memcpy(f.adv, advances, sizeof f.adv);
}

void History::push(const Sample& s) {
    cpu[head] = s.cpu;
    gpu[head] = s.gpu;
    ram[head] = s.ram;
    head = (head + 1) % N;
    if (count < N) count++;
}

float History::at(const float* series, int k) const {
    int missing = N - count;
    if (k < missing) return -1.f;
    int idx = (head - count + (k - missing) + N) % N;
    return series[idx];
}

Layout computeLayout(float density, bool compact) {
    Layout l;
    l.cell = (int)(density * 1.4f + 0.5f);
    if (l.cell < 2) l.cell = 2;
    l.compact = compact;
    const int c = l.cell, line = g_fonts[kFontLarge].lineH();
    l.width = (kInnerW + 2 * kPad) * c;
    if (compact) l.height = 2 * kPad * c + 3 * line + 2 * kTextGap * c;
    else l.height = 2 * kPad * c + 3 * (line + (kTextGap + kGraphH) * c) + 2 * kRowGap * c;
    return l;
}

void renderOverlay(uint32_t* pixels, int stride, int w, int h, const Layout& L,
                   const History& hist, const Sample& s, bool daemonLinked) {
    Canvas cv{pixels, stride, w, h};
    for (int y = 0; y < h; y++) memset(pixels + y * stride, 0, (size_t)w * 4);

    const int c = L.cell;
    cv.fillRoundRect(0, 0, w, h, 4 * c, kBg);
    cv.strokeRoundRect(0, 0, w, h, 4 * c, kBorder);
    // chấm xanh nhỏ ở góc: đang nhận số liệu từ statd
    if (daemonLinked) cv.fillRoundRect(w - 3 * c, c + c / 2, 3 * c / 2, 3 * c / 2, 3 * c / 4, kCpuColor);

    const FontAtlas& small = g_fonts[kFontSmall];
    const FontAtlas& large = g_fonts[kFontLarge];
    RowText rows[3];
    buildRows(s, rows);
    const float* series[3] = {hist.cpu, hist.gpu, hist.ram};
    const int x0 = kPad * c;
    const int right = w - kPad * c;
    const float valueX = (float)x0 + textWidth(small, "RAM") + 2.5f * (float)c;
    const int line = large.lineH();
    int y = kPad * c;

    for (int i = 0; i < 3; i++) {
        int baseline = y + large.ascent;
        drawText(cv, small, (float)x0, baseline, rows[i].label, rows[i].color);
        drawText(cv, large, valueX, baseline, rows[i].value, kWhite);
        drawText(cv, small, (float)right - textWidth(small, rows[i].extra), baseline, rows[i].extra, kGrey);
        y += line + kTextGap * c;
        if (!L.compact) {
            drawGraph(cv, x0, y, right - x0, kGraphH * c, hist, series[i], rows[i].color, c);
            y += (kGraphH + kRowGap) * c;
        }
    }
}
