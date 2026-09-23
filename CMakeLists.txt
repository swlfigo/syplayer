# 顶层构建：C++23 静态库 syp_dl（平台无关的 dl 层）+ Apple 平台后端 + 可选单测
cmake_minimum_required(VERSION 3.20)
project(syplayer LANGUAGES CXX)
if(APPLE)
    enable_language(OBJCXX)
    set(CMAKE_OBJCXX_STANDARD 23)
    set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
    set(CMAKE_OBJCXX_EXTENSIONS OFF)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 警告 flag 单一来源：库和测试都 PRIVATE 链这个 INTERFACE，避免每个目标抄一份。
add_library(syp_warnings INTERFACE)
target_compile_options(syp_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Werror -Wshadow -Wconversion -Wsign-conversion
)

# syp_dl：核心资产，零平台依赖。不含 .mm，不链任何 framework。
add_library(syp_dl STATIC
    src/dl/hole_set.cpp
    src/dl/m3u8_scan.cpp
    src/dl/cache_index.cpp
    src/dl/cache_file.cpp
    src/dl/cache_store.cpp
    src/dl/dl_task.cpp
    src/dl/rate_limiter.cpp
    src/dl/health_ticker.cpp
    src/dl/preconnector.cpp
    src/dl/scheduler.cpp
    src/dl/preloader.cpp
    src/dl/source_bridge.cpp
    src/dl/syp_source_api.cpp
    src/dl/syp_preload_api.cpp
    src/dl/syp_net_api.cpp
)
target_include_directories(syp_dl
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(syp_dl PRIVATE syp_warnings)

# Apple HTTP 后端是 dl 层下面的一条缝，不是 dl 层本身。
if(APPLE)
    add_library(syp_platform_apple STATIC
        src/platform/apple/apple_http_backend.mm
    )
    target_compile_options(syp_platform_apple PRIVATE
        $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
    )
    target_link_libraries(syp_platform_apple
        PUBLIC
            syp_dl
            "-framework Foundation"
        PRIVATE
            syp_warnings
    )
endif()

# 纪律：syp_dl 的源和链接接口都不许再漂回平台实现。
function(syp_assert_dl_platform_free)
    get_target_property(_syp_dl_srcs syp_dl SOURCES)
    foreach(_s IN LISTS _syp_dl_srcs)
        if(_s MATCHES "\\.(mm|m)$")
            message(FATAL_ERROR "syp_dl must stay platform-free; found ObjC source: ${_s}")
        endif()
    endforeach()
    get_target_property(_syp_dl_iface syp_dl INTERFACE_LINK_LIBRARIES)
    if(_syp_dl_iface AND NOT _syp_dl_iface STREQUAL "INTERFACE_LINK_LIBRARIES-NOTFOUND")
        foreach(_l IN LISTS _syp_dl_iface)
            if(_l MATCHES "framework|-framework|Foundation|CFNetwork|OkHttp")
                message(FATAL_ERROR
                    "syp_dl INTERFACE_LINK_LIBRARIES must not contain platform frameworks: ${_l}")
            endif()
        endforeach()
    endif()
endfunction()
syp_assert_dl_platform_free()

# ---------------------------------------------------------------- FFmpeg
# 产物由 tools/build-ffmpeg.sh 生成，不进仓库。探测到就自动启用相关目标，
# 探测不到就跳过——别人 clone 下来没有 xcframework 时构建照常绿。
# 要强制关闭：把 SYP_FFMPEG_XCFRAMEWORK 指向一个不存在的路径。
set(SYP_FFMPEG_XCFRAMEWORK
    "${CMAKE_CURRENT_SOURCE_DIR}/build-ffmpeg/out/SYFFmpeg.xcframework"
    CACHE PATH "SYFFmpeg.xcframework 的路径")
# 早先的构建目录在缓存里记着改名前的 build-ffmpeg/out/FFmpeg.xcframework。缓存
# 变量不会跟着默认值更新，那份旧产物删掉后这里会静默探测失败、所有 FFmpeg 相关
# 目标与测试无声消失。遇到旧默认值就改成新的。
if(SYP_FFMPEG_XCFRAMEWORK STREQUAL "${CMAKE_CURRENT_SOURCE_DIR}/build-ffmpeg/out/FFmpeg.xcframework")
    message(STATUS "SYP_FFMPEG_XCFRAMEWORK 是改名前的旧默认值，改用 SYFFmpeg.xcframework")
    set_property(CACHE SYP_FFMPEG_XCFRAMEWORK PROPERTY VALUE
        "${CMAKE_CURRENT_SOURCE_DIR}/build-ffmpeg/out/SYFFmpeg.xcframework")
endif()

set(SYP_HAVE_FFMPEG OFF)
set(SYP_HAVE_FIXTURES OFF)
if(APPLE AND CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")
    set(_syp_slice "${SYP_FFMPEG_XCFRAMEWORK}/macos-arm64")
    if(EXISTS "${_syp_slice}/SYFFmpeg.framework/Headers/libavformat/avformat.h"
       AND EXISTS "${_syp_slice}/SYFFmpeg.framework/Headers/syp_ffmpeg_prefix.h")
        set(SYP_HAVE_FFMPEG ON)
        set(SYP_FFMPEG_SLICE_DIR "${_syp_slice}")
    endif()
endif()
# SYP_HAVE_MACOS_SLICE 是 SYP_HAVE_FFMPEG 的别名——两者判据完全相同，只是
# 单独起个名字，跟下面的 SYP_HAVE_IOS_SLICE 对称，方便哨兵测试
# （tests/test_matrix_sentinel.cpp）逐项汇报，而不是含糊地只报一个总开关。
set(SYP_HAVE_MACOS_SLICE ${SYP_HAVE_FFMPEG})

# ios-arm64 slice 探测独立于上面的 macOS 构建开关：即使当前不是在给 macOS
# 构建（或者机器不是 arm64），也要能查出 xcframework 里有没有这个 slice——
# 哨兵测试要靠它揪出 check-deploy-target.sh 静默跳过 iOS availability 检查
# 的情况。路径与 check-deploy-target.sh 探测的
# SYP_FFMPEG_IOS_HEADERS 保持一致。
set(SYP_HAVE_IOS_SLICE OFF)
set(_syp_ios_slice "${SYP_FFMPEG_XCFRAMEWORK}/ios-arm64")
if(EXISTS "${_syp_ios_slice}/SYFFmpeg.framework/Headers/libavformat/avio.h")
    set(SYP_HAVE_IOS_SLICE ON)
endif()

# ios-arm64-maccatalyst slice 探测，同上，独立于其它开关。
# macOS 上想用 UIKit 只有 Mac Catalyst 一条路，需要这个 slice 才能链。
set(SYP_HAVE_CATALYST_SLICE OFF)
set(_syp_catalyst_slice "${SYP_FFMPEG_XCFRAMEWORK}/ios-arm64-maccatalyst")
if(EXISTS "${_syp_catalyst_slice}/SYFFmpeg.framework/Headers/libavformat/avio.h")
    set(SYP_HAVE_CATALYST_SLICE ON)
endif()

# CI 用：矩阵任何一项缺失都要红，而不是「没装 FFmpeg 就照常绿」。
# 本地开发机默认 OFF——保持"没装 FFmpeg 时构建照常绿"这条既有取舍不变。
option(SYP_REQUIRE_FULL_MATRIX
    "验证矩阵必须完整（CI 用）：SYP_HAVE_FFMPEG/SYP_HAVE_FIXTURES/三个 slice 缺任何一项都判哨兵测试失败" OFF)

if(SYP_HAVE_FFMPEG)
    message(STATUS "FFmpeg: ${SYP_FFMPEG_SLICE_DIR}")

    # syp_ffmpeg：SYFFmpeg 的导入目标，头文件、映射头与链接参数都挂在这里，
    # 以 INTERFACE 形式传给所有（直接或间接）链到它的目标。
    #
    # SYFFmpeg 只导出 syp_ 前缀的名字（接入方自带另一份 FFmpeg 时两边互不串用），
    # 所以凡是包含 FFmpeg 头的编译单元都必须先 -include 映射头
    # syp_ffmpeg_prefix.h，把 av_read_frame 之类的原名改写成 syp_ 版本；漏了
    # 的话链接期就是未定义符号。挂成 INTERFACE 编译选项而不是逐个目标加，
    # 是为了让测试、工具、syp_platform_apple 经 syp_media 链接时自动带上。
    # tools/check-ffmpeg-symbols.sh 以 syp_media / syp_platform_apple 静态库为
    # 消费方反查：未定义引用里不得有未加前缀的 FFmpeg 名。
    set(SYP_FFMPEG_HEADERS "${SYP_FFMPEG_SLICE_DIR}/SYFFmpeg.framework/Headers")
    add_library(syp_ffmpeg INTERFACE)
    # 用 Headers 目录而非 -F 的 <SYFFmpeg/...> 形式，让源码写标准 include，
    # 将来 Android/Linux 上换成普通 .so 时源码一个字不用改。
    # 标 SYSTEM：FFmpeg 头文件自身在 -Wconversion/-Wsign-conversion 下报警，
    # 这条纪律管的是我们自己的代码，不是第三方头文件。
    target_include_directories(syp_ffmpeg SYSTEM INTERFACE "${SYP_FFMPEG_HEADERS}")
    # 只对 C/C++/ObjC/ObjC++ 生效（-include 对 Swift、Metal 等无意义）。
    target_compile_options(syp_ffmpeg INTERFACE
        "$<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>:SHELL:-include \"${SYP_FFMPEG_HEADERS}/syp_ffmpeg_prefix.h\">")
    target_link_libraries(syp_ffmpeg INTERFACE
        "-F${SYP_FFMPEG_SLICE_DIR}" "-framework SYFFmpeg")

    # syp_media：dl 层之上的缝。FFmpeg 只挂在这里，syp_dl 仍然零依赖。
    add_library(syp_media STATIC
        src/media/audio_ring.cpp
        src/media/avio_bridge.cpp
        src/media/demuxer.cpp
        src/media/ffmpeg_audio_decoder.cpp
        src/media/ffmpeg_video_decoder.cpp
        src/media/frame.cpp
        src/media/frame_queue.cpp
        src/media/hls/hls_session.cpp
        src/media/hls/playlist_fetcher.cpp
        src/media/hls/url_rewrite.cpp
        src/media/media_info_provider.cpp
        src/media/packet_queue.cpp
        src/media/pipeline.cpp
        src/media/time_source.cpp
        src/media/track_player.cpp
        src/media/video_geometry.cpp)
    target_include_directories(syp_media PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(syp_media
        PUBLIC  syp_dl syp_ffmpeg
        PRIVATE syp_warnings)

    # 动态 framework：不设 rpath 的话可执行文件在运行期才报找不到 dylib。
    set(SYP_FFMPEG_RPATH "${SYP_FFMPEG_SLICE_DIR}")

    # AudioUnitSink：既要 syp_media（AudioRing/IAudioSink/
    # Frame + swresample 走同一份 FFmpeg），又要 AudioToolbox/CoreAudio
    # 这两个真正碰硬件的 framework——两者都只在这个 SYP_HAVE_FFMPEG 门内
    # 才具备，所以源文件挂在这里而不是上面那个无条件的 `if(APPLE)` 块
    # （那个块此时已经 add_library 过 syp_platform_apple，只是还没有
    # FFmpeg/AudioToolbox 可链，这里补上）。没有 FFmpeg 的机器上
    # syp_platform_apple 仍然存在、仍然能编——只是不含音频这部分，跟本
    # 仓库"探测不到就跳过、构建照常绿"的一贯取舍一致。
    if(APPLE)
        target_sources(syp_platform_apple PRIVATE src/platform/apple/audio_unit_sink.mm)
        target_link_libraries(syp_platform_apple
            PUBLIC
                syp_media
                "-framework AudioToolbox"
                "-framework CoreAudio"
                # iOS/Catalyst 分支用 AVAudioSession 读
                # outputLatency/IOBufferDuration、注册路由变化观察者。纯
                # macOS 构建这台开发机上 `#if !TARGET_OS_OSX` 分支不会被
                # 编译，但链接这个 framework 本身在 macOS 上无害
                # （AVFoundation 在 macOS 上也存在，只是不含
                # AVAudioSession），不需要按平台再拆一次链接条件。
                "-framework AVFoundation"
        )

        # MetalRenderer：跟上面 AudioUnitSink 同一个理由挂在
        # 这个 SYP_HAVE_FFMPEG 门里——metal_renderer.mm 间接 #include
        # "media/frame.h" -> FFmpeg 头，present() 也需要 Frame 的
        # width()/height()/pix_fmt()/plane()/stride()。
        #
        # video_shaders.metal 的着色器源码用 -[MTLDevice newLibraryWithSource:]
        # 在运行期编译，不走 `xcrun metal`/`metallib` 离线编译流水线——
        # 这台开发机的 Xcode 装的是标准工具链，没有独立的 "Metal
        # Toolchain" 组件（`xcrun metal` 报 "missing Metal Toolchain"，
        # 需要联网装 `xcodebuild -downloadComponent MetalToolchain`），
        # 四套构建（Debug/Release/ASan+UBSan/TSan）都不能依赖这个没装、
        # 需要联网才能装的组件。运行期编译走 Metal.framework 自带的
        # 编译器，标准工具链自带。
        #
        # 着色器源码内嵌进一个生成头，由 tools/gen-embedded-header.sh 生成
        # ——顶层 CMakeLists.txt（这里）与 tools/check-deploy-target.sh
        # （standalone -fsyntax-only 检查，不经过 CMake）两处都要生成同一
        # 份内嵌头，共用这一份脚本，避免各写一份然后漂移。
        set(SYP_METAL_SHADER_SRC_FILE
            "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/apple/video_shaders.metal")
        set(SYP_METAL_SHADER_GEN_DIR "${CMAKE_BINARY_DIR}/generated/syp")
        set(SYP_METAL_SHADER_GEN_HEADER
            "${SYP_METAL_SHADER_GEN_DIR}/syp_video_shaders_metal_source.h")
        execute_process(
            COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen-embedded-header.sh"
                    "${SYP_METAL_SHADER_SRC_FILE}" "${SYP_METAL_SHADER_GEN_HEADER}"
                    "syp::platform" "kVideoShadersMetalSource"
            RESULT_VARIABLE _syp_metal_shader_gen_rc)
        if(NOT _syp_metal_shader_gen_rc EQUAL 0)
            message(FATAL_ERROR "生成视频着色器内嵌头失败（gen-embedded-header.sh 非 0 退出）")
        endif()
        # 编辑 video_shaders.metal 之后要重新生成内嵌头——file(READ)/execute_process
        # 都是 configure-time 操作，不会自动侦测源文件变化；把它登记进
        # CMAKE_CONFIGURE_DEPENDS，CMake 检测到这个文件比上次 configure
        # 新就会自动重新 configure（跟 xcframework 探测那批变量同一条纪律：
        # 静默用旧内容比重新 configure 慢一点更危险）。
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            "${SYP_METAL_SHADER_SRC_FILE}")

        target_sources(syp_platform_apple PRIVATE src/platform/apple/metal_renderer.mm)
        target_include_directories(syp_platform_apple PRIVATE "${SYP_METAL_SHADER_GEN_DIR}")
        target_link_libraries(syp_platform_apple
            PUBLIC
                "-framework Metal"
                "-framework QuartzCore"   # CAMetalLayer 上屏
        )

        # VideoToolbox 硬解后端（经 FFmpeg hwaccel）。跟上面 AudioUnitSink/
        # MetalRenderer 同一个理由挂在这个 SYP_HAVE_FFMPEG 门里——
        # vt_decode_backend.mm 需要 hw_decode_backend.h（AVCodecContext/
        # AVCodecParameters），只有这里才具备 FFmpeg 头。
        target_sources(syp_platform_apple PRIVATE src/platform/apple/vt_decode_backend.mm)
        target_link_libraries(syp_platform_apple
            PUBLIC
                "-framework VideoToolbox"
                "-framework CoreVideo"
                "-framework CoreMedia"
        )
    endif()

    # 素材：种子写进 CMakeCache.txt，只在这个 build 目录第一次 configure 时抽取；
    # 之后同一个 build 目录内重复 configure 复用同一份种子和素材，
    # NOT EXISTS 守卫才真正生效——不然每次 configure 都是新种子、新目录，
    # 十几秒 ffmpeg 白跑一遍，旧目录也没人清理。要固定 / 回填复现，
    # 传 -DSYP_FIXTURE_SEED=<N>（会直接进 cache，跳过随机抽取）。
    # 种子打在 configure 输出里。
    find_program(SYP_FFMPEG_CLI ffmpeg)
    if(SYP_FFMPEG_CLI)
        if(NOT DEFINED SYP_FIXTURE_SEED)
            string(RANDOM LENGTH 8 ALPHABET "0123456789" _syp_seed_init)
            set(SYP_FIXTURE_SEED "${_syp_seed_init}" CACHE STRING
                "验证素材种子；固定后同一 build 目录复现同一份素材")
        endif()
        set(SYP_FIXTURE_DIR "${CMAKE_BINARY_DIR}/fixtures/${SYP_FIXTURE_SEED}")
        # 只查 manifest.json 存在与否曾经是个陷阱：gen-fixtures.sh 新增
        # bframes.mp4/bframes_faststart.mp4 之后，旧 build 目录里
        # manifest.json 已经存在（旧 schema，没有 bframes* 字段），这条守卫
        # 会打印"复用已生成的验证素材"、直接跳过重新生成——bframes*.mp4
        # 根本不存在，用到它们的用例会以 ref.error_stage 非空的形式变红，
        # 但报错信息完全指不到"素材目录过期"这个真因。所以除了
        # manifest.json，还要查**gen-fixtures.sh 产出的全部文件里最后落盘
        # 的那个**——素材集合以后再扩，这个哨兵要跟着换，是有意的耦合，
        # 不是遗漏。
        #
        # 哨兵从 bframes_faststart.mp4 换成了 hls/vod_demuxed/master.m3u8：
        # gen-fixtures.sh 加了三棵 HLS 树（gen-fixtures.sh:379-381，顺序
        # vod_single → vod_multi → vod_demuxed），bframes_faststart.mp4
        # 自那时起就不再是最后落盘的那个了，上面那句注释一并更正。
        # 挑最后一个的理由正是这条守卫的全部意义：中途落盘的任何一个都可能
        # 在"生成到一半"的目录里已经存在。
        #
        # 这条耦合被漏掉的实际后果：**已经存在**的构建目录（manifest.json
        # 与 bframes_faststart.mp4 都在）会打印"复用已生成的验证素材"并
        # 跳过重新生成，test_hls_e2e 在那里以"读不到 fixture 文件"的形式
        # 变红，而新一轮生成的构建目录里全绿——症状是"同一份代码在两个
        # 构建目录里结论相反"，真因指不到素材目录过期。
        #
        # 追加 hevc.mp4 到哨兵：gen-fixtures.sh 在 bframes_faststart.mp4
        # 之后、HLS 素材之前新增了 hevc.mp4——它不是脚本里最后落盘的文件
        # （HLS 三棵树和 manifest.json 都在它之后生成），但同样受"已存在的
        # 旧 build 目录缺这个文件"这条陷阱影响：manifest.json 与
        # hls/vod_demuxed/master.m3u8 都在的旧目录会被判成"已生成"而跳过
        # 重新生成，hevc.mp4 却不存在。三个条件一起 OR，任一缺失都触发
        # 重新生成。
        if(NOT EXISTS "${SYP_FIXTURE_DIR}/manifest.json" OR
           NOT EXISTS "${SYP_FIXTURE_DIR}/hls/vod_demuxed/master.m3u8" OR
           NOT EXISTS "${SYP_FIXTURE_DIR}/hevc.mp4")
            message(STATUS "生成验证素材 seed=${SYP_FIXTURE_SEED} → ${SYP_FIXTURE_DIR}")
            execute_process(
                COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen-fixtures.sh"
                        --seed "${SYP_FIXTURE_SEED}" --out "${SYP_FIXTURE_DIR}"
                RESULT_VARIABLE _syp_fix_rc)
            if(NOT _syp_fix_rc EQUAL 0)
                message(FATAL_ERROR "gen-fixtures.sh 失败（rc=${_syp_fix_rc}）")
            endif()
        else()
            message(STATUS "复用已生成的验证素材 seed=${SYP_FIXTURE_SEED} → ${SYP_FIXTURE_DIR}")
        endif()
        set(SYP_HAVE_FIXTURES ON)
    else()
        message(STATUS "未找到 ffmpeg CLI（brew install ffmpeg），跳过端到端验证测试")
        set(SYP_HAVE_FIXTURES OFF)
    endif()

    add_subdirectory(tools)

    # 构建期符号隔离检查：用 tools/check-ffmpeg-symbols.sh 反查链 FFmpeg 的
    # 静态库，未定义引用里不得有未加 syp_ 前缀的 FFmpeg 名（有编译单元漏了
    # -include 映射头、或绕过 syp_ffmpeg 自己拼了 FFmpeg 头目录）。
    # 为什么要在静态库这一层拦：静态库本身链接不报错，漏映射的引用要到链接
    # 可执行文件时才变成"未定义符号"，而链接报错指不出是哪个编译单元的问题；
    # 更糟的是如果那份可执行文件恰好还链了别的 FFmpeg，就会静默绑到别人那份。
    # 参与 ALL：库有变化就重查，查不过整个构建失败。stamp 只在通过时写出。
    set(_syp_sym_libs
        $<TARGET_FILE:syp_media>
        $<TARGET_FILE:syp_platform_apple>
        $<TARGET_FILE:syp_probe_core>)
    set(SYP_FFMPEG_SYMBOLS_CHECK_COMMAND
        bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/check-ffmpeg-symbols.sh
        --expect-slices 4 ${SYP_FFMPEG_XCFRAMEWORK} ${_syp_sym_libs})
    set(_syp_sym_stamp ${CMAKE_CURRENT_BINARY_DIR}/syp_ffmpeg_symbols_check.stamp)
    add_custom_command(
        OUTPUT ${_syp_sym_stamp}
        COMMAND ${CMAKE_COMMAND} -E remove -f ${_syp_sym_stamp}
        COMMAND ${SYP_FFMPEG_SYMBOLS_CHECK_COMMAND}
        COMMAND ${CMAKE_COMMAND} -E touch ${_syp_sym_stamp}
        DEPENDS syp_media syp_platform_apple syp_probe_core ${_syp_sym_libs}
                ${CMAKE_CURRENT_SOURCE_DIR}/tools/check-ffmpeg-symbols.sh
                ${SYP_FFMPEG_XCFRAMEWORK}/Info.plist
        COMMENT "检查 FFmpeg 符号隔离（tools/check-ffmpeg-symbols.sh）"
        VERBATIM)
    add_custom_target(syp_ffmpeg_symbols_check ALL DEPENDS ${_syp_sym_stamp})
else()
    message(STATUS "FFmpeg: 未找到 ${SYP_FFMPEG_XCFRAMEWORK}/macos-arm64，跳过 syp_media / syp_probe / probe_e2e")
endif()

# 构建期纯度检查：只 force_load syp_dl 归档、不走它的 CMake 链接接口。
# .mm 被塞回 syp_dl → 缺 ObjC/Foundation 符号，链接失败。
# 默认参与 ALL，不注册为 ctest（四个测试目标保持不变）。
add_executable(syp_dl_purity_check tests/syp_dl_purity_check.cpp)
target_include_directories(syp_dl_purity_check PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(syp_dl_purity_check PRIVATE syp_warnings)
add_dependencies(syp_dl_purity_check syp_dl)
if(APPLE)
    target_link_options(syp_dl_purity_check PRIVATE
        "SHELL:-Wl,-force_load,$<TARGET_FILE:syp_dl>"
    )
else()
    target_link_libraries(syp_dl_purity_check PRIVATE syp_dl)
endif()
# target_link_options / add_dependencies 都不把归档文件登记为链接依赖，
# 增量构建时 syp_dl 更新不会重链本目标。显式钉上 TARGET_FILE。
set_property(TARGET syp_dl_purity_check APPEND PROPERTY LINK_DEPENDS $<TARGET_FILE:syp_dl>)

# PROJECT_IS_TOP_LEVEL 是 3.21 引入的；被 add_subdirectory 嵌入时默认不编测试。
if(CMAKE_VERSION VERSION_LESS "3.21")
    if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
        set(SYP_IS_TOP_LEVEL ON)
    else()
        set(SYP_IS_TOP_LEVEL OFF)
    endif()
else()
    set(SYP_IS_TOP_LEVEL ${PROJECT_IS_TOP_LEVEL})
endif()

option(SYP_BUILD_TESTS "Build unit tests" ${SYP_IS_TOP_LEVEL})
if(SYP_BUILD_TESTS)
    if(SYP_IS_TOP_LEVEL)
        enable_testing()
    endif()
    add_subdirectory(tests)
endif()
