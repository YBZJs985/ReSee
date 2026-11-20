cmake_minimum_required(VERSION 3.24)

project(ReSee VERSION 2.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# 依赖路径（可通过 -DCMAKE_PREFIX_PATH / -DFFMPEG_ROOT 覆盖；若根目录存在 _qt6 / _ffmpeg 软链或目录则自动使用）
if(NOT DEFINED CMAKE_PREFIX_PATH AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/_qt6")
    set(CMAKE_PREFIX_PATH "${CMAKE_CURRENT_SOURCE_DIR}/_qt6")
endif()
if(NOT DEFINED FFMPEG_ROOT AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/_ffmpeg")
    set(FFMPEG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/_ffmpeg")
elseif(APPLE AND NOT DEFINED FFMPEG_ROOT AND EXISTS "/opt/homebrew/opt/ffmpeg")
    set(FFMPEG_ROOT "/opt/homebrew/opt/ffmpeg")
endif()

find_package(Qt6 REQUIRED COMPONENTS
    Core
    Gui
    GuiPrivate
    Qml
    Quick
    QuickControls2
    QuickDialogs2
    Multimedia
    ShaderTools
    Concurrent
)

# SQLite3 amalgamation
add_library(sqlite3_static STATIC
    third_party/sqlite/sqlite3.c
)
target_include_directories(sqlite3_static PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite
)
target_compile_definitions(sqlite3_static PRIVATE
    SQLITE_THREADSAFE=1
    SQLITE_ENABLE_FTS5=1
    SQLITE_DEFAULT_MEMSTATUS=0
)
if(MSVC)
    target_compile_options(sqlite3_static PRIVATE /W0)
endif()

# FFmpeg 导入目标
find_path(FFMPEG_INCLUDE_DIR libavcodec/avcodec.h HINTS "${FFMPEG_ROOT}/include" REQUIRED)
find_library(AVCODEC_LIB NAMES avcodec HINTS "${FFMPEG_ROOT}/lib" REQUIRED)
find_library(AVFORMAT_LIB NAMES avformat HINTS "${FFMPEG_ROOT}/lib" REQUIRED)
find_library(AVUTIL_LIB NAMES avutil HINTS "${FFMPEG_ROOT}/lib" REQUIRED)
find_library(SWSCALE_LIB NAMES swscale HINTS "${FFMPEG_ROOT}/lib" REQUIRED)
find_library(SWRESAMPLE_LIB NAMES swresample HINTS "${FFMPEG_ROOT}/lib" REQUIRED)
find_library(AVFILTER_LIB NAMES avfilter HINTS "${FFMPEG_ROOT}/lib" REQUIRED)

add_library(FFmpeg::FFmpeg INTERFACE IMPORTED)
target_include_directories(FFmpeg::FFmpeg INTERFACE "${FFMPEG_INCLUDE_DIR}")
target_link_libraries(FFmpeg::FFmpeg INTERFACE
    "${AVCODEC_LIB}"
    "${AVFORMAT_LIB}"
    "${AVUTIL_LIB}"
    "${SWSCALE_LIB}"
    "${SWRESAMPLE_LIB}"
    "${AVFILTER_LIB}"
)

# 核心引擎库（与 GUI 解耦，供桌面端与 bench_cli 共享）
set(CORE_SOURCES
    src/core/Types.h
    src/core/TimeUtils.h
    src/core/TimeUtils.cpp
    src/core/Logger.h
    src/core/Logger.cpp
    src/core/AppConfig.h
    src/core/AppConfig.cpp
    src/core/SunTimes.h
    src/core/SunTimes.cpp
    src/core/PlatformNotifier.h
    src/core/PlatformNotifier.cpp

    src/index/SidxParser.h
    src/index/SidxParser.cpp
    src/index/IndexDatabase.h
    src/index/IndexDatabase.cpp
    src/index/FileScanner.h
    src/index/FileScanner.cpp

    src/cache/NasBlockCache.h
    src/cache/NasBlockCache.cpp

    src/decode/HwAccelContext.h
    src/decode/HwAccelContext.cpp
    src/decode/Fmp4StreamSource.h
    src/decode/Fmp4StreamSource.cpp
    src/decode/StreamDecoder.h
    src/decode/StreamDecoder.cpp
    src/decode/AudioPlayer.h
    src/decode/AudioPlayer.cpp

    src/sync/MasterClock.h
    src/sync/MasterClock.cpp
    src/sync/TimestampOcr.h
    src/sync/TimestampOcr.cpp
    src/sync/SyncScheduler.h
    src/sync/SyncScheduler.cpp

    src/health/HealthMonitor.h
    src/health/HealthMonitor.cpp
)

add_library(sync_engine STATIC ${CORE_SOURCES})
target_include_directories(sync_engine PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(sync_engine PUBLIC
    Qt6::Core
    Qt6::Gui
    Qt6::Multimedia
    Qt6::Concurrent
    sqlite3_static
    FFmpeg::FFmpeg
)

if(WIN32)
    target_compile_definitions(sync_engine PUBLIC
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        SYNC_PLATFORM_WINDOWS=1
    )
    target_link_libraries(sync_engine PUBLIC d3d11 dxgi psapi shell32 user32)
elseif(APPLE)
    target_compile_definitions(sync_engine PUBLIC SYNC_PLATFORM_MACOS=1)
    find_library(VIDEOTOOLBOX_FRAMEWORK VideoToolbox REQUIRED)
    find_library(COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
    find_library(COREMEDIA_FRAMEWORK CoreMedia REQUIRED)
    find_library(COREVIDEO_FRAMEWORK CoreVideo REQUIRED)
    target_link_libraries(sync_engine PUBLIC
        ${VIDEOTOOLBOX_FRAMEWORK}
        ${COREFOUNDATION_FRAMEWORK}
        ${COREMEDIA_FRAMEWORK}
        ${COREVIDEO_FRAMEWORK}
    )
endif()

if(MSVC)
    target_compile_options(sync_engine PRIVATE /W4 /utf-8 /wd4100 /wd4244 /wd4267)
else()
    target_compile_options(sync_engine PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

# GUI 主程序
qt_standard_project_setup(REQUIRES 6.8)

set(APP_SOURCES
    src/render/TimelineTrackItem.h
    src/render/TimelineTrackItem.cpp
    src/render/VideoRenderItem.h
    src/render/VideoRenderItem.cpp
    src/app/PlaybackController.h
    src/app/PlaybackController.cpp
    src/app/TimelineModel.h
    src/app/TimelineModel.cpp
    src/app/DateNavModel.h
    src/app/DateNavModel.cpp
    src/app/BookmarkModel.h
    src/app/BookmarkModel.cpp
    src/app/DiagnosticsModel.h
    src/app/DiagnosticsModel.cpp
    src/app/HealthModel.h
    src/app/HealthModel.cpp
    src/app/main.cpp
)
if(WIN32)
    list(APPEND APP_SOURCES resources/app_icon.rc)
endif()

qt_add_executable(ReSee ${APP_SOURCES})
if(APPLE)
    # QML 模块会在构建目录生成同名 ReSee/ 目录，避免与可执行文件输出冲突。
    set_target_properties(ReSee PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.resee.desktop"
        MACOSX_BUNDLE_BUNDLE_NAME "ReSee"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_ICON_FILE "app_icon.icns"
    )
    set_source_files_properties(resources/app_icon.icns PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    target_sources(ReSee PRIVATE resources/app_icon.icns)
endif()
set_source_files_properties(qml/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
qt_add_resources(ReSee "toolbar_icons"
    PREFIX "/ui"
    FILES
        resources/toolbar/play.svg
        resources/toolbar/pause.svg
        resources/toolbar/previous.svg
        resources/toolbar/next.svg
        resources/toolbar/settings.svg
        resources/toolbar/audio.svg
        resources/toolbar/audio-active.svg
        resources/toolbar/mute.svg
        resources/toolbar/focus.svg
        resources/toolbar/restore.svg
)
qt_add_resources(ReSee "app_icons"
    PREFIX "/icons"
    FILES
        resources/app_icon.png
)
target_include_directories(ReSee PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render
)

qt_add_qml_module(ReSee
    URI ReSee
    VERSION 1.0
    QML_FILES
        qml/Main.qml
        qml/Theme.qml
        qml/SegmentSwitch.qml
        qml/KeyRecorder.qml
        qml/IconButton.qml
        qml/PrimaryButton.qml
        qml/HudLabel.qml
        qml/TransportBar.qml
        qml/DarkButton.qml
        qml/DarkCheckBox.qml
        qml/DarkColorPicker.qml
        qml/DateNavSidebar.qml
        qml/VideoTile.qml
        qml/ControlBar.qml
        qml/TimelinePanel.qml
        qml/AudioMonitorPanel.qml
        qml/CalendarJumpPopup.qml
        qml/BookmarkDrawer.qml
        qml/BookmarkDialog.qml
        qml/SettingsDialog.qml
        qml/DiagnosticsDrawer.qml
)

qt_add_shaders(ReSee "nv12_shaders"
    BATCHABLE
    PREFIX "/shaders"
    FILES
        shaders/nv12.vert
        shaders/nv12.frag
)

target_link_libraries(ReSee PRIVATE
    sync_engine
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::QuickDialogs2
    Qt6::ShaderTools
    Qt6::GuiPrivate
)

if(WIN32)
    # 先保留控制台可选调试输出，Release 打包可通过 WIN32_EXECUTABLE 控制
    set_target_properties(ReSee PROPERTIES
        WIN32_EXECUTABLE $<BOOL:${MC_WIN32_GUI}>
    )
    if(MSVC)
        target_compile_options(ReSee PRIVATE /W4 /utf-8 /wd4100)
    endif()
endif()

# 可选命令行基准测试与单元测试工具
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/bench_cli.cpp")
    add_executable(bench_cli
        tests/bench_cli.cpp
    )
    target_link_libraries(bench_cli PRIVATE sync_engine)
    if(MSVC)
        target_compile_options(bench_cli PRIVATE /W4 /utf-8 /wd4100)
    endif()
endif()

# 复制 Windows 运行所需的 FFmpeg DLL 到输出目录（便于开发期直接启动）
if(WIN32 AND EXISTS "${FFMPEG_ROOT}/bin")
    file(GLOB FFMPEG_DLLS "${FFMPEG_ROOT}/bin/*.dll")
    foreach(DLL_FILE ${FFMPEG_DLLS})
        add_custom_command(TARGET ReSee POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DLL_FILE}" "$<TARGET_FILE_DIR:ReSee>"
        )
        if(TARGET bench_cli)
            add_custom_command(TARGET bench_cli POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DLL_FILE}" "$<TARGET_FILE_DIR:bench_cli>"
            )
        endif()
    endforeach()
endif()
