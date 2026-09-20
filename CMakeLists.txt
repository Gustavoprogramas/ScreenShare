cmake_minimum_required(VERSION 3.20)
project(RemoteDesktop LANGUAGES CXX)

# ============================================================
#  C++ Standard — C++20 for std::optional, designated init
# ============================================================
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ============================================================
#  Compiler flags (MSVC + Clang-cl)
# ============================================================
if(MSVC)
    add_compile_options(
        /O2            # Full optimization
        /Ob2           # Aggressive inline
        /arch:AVX2     # AVX2 SIMD (Ryzen 7 5700X)
        /W3            # Warnings level 3
        /wd4100        # Unreferenced formal parameter (FFmpeg headers)
        /wd4244        # Narrowing conversion (FFmpeg)
        /wd4996        # Deprecated POSIX names
        /MP            # Multi-processor compilation
        /bigobj        # Large object files
    )
    add_compile_definitions(
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _WIN32_WINNT=0x0A00  # Windows 10+
        _CRT_SECURE_NO_WARNINGS
    )
endif()

# ============================================================
#  Deps directory: deps/ffmpeg and deps/sdl2
#  Run setup_deps.ps1 first to populate these.
# ============================================================
set(DEPS_DIR "${CMAKE_SOURCE_DIR}/deps")
set(FFMPEG_DIR "${DEPS_DIR}/ffmpeg")
set(SDL2_DIR   "${DEPS_DIR}/sdl2")

# ---- FFmpeg --------------------------------------------------
# Expected layout after setup_deps.ps1:
#   deps/ffmpeg/include/libavcodec/avcodec.h
#   deps/ffmpeg/lib/avcodec.lib
#   deps/ffmpeg/bin/avcodec-61.dll  (or similar)
if(NOT EXISTS "${FFMPEG_DIR}/include/libavcodec/avcodec.h")
    message(FATAL_ERROR
        "FFmpeg headers not found at ${FFMPEG_DIR}/include/\n"
        "Run: powershell -ExecutionPolicy Bypass -File setup_deps.ps1")
endif()

add_library(ffmpeg INTERFACE)
target_include_directories(ffmpeg INTERFACE "${FFMPEG_DIR}/include")
target_link_directories(ffmpeg INTERFACE "${FFMPEG_DIR}/lib")
target_link_libraries(ffmpeg INTERFACE
    avcodec
    avutil
    swscale
    swresample
)

# ---- SDL2 ----------------------------------------------------
# Headers are in deps/sdl2/include/SDL2/ (copied there by setup_deps.ps1
# or by the post-setup step that mirrors the flat layout into SDL2/ subdir).
if(NOT EXISTS "${SDL2_DIR}/include/SDL2/SDL.h")
    message(FATAL_ERROR
        "SDL2 headers not found at ${SDL2_DIR}/include/SDL2/\n"
        "Run: powershell -ExecutionPolicy Bypass -File setup_deps.ps1")
endif()

add_library(sdl2 INTERFACE)
target_include_directories(sdl2 INTERFACE "${SDL2_DIR}/include")
target_link_directories(sdl2 INTERFACE "${SDL2_DIR}/lib/x64")
target_link_libraries(sdl2 INTERFACE SDL2 SDL2main)


# ---- Windows system libraries --------------------------------
set(WIN_LIBS
    d3d11
    dxgi
    dxguid
    ws2_32
    winmm
)

# ============================================================
#  ImGui
# ============================================================
set(IMGUI_DIR "${DEPS_DIR}/imgui")
if(NOT EXISTS "${IMGUI_DIR}/imgui.h")
    message(FATAL_ERROR "ImGui not found in ${IMGUI_DIR}")
endif()

add_library(imgui STATIC
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_demo.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_sdlrenderer2.cpp"
)
target_include_directories(imgui PUBLIC "${IMGUI_DIR}" "${IMGUI_DIR}/backends")
target_link_libraries(imgui PUBLIC sdl2)

# ============================================================
#  Shared sources
# ============================================================
set(SHARED_HEADERS
    shared/protocol.h
    shared/ring_buffer.h
)

# ============================================================
#  HOST executable
# ============================================================
add_executable(RemoteHost WIN32
    host/main_host.cpp

    # Capture
    host/capture/dxgi_capture.h
    host/capture/dxgi_capture.cpp

    # Encode
    host/encode/hw_encoder.h
    host/encode/hw_encoder.cpp

    # Network
    host/network/video_sender.h
    host/network/video_sender.cpp
    host/network/input_receiver.h
    host/network/input_receiver.cpp

    # Input
    host/input/input_injector.h
    host/input/input_injector.cpp

    # Shared headers
    ${SHARED_HEADERS}
)

# Use console subsystem temporarily for debugging
set_target_properties(RemoteHost PROPERTIES
    WIN32_EXECUTABLE FALSE
    OUTPUT_NAME "RemoteHost"
)

target_include_directories(RemoteHost PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/shared
)

target_link_libraries(RemoteHost PRIVATE
    ffmpeg
    sdl2
    imgui
    ${WIN_LIBS}
)

# ============================================================
#  CLIENT executable
# ============================================================
add_executable(RemoteClient
    client/main_client.cpp

    # Network
    client/network/video_receiver.h
    client/network/video_receiver.cpp
    client/network/input_sender.h
    client/network/input_sender.cpp

    # Decode
    client/decode/hw_decoder.h
    client/decode/hw_decoder.cpp

    # Render
    client/render/sdl_renderer.h
    client/render/sdl_renderer.cpp

    # Shared headers
    ${SHARED_HEADERS}
)

set_target_properties(RemoteClient PROPERTIES
    WIN32_EXECUTABLE TRUE
    OUTPUT_NAME "RemoteClient"
)

target_include_directories(RemoteClient PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/shared
)

target_link_libraries(RemoteClient PRIVATE
    ffmpeg
    sdl2
    imgui
    ${WIN_LIBS}
)

# ============================================================
#  Post-build: copy DLLs next to the executables
# ============================================================

# Copy FFmpeg DLLs
file(GLOB FFMPEG_DLLS "${FFMPEG_DIR}/bin/*.dll")
foreach(DLL ${FFMPEG_DLLS})
    add_custom_command(TARGET RemoteHost  POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DLL}" $<TARGET_FILE_DIR:RemoteHost>)
    add_custom_command(TARGET RemoteClient POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DLL}" $<TARGET_FILE_DIR:RemoteClient>)
endforeach()

# Copy SDL2 DLL
add_custom_command(TARGET RemoteClient POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${SDL2_DIR}/lib/x64/SDL2.dll"
        $<TARGET_FILE_DIR:RemoteClient>
)

# ============================================================
#  Install
# ============================================================
install(TARGETS RemoteHost RemoteClient
    RUNTIME DESTINATION bin
)
install(FILES ${FFMPEG_DLLS} DESTINATION bin)
install(FILES "${SDL2_DIR}/lib/x64/SDL2.dll" DESTINATION bin)

message(STATUS "")
message(STATUS "=== Remote Desktop Build Configuration ===")
message(STATUS "  Host executable  : RemoteHost")
message(STATUS "  Client executable: RemoteClient")
message(STATUS "  FFmpeg dir       : ${FFMPEG_DIR}")
message(STATUS "  SDL2 dir         : ${SDL2_DIR}")
message(STATUS "")
message(STATUS "Build commands:")
message(STATUS "  cmake -B build -S . -DCMAKE_BUILD_TYPE=Release")
message(STATUS "  cmake --build build --config Release")
message(STATUS "")
