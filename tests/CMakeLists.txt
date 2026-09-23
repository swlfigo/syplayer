# 哨兵测试：断言"验证矩阵没有子集缺失"，必须永远注册——不放在任何
# SYP_HAVE_FFMPEG / SYP_HAVE_FIXTURES / SYP_REQUIRE_FULL_MATRIX 的 if() 块
# 里，否则它自己也会跟着"无声消失"。判据与背景见
# tests/test_matrix_sentinel.cpp 顶部注释。
add_executable(test_matrix_sentinel test_matrix_sentinel.cpp)
target_link_libraries(test_matrix_sentinel PRIVATE syp_warnings)
target_compile_definitions(test_matrix_sentinel PRIVATE
    SYP_HAVE_FFMPEG=$<BOOL:${SYP_HAVE_FFMPEG}>
    SYP_HAVE_FIXTURES=$<BOOL:${SYP_HAVE_FIXTURES}>
    SYP_HAVE_IOS_SLICE=$<BOOL:${SYP_HAVE_IOS_SLICE}>
    SYP_HAVE_CATALYST_SLICE=$<BOOL:${SYP_HAVE_CATALYST_SLICE}>
    SYP_REQUIRE_FULL_MATRIX=$<BOOL:${SYP_REQUIRE_FULL_MATRIX}>
    "SYP_FFMPEG_XCFRAMEWORK_PATH=\"${SYP_FFMPEG_XCFRAMEWORK}\"")
add_test(NAME matrix_sentinel COMMAND test_matrix_sentinel)
set_tests_properties(matrix_sentinel PROPERTIES TIMEOUT 15)

# 单测：零第三方依赖。dl 层目标只链 syp_dl；Apple 后端测链 syp_platform_apple
# （test_source_bridge 在 APPLE 下也链它，为一条端到端用例）。
add_executable(test_hole_set test_hole_set.cpp)
target_link_libraries(test_hole_set PRIVATE syp_dl syp_warnings)
add_test(NAME hole_set COMMAND test_hole_set)
set_tests_properties(hole_set PROPERTIES TIMEOUT 15)

# test_video_geometry：显示几何纯函数。
# 直接编 src/media/video_geometry.cpp 进来、不链 syp_media——那个库挂在
# SYP_HAVE_FFMPEG 下，而本文件零 FFmpeg 依赖，没理由跟着一起消失。
add_executable(test_video_geometry test_video_geometry.cpp
    ${CMAKE_SOURCE_DIR}/src/media/video_geometry.cpp)
target_include_directories(test_video_geometry PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_video_geometry PRIVATE syp_warnings)
add_test(NAME video_geometry COMMAND test_video_geometry)
set_tests_properties(video_geometry PROPERTIES TIMEOUT 15)

add_executable(test_cache_index test_cache_index.cpp)
target_link_libraries(test_cache_index PRIVATE syp_dl syp_warnings)
add_test(NAME cache_index COMMAND test_cache_index)
set_tests_properties(cache_index PROPERTIES TIMEOUT 15)

find_package(Threads REQUIRED)

# test_cache_store：进程内缓存注册表。用真线程做并发 acquire，
# 所以要 Threads::Threads；零 FFmpeg 依赖，跟 test_cache_index 同级。
# 位置在 find_package(Threads REQUIRED) 之后（不是紧跟 test_cache_index）——
# 放到那一行之前 Threads::Threads 还不存在。
add_executable(test_cache_store test_cache_store.cpp)
target_include_directories(test_cache_store PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_cache_store PRIVATE syp_dl syp_warnings Threads::Threads)
add_test(NAME cache_store COMMAND test_cache_store)
set_tests_properties(cache_store PROPERTIES TIMEOUT 30)

# AudioRing 零 FFmpeg 依赖（只 #include <atomic> <cstdint> <vector>
# <algorithm> <cstring>），跟 test_hole_set 同级，不挂在 SYP_HAVE_FFMPEG
# 门下——挂上去会让这条测试在没装 FFmpeg 的机器上无声消失，这个仓库有
# test_matrix_sentinel 专门盯"验证矩阵子集缺失"这类问题，不该自己再造一个。
# 直接编译 audio_ring.cpp，不链 syp_media。
add_executable(test_audio_ring test_audio_ring.cpp ${CMAKE_SOURCE_DIR}/src/media/audio_ring.cpp)
target_include_directories(test_audio_ring PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_audio_ring PRIVATE syp_warnings Threads::Threads)
add_test(NAME audio_ring COMMAND test_audio_ring)
set_tests_properties(audio_ring PROPERTIES TIMEOUT 30)

# HLS URL 改写：零 FFmpeg 依赖（只 #include <string> <algorithm> <cctype>），
# 跟 test_audio_ring 同级，不挂在 SYP_HAVE_FFMPEG 门下——这条测试必须无条件
# 注册。直接编译 url_rewrite.cpp，不链 syp_media。
add_executable(test_hls_url_rewrite test_hls_url_rewrite.cpp ${CMAKE_SOURCE_DIR}/src/media/hls/url_rewrite.cpp)
# ${CMAKE_CURRENT_SOURCE_DIR}：support/hls_cases.h（与 test_preloader_hls
# 共享的那张 HLS 判据表）。
target_include_directories(test_hls_url_rewrite PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_hls_url_rewrite PRIVATE syp_warnings)
add_test(NAME hls_url_rewrite COMMAND test_hls_url_rewrite)
set_tests_properties(hls_url_rewrite PROPERTIES TIMEOUT 15)

add_executable(test_dl_task test_dl_task.cpp support/stub_backend.cpp)
target_include_directories(test_dl_task PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_dl_task PRIVATE syp_dl syp_warnings Threads::Threads)
add_test(NAME dl_task COMMAND test_dl_task)
set_tests_properties(dl_task PROPERTIES TIMEOUT 30)

