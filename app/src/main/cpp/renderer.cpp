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

constexpr uint32_t kBg       = rgba(14, 16, 22, 248);
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

constexpr int kFirstChar = 32, kNumAscii = 95, kNumChars = 96;  // glyph cuối là '°'

// Lấy chỉ số glyph của ký tự tiếp theo trong chuỗi UTF-8 và tiến con trỏ, -1 nếu không có glyph.
int nextGlyph(const char*& s) {
    unsigned char c = (unsigned char)*s++;
    if (c == 0xC2 && (unsigned char)*s == 0xB0) { s++; return kNumAscii; }
    int i = c - kFirstChar;
    return i >= 0 && i < kNumAscii ? i : -1;
}

struct FontAtlas {
    std::vector<uint8_t> alpha;   // kNumChars ô xếp ngang
    int cellW = 0, cellH = 0, pad = 0, ascent = 0;
    float adv[kNumChars] = {};
    int lineH() const { return cellH - 2 * pad; }
} g_fonts[kFontCount];

float textWidth(const FontAtlas& f, const char* s) {
    float w = 0;
    while (*s) {
        int i = nextGlyph(s);
        if (i >= 0) w += f.adv[i];
    }
    return w;
}

// (x, baseline) là điểm bắt đầu trên đường chân chữ.
void drawText(Canvas& cv, const FontAtlas& f, float x, int baseline, const char* s, uint32_t color) {
    if (f.alpha.empty()) return;
    const int rowStride = f.cellW * kNumChars;
    const int gy = baseline - f.ascent - f.pad;
    while (*s) {
        int i = nextGlyph(s);
        if (i < 0) continue;
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
constexpr int kSepH = 5;        // vạch ngăn trước phần chi tiết
constexpr int kBarsH = 12;      // cột tải từng nhân

uint32_t loadColor(float pct) {
    if (pct < 50) return kCpuColor;
    if (pct < 80) return rgba(255, 213, 79, 255);
    return rgba(239, 83, 80, 255);
}

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

void drawCoreBars(Canvas& cv, int x, int y, int w, int h, const Sample& s, int cell) {
    const FontAtlas& tiny = g_fonts[kFontTiny];
    int n = s.nCores > 0 ? s.nCores : 1;
    int gap = cell;
    int bw = (w - (n - 1) * gap) / n;
    // cùng một đơn vị cho mọi cột: MHz nếu đủ chỗ cho 4 chữ số, không thì GHz
    const bool mhz = textWidth(tiny, "8888") <= (float)bw;
    for (int i = 0; i < n; i++) {
        int bx = x + i * (bw + gap);
        cv.fillRect(bx, y, bw, h, kGraphBg);
        float l = s.coreLoad[i];
        if (l >= 0) {
            int fh = (int)(l / 100.f * (float)h + 0.5f);
            cv.fillRect(bx, y + h - fh, bw, fh, withAlpha(loadColor(l), 190));
        }
        char t[8];
        if (s.coreMhz[i] <= 0) snprintf(t, sizeof t, "-");
        else if (mhz) snprintf(t, sizeof t, "%d", s.coreMhz[i]);
        else snprintf(t, sizeof t, "%.1f", (float)s.coreMhz[i] / 1000.f);
        float tw = textWidth(tiny, t);
        if (tw <= (float)bw)
            drawText(cv, tiny, (float)bx + ((float)bw - tw) / 2, y + (h + tiny.ascent) / 2 - cell / 2, t, kWhite);
    }
}

void fmtTemp(char* buf, size_t n, float c) {
    if (c <= kNoValue) snprintf(buf, n, "--");
    else snprintf(buf, n, "%d\xC2\xB0", (int)(c + 0.5f));
}

// Cắt chuỗi cho vừa maxW, thêm ".." ở cuối.
void fitText(const FontAtlas& f, const char* in, char* out, size_t cap, float maxW) {
    snprintf(out, cap, "%s", in);
    size_t len = strlen(out);
    while (len > 1 && textWidth(f, out) > maxW) {
        out[--len] = 0;
        if (len > 2) { out[len - 2] = '.'; out[len - 1] = '.'; }
    }
}

int detailLineStep(int cell) { return g_fonts[kFontSmall].lineH() + cell; }

int detailsHeight(int details, int cell) {
    if (!details) return 0;
    int h = kSepH * cell;
    int step = detailLineStep(cell);
    if (details & kDetailCores) h += (kBarsH + 2) * cell;
    if (details & kDetailTemps) h += step;
    if (details & kDetailBattery) h += step;
    if (details & kDetailSwap) h += step;
    if (details & kDetailTopProcs) h += kMaxTopProcs * step;
    return h - cell;  // bỏ khoảng trống sau dòng cuối
}

void drawDetails(Canvas& cv, const Layout& L, const Sample& s, int x0, int right, int y) {
    const int c = L.cell;
    const FontAtlas& small = g_fonts[kFontSmall];
    const int step = detailLineStep(c);
    const int w = right - x0;
    char a[32], b[32], t[32];

    cv.fillRect(x0, y + kSepH * c / 2 - c, w, 1, kGrid);
    y += kSepH * c;

    if (L.details & kDetailCores) {
        drawCoreBars(cv, x0, y, w, kBarsH * c, s, c);
        y += (kBarsH + 2) * c;
    }
    if (L.details & kDetailTemps) {
        const char* labels[3] = {"CPU", "GPU", "PIN"};
        const uint32_t colors[3] = {kCpuColor, kGpuColor, kRamColor};
        const float temps[3] = {s.cpuTemp, s.gpuTemp, s.batTemp};
        for (int i = 0; i < 3; i++) {
            float x = (float)(x0 + w * i / 3);
            int baseline = y + small.ascent;
            drawText(cv, small, x, baseline, labels[i], colors[i]);
            fmtTemp(t, sizeof t, temps[i]);
            drawText(cv, small, x + textWidth(small, labels[i]) + 1.5f * (float)c, baseline, t, kWhite);
        }
        y += step;
    }
    if (L.details & kDetailBattery) {
        int baseline = y + small.ascent;
        drawText(cv, small, (float)x0, baseline, "PIN", kGrey);
        if (s.batLevel >= 0) snprintf(a, sizeof a, "%d%%", s.batLevel); else snprintf(a, sizeof a, "--");
        if (s.batWatts > kNoValue) snprintf(b, sizeof b, "%+.2f W", s.batWatts); else snprintf(b, sizeof b, "--");
        if (s.batVolts > kNoValue) snprintf(t, sizeof t, "%.2f V", s.batVolts); else t[0] = 0;
        drawText(cv, small, (float)(x0 + w / 5), baseline, a, kWhite);
        drawText(cv, small, (float)(x0 + w * 9 / 20), baseline, b, s.batWatts > 0 ? kCpuColor : kWhite);
        drawText(cv, small, (float)right - textWidth(small, t), baseline, t, kGrey);
        y += step;
    }
    if (L.details & kDetailSwap) {
        int baseline = y + small.ascent;
        drawText(cv, small, (float)x0, baseline, "SWAP", kGrey);
        if (s.swapTotalMb > 0)
            snprintf(t, sizeof t, "%.1f / %.1f GB", s.swapUsedMb / 1024.f, s.swapTotalMb / 1024.f);
        else
            snprintf(t, sizeof t, "--");
        drawText(cv, small, (float)right - textWidth(small, t), baseline, t, kWhite);
        y += step;
    }
    if (L.details & kDetailTopProcs) {
        if (s.nTop == 0) {
            drawText(cv, small, (float)x0, y + small.ascent, "TOP: needs statd", kGrey);
        }
        for (int i = 0; i < s.nTop; i++) {
            int baseline = y + i * step + small.ascent;
            snprintf(b, sizeof b, "%.0f%%", s.topPct[i]);
            float pw = textWidth(small, b);
            fitText(small, s.topName[i], t, sizeof t, (float)w - pw - 3.f * (float)c);
            drawText(cv, small, (float)x0, baseline, t, kWhite);
            drawText(cv, small, (float)right - pw, baseline, b, loadColor(s.topPct[i] / 2.f));
        }
    }
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

Layout computeLayout(float density, bool compact, int details) {
    Layout l;
    l.cell = (int)(density * 1.4f + 0.5f);
    if (l.cell < 2) l.cell = 2;
    l.compact = compact;
    l.details = details;
    const int c = l.cell, line = g_fonts[kFontLarge].lineH();
    l.width = (kInnerW + 2 * kPad) * c;
    if (compact) l.height = 2 * kPad * c + 3 * line + 2 * kTextGap * c;
    else l.height = 2 * kPad * c + 3 * (line + (kTextGap + kGraphH) * c) + 2 * kRowGap * c;
    l.height += detailsHeight(details, c);
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
            y += (kGraphH + (i < 2 ? kRowGap : 0)) * c;
        }
    }
    if (L.details) drawDetails(cv, L, s, x0, right, L.compact ? y - kTextGap * c : y);
}
