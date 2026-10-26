#include "stats.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <dirent.h>
#include <unistd.h>

namespace {

// Đọc toàn bộ file nhỏ (sysfs/procfs) vào buf. Trả về số byte, -1 nếu lỗi.
int readSmallFile(const char* path, char* buf, int cap) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    int n = (int)read(fd, buf, cap - 1);
    close(fd);
    if (n < 0) return -1;
    buf[n] = 0;
    return n;
}

// Lấy tối đa `max` số nguyên trong chuỗi, bỏ qua ký tự không phải số.
int parseInts(const char* s, long long* out, int max) {
    int n = 0;
    while (*s && n < max) {
        if ((*s >= '0' && *s <= '9') || (*s == '-' && s[1] >= '0' && s[1] <= '9')) {
            char* end;
            out[n++] = strtoll(s, &end, 10);
            s = end;
        } else {
            s++;
        }
    }
    return n;
}

enum GpuKind { kPercent, kBusyTotal };

struct GpuLoadPath { const char* path; GpuKind kind; };

// Đường dẫn tải GPU phổ biến: Qualcomm Adreno (kgsl), Samsung (Exynos/Snapdragon),
// MediaTek (ged), ARM Mali. Thường cần quyền shell hoặc root mới đọc được.
const GpuLoadPath kGpuLoadPaths[] = {
    {"/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage", kPercent},
    {"/sys/class/kgsl/kgsl-3d0/gpubusy", kBusyTotal},
    {"/sys/kernel/gpu/gpu_busy", kPercent},
    {"/sys/kernel/ged/hal/gpu_utilization", kPercent},
    {"/sys/class/misc/mali0/device/utilization", kPercent},
    {"/sys/module/ged/parameters/gpu_loading", kPercent},
};

struct GpuFreqPath { const char* path; long long divToMhz; };

const GpuFreqPath kGpuFreqPaths[] = {
    {"/sys/class/kgsl/kgsl-3d0/gpuclk", 1000000},
    {"/sys/class/kgsl/kgsl-3d0/devfreq/cur_freq", 1000000},
    {"/sys/kernel/gpu/gpu_clock", 1},
    {"/sys/class/misc/mali0/device/clock", 1000},
};

// Mali trên nhiều SoC nằm ở /sys/devices/platform/<addr>.mali/utilization.
std::string g_maliGlobPath;

bool readGpuLoad(const char* path, GpuKind kind, uint64_t& prevBusy, uint64_t& prevTotal,
                 float& out) {
    char buf[256];
    if (readSmallFile(path, buf, sizeof buf) <= 0) return false;
    long long v[2];
    int n = parseInts(buf, v, 2);
    if (kind == kPercent) {
        if (n < 1 || v[0] < 0 || v[0] > 100) return false;
        out = (float)v[0];
        return true;
    }
    // "busy total" là bộ đếm cộng dồn hoặc theo cửa sổ, tuỳ kernel.
    if (n < 2) return false;
    uint64_t busy = (uint64_t)v[0], total = (uint64_t)v[1];
    if (total == 0) { out = 0.f; return true; }
    if (busy <= total && (prevTotal == 0 || total < prevTotal)) {
        out = 100.f * (float)busy / (float)total;  // giá trị theo cửa sổ
    } else {
        uint64_t db = busy - prevBusy, dt = total - prevTotal;
        out = dt ? 100.f * (float)db / (float)dt : 0.f;
    }
    prevBusy = busy;
    prevTotal = total;
    return true;
}

bool startsWith(const char* s, const char* prefix) {
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

// Nhiệt độ lớn nhất (°C) trong các thermal zone, kNoValue nếu không đọc được.
float maxTemp(const std::vector<std::string>& paths) {
    float best = kNoValue;
    for (const std::string& p : paths) {
        char buf[32];
        long long v;
        if (readSmallFile(p.c_str(), buf, sizeof buf) <= 0 || parseInts(buf, &v, 1) != 1) continue;
        float c = v > 1000 || v < -1000 ? (float)v / 1000.f : (float)v;  // đa số kernel dùng m°C
        if (c > -30.f && c < 150.f && c > best) best = c;
    }
    return best;
}

}  // namespace

bool readFirstInt(const char* path, long long& v) {
    char buf[128];
    if (readSmallFile(path, buf, sizeof buf) <= 0) return false;
    return parseInts(buf, &v, 1) == 1;
}