add_executable(test_scheduler test_scheduler.cpp support/stub_backend.cpp)
target_include_directories(test_scheduler PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_scheduler PRIVATE syp_dl syp_warnings Threads::Threads)
add_test(NAME scheduler COMMAND test_scheduler)
# 120，不是 45：慢判定用例（每片 3072000 字节 × 3、同步桩逐块泵）
# 在 TSan 下单独跑实测 46.7s，已越过 45；-j4 负载下全量 TSan 在第 64/65 条被
# ctest 按 Timeout 杀掉。普通构建 -j4 下约 4s，抬高零代价。
set_tests_properties(scheduler PROPERTIES TIMEOUT 120)

# test_rate_limiter：令牌桶。纯逻辑用例注入假时钟、不起线程；
# 最后两条起真实线程，TSan 构建覆盖订阅析构与回调的竞争。
add_executable(test_rate_limiter test_rate_limiter.cpp)
target_include_directories(test_rate_limiter PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_rate_limiter PRIVATE syp_dl syp_warnings Threads::Threads)
add_test(NAME rate_limiter COMMAND test_rate_limiter)
set_tests_properties(rate_limiter PROPERTIES TIMEOUT 30)

# test_health_ticker：坏任务检测的计时源。纯逻辑用例注入假时钟、不起线程；
# 线程模式用真实时钟。
add_executable(test_health_ticker test_health_ticker.cpp)
target_include_directories(test_health_ticker PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_health_ticker PRIVATE syp_dl syp_warnings Threads::Threads)
add_test(NAME health_ticker COMMAND test_health_ticker)
set_tests_properties(health_ticker PROPERTIES TIMEOUT 30)

# test_preconnector：预连接管理器。同步桩 + 假时钟；析构用例另起异步桩。
add_executable(test_preconnector test_preconnector.cpp support/stub_backend.cpp)
target_include_directories(test_preconnector PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_preconnector PRIVATE syp_dl syp_warnings Threads::Threads)
add_test(NAME preconnector COMMAND test_preconnector)
set_tests_properties(preconnector PROPERTIES TIMEOUT 30)

# 新增 support/loopback_server.cpp：loopback_serves_multiple_
# routes_and_counts_them 直接用真 socket 打 LoopbackServer 的路由，不经
# syp_source，所以不需要额外链别的库——跟 test_probe_e2e/test_decode_e2e/
# test_sync_e2e 链同一份 loopback_server.cpp 一样，零 FFmpeg 依赖。
add_executable(test_source_bridge test_source_bridge.cpp support/stub_backend.cpp
    support/loopback_server.cpp)
