// Cầu nối JNI: luồng native lấy mẫu thông số và vẽ thẳng lên Surface của overlay.
#include <android/bitmap.h>
#include <android/log.h>
#include <android/native_window_jni.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <jni.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#include "renderer.h"
#include "stats.h"

#define LOG_TAG "HwOverlay"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

using Clock = std::chrono::steady_clock;

namespace {

constexpr int kStatdPort = 27183;

// Kết nối tới daemon statd (chạy bằng quyền shell qua adb) để lấy số liệu mà app thường bị chặn.
class DaemonLink {
public:
    ~DaemonLink() { closeFd(); }

    // Đọc dữ liệu mới từ daemon và ghi đè các trường hợp lệ vào s. Trả về true nếu đang có dữ liệu.
    bool merge(Sample& s) {
        auto now = Clock::now();
        if (fd_ < 0 && now - lastTry_ > std::chrono::seconds(3)) {
            lastTry_ = now;
            tryConnect();
        }
        if (fd_ >= 0) readAvailable();
        if (!have_ || now - lastData_ > std::chrono::seconds(3)) return false;

        if (d_.cpu >= 0) {
            s.cpu = d_.cpu;
            s.cpuFromFreq = d_.cpuFromFreq;
            s.nCores = d_.nCores;
            for (int i = 0; i < kMaxCores; i++) {
                s.coreLoad[i] = d_.coreLoad[i];
                s.coreMhz[i] = d_.coreMhz[i];
            }
        }
        if (d_.gpu >= 0) s.gpu = d_.gpu;
        if (d_.gpuMhz > 0) s.gpuMhz = d_.gpuMhz;
        if (d_.gpuMemMb >= 0) s.gpuMemMb = d_.gpuMemMb;
        return true;
    }

private:
    void tryConnect() {
        int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0) return;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(kStatdPort);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (connect(fd, (sockaddr*)&addr, sizeof addr) != 0) {
            if (errno != EINPROGRESS) { close(fd); return; }
            pollfd p{fd, POLLOUT, 0};
            int err = 0;
            socklen_t len = sizeof err;
            if (poll(&p, 1, 100) != 1 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err) {
                close(fd);
                return;
            }
        }
        fd_ = fd;
        buf_.clear();
        have_ = false;
        LOGI("statd connected");
    }

    void readAvailable() {
        char tmp[2048];
        for (;;) {
            ssize_t n = recv(fd_, tmp, sizeof tmp, 0);
            if (n > 0) { buf_.append(tmp, (size_t)n); continue; }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            closeFd();  // daemon đã tắt
            return;
        }
        size_t last = buf_.rfind('\n');
        if (last == std::string::npos) {
            if (buf_.size() > 8192) buf_.clear();
            return;
        }
        // Chỉ cần dòng hoàn chỉnh mới nhất.
        size_t prev = last ? buf_.rfind('\n', last - 1) : std::string::npos;
        size_t start = prev == std::string::npos ? 0 : prev + 1;
        Sample d;
        if (last > start && sampleFromLine(buf_.substr(start, last - start).c_str(), d)) {
            d_ = d;
            have_ = true;
            lastData_ = Clock::now();
        }
        buf_.erase(0, last + 1);
    }

    void closeFd() {
        if (fd_ >= 0) close(fd_);
        fd_ = -1;
        have_ = false;
    }

    int fd_ = -1;
    std::string buf_;
    Sample d_;
    bool have_ = false;
    Clock::time_point lastTry_{};
    Clock::time_point lastData_{};
};

std::mutex g_mu;
std::condition_variable g_cv;
std::thread g_thread;
bool g_run = false;
std::string g_status;

void renderLoop(ANativeWindow* win, Layout layout, int intervalMs) {
    ANativeWindow_setBuffersGeometry(win, layout.width, layout.height, WINDOW_FORMAT_RGBA_8888);
    StatsCollector collector;
    DaemonLink link;
    History hist;

    auto next = Clock::now();
    for (;;) {
        Sample s;
        collector.sample(s);
        bool linked = link.merge(s);
        hist.push(s);

        ANativeWindow_Buffer buf;
        if (ANativeWindow_lock(win, &buf, nullptr) == 0) {
            renderOverlay((uint32_t*)buf.bits, buf.stride, buf.width, buf.height, layout, hist, s, linked);
            ANativeWindow_unlockAndPost(win);
        }

        std::unique_lock<std::mutex> lk(g_mu);
        g_status = collector.describe() + (linked ? "\nstatd: ĐÃ KẾT NỐI" : "\nstatd: chưa chạy");
        next += std::chrono::milliseconds(intervalMs);
        if (g_cv.wait_until(lk, next, [] { return !g_run; })) break;
    }
    ANativeWindow_release(win);
}

void stopLoop() {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_run = false;
    }
    g_cv.notify_all();
    if (g_thread.joinable()) g_thread.join();
}

}  // namespace

extern "C" {

JNIEXPORT void JNICALL
Java_com_example_hwoverlay_NativeBridge_setFont(JNIEnv* env, jclass, jint slot, jobject atlas,
                                               jfloatArray advances, jint cellW, jint cellH, jint pad,
                                               jint ascent) {
    AndroidBitmapInfo info;
    void* px = nullptr;
    if (AndroidBitmap_getInfo(env, atlas, &info) != 0 || info.format != ANDROID_BITMAP_FORMAT_A_8 ||
        AndroidBitmap_lockPixels(env, atlas, &px) != 0) {
        return;
    }
    jfloat adv[95] = {};
    env->GetFloatArrayRegion(advances, 0, 95, adv);
    stopLoop();
    setFontAtlas(slot, (const uint8_t*)px, (int)info.stride, cellW, cellH, pad, ascent, adv);
    AndroidBitmap_unlockPixels(env, atlas);
}

JNIEXPORT jintArray JNICALL
Java_com_example_hwoverlay_NativeBridge_measure(JNIEnv* env, jclass, jfloat density, jboolean compact) {
    Layout l = computeLayout(density, compact);
    jint wh[2] = {l.width, l.height};
    jintArray arr = env->NewIntArray(2);
    env->SetIntArrayRegion(arr, 0, 2, wh);
    return arr;
}

JNIEXPORT void JNICALL
Java_com_example_hwoverlay_NativeBridge_start(JNIEnv* env, jclass, jobject surface, jfloat density,
                                             jboolean compact, jint intervalMs) {
    stopLoop();
    ANativeWindow* win = ANativeWindow_fromSurface(env, surface);
    if (!win) return;
    Layout layout = computeLayout(density, compact);
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_run = true;
    }
    g_thread = std::thread(renderLoop, win, layout, intervalMs < 100 ? 100 : intervalMs);
}

JNIEXPORT void JNICALL
Java_com_example_hwoverlay_NativeBridge_stop(JNIEnv*, jclass) {
    stopLoop();
}

JNIEXPORT jstring JNICALL
Java_com_example_hwoverlay_NativeBridge_status(JNIEnv* env, jclass) {
    std::string s;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_run) s = g_status;
    }
    if (s.empty()) {
        // Overlay chưa chạy: thử đọc một lượt để báo nguồn dữ liệu nào khả dụng.
        StatsCollector c;
        Sample tmp;
        c.sample(tmp);
        s = c.describe() + "\nstatd: (bật overlay để kiểm tra)";
    }
    return env->NewStringUTF(s.c_str());
}

}  // extern "C"