StatsCollector::StatsCollector() {
    nCores_ = (int)sysconf(_SC_NPROCESSORS_CONF);
    if (nCores_ < 1) nCores_ = 1;
    if (nCores_ > kMaxCores) nCores_ = kMaxCores;
    for (int i = 0; i < kMaxCores; i++) {
        coreMaxMhz_[i] = -1;
        if (i >= nCores_) continue;
        char path[96];
        long long v;
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", i);
        if (readFirstInt(path, v) && v > 0) coreMaxMhz_[i] = (int)(v / 1000);
    }

    float dummy;
    for (int i = 0; i < (int)(sizeof kGpuLoadPaths / sizeof *kGpuLoadPaths); i++) {
        if (readGpuLoad(kGpuLoadPaths[i].path, kGpuLoadPaths[i].kind, prevGpuBusy_,
                        prevGpuTotal_, dummy)) {
            gpuLoadSrc_ = i;
            break;
        }
    }
    if (gpuLoadSrc_ < 0) {
        // glob() chỉ có từ API 28 nên tự duyệt thư mục.
        if (DIR* d = opendir("/sys/devices/platform")) {
            while (dirent* e = readdir(d)) {
                if (!strstr(e->d_name, "mali")) continue;
                std::string path = std::string("/sys/devices/platform/") + e->d_name + "/utilization";
                if (readGpuLoad(path.c_str(), kPercent, prevGpuBusy_, prevGpuTotal_, dummy)) {
                    g_maliGlobPath = path;
                    gpuLoadSrc_ = 1000;
                    break;
                }
            }
            closedir(d);
        }
    }
    for (int i = 0; i < (int)(sizeof kGpuFreqPaths / sizeof *kGpuFreqPaths); i++) {
        long long v;
        if (readFirstInt(kGpuFreqPaths[i].path, v) && v > 0) {
            gpuFreqSrc_ = i;
            break;
        }
    }

    // Phân loại thermal zone theo tên: Qualcomm (cpu-*, cpuss-*, gpuss-*), MediaTek (mtktscpu),
    // Exynos (BIG/MID/LITTLE, G3D), pin (battery, bat_therm).
    for (int i = 0; i < 128; i++) {
        char path[64], type[64];
        snprintf(path, sizeof path, "/sys/class/thermal/thermal_zone%d/type", i);
        int n = readSmallFile(path, type, sizeof type);
        if (n <= 0) {
            if (i > 0 && access(path, F_OK) != 0) break;
            continue;
        }
        for (char* c = type; *c; c++) {
            if (*c == '\n') *c = 0;
            else if (*c >= 'A' && *c <= 'Z') *c += 32;
        }
        snprintf(path, sizeof path, "/sys/class/thermal/thermal_zone%d/temp", i);
        if (startsWith(type, "cpu") || strstr(type, "tscpu") || !strcmp(type, "big") ||
            !strcmp(type, "mid") || !strcmp(type, "little")) {
            cpuTempPaths_.push_back(path);
        } else if (startsWith(type, "gpu") || strstr(type, "tsgpu") || !strcmp(type, "g3d")) {
            gpuTempPaths_.push_back(path);
        } else if (!strcmp(type, "battery") || startsWith(type, "bat_therm") ||
                   !strcmp(type, "battery_therm")) {
            batTempPaths_.push_back(path);
        }
    }
}

void StatsCollector::sample(Sample& out) {
    readCpu(out);
    readGpu(out);
    readRam(out);
    readTemps(out);
}

void StatsCollector::readTemps(Sample& out) {
    out.cpuTemp = maxTemp(cpuTempPaths_);
    out.gpuTemp = maxTemp(gpuTempPaths_);
    out.batTemp = maxTemp(batTempPaths_);
}