target_include_directories(test_source_bridge PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# slow_loris_connection_is_replaced_end_to_end 要一个真实 HTTP 后端
# 对着 LoopbackServer 发请求（慢替换只有在真实时钟 + 真实连接上才看得到端到端
# 效果），门是 APPLE，与 test_preloader 同一条理由；其余用例仍走 StubBackend。
# APPLE 下只链 syp_platform_apple（它已传递带上 syp_dl），同时写两者 ld 会报
# duplicate libraries。
if(APPLE)
    target_link_libraries(test_source_bridge PRIVATE syp_platform_apple syp_warnings Threads::Threads)
else()
    target_link_libraries(test_source_bridge PRIVATE syp_dl syp_warnings Threads::Threads)
endif()
add_test(NAME source_bridge COMMAND test_source_bridge)
set_tests_properties(source_bridge PROPERTIES TIMEOUT 60)

if(APPLE)
    add_executable(test_apple_http_backend
        test_apple_http_backend.cpp
        support/loopback_server.cpp
    )
    target_include_directories(test_apple_http_backend PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(test_apple_http_backend PRIVATE
        syp_platform_apple syp_warnings Threads::Threads)
    add_test(NAME apple_http_backend COMMAND test_apple_http_backend)
    # 60，不是 30：本用例套件的硬看门狗 kSelfDestroyHardMs = 30000ms。ctest
    # TIMEOUT 30 会跟硬看门狗在同一刻打进程，正好抵消看门狗为诊断专门加的
    # setvbuf 行缓冲——ctest 杀进程时诊断输出可能还没来得及冲出去。正常路径
    # 实测 1.26s，抬到 60 零代价。
    set_tests_properties(apple_http_backend PROPERTIES TIMEOUT 60)

    # test_preloader：预加载的额度分配与"暖够就停"。
    # 门是 APPLE 而不是 SYP_HAVE_FFMPEG——用例需要的是一个**真实的 HTTP
    # 后端**（对着 LoopbackServer 发请求），不是 FFmpeg；跟
    # test_apple_http_backend 同一条理由。StubBackend 给不了
    # hold_route() 那种"已收到请求、还没写响应"的可观测中间态。
    add_executable(test_preloader test_preloader.cpp support/loopback_server.cpp)
    target_include_directories(test_preloader PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(test_preloader PRIVATE
        syp_platform_apple syp_warnings Threads::Threads)
    add_test(NAME preloader COMMAND test_preloader)
    # 120：用例本身最慢的一条是 failed_url_becomes_failed_not_stuck
    # （一次 404 + 重试收敛），实测 < 3s；余量留给 sanitizer 插桩。
    set_tests_properties(preloader PROPERTIES TIMEOUT 120)

    # test_preloader_hls：最小播放列表扫描器（纯函数）+ HLS 预
    # 加载端到端。纯函数部分零依赖，但端到端部分需要真实 HTTP 后端，所以
    # 整条挂在 APPLE 门下，跟 test_preloader 同待遇。
    add_executable(test_preloader_hls test_preloader_hls.cpp support/loopback_server.cpp)
    target_include_directories(test_preloader_hls PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(test_preloader_hls PRIVATE
        syp_platform_apple syp_warnings Threads::Threads)
    add_test(NAME preloader_hls COMMAND test_preloader_hls)
    set_tests_properties(preloader_hls PROPERTIES TIMEOUT 120)
endif()

if(SYP_HAVE_FFMPEG)
    # FFmpeg 符号隔离：同一条检查在顶层 CMakeLists.txt 里已作为
    # syp_ffmpeg_symbols_check 挂在 ALL 上；这里再登记成 ctest，让"只跑 ctest"
    # 的验证口径也覆盖到（例如库被单独重编、ALL 那一步被跳过的时候）。
    add_test(NAME ffmpeg_symbols COMMAND ${SYP_FFMPEG_SYMBOLS_CHECK_COMMAND})

    # 接入方自带 FFmpeg 的冲突集成测试（判据见 test_ffmpeg_coexist.cpp 顶部）。
    #
    # 三块拼成一个测试程序：
    #   - 假的"接入方 FFmpeg"：导出真实的 FFmpeg 函数名，静态库与动态库各一份；
    #   - ffmpeg_coexist_integrator：接入方自己的代码（main 与断言）；
    #   - ffmpeg_coexist_syp_side：我们这一侧，经 syp_media 走 SYFFmpeg。
    # 前两块**不能**碰 syp_ffmpeg：它的 INTERFACE 编译选项会 -include 映射头，
    # 把接入方写的 avformat_version 改写成 syp_avformat_version，测试就不再
    # 是"两份 FFmpeg 抢同一个名字"了。所以它们是独立目标、不链 syp_media；
    # 可执行文件本身不带任何源文件，只收这些目标的产物——syp_ffmpeg 的编译
    # 选项只作用于编译，可执行文件链 syp_media 不会改写已经编好的目标文件。
    #
    # 假库拆成多个归档成员：记账一个、每个假 FFmpeg 函数（或同一源文件里的
    # 一对）各一个，理由见 support/fake_ffmpeg/fake_ffmpeg_internal.h。
    set(_syp_fake_ffmpeg_srcs
        support/fake_ffmpeg/fake_ffmpeg_counts.cpp
        support/fake_ffmpeg/fake_avformat_version.cpp
        support/fake_ffmpeg/fake_avcodec_version.cpp
        support/fake_ffmpeg/fake_av_mem.cpp
        support/fake_ffmpeg/fake_av_log.cpp
        support/fake_ffmpeg/fake_avformat_open_input.cpp
        support/fake_ffmpeg/fake_avformat_find_stream_info.cpp
        support/fake_ffmpeg/fake_avformat_close_input.cpp
        support/fake_ffmpeg/fake_av_read_frame.cpp
        support/fake_ffmpeg/fake_avcodec_find_decoder.cpp)
    add_library(syp_fake_integrator_ffmpeg STATIC ${_syp_fake_ffmpeg_srcs})
    target_include_directories(syp_fake_integrator_ffmpeg PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(syp_fake_integrator_ffmpeg PRIVATE syp_warnings)
    add_library(syp_fake_integrator_ffmpeg_shared SHARED ${_syp_fake_ffmpeg_srcs})
    target_include_directories(syp_fake_integrator_ffmpeg_shared PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(syp_fake_integrator_ffmpeg_shared PRIVATE syp_warnings)

    add_library(ffmpeg_coexist_integrator OBJECT test_ffmpeg_coexist.cpp)
    target_include_directories(ffmpeg_coexist_integrator PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(ffmpeg_coexist_integrator PRIVATE syp_warnings)
    # 绝对路径烤进去：ctest 与手工直接跑二进制时 cwd 不同，不能靠相对路径。
    target_compile_definitions(ffmpeg_coexist_integrator PRIVATE
        "SYP_SAMPLE_MP4_PATH=\"${CMAKE_SOURCE_DIR}/demo/shared/Resources/sample.mp4\"")

    add_library(ffmpeg_coexist_syp_side OBJECT support/coexist_syp_side.cpp)
    target_include_directories(ffmpeg_coexist_syp_side PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(ffmpeg_coexist_syp_side PRIVATE syp_media syp_warnings)

    # 链接方式。ld64 按命令行顺序在库里找未定义符号，所以"谁在前"必须落到
    # 链接命令上，不能交给 CMake 的依赖排序（它总把直接依赖排在传递依赖
    # 前面，SYFFmpeg 经 syp_media 传递而来，永远落在假库后面）：
    #   *_syffmpeg_first：SYFFmpeg 写进链接选项（排在全部目标文件与库之前），
    #                     假库作为普通库排在后面；
    #   *_fake_first    ：假库写进链接选项排在最前，SYFFmpeg 照常经 syp_media 在后；
    #   static_all_load ：-Wl,-all_load，把所有静态库成员（含假库）无条件拉进来。
    # ld64 的取舍：静态库里的定义只有在它所在的成员被加载时才参与——成员
    # 被别的引用强制加载了，或者静态库排在前面、先被搜到；否则排在前面的
    # 动态库胜出。所以 SYFFmpeg 若退化成也导出原名，排在它后面的接入方
    # FFmpeg（静态或动态）都会被抢走调用：*_syffmpeg_first 两个变体守这一条；
    # 另外三个变体证明假库在前、或被 -all_load 全部加载时也不串用。
    # 每个变体的实际链接顺序可以在 build 目录的 link.txt 里核对。
    foreach(_variant IN ITEMS
            static_syffmpeg_first static_fake_first static_all_load
            dylib_syffmpeg_first dylib_fake_first)
        set(_exe test_ffmpeg_coexist_${_variant})
        if(_variant MATCHES "^dylib_")
            set(_fake syp_fake_integrator_ffmpeg_shared)
        else()
            set(_fake syp_fake_integrator_ffmpeg)
        endif()
        add_executable(${_exe}
            $<TARGET_OBJECTS:ffmpeg_coexist_integrator>
            $<TARGET_OBJECTS:ffmpeg_coexist_syp_side>)
        target_link_libraries(${_exe} PRIVATE syp_media ${_fake})
        # 假动态库所在目录由 CMake 自动加进构建期 rpath，这里只补 SYFFmpeg 的。
        set_target_properties(${_exe} PROPERTIES BUILD_RPATH "${SYP_FFMPEG_RPATH}")
        if(_variant MATCHES "_syffmpeg_first$")
            target_link_options(${_exe} PRIVATE
                "-F${SYP_FFMPEG_SLICE_DIR}" "SHELL:-framework SYFFmpeg")
        elseif(_variant MATCHES "_fake_first$")
            target_link_options(${_exe} PRIVATE "$<TARGET_FILE:${_fake}>")
        else()
            target_link_options(${_exe} PRIVATE "-Wl,-all_load")
        endif()
        add_test(NAME ffmpeg_coexist_${_variant} COMMAND ${_exe})
        set_tests_properties(ffmpeg_coexist_${_variant} PROPERTIES TIMEOUT 60)
    endforeach()

    # syp_platform_apple PUBLIC 链了 syp_media（AudioUnitSink
    # 需要 AudioRing/IAudioSink/swresample），这条依赖是动态 SYFFmpeg.framework
    # ——任何链 syp_platform_apple 的可执行文件运行期都要能找到它。
    # test_apple_http_backend 在上面 SYP_HAVE_FFMPEG 门之外就已经创建（它
    # 本身不需要 FFmpeg，只是现在间接链上了），CMake 的 target_link_libraries
    # 会把 "-framework SYFFmpeg" 正确带到它的链接命令里（链接期就是绿的），
    # 但 BUILD_RPATH 不是 usage requirement，不会跟着 PUBLIC 链接自动传播，
    # 必须在这里补一次，否则运行期 dyld 报
    # "Library not loaded: @rpath/SYFFmpeg.framework/... no LC_RPATH's found"。
    if(TARGET test_apple_http_backend)
        set_target_properties(test_apple_http_backend PROPERTIES BUILD_RPATH "${SYP_FFMPEG_RPATH}")
    endif()
    # 同上：test_preloader 也链 syp_platform_apple，而 BUILD_RPATH 不是 usage
    # requirement，不会跟着 PUBLIC 链接传播，必须在这里补一份。
    if(TARGET test_preloader)
        set_target_properties(test_preloader PROPERTIES BUILD_RPATH "${SYP_FFMPEG_RPATH}")
    endif()
    # 同上：test_preloader_hls 也链 syp_platform_apple。
    if(TARGET test_preloader_hls)
        set_target_properties(test_preloader_hls PROPERTIES BUILD_RPATH "${SYP_FFMPEG_RPATH}")
    endif()
    # 同上：test_source_bridge 在 APPLE 下也链 syp_platform_apple。
    if(APPLE)
        set_target_properties(test_source_bridge PROPERTIES BUILD_RPATH "${SYP_FFMPEG_RPATH}")
    endif()

    # stub_backend.cpp 只依赖公开的 syp_http.h，零平台依赖——不链 syp_platform_apple。
    add_executable(test_avio_bridge test_avio_bridge.cpp support/stub_backend.cpp)
    target_include_directories(test_avio_bridge PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(test_avio_bridge PRIVATE syp_media syp_warnings Threads::Threads)
    set_target_properties(test_avio_bridge PROPERTIES BUILD_RPATH "${SYP_FFMPEG_RPATH}")
    add_test(NAME avio_bridge COMMAND test_avio_bridge)
    set_tests_properties(avio_bridge PROPERTIES TIMEOUT 60)

    add_executable(test_frame test_frame.cpp)
    target_include_directories(test_frame PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(test_frame PRIVATE syp_media syp_warnings)
    set_target_properties(test_frame PROPERTIES BUILD_RPATH "${SYP_FFMPEG_RPATH}")
    add_test(NAME frame COMMAND test_frame)
    set_tests_properties(frame PROPERTIES TIMEOUT 30)

    # test_packet_queue：PacketQueue 加锁后成为加载线程与泵线程之间的
    # 跨线程边界，并发用例是 TSan 目标。只依赖 FFmpeg 头与 syp_media，跟
    # test_frame 同待遇。
    add_executable(test_packet_queue test_packet_queue.cpp)
    target_include_directories(test_packet_queue PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(test_packet_queue PRIVATE syp_media syp_warnings Threads::Threads)
    set_target_properties(test_packet_queue PROPERTIES BUILD_RPATH "${SYP_FFMPEG_RPATH}")
    add_test(NAME packet_queue COMMAND test_packet_queue)
    set_tests_properties(packet_queue PROPERTIES TIMEOUT 30)

    # time_source.h 间接 include audio_sink.h -> frame.h -> FFmpeg 头，
    # 跟 test_frame 同待遇：挂在 SYP_HAVE_FFMPEG 门下，不是 test_audio_ring
    # 那种零依赖情形。
    add_executable(test_time_source test_time_source.cpp)
    target_include_directories(test_time_source PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(test_time_source PRIVATE syp_media syp_warnings)
    set_target_properties(test_time_source PROPERTIES BUILD_RPATH "${SYP_FFMPEG_RPATH}")
    add_test(NAME time_source COMMAND test_time_source)
    set_tests_properties(time_source PROPERTIES TIMEOUT 30)

    # test_fakes：FakeAudioSink / FakeRenderer 本身的判据检验——它俩是
    # 场景 A~I 全部数值断言的判据来源，同样依赖 Frame（间接拉
    # FFmpeg 头），跟 test_frame / test_time_source 同待遇。
    add_executable(test_fakes test_fakes.cpp support/fake_audio_sink.cpp support/fake_renderer.cpp)
    target_include_directories(test_fakes PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(test_fakes PRIVATE syp_media syp_warnings)
    set_target_properties(test_fakes PROPERTIES BUILD_RPATH "${SYP_FFMPEG_RPATH}")
    add_test(NAME fakes COMMAND test_fakes)
    set_tests_properties(fakes PROPERTIES TIMEOUT 30)

    # test_track_player：同步核心。跟 test_fakes 一样只依赖
    # SYP_HAVE_FFMPEG，不依赖 SYP_HAVE_FIXTURES/SYP_FIXTURE_DIR——素材是
    # 用例现场用 ffmpeg CLI 合成的极小 CFR 视频（跟 test_decode_e2e.cpp
    # 场景 G 同一个"现场生成、用完即删"模式），不进 gen-fixtures.sh 的
    # 验证素材矩阵。SYP_FFMPEG_CLI_PATH 同 test_decode_e2e：编译期烤进
    # configure 期 find_program 探测到的绝对路径，不裸调 PATH 里的
    # "ffmpeg"（运行期 PATH 跟 configure 期不一定一致）。
    add_executable(test_track_player test_track_player.cpp
        support/fake_audio_sink.cpp support/fake_renderer.cpp)
    target_include_directories(test_track_player PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(test_track_player PRIVATE syp_media syp_warnings)
    set_target_properties(test_track_player PROPERTIES BUILD_RPATH "${SYP_FFMPEG_RPATH}")
    target_compile_definitions(test_track_player PRIVATE
        "SYP_FFMPEG_CLI_PATH=\"${SYP_FFMPEG_CLI}\"")
    add_test(NAME track_player COMMAND test_track_player)
    set_tests_properties(track_player PROPERTIES TIMEOUT 60)

    # test_audio_unit_sink：只测两段
    # 可以脱离硬件独立验证的算术——commit_frame()（write() 的"部分写入 →
    # 整帧成败"折算）、compute_played_us()（played_us() 的快照相减 + 延迟
    # 扣除 + 倍速换算）。真正碰 AudioComponentInstance 的路径（open() 的
    # 硬件初始化、render callback 在真实设备上跑）结构性地没有自动化
    # 覆盖，不在这里、也不该在这里补：CI 机器
    # 不一定有音频设备/权限，勉强开真实 AudioUnit 会让这条 ctest 在没有
    # 音频硬件的机器上变得不确定，比记成结构性盲区更差。
    if(APPLE)
        add_executable(test_audio_unit_sink test_audio_unit_sink.cpp)
        target_include_directories(test_audio_unit_sink PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_link_libraries(test_audio_unit_sink PRIVATE syp_platform_apple syp_warnings)
        set_target_properties(test_audio_unit_sink PROPERTIES BUILD_RPATH "${SYP_FFMPEG_RPATH}")
        add_test(NAME audio_unit_sink COMMAND test_audio_unit_sink)
        set_tests_properties(audio_unit_sink PROPERTIES TIMEOUT 30)
    endif()

    # test_metal_renderer：跟 test_audio_unit_sink 同一形状——
    # 纯函数（is_pix_fmt_supported/select_color_matrix/pack_plane_rows）
    # 零 GPU 依赖，普通 .cpp 直接单测；present()/debug_copy_output_rgba()
    # 的接线用例真的碰 GPU（MTLCreateSystemDefaultDevice()），但不需要
    # 显示器/窗口/CAMetalLayer——只需要一块 Metal 设备，这台开发机
    # （Apple Silicon macOS）具备，用例本身仍然按"设备不可用就 skip、不
    # 算失败"的纪律写（同 test_audio_unit_sink.cpp 里 open() 失败就 skip
    # 的做法），CI 机器没有 GPU 时不会让这条 ctest 变得不确定。
    if(APPLE)
        # support/metal_layer_helper.mm 造 IOSurface 支撑的 NV12/BGRA
        # CVPixelBuffer 帧与离屏 CAMetalLayer（.mm 天然吃顶层 OBJCXX 的 -fobjc-arc）；
        # 用到 CoreVideo/QuartzCore/Foundation，显式链上，不靠 syp_platform_apple 传递。
        add_executable(test_metal_renderer test_metal_renderer.cpp support/metal_layer_helper.mm)
        target_include_directories(test_metal_renderer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_link_libraries(test_metal_renderer PRIVATE syp_platform_apple syp_warnings
            "-framework CoreVideo" "-framework QuartzCore" "-framework Foundation")
        set_target_properties(test_metal_renderer PROPERTIES BUILD_RPATH "${SYP_FFMPEG_RPATH}")
        add_test(NAME metal_renderer COMMAND test_metal_renderer)
        set_tests_properties(metal_renderer PROPERTIES TIMEOUT 30)
        add_executable(test_inflight_gate test_inflight_gate.cpp)
        target_include_directories(test_inflight_gate PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_link_libraries(test_inflight_gate PRIVATE syp_platform_apple syp_warnings Threads::Threads)
        set_target_properties(test_inflight_gate PROPERTIES BUILD_RPATH "${SYP_FFMPEG_RPATH}")
        add_test(NAME inflight_gate COMMAND test_inflight_gate)
        set_tests_properties(inflight_gate PROPERTIES TIMEOUT 30)
    endif()

    # test_hls_playlist_fetcher：把异步 syp_http_backend
    # 包成一次阻塞取回。真正的门是"需要平台 HTTP 后端"（syp_apple_http_backend），
    # 不是 FFmpeg 本身——但 playlist_fetcher.h 挂在 syp_media 里（跟
    # url_rewrite.cpp 同一个库），且 syp_media 只在 SYP_HAVE_FFMPEG 时存在，
    # 所以跟 test_audio_unit_sink/test_metal_renderer 一样落在
    # if(SYP_HAVE_FFMPEG) 内的 if(APPLE) 里。链 syp_platform_apple 拿
    # syp_apple_http_backend()，链 syp_media 拿 fetch_playlist()，
    # 外加 loopback_server.cpp 起真实回环服务器（不打外网）。
    if(APPLE)
        add_executable(test_hls_playlist_fetcher
            test_hls_playlist_fetcher.cpp
            support/loopback_server.cpp
        )
        target_include_directories(test_hls_playlist_fetcher PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_link_libraries(test_hls_playlist_fetcher PRIVATE
            syp_media syp_platform_apple syp_warnings Threads::Threads)
        set_target_properties(test_hls_playlist_fetcher PROPERTIES BUILD_RPATH "${SYP_FFMPEG_RPATH}")
        add_test(NAME hls_playlist_fetcher COMMAND test_hls_playlist_fetcher)
        # 60：跟 test_apple_http_backend 同一个理由——留够诊断输出冲刷的余量，
        # 而 hang_times_out_and_does_not_leak_the_handle 本身最多阻塞约 1.5s
        # （1000ms connect + 500ms read + cancel 收敛），远算不上慢。
        set_tests_properties(hls_playlist_fetcher PROPERTIES TIMEOUT 60)
    endif()
endif()

if(SYP_HAVE_FFMPEG AND SYP_HAVE_FIXTURES)
    add_executable(test_packet_digest test_packet_digest.cpp)
    target_include_directories(test_packet_digest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(test_packet_digest PRIVATE syp_probe_core syp_warnings)
    set_target_properties(test_packet_digest PROPERTIES BUILD_RPATH "${SYP_FFMPEG_RPATH}")
    add_test(NAME packet_digest COMMAND test_packet_digest)
    set_tests_properties(packet_digest PROPERTIES
        TIMEOUT 180
        ENVIRONMENT "SYP_FIXTURE_DIR=${SYP_FIXTURE_DIR}")

    add_executable(test_demuxer test_demuxer.cpp)
    target_include_directories(test_demuxer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(test_demuxer PRIVATE syp_media syp_warnings)
    set_target_properties(test_demuxer PROPERTIES BUILD_RPATH "${SYP_FFMPEG_RPATH}")
    # 旋转/SAR 采集用例现场用 ffmpeg CLI 合成素材（跟
    # test_track_player 同一个模式），需要 support/synth_media.h 与编译期
    # 烤进的 ffmpeg 绝对路径。这个目标落在 SYP_HAVE_FFMPEG AND
    # SYP_HAVE_FIXTURES 门内——SYP_FFMPEG_CLI 由顶层 CMakeLists.txt 在
    # SYP_HAVE_FIXTURES 判定之前 find_program 探测（find_program 那一步
    # 不依赖 gen-fixtures.sh 是否成功），只要这个目标能被 configure 到，
    # SYP_FFMPEG_CLI 一定已经有值——同 test_track_player 目标的前提。
    target_compile_definitions(test_demuxer PRIVATE
        "SYP_FFMPEG_CLI_PATH=\"${SYP_FFMPEG_CLI}\"")
    add_test(NAME demuxer COMMAND test_demuxer)
    set_tests_properties(demuxer PROPERTIES
        TIMEOUT 60
        ENVIRONMENT "SYP_FIXTURE_DIR=${SYP_FIXTURE_DIR}")

    add_executable(test_decoder test_decoder.cpp)
    target_include_directories(test_decoder PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(test_decoder PRIVATE syp_media syp_warnings)
    set_target_properties(test_decoder PROPERTIES BUILD_RPATH "${SYP_FFMPEG_RPATH}")
    add_test(NAME decoder COMMAND test_decoder)
    set_tests_properties(decoder PROPERTIES
        TIMEOUT 60
        ENVIRONMENT "SYP_FIXTURE_DIR=${SYP_FIXTURE_DIR}")

    add_executable(test_pipeline test_pipeline.cpp)
    target_include_directories(test_pipeline PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    # syp_probe_core（不是单独的 syp_media）：B 帧回归用例要用
    # frame_digest.h 的 decode_reference()/decode_pipeline()/diff_frames()
    # 跟 FFmpeg 参照路径逐帧比对，而不是重新手写一份比较逻辑（tools/
    # syp_probe/frame_digest.cpp 顶部注释：两份比较逻辑会各自漂移）。
    # syp_probe_core PUBLIC 链 syp_media，media 相关符号仍然可见。
    target_link_libraries(test_pipeline PRIVATE syp_probe_core syp_warnings)
    set_target_properties(test_pipeline PROPERTIES BUILD_RPATH "${SYP_FFMPEG_RPATH}")
    # 封面图恒软解用例要用 support/synth_media.h 的
    # synth_audio_with_cover_art()，跟 test_track_player 同一个模式，需要
    # 编译期烤进 ffmpeg CLI 的绝对路径。
    target_compile_definitions(test_pipeline PRIVATE
        "SYP_FFMPEG_CLI_PATH=\"${SYP_FFMPEG_CLI}\"")
    add_test(NAME pipeline COMMAND test_pipeline)
    # 180，不是 60：线程模式用例（加载线程 + 慢速 AVIO 桩 + 多轮 seek）
    # 在 TSan 下整套已跑到 35s 上下，贴着原来 60s 窗口的一半；普通构建约 16s。三个构建
    # （build/build-tsan/build-asan）共用这一个值，给 sanitizer 插桩开销与机器负载
    # （ctest -j 并行）留足余量——挂死由各用例自带的 Watchdog 兜底报出位置，不靠这里。
    # 没拆成独立 ctest 目标：tiny_test 没有按名过滤，拆分意味着把共用脚手架
    # （fixture/GatedMemSource/驱动器）挪进公共头，改动面大于收益。
    set_tests_properties(pipeline PROPERTIES
        TIMEOUT 180
        ENVIRONMENT "SYP_FIXTURE_DIR=${SYP_FIXTURE_DIR}")

    add_executable(test_frame_digest test_frame_digest.cpp)
    target_include_directories(test_frame_digest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(test_frame_digest PRIVATE syp_probe_core syp_warnings)
    set_target_properties(test_frame_digest PROPERTIES BUILD_RPATH "${SYP_FFMPEG_RPATH}")
    add_test(NAME frame_digest COMMAND test_frame_digest)
    set_tests_properties(frame_digest PROPERTIES
        TIMEOUT 60
        ENVIRONMENT "SYP_FIXTURE_DIR=${SYP_FIXTURE_DIR}")

    add_executable(test_probe_e2e test_probe_e2e.cpp support/loopback_server.cpp)
    target_include_directories(test_probe_e2e PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(test_probe_e2e PRIVATE syp_probe_core syp_warnings Threads::Threads)
    set_target_properties(test_probe_e2e PROPERTIES BUILD_RPATH "${SYP_FFMPEG_RPATH}")
    add_test(NAME probe_e2e COMMAND test_probe_e2e)
    set_tests_properties(probe_e2e PROPERTIES
        TIMEOUT 900
        ENVIRONMENT "SYP_FIXTURE_DIR=${SYP_FIXTURE_DIR}")

    # 解码路径的正确性核心场景 A~D，是 test_probe_e2e 在解码
    # 维度的对应物（demux 换成 Pipeline 逐帧解码）。场景 B 经 dl 层，需要
    # 同一份 loopback_server.cpp。
    add_executable(test_decode_e2e test_decode_e2e.cpp support/loopback_server.cpp)
    target_include_directories(test_decode_e2e PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(test_decode_e2e PRIVATE syp_probe_core syp_warnings Threads::Threads)
    set_target_properties(test_decode_e2e PROPERTIES BUILD_RPATH "${SYP_FFMPEG_RPATH}")
    # 场景 G 需要现场调一次 ffmpeg CLI 生成素材，不能靠运行期
    # PATH 里裸的 "ffmpeg"——configure 期与运行期 PATH 不一致（沙箱、CI
    # runner、ctest 被不同 shell 拉起）会失配。这里把 configure 期已经
    # 探测到的绝对路径（顶层 CMakeLists.txt 的 find_program(SYP_FFMPEG_CLI
    # ffmpeg)）编译期烤进去，跟 test_matrix_sentinel 里
    # SYP_FFMPEG_XCFRAMEWORK_PATH 那行同一种写法。
    target_compile_definitions(test_decode_e2e PRIVATE
        "SYP_FFMPEG_CLI_PATH=\"${SYP_FFMPEG_CLI}\"")
    add_test(NAME decode_e2e COMMAND test_decode_e2e)
    set_tests_properties(decode_e2e PROPERTIES
        TIMEOUT 300
        ENVIRONMENT "SYP_FIXTURE_DIR=${SYP_FIXTURE_DIR}")

    # test_sync_e2e：同步与呈现的九条端到端场景 A~I。跟
    # test_decode_e2e 同一套骨架（看门狗、LoopbackServer+AvioBridge 场景 B、
    # SYP_FFMPEG_CLI_PATH 烤绝对路径），只是验证的层从"解码帧内容"换成
    # "呈现时刻/时钟/丢帧策略"——判据来自 FakeAudioSink/FakeRenderer
    # （tests/support/），不是跟 FFmpeg 参照逐帧比对。场景 F/G 现场合成
    # video-only 素材，需要 SYP_FFMPEG_CLI_PATH，跟 test_track_player 同理。
    add_executable(test_sync_e2e test_sync_e2e.cpp
        support/loopback_server.cpp
        support/fake_audio_sink.cpp
        support/fake_renderer.cpp)
    target_include_directories(test_sync_e2e PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(test_sync_e2e PRIVATE syp_probe_core syp_warnings Threads::Threads)
    set_target_properties(test_sync_e2e PROPERTIES BUILD_RPATH "${SYP_FFMPEG_RPATH}")
    target_compile_definitions(test_sync_e2e PRIVATE
        "SYP_FFMPEG_CLI_PATH=\"${SYP_FFMPEG_CLI}\"")
    add_test(NAME sync_e2e COMMAND test_sync_e2e)
    set_tests_properties(sync_e2e PROPERTIES
        TIMEOUT 300
        ENVIRONMENT "SYP_FIXTURE_DIR=${SYP_FIXTURE_DIR}")

    # test_hls_e2e：HlsSession 双通道 io_open + VOD 单码率
    # 端到端。
    #
    # 门落在 if(SYP_HAVE_FFMPEG AND SYP_HAVE_FIXTURES) 内的 if(APPLE) 里，
    # 两条理由都是硬约束，不是保守：
    #   1. 素材：用例要读 $SYP_FIXTURE_DIR/hls/vod_single/（gen-fixtures.sh
    #      生成的 master/media/init/seg*/source.mp4）。挂在没有 SYP_HAVE_FIXTURES
    #      的门里，在没生成素材的机器上会变成必红而不是不注册。
    #   2. 平台：分片通道走 syp_source，播放列表通道走
    #      syp::dl::current_http_backend()，两者都要先注册平台 HTTP 后端
    #      （ensure_apple_backend，目前只有 Apple 实现）——跟
    #      test_hls_playlist_fetcher 同一个理由。
    # 这两条都消失的那天（有了第二个平台后端、素材成为必备），这个门可以
    # 跟着放宽；test_matrix_sentinel 盯的就是这类"子集缺失"。
    #
    # 链 syp_probe_core 而不是裸 syp_media：用例要用 scenarios.h 的
    # ensure_apple_backend()/TempCacheDir，跟 test_decode_e2e/test_sync_e2e
    # 同一份脚手架，不再本地复刻一遍。syp_probe_core PUBLIC 链 syp_media +
    # syp_platform_apple，媒体与后端符号仍然可见。
    if(APPLE)
        # support/fake_audio_sink.cpp + fake_renderer.cpp：
        # track_player_never_binds_a_discarded_variant_stream 要真的建一个
        # TrackPlayer 才能问 video_track_index()，用的是 test_track_player
        # 那同一份 fake，不再本地复刻。
        # support/stub_backend.cpp：结构性验收把全局
        # syp_http_backend 整个换成内存桩（真实网络物理不可达），用的是
        # test_source_bridge/test_avio_bridge 那同一份桩。
        add_executable(test_hls_e2e test_hls_e2e.cpp support/loopback_server.cpp
            support/stub_backend.cpp
            support/fake_audio_sink.cpp support/fake_renderer.cpp)
        target_include_directories(test_hls_e2e PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_link_libraries(test_hls_e2e PRIVATE
            syp_probe_core syp_warnings Threads::Threads)
        set_target_properties(test_hls_e2e PROPERTIES BUILD_RPATH "${SYP_FFMPEG_RPATH}")
        add_test(NAME hls_e2e COMMAND test_hls_e2e)
        # 300：同 decode_e2e/sync_e2e。用例自带的看门狗是 soft=60s/hard=180s，
        # 两者不会互相抵消——硬看门狗先于 ctest TIMEOUT 打进程，诊断输出
        # 来得及冲出去。
        set_tests_properties(hls_e2e PROPERTIES
            TIMEOUT 300
            ENVIRONMENT "SYP_FIXTURE_DIR=${SYP_FIXTURE_DIR}")

        # test_vt_decode：VideoToolbox 硬解集成测，macOS 宿主原生跑。
        # 原先挂在 if(SYP_HAVE_FFMPEG) 的 if(APPLE) 里，但用例全都读
        # $SYP_FIXTURE_DIR（faststart/bframes/hevc/hls），没生成素材的机器上会必红而
        # 不是不注册——跟 test_hls_e2e 同一条理由，移到本门内。
        # 用 .mm（ObjC++ 源）而不是给 .cpp 挂 `-x objective-c++` 的
        # COMPILE_FLAGS——顶层 CMakeLists.txt 已经 enable_language(OBJCXX) 并对
        # OBJCXX 全局加了 -fobjc-arc，.mm 源文件天然吃到这条设置，不需要每个
        # 文件单独再设一遍编译期 hack。
        # HLS 硬解用例要用 scenarios.h 的 ensure_apple_backend()/
        # TempCacheDir 以及 support/loopback_server.cpp 起真实回环服务器——
        # 跟 test_hls_e2e/test_hls_playlist_fetcher 同一套脚手架，链
        # syp_probe_core（PUBLIC 链 syp_media + syp_platform_apple）而不是
        # 裸 syp_platform_apple。
        # test_media_info_provider：time→byte 的 FFmpeg 实现。
        # 三重门都是硬约束：素材（faststart.mp4）、FFmpeg（Demuxer）、
        # 平台 HTTP 后端（provider 真的开一次连接）——跟 test_hls_e2e 同理。
        # 链 syp_probe_core 拿 scenarios.h 的 ensure_apple_backend()/TempCacheDir。
        add_executable(test_media_info_provider test_media_info_provider.cpp
            support/loopback_server.cpp)
        target_include_directories(test_media_info_provider PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR})
        target_link_libraries(test_media_info_provider PRIVATE
            syp_probe_core syp_warnings Threads::Threads)
        set_target_properties(test_media_info_provider PROPERTIES
            BUILD_RPATH "${SYP_FFMPEG_RPATH}")
        add_test(NAME media_info_provider COMMAND test_media_info_provider)
        set_tests_properties(media_info_provider PROPERTIES
            TIMEOUT 300
            ENVIRONMENT "SYP_FIXTURE_DIR=${SYP_FIXTURE_DIR}")

        add_executable(test_vt_decode test_vt_decode.mm support/loopback_server.cpp)
        target_include_directories(test_vt_decode PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_link_libraries(test_vt_decode PRIVATE
            syp_probe_core syp_warnings Threads::Threads)
        set_target_properties(test_vt_decode PROPERTIES BUILD_RPATH "${SYP_FFMPEG_RPATH}")
        add_test(NAME vt_decode COMMAND test_vt_decode)
        # 1800，不是 120：早期只有轻量的接线/结构性用例，120s 够用；
        # 后来加了 hardware_and_software_decode_are_bit_exact——对
        # faststart/bframes/hevc 三个素材各跑一遍 Demuxer+FFmpegVideoDecoder
        # 的软解、硬解全量解码，外加逐帧把 CVPixelBuffer 的两个平面拷进
        # std::vector 再逐字节比较——是这个可执行文件里最重的用例，加上
        # seek-to-eof 两遍、HLS 软/硬解各一遍。普通构建下全套 8 个用例
        # 42s；ASan 下光是 bit-exact 用例处理完第一个素材就将近用满原来
        # 120s 的窗口；TSan 因为要跟踪 VideoToolbox/CoreMedia 内部起的
        # 线程/XPC，開销更大——实测跑完第一个素材（faststart，0
        # mismatched，无 sanitizer 报告）就已经四分多钟。这些都是
        # sanitizer 插桩本身的合法开销，不是卡死；1800s 给三个构建
        # （build/build-tsan/build-asan）共用同一个足够宽的窗口，跟
        # test_probe_e2e 的 TIMEOUT 900 同一个"验证性用例给足时间"的道理，
        # 只是这里实测需要的余量更大。
        set_tests_properties(vt_decode PROPERTIES
            TIMEOUT 1800
            ENVIRONMENT "SYP_FIXTURE_DIR=${SYP_FIXTURE_DIR}")
    endif()
endif()
