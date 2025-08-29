cmake_minimum_required(VERSION 3.10)
project(ten_vad_jni)

# 日志库
find_library(log-lib log)

# 头文件（ten_vad.h + jni）
get_filename_component(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../ ABSOLUTE)
include_directories("${ROOT}/include" ${CMAKE_CURRENT_SOURCE_DIR})

# JNI 桥接库
add_library(ten_vad_jni SHARED native-lib.c)

if(ANDROID)
    # 声明一个已存在的 .so 为 IMPORTED 目标，按 ABI 绑定到 APK 里的那个文件
    add_library(ten_vad SHARED IMPORTED)
    set_target_properties(ten_vad PROPERTIES
            IMPORTED_LOCATION "${CMAKE_SOURCE_DIR}/../jniLibs/${ANDROID_ABI}/libten_vad.so"
            # 防止某些设备/NDK 版本对 SONAME 挑剔
            IMPORTED_NO_SONAME TRUE
    )

    # 让 jni 库链接到 ten_vad（这样会产生 DT_NEEDED=libten_vad.so）
    target_link_libraries(ten_vad_jni ten_vad ${log-lib})
else()
    message(FATAL_ERROR "This CMakeLists is for Android only")
endif()