void StatsCollector::readCpu(Sample& out) {
    out.nCores = nCores_;
    freqOk_ = false;
    for (int i = 0; i < nCores_; i++) {
        char path[96];
        long long v;
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", i);
        if (readFirstInt(path, v) && v > 0) {
            out.coreMhz[i] = (int)(v / 1000);
            freqOk_ = true;
        } else {
            out.coreMhz[i] = -1;
        }
    }

    if (procStatOk_) {
        // Android 8+ chặn /proc/stat với app thường; daemon chạy bằng shell thì đọc được.
        char buf[4096];
        if (readSmallFile("/proc/stat", buf, sizeof buf) <= 0) {
            procStatOk_ = false;
        } else {
            bool seen[kMaxCores + 1] = {};
            uint64_t busy[kMaxCores + 1] = {}, total[kMaxCores + 1] = {};
            for (char* line = buf; line && *line; ) {
                char* next = strchr(line, '\n');
                if (next) *next++ = 0;
                if (strncmp(line, "cpu", 3) != 0) break;
                int idx;  // 0 = tổng, 1..n = cpu0..cpuN-1
                const char* p = line + 3;
                if (*p == ' ') {
                    idx = 0;
                } else {
                    idx = atoi(p) + 1;
                    if (idx > nCores_) { line = next; continue; }
                }
                // user nice system idle iowait irq softirq steal
                long long f[8] = {};
                const char* fields = strchr(p, ' ');
                int n = fields ? parseInts(fields, f, 8) : 0;
                if (n >= 4) {
                    uint64_t idle = (uint64_t)f[3] + (uint64_t)f[4];
                    uint64_t t = 0;
                    for (int k = 0; k < n; k++) t += (uint64_t)f[k];
                    busy[idx] = t - idle;
                    total[idx] = t;
                    seen[idx] = true;
                }
                line = next;
            }
            for (int i = 0; i <= nCores_; i++) {
                float load = -1.f;
                if (seen[i] && havePrev_ && total[i] > prevTotal_[i]) {
                    load = 100.f * (float)(busy[i] - prevBusy_[i]) / (float)(total[i] - prevTotal_[i]);
                    if (load < 0) load = 0;
                    if (load > 100) load = 100;
                } else if (seen[i] && havePrev_) {
                    load = 0.f;
                }
                if (seen[i]) { prevBusy_[i] = busy[i]; prevTotal_[i] = total[i]; }
                else { prevBusy_[i] = prevTotal_[i] = 0; }
                if (i == 0) out.cpu = load;
                else out.coreLoad[i - 1] = load;
            }
            out.cpuFromFreq = false;
            havePrev_ = true;
            return;
        }
    }

    // Không có /proc/stat: ước lượng theo tần số hiện tại / tần số tối đa.
    float sum = 0.f;
    int online = 0;
    for (int i = 0; i < nCores_; i++) {
        if (out.coreMhz[i] > 0 && coreMaxMhz_[i] > 0) {
            float l = 100.f * (float)out.coreMhz[i] / (float)coreMaxMhz_[i];
            if (l > 100) l = 100;
            out.coreLoad[i] = l;
            sum += l;
            online++;
        } else {
            out.coreLoad[i] = -1.f;
        }
    }
    out.cpu = online ? sum / (float)online : -1.f;
    out.cpuFromFreq = true;
}

void StatsCollector::readGpu(Sample& out) {
    out.gpu = -1.f;
    if (gpuLoadSrc_ == 1000) {
        if (!readGpuLoad(g_maliGlobPath.c_str(), kPercent, prevGpuBusy_, prevGpuTotal_, out.gpu))
            out.gpu = -1.f;
    } else if (gpuLoadSrc_ >= 0) {
        const GpuLoadPath& p = kGpuLoadPaths[gpuLoadSrc_];
        if (!readGpuLoad(p.path, p.kind, prevGpuBusy_, prevGpuTotal_, out.gpu)) out.gpu = -1.f;
    }
    out.gpuMhz = -1;
    if (gpuFreqSrc_ >= 0) {
        long long v;
        if (readFirstInt(kGpuFreqPaths[gpuFreqSrc_].path, v) && v > 0)
            out.gpuMhz = (int)(v / kGpuFreqPaths[gpuFreqSrc_].divToMhz);
    }
}

void StatsCollector::readRam(Sample& out) {
    char buf[2048];
    ramOk_ = false;
    if (readSmallFile("/proc/meminfo", buf, sizeof buf) <= 0) return;
    long long total = -1, avail = -1;
    const char* p = strstr(buf, "MemTotal:");
    if (p) parseInts(p, &total, 1);
    p = strstr(buf, "MemAvailable:");
    if (p) parseInts(p, &avail, 1);
    if (total <= 0 || avail < 0) return;
    out.ramTotalMb = (float)total / 1024.f;
    out.ramUsedMb = (float)(total - avail) / 1024.f;
    out.ram = 100.f * out.ramUsedMb / out.ramTotalMb;
    ramOk_ = true;

    long long swapTotal = -1, swapFree = -1;
    if ((p = strstr(buf, "SwapTotal:"))) parseInts(p, &swapTotal, 1);
    if ((p = strstr(buf, "SwapFree:"))) parseInts(p, &swapFree, 1);
    if (swapTotal > 0 && swapFree >= 0) {
        out.swapTotalMb = (float)swapTotal / 1024.f;
        out.swapUsedMb = (float)(swapTotal - swapFree) / 1024.f;
    }
}

