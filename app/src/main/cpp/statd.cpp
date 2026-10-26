// statd: daemon nhỏ chạy bằng quyền shell (adb) hoặc root, đọc các thông số mà app thường
// bị SELinux chặn (/proc/stat, GPU sysfs, dumpsys gpu) rồi phát qua TCP 127.0.0.1:27183.
// Máy khóa cả GPU sysfs với shell (vd. OPPO/OnePlus Android 14+) thì tải GPU được tính từ
// `dumpsys gpu --gpuwork`: tổng thời gian GPU bận của mọi uid (tracepoint gpu_work_period).
//
//   adb push statd /data/local/tmp/ && adb shell /data/local/tmp/statd
//   adb shell su -c /data/local/tmp/statd      # máy đã root: đọc được cả tải GPU
//
// Tuỳ chọn: -f chạy foreground (in ra stdout), -i <ms> chu kỳ lấy mẫu, -p <port>.
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <vector>

#include "stats.h"

static float readGpuMemMb() {
    FILE* f = popen("dumpsys gpu 2>/dev/null", "r");
    if (!f) return -1.f;
    char line[256];
    float mb = -1.f;
    while (fgets(line, sizeof line, f)) {
        const char* p = strstr(line, "Global total:");
        if (p) { mb = (float)(strtoll(p + 13, nullptr, 10) / (1024.0 * 1024.0)); break; }
    }
    // đọc hết output để dumpsys không bị SIGPIPE giữa chừng
    while (fgets(line, sizeof line, f)) {}
    pclose(f);
    return mb;
}

// Tổng thời gian GPU bận (ns) cộng dồn của mọi uid. False nếu máy không hỗ trợ gpuwork.
static bool readGpuWorkActiveNs(uint64_t& total) {
    FILE* f = popen("dumpsys gpu --gpuwork 2>/dev/null", "r");
    if (!f) return false;
    char line[256];
    bool found = false;
    total = 0;
    while (fgets(line, sizeof line, f)) {
        // gpu_id uid total_active_duration_ns total_inactive_duration_ns
        unsigned long long gpuId, uid, active, inactive;
        if (sscanf(line, "%llu %llu %llu %llu", &gpuId, &uid, &active, &inactive) == 4) {
            total += active;
            found = true;
        }
    }
    pclose(f);
    return found;
}

static double monotonicSec() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

// Tải GPU = Δ(thời gian bận) / Δ(thời gian thực), lấy trên cửa sổ ~1.5s cho đỡ giật
// (mỗi uid chỉ được cộng khi kết thúc một work period).
class GpuWorkMeter {
public:
    float update() {
        if (!supported_) return -1.f;
        uint64_t busy;
        if (!readGpuWorkActiveNs(busy)) {
            supported_ = false;
            return -1.f;
        }
        double now = monotonicSec();
        hist_.push_back({now, busy});
        while (hist_.size() > 2 && now - hist_.front().t > 1.5) hist_.pop_front();
        if (hist_.size() < 2) return -1.f;
        const Point& a = hist_.front();
        double dt = now - a.t;
        if (dt <= 0 || busy < a.busy) return -1.f;
        float pct = (float)((double)(busy - a.busy) / (dt * 1e9) * 100.0);
        return pct > 100.f ? 100.f : pct;
    }

private:
    struct Point { double t; uint64_t busy; };
    std::deque<Point> hist_;
    bool supported_ = true;
};

int main(int argc, char** argv) {
    bool foreground = false;
    int intervalMs = 500, port = 27183;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-f")) foreground = true;
        else if (!strcmp(argv[i], "-i") && i + 1 < argc) intervalMs = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-p") && i + 1 < argc) port = atoi(argv[++i]);
    }
    if (intervalMs < 100) intervalMs = 100;
    signal(SIGPIPE, SIG_IGN);

    int srv = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(srv, (sockaddr*)&addr, sizeof addr) != 0 || listen(srv, 4) != 0) {
        fprintf(stderr, "statd: không mở được cổng %d (đã có statd khác đang chạy?)\n", port);
        return 1;
    }
    fcntl(srv, F_SETFL, O_NONBLOCK);

    if (!foreground) {
        // Tách khỏi phiên adb shell để daemon vẫn chạy sau khi đóng terminal.
        pid_t pid = fork();
        if (pid < 0) return 1;
        if (pid > 0) {
            printf("statd đang chạy nền (pid %d), cổng %d\n", pid, port);
            return 0;
        }
        setsid();
        int devnull = open("/dev/null", O_RDWR);
        dup2(devnull, 0);
        dup2(devnull, 1);
        dup2(devnull, 2);
    }

    StatsCollector collector;
    GpuWorkMeter gpuWork;
    std::vector<int> clients;
    float gpuMem = -1.f;
    unsigned tick = 0;
    for (;;) {
        for (int c; (c = accept4(srv, nullptr, nullptr, SOCK_CLOEXEC)) >= 0;) clients.push_back(c);

        Sample s;
        collector.sample(s);
        if (!clients.empty() && tick++ % 4 == 0) gpuMem = readGpuMemMb();
        s.gpuMemMb = gpuMem;
        if (s.gpu < 0 && !clients.empty()) s.gpu = gpuWork.update();
        std::string line = sampleToLine(s);
        if (foreground) fputs(line.c_str(), stdout), fflush(stdout);

        for (size_t i = 0; i < clients.size();) {
            if (send(clients[i], line.data(), line.size(), MSG_NOSIGNAL | MSG_DONTWAIT) < 0) {
                close(clients[i]);
                clients.erase(clients.begin() + (long)i);
            } else {
                i++;
            }
        }
        usleep((useconds_t)intervalMs * 1000);
    }
}
