#include "blocker.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <unordered_set>

namespace {

constexpr const char* kListFile = "/data/local/tmp/statd.block";

// Chạy lệnh không qua shell (tránh chèn lệnh), chờ xong, bỏ output.
int run(const std::vector<const char*>& argv) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR);
        dup2(devnull, 1);
        dup2(devnull, 2);
        std::vector<char*> args;
        for (const char* a : argv) args.push_back(const_cast<char*>(a));
        args.push_back(nullptr);
        execvp(args[0], args.data());
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// Package của activity đang ở trên cùng, rỗng nếu không xác định được.
std::string foregroundPackage() {
    FILE* f = popen("dumpsys activity activities 2>/dev/null", "r");
    if (!f) return {};
    char line[512];
    std::string pkg;
    while (fgets(line, sizeof line, f)) {
        // topResumedActivity=ActivityRecord{184823292 u0 com.foo/.MainActivity t11854}
        const char* p = strstr(line, "topResumedActivity=");
        if (!p) continue;
        p = strstr(p, " u0 ");
        if (!p) p = strstr(line, " u");
        if (p) {
            p = strchr(p + 1, ' ');
            const char* slash = p ? strchr(p, '/') : nullptr;
            if (p && slash) pkg.assign(p + 1, slash);
        }
        break;
    }
    while (fgets(line, sizeof line, f)) {}
    pclose(f);
    return pkg;
}

// Các package đang có tiến trình (lấy từ cmdline, bỏ hậu tố ":service").
std::unordered_set<std::string> runningPackages() {
    std::unordered_set<std::string> out;
    DIR* d = opendir("/proc");
    if (!d) return out;
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        char path[32], buf[128];
        snprintf(path, sizeof path, "/proc/%s/cmdline", e->d_name);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;
        ssize_t n = read(fd, buf, sizeof buf - 1);
        close(fd);
        if (n <= 0) continue;
        buf[n] = 0;
        if (char* colon = strchr(buf, ':')) *colon = 0;
        if (strchr(buf, '.') && !strchr(buf, '/')) out.insert(buf);
    }
    closedir(d);
    return out;
}

bool isProtected(const std::string& pkg) {
    return pkg == kAppPackage || pkg == "android" || pkg.rfind("com.android.", 0) == 0;
}

}  // namespace

bool BackgroundBlocker::validPackage(const std::string& pkg) {
    if (pkg.empty() || pkg.size() > 200) return false;
    bool dot = false;
    for (size_t i = 0; i < pkg.size(); i++) {
        char c = pkg[i];
        if (c == '.') {
            if (i == 0 || pkg[i - 1] == '.') return false;
            dot = true;
        } else if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
                     (c >= '0' && c <= '9' && i > 0))) {
            return false;
        }
    }
    return dot && pkg.back() != '.';
}

void BackgroundBlocker::load() {
    FILE* f = fopen(kListFile, "r");
    if (!f) return;
    std::vector<std::string> pkgs;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (validPackage(line) && !isProtected(line)) pkgs.push_back(line);
    }
    fclose(f);
    setList(pkgs);
}

void BackgroundBlocker::save() const {
    FILE* f = fopen(kListFile, "w");
    if (!f) return;
    for (const std::string& p : pkgs_) fprintf(f, "%s\n", p.c_str());
    fclose(f);
}

std::string BackgroundBlocker::setList(const std::vector<std::string>& pkgs) {
    for (const std::string& p : pkgs) {
        if (!validPackage(p)) return "invalid package " + p;
        if (isProtected(p)) return "protected package " + p;
    }
    double now = 0;
    {
        timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        now = (double)ts.tv_sec;
    }
    // Bỏ chặn: trả standby bucket về "active" để hệ thống tự đánh giá lại.
    for (const std::string& old : pkgs_) {
        if (std::find(pkgs.begin(), pkgs.end(), old) == pkgs.end()) {
            run({"am", "set-standby-bucket", old.c_str(), "active"});
            lastForeground_.erase(old);
        }
    }
    for (const std::string& p : pkgs) {
        if (std::find(pkgs_.begin(), pkgs_.end(), p) == pkgs_.end()) {
            run({"am", "set-standby-bucket", p.c_str(), "restricted"});
            lastForeground_[p] = now;  // tính thời gian chờ từ lúc thêm vào
        }
    }
    pkgs_ = pkgs;
    save();
    return {};
}

void BackgroundBlocker::enforce(double now) {
    if (pkgs_.empty()) return;
    std::string fg = foregroundPackage();
    std::unordered_set<std::string> running = runningPackages();
    for (const std::string& p : pkgs_) {
        if (p == fg) {
            lastForeground_[p] = now;
            continue;
        }
        if (!running.count(p)) continue;
        auto it = lastForeground_.find(p);
        if (it == lastForeground_.end()) { lastForeground_[p] = now; continue; }
        if (now - it->second < kGraceSec) continue;
        if (run({"am", "force-stop", p.c_str()}) == 0) {
            kills_++;
            lastKilled_ = p;
        }
        it->second = now;  // không gọi lại liên tục nếu app tự bật lại ngay
    }
}

int packageUid(const char* pkg) {
    char cmd[256];
    snprintf(cmd, sizeof cmd, "cmd package list packages -U %s 2>/dev/null", pkg);
    FILE* f = popen(cmd, "r");
    if (!f) return -1;
    char line[256];
    int uid = -1;
    std::string want = std::string("package:") + pkg + " ";
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, want.c_str(), want.size()) != 0) continue;
        const char* u = strstr(line, "uid:");
        if (u) uid = atoi(u + 4);
    }
    pclose(f);
    return uid;
}

int peerUid(int fd) {
    sockaddr_storage peer{}, local{};
    socklen_t pl = sizeof peer, ll = sizeof local;
    if (getpeername(fd, (sockaddr*)&peer, &pl) != 0 || getsockname(fd, (sockaddr*)&local, &ll) != 0)
        return -1;
    auto portOf = [](const sockaddr_storage& a) {
        return a.ss_family == AF_INET6 ? ntohs(((const sockaddr_in6&)a).sin6_port)
                                       : ntohs(((const sockaddr_in&)a).sin_port);
    };
    unsigned peerPort = portOf(peer), localPort = portOf(local);
    // Tìm socket phía client: local port = peerPort, remote port = cổng của statd.
    for (const char* file : {"/proc/net/tcp", "/proc/net/tcp6"}) {
        FILE* f = fopen(file, "r");
        if (!f) continue;
        char line[512];
        fgets(line, sizeof line, f);  // tiêu đề
        while (fgets(line, sizeof line, f)) {
            char localAddr[64], remAddr[64];
            unsigned st, uid;
            if (sscanf(line, "%*d: %63s %63s %x %*s %*s %*s %u", localAddr, remAddr, &st, &uid) != 4)
                continue;
            const char* lp = strrchr(localAddr, ':');
            const char* rp = strrchr(remAddr, ':');
            if (!lp || !rp) continue;
            if (strtoul(lp + 1, nullptr, 16) == peerPort && strtoul(rp + 1, nullptr, 16) == localPort) {
                fclose(f);
                return (int)uid;
            }
        }
        fclose(f);
    }
    return -1;
}