std::string StatsCollector::describe() const {
    std::string s = "CPU: ";
    if (procStatOk_) s += "/proc/stat (tải thật)";
    else if (freqOk_) s += "ước lượng theo tần số (/proc/stat bị chặn)";
    else s += "không đọc được";
    s += "\nGPU tải: ";
    if (gpuLoadSrc_ == 1000) s += g_maliGlobPath;
    else if (gpuLoadSrc_ >= 0) s += kGpuLoadPaths[gpuLoadSrc_].path;
    else s += "bị chặn (cần statd/root)";
    s += "\nGPU xung: ";
    s += gpuFreqSrc_ >= 0 ? kGpuFreqPaths[gpuFreqSrc_].path : "bị chặn";
    s += "\nRAM: ";
    s += ramOk_ ? "/proc/meminfo" : "không đọc được";
    return s;
}

std::string sampleToLine(const Sample& s) {
    std::string line;
    char tmp[64];
    snprintf(tmp, sizeof tmp, "cpu=%.1f ff=%d n=%d cl=", s.cpu, s.cpuFromFreq ? 1 : 0, s.nCores);
    line += tmp;
    for (int i = 0; i < s.nCores; i++) {
        snprintf(tmp, sizeof tmp, i ? ",%.0f" : "%.0f", s.coreLoad[i]);
        line += tmp;
    }
    line += " cf=";
    for (int i = 0; i < s.nCores; i++) {
        snprintf(tmp, sizeof tmp, i ? ",%d" : "%d", s.coreMhz[i]);
        line += tmp;
    }
    snprintf(tmp, sizeof tmp, " gpu=%.1f gf=%d gm=%.0f", s.gpu, s.gpuMhz, s.gpuMemMb);
    line += tmp;
    if (s.nTop > 0) {
        // tên tiến trình đã được statd làm sạch: không có khoảng trắng, ':' hay ','
        line += " tp=";
        for (int i = 0; i < s.nTop; i++) {
            snprintf(tmp, sizeof tmp, i ? ",%s:%.0f" : "%s:%.0f", s.topName[i], s.topPct[i]);
            line += tmp;
        }
    }
    line += "\n";
    return line;
}

bool sampleFromLine(const char* line, Sample& s) {
    char buf[1024];
    strncpy(buf, line, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    bool any = false;
    char* save = nullptr;
    for (char* tok = strtok_r(buf, " \n", &save); tok; tok = strtok_r(nullptr, " \n", &save)) {
        char* eq = strchr(tok, '=');
        if (!eq) continue;
        *eq = 0;
        const char* k = tok;
        const char* v = eq + 1;
        if (!strcmp(k, "cpu")) { s.cpu = strtof(v, nullptr); any = true; }
        else if (!strcmp(k, "ff")) s.cpuFromFreq = atoi(v) != 0;
        else if (!strcmp(k, "n")) { s.nCores = atoi(v); if (s.nCores > kMaxCores) s.nCores = kMaxCores; }
        else if (!strcmp(k, "cl") || !strcmp(k, "cf")) {
            bool load = k[1] == 'l';
            long long vals[kMaxCores];
            int n = parseInts(v, vals, kMaxCores);
            for (int i = 0; i < n; i++) {
                if (load) s.coreLoad[i] = (float)vals[i];
                else s.coreMhz[i] = (int)vals[i];
            }
        }
        else if (!strcmp(k, "gpu")) s.gpu = strtof(v, nullptr);
        else if (!strcmp(k, "gf")) s.gpuMhz = atoi(v);
        else if (!strcmp(k, "gm")) s.gpuMemMb = strtof(v, nullptr);
        else if (!strcmp(k, "tp")) {
            s.nTop = 0;
            char* save2 = nullptr;
            char list[512];
            strncpy(list, v, sizeof list - 1);
            list[sizeof list - 1] = 0;
            for (char* e = strtok_r(list, ",", &save2); e && s.nTop < kMaxTopProcs;
                 e = strtok_r(nullptr, ",", &save2)) {
                char* colon = strrchr(e, ':');
                if (!colon) continue;
                *colon = 0;
                snprintf(s.topName[s.nTop], sizeof s.topName[0], "%s", e);
                s.topPct[s.nTop++] = strtof(colon + 1, nullptr);
            }
        }
    }
    return any;
}
