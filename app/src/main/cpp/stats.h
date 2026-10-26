// Thu thập thông số CPU / GPU / RAM từ /proc và /sys.
// Dùng chung cho thư viện JNI của app và daemon statd (chạy qua adb shell).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

constexpr int kMaxCores = 16;
constexpr int kMaxTopProcs = 3;
constexpr float kNoValue = -1000.f;  // giá trị chưa đọc được (dùng cho số có thể âm)

struct Sample {
    // CPU
    float cpu = -1.f;            // tổng % tải, -1 = không đọc được
    bool cpuFromFreq = false;    // true = ước lượng từ tần số (không có /proc/stat)
    int nCores = 0;
    float coreLoad[kMaxCores];   // % mỗi nhân, -1 = offline / không rõ
    int coreMhz[kMaxCores];      // MHz mỗi nhân, -1 = không rõ
    // GPU
    float gpu = -1.f;            // % tải
    int gpuMhz = -1;
    float gpuMemMb = -1.f;       // bộ nhớ GPU toàn hệ thống (từ dumpsys gpu, chỉ daemon)
    // RAM
    float ram = -1.f;            // % đã dùng
    float ramUsedMb = 0.f;
    float ramTotalMb = 0.f;
    float swapUsedMb = 0.f;
    float swapTotalMb = 0.f;
    // Nhiệt độ (°C), từ /sys/class/thermal
    float cpuTemp = kNoValue;
    float gpuTemp = kNoValue;
    float batTemp = kNoValue;
    // Pin, do Java đẩy xuống qua BatteryManager (sysfs pin bị chặn)
    int batLevel = -1;
    float batWatts = kNoValue;   // dương = đang sạc, âm = đang xả
    float batVolts = kNoValue;
    // Tiến trình tốn CPU nhất (chỉ statd đọc được /proc/<pid>/stat)
    int nTop = 0;
    char topName[kMaxTopProcs][24] = {};
    float topPct[kMaxTopProcs] = {};  // % của một nhân, như lệnh top

    Sample() {
        for (int i = 0; i < kMaxCores; i++) { coreLoad[i] = -1.f; coreMhz[i] = -1; }
    }
};

class StatsCollector {
public:
    StatsCollector();
    void sample(Sample& out);
    // Mô tả nguồn dữ liệu đang dùng, để hiển thị trong app.
    std::string describe() const;

private:
    void readCpu(Sample& out);
    void readGpu(Sample& out);
    void readRam(Sample& out);
    void readTemps(Sample& out);

    int nCores_;
    int coreMaxMhz_[kMaxCores];
    uint64_t prevBusy_[kMaxCores + 1] = {};
    uint64_t prevTotal_[kMaxCores + 1] = {};
    bool havePrev_ = false;
    bool procStatOk_ = true;
    bool freqOk_ = false;
    bool ramOk_ = false;

    int gpuLoadSrc_ = -1;   // chỉ số trong bảng đường dẫn, -1 = chưa tìm thấy
    int gpuFreqSrc_ = -1;
    uint64_t prevGpuBusy_ = 0, prevGpuTotal_ = 0;

    std::vector<std::string> cpuTempPaths_, gpuTempPaths_, batTempPaths_;
};

// Định dạng một dòng text để daemon gửi cho app qua TCP localhost.
std::string sampleToLine(const Sample& s);
bool sampleFromLine(const char* line, Sample& s);

// Đọc số nguyên đầu tiên trong file, trả về false nếu không đọc được.
bool readFirstInt(const char* path, long long& v);
