// Chặn app chạy nền (chỉ dùng trong statd, cần quyền shell).
//
// App nằm trong danh sách mà rời màn hình quá kGraceSec giây và vẫn còn tiến trình thì bị
// `am force-stop`: Android coi app ở trạng thái "stopped", không giao broadcast/job/alarm cho tới
// khi người dùng tự mở lại. Khi thêm vào danh sách, app cũng bị đưa vào standby bucket
// "restricted" để hạn chế job/alarm ngay cả khi đang chạy.
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

// Package của app overlay: được phép gửi lệnh cho statd và không bao giờ bị chặn.
constexpr const char* kAppPackage = "com.utc.hwoverlay";

class BackgroundBlocker {
public:
    static constexpr double kGraceSec = 10.0;

    // Đọc danh sách đã lưu (/data/local/tmp/statd.block).
    void load();
    // Thay toàn bộ danh sách. Trả về thông báo lỗi, rỗng nếu thành công.
    std::string setList(const std::vector<std::string>& pkgs);
    // Gọi định kỳ (~2 giây).
    void enforce(double now);

    const std::vector<std::string>& list() const { return pkgs_; }
    int kills() const { return kills_; }
    const std::string& lastKilled() const { return lastKilled_; }

    static bool validPackage(const std::string& pkg);

private:
    void save() const;

    std::vector<std::string> pkgs_;
    std::unordered_map<std::string, double> lastForeground_;  // lần cuối thấy ở màn hình
    int kills_ = 0;
    std::string lastKilled_;
};

// uid của tiến trình phía bên kia kết nối TCP localhost (tra trong /proc/net/tcp*), -1 nếu không tìm thấy.
int peerUid(int fd);

// uid của một package (qua `cmd package list packages -U`), -1 nếu không có.
int packageUid(const char* pkg);
