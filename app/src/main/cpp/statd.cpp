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
#include <dirent.h>
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
#include <algorithm>
#include <deque>
#include <unordered_map>
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

// Top tiến trình theo CPU, giống lệnh `top`: % của một nhân trong khoảng giữa hai lần quét.
class ProcTop {
public:
    void sample(Sample& out) {
        double now = monotonicSec();
        double dt = now - lastT_;
        std::unordered_map<int, uint64_t> ticks;
        std::vector<std::pair<uint64_t, int>> deltas;  // (Δticks, pid)
        if (DIR* d = opendir("/proc")) {
            while (dirent* e = readdir(d)) {
                if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
                int pid = atoi(e->d_name);
                uint64_t t;
                if (!readTicks(pid, t)) continue;
                ticks[pid] = t;
                auto it = prev_.find(pid);
                if (it != prev_.end() && t > it->second) deltas.push_back({t - it->second, pid});
            }
            closedir(d);
        }
        prev_.swap(ticks);
        lastT_ = now;
        if (dt <= 0 || dt > 10) return;  // lần quét đầu tiên

        size_t n = std::min(deltas.size(), (size_t)kMaxTopProcs);
        std::partial_sort(deltas.begin(), deltas.begin() + (long)n, deltas.end(),
                          [](const auto& a, const auto& b) { return a.first > b.first; });
        static const double hz = (double)sysconf(_SC_CLK_TCK);
        out.nTop = 0;
        for (size_t i = 0; i < n; i++) {
            procName(deltas[i].second, out.topName[out.nTop], sizeof out.topName[0]);
            out.topPct[out.nTop++] = (float)((double)deltas[i].first / hz / dt * 100.0);
        }
    }

private:
    static bool readTicks(int pid, uint64_t& t) {
        char path[32], buf[512];
        snprintf(path, sizeof path, "/proc/%d/stat", pid);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) return false;
        ssize_t n = read(fd, buf, sizeof buf - 1);
        close(fd);
        if (n <= 0) return false;
        buf[n] = 0;
        // tên tiến trình nằm trong (...) và có thể chứa khoảng trắng: bắt đầu sau ')' cuối cùng
        const char* p = strrchr(buf, ')');
        if (!p) return false;
        unsigned long long utime, stime;
        if (sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &utime, &stime) != 2)
            return false;
        t = utime + stime;
        return true;
    }

    // Tên gói (cmdline) cho app, tên trong /proc/<pid>/stat cho tiến trình nhân.
    static void procName(int pid, char* out, size_t cap) {
        char path[32], buf[256] = {};
        snprintf(path, sizeof path, "/proc/%d/cmdline", pid);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        ssize_t n = fd >= 0 ? read(fd, buf, sizeof buf - 1) : -1;
        if (fd >= 0) close(fd);
        const char* name = buf;
        if (n > 0) {
            const char* slash = strrchr(buf, '/');
            if (slash) name = slash + 1;
        } else {
            snprintf(path, sizeof path, "/proc/%d/stat", pid);
            fd = open(path, O_RDONLY | O_CLOEXEC);
            n = fd >= 0 ? read(fd, buf, sizeof buf - 1) : -1;
            if (fd >= 0) close(fd);
            char* l = strchr(buf, '(');
            char* r = strrchr(buf, ')');
            if (n <= 0 || !l || !r || r < l) { snprintf(out, cap, "pid%d", pid); return; }
            *r = 0;
            name = l + 1;
        }
        size_t j = 0;
        for (const char* c = name; *c && j + 1 < cap; c++)
            out[j++] = (*c == ' ' || *c == ':' || *c == ',' || (unsigned char)*c < 32) ? '_' : *c;
        out[j] = 0;
        if (!j) snprintf(out, cap, "pid%d", pid);
    }

    std::unordered_map<int, uint64_t> prev_;
    double lastT_ = 0;
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
    ProcTop procTop;
    Sample lastTop;
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
        // quét ~1000 tiến trình mỗi giây một lần là đủ
        if (!clients.empty() && tick % 2 == 0) procTop.sample(lastTop);
        s.nTop = lastTop.nTop;
        memcpy(s.topName, lastTop.topName, sizeof s.topName);
        memcpy(s.topPct, lastTop.topPct, sizeof s.topPct);
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
