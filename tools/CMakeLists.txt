# syp_probe_core：验证逻辑独立成库，让可执行工具与 ctest 测试共用同一套比对代码。
# 两份比对逻辑会各自漂移，验证工具最忌讳这个。
add_library(syp_probe_core STATIC
    syp_probe/frame_digest.cpp
    syp_probe/packet_digest.cpp
    syp_probe/scenarios.cpp
)
target_include_directories(syp_probe_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/syp_probe)
target_link_libraries(syp_probe_core
    PUBLIC  syp_media syp_platform_apple
    PRIVATE syp_warnings)

# syp_probe：命令行工具本体，可直接打真网址排查线上资源。
add_executable(syp_probe syp_probe/main.cpp)
target_link_libraries(syp_probe PRIVATE syp_probe_core syp_warnings)
set_target_properties(syp_probe PROPERTIES BUILD_RPATH "${SYP_FFMPEG_RPATH}")

# present_window_harness：真实 NSWindow + CAMetalLayer 上屏压测，按 60fps
# 提前提交、BUSY 重试/丢弃两种策略对照打印丢帧数。需要图形会话，手动运行，不进 ctest；
# EXCLUDE_FROM_ALL，默认构建不编它：cmake --build build --target present_window_harness。
# 只在 macOS 构建（AppKit）；造帧复用 tests/support/metal_layer_helper.mm。
if(APPLE AND CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    add_executable(present_window_harness EXCLUDE_FROM_ALL
        present_window_harness/main.mm
        ${CMAKE_SOURCE_DIR}/tests/support/metal_layer_helper.mm)
    target_include_directories(present_window_harness PRIVATE
        ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/tests)
    target_link_libraries(present_window_harness PRIVATE syp_platform_apple syp_warnings
        "-framework AppKit" "-framework CoreVideo" "-framework QuartzCore"
        "-framework Metal" "-framework Foundation")
    set_target_properties(present_window_harness PROPERTIES BUILD_RPATH "${SYP_FFMPEG_RPATH}")
endif()
