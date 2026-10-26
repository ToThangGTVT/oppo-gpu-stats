cmake_minimum_required(VERSION 3.22.1)
project(hwoverlay CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Thư viện JNI cho app: lấy mẫu + vẽ overlay.
add_library(hwoverlay SHARED jni_bridge.cpp stats.cpp renderer.cpp)
target_compile_options(hwoverlay PRIVATE -O2 -Wall)
target_link_libraries(hwoverlay android jnigraphics log)

# statd (daemon chạy qua adb shell) được build riêng bằng tools/statd.sh.
