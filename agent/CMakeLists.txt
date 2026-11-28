find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)

set(agent_common_sources
    config/command_line.config.cpp
    features/connection/connection.config.cpp
    features/connection/control.runtime.cpp
    features/connection/tls.transport.cpp
    features/connection/quic.transport.cpp
    features/connection/native.transport.cpp
    features/connection/connection.transport.cpp
    features/enrollment/enrollment.runtime.cpp
    features/enrollment/enrollment.transport.cpp
    features/file/file.runtime.cpp
    features/terminal/terminal.runtime.cpp
    features/update/update.runtime.cpp
)

if(WIN32)
    find_package(libde265 CONFIG REQUIRED)
    find_package(Opus CONFIG REQUIRED)
    set(dav1d_root "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
    find_path(DAV1D_INCLUDE_DIR dav1d/dav1d.h PATHS "${dav1d_root}/include" NO_DEFAULT_PATH REQUIRED)
    find_library(DAV1D_RELEASE dav1d PATHS "${dav1d_root}/lib" NO_DEFAULT_PATH REQUIRED)
    find_library(DAV1D_DEBUG dav1d PATHS "${dav1d_root}/debug/lib" NO_DEFAULT_PATH REQUIRED)
    add_library(dav1d::dav1d STATIC IMPORTED)
    set_target_properties(dav1d::dav1d PROPERTIES
        IMPORTED_CONFIGURATIONS "DEBUG;RELEASE"
        IMPORTED_LOCATION_RELEASE "${DAV1D_RELEASE}"
        IMPORTED_LOCATION_DEBUG "${DAV1D_DEBUG}"
        MAP_IMPORTED_CONFIG_RELWITHDEBINFO Release
        MAP_IMPORTED_CONFIG_MINSIZEREL Release
        INTERFACE_INCLUDE_DIRECTORIES "${DAV1D_INCLUDE_DIR}")
    add_library(ultraterimal-login-serialization STATIC
        platform/windows/login_serialization.transport.cpp
        platform/windows/login_provider.transport.cpp
        platform/windows/login_peer.transport.cpp
        platform/windows/login_inbox.transport.cpp
        platform/windows/login_credential.transport.cpp)
    target_compile_features(ultraterimal-login-serialization PUBLIC cxx_std_23)
    target_include_directories(ultraterimal-login-serialization PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_definitions(ultraterimal-login-serialization PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
    target_compile_options(ultraterimal-login-serialization PRIVATE /W4 /WX /permissive- /utf-8 /EHsc "$<$<CONFIG:Release>:/Gy;/Gw;/GF;/GL>")
    set_target_properties(ultraterimal-login-serialization PROPERTIES STATIC_LIBRARY_OPTIONS "$<$<CONFIG:Release>:/LTCG>")
    target_link_libraries(ultraterimal-login-serialization PUBLIC ultraterimal::protocol PRIVATE advapi32 secur32 ole32 user32)
    add_library(ultraterimal-login-provider SHARED platform/windows/login_module.runtime.cpp platform/windows/login_module.def)
    target_compile_features(ultraterimal-login-provider PRIVATE cxx_std_23)
    target_compile_options(ultraterimal-login-provider PRIVATE /W4 /WX /permissive- /utf-8 /EHsc "$<$<CONFIG:Release>:/Gy;/Gw;/GF;/GL>")
    target_link_options(ultraterimal-login-provider PRIVATE "$<$<CONFIG:Release>:/LTCG;/OPT:REF;/OPT:ICF>")
    target_link_libraries(ultraterimal-login-provider PRIVATE ultraterimal-login-serialization)
    set_target_properties(ultraterimal-login-provider PROPERTIES OUTPUT_NAME ultraterimal_login)
    # OpenH264 provides software encoding and independent baseline validation.
    # Resolve only the manifest's static CRT configurations.
    set(openh264_root "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
    find_path(OPENH264_INCLUDE_DIR wels/codec_api.h PATHS "${openh264_root}/include" NO_DEFAULT_PATH REQUIRED)
    find_library(OPENH264_RELEASE openh264 PATHS "${openh264_root}/lib" NO_DEFAULT_PATH REQUIRED)
    find_library(OPENH264_DEBUG openh264 PATHS "${openh264_root}/debug/lib" NO_DEFAULT_PATH REQUIRED)
    add_library(OpenH264::OpenH264 STATIC IMPORTED GLOBAL)
    set_target_properties(OpenH264::OpenH264 PROPERTIES
        IMPORTED_CONFIGURATIONS "DEBUG;RELEASE"
        IMPORTED_LOCATION_RELEASE "${OPENH264_RELEASE}"
        IMPORTED_LOCATION_DEBUG "${OPENH264_DEBUG}"
        MAP_IMPORTED_CONFIG_RELWITHDEBINFO Release
        MAP_IMPORTED_CONFIG_MINSIZEREL Release
        INTERFACE_INCLUDE_DIRECTORIES "${OPENH264_INCLUDE_DIR}")

    add_library(ultraterimal-agent-core-objects OBJECT
        ${agent_common_sources}
        features/desktop/audio.protocol.cpp
        features/desktop/audio_codec.transport.cpp
        features/desktop/audio.runtime.cpp
        features/desktop/software_video.transport.cpp
        features/desktop/video.transport.cpp
        features/desktop/avc.protocol.cpp
        features/desktop/codecs.protocol.cpp
        features/desktop/video_color.protocol.cpp
        features/desktop/video_color_avx2.protocol.cpp
        features/desktop/desktop.transport.cpp
        features/desktop/png.protocol.cpp
        features/desktop/tool.runtime.cpp
        features/desktop/worker.runtime.cpp
        features/desktop/bridge.runtime.cpp
        features/desktop/relay.runtime.cpp
        features/desktop/session.runtime.cpp
        platform/windows/credential.transport.cpp
        platform/windows/windows_login.transport.cpp
        platform/windows/system_restart.transport.cpp
        platform/windows/update.transport.cpp
        platform/windows/file.transport.cpp
        platform/windows/login_broker.transport.cpp
        platform/windows/login_install.transport.cpp
        platform/windows/capability.transport.cpp
        platform/windows/audio_capture.transport.cpp
        platform/windows/capture.transport.cpp
        platform/windows/capture_pixels.protocol.cpp
        platform/windows/idd_capture.transport.cpp
        platform/windows/idd_install.transport.cpp
        platform/windows/idd_topology.transport.cpp
        platform/windows/cursor.transport.cpp
        platform/windows/deadline_timer.transport.cpp
        platform/windows/pipe.transport.cpp
        platform/windows/desktop_process.transport.cpp
        platform/windows/desktop_launch.transport.cpp
        platform/windows/desktop.transport.cpp
        platform/windows/desktop_tool.transport.cpp
        platform/windows/exec.transport.cpp
        platform/windows/input.transport.cpp
        platform/windows/media_events.transport.cpp
        platform/windows/media_encoder.transport.cpp
        platform/windows/service.runtime.cpp
        platform/windows/console_session.transport.cpp
        platform/windows/terminal.transport.cpp
        platform/windows/terminal_session.transport.cpp
    )
    # Only this out-of-line kernel may contain AVX2 instructions. Keep startup,
    # feature detection and the SSE2 fallback at the baseline x64 instruction set.
    # Disable /GL on this TU so LTCG cannot inline AVX2 instructions into baseline callers.
    set_source_files_properties(features/desktop/video_color_avx2.protocol.cpp PROPERTIES COMPILE_OPTIONS "/arch:AVX2;/GL-")
    add_library(ultraterimal-agent-core STATIC
        features/desktop/video.runtime.cpp
        features/desktop/video_worker.runtime.cpp)
    set_target_properties(ultraterimal-agent-core PROPERTIES STATIC_LIBRARY_OPTIONS "$<$<CONFIG:Release>:/LTCG>")
    target_link_options(ultraterimal-agent-core INTERFACE "$<$<CONFIG:Release>:/LTCG>")
    target_link_libraries(ultraterimal-agent-core PRIVATE ultraterimal-agent-core-objects)
    set(agent_core_targets ultraterimal-agent-core-objects ultraterimal-agent-core)

    foreach(agent_core_target IN LISTS agent_core_targets)
        target_compile_features(${agent_core_target} PUBLIC cxx_std_23)
        set_target_properties(${agent_core_target} PROPERTIES
            CXX_EXTENSIONS OFF
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        target_include_directories(${agent_core_target} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
        target_compile_definitions(${agent_core_target} PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
        target_compile_options(${agent_core_target} PUBLIC /bigobj /W4 /WX /permissive- /utf-8 /EHsc PRIVATE "$<$<CONFIG:Release>:/Gy;/Gw;/GF;/GL>")
        target_link_libraries(${agent_core_target} PRIVATE ultraterimal-login-serialization advapi32 wtsapi32 userenv dxgi d3d11 gdi32 crypt32 bcrypt ole32 mfuuid strmiids msi setupapi OpenH264::OpenH264 de265 dav1d::dav1d Opus::opus ZLIB::ZLIB
            PUBLIC ultraterimal::protocol ruvia::core OpenSSL::SSL OpenSSL::Crypto)
    endforeach()
    add_subdirectory(platform/windows/idd)
    add_executable(ultraterimal-agent main.cpp)
    target_compile_options(ultraterimal-agent PRIVATE "$<$<CONFIG:Release>:/Gy;/Gw;/GF;/GL>")
    target_link_libraries(ultraterimal-agent PRIVATE ultraterimal-agent-core)
    target_link_options(ultraterimal-agent PRIVATE "$<$<CONFIG:Release>:/LTCG;/OPT:REF;/OPT:ICF>")
    set_target_properties(ultraterimal-agent PROPERTIES OUTPUT_NAME agent)
else()
    add_library(ultraterimal-agent-core-objects OBJECT
        ${agent_common_sources}
        platform/linux/credential.transport.cpp
        platform/linux/capability.transport.cpp
        platform/linux/exec.transport.cpp
        platform/linux/file.transport.cpp
        platform/linux/system_restart.transport.cpp
        platform/linux/update.transport.cpp
        platform/linux/terminal.transport.cpp
    )
    add_library(ultraterimal-agent-core STATIC
        platform/linux/service.runtime.cpp)
    target_link_libraries(ultraterimal-agent-core PUBLIC ultraterimal-agent-core-objects)
    set(agent_core_targets ultraterimal-agent-core-objects ultraterimal-agent-core)

    foreach(agent_core_target IN LISTS agent_core_targets)
        target_compile_features(${agent_core_target} PUBLIC cxx_std_23)
        set_target_properties(${agent_core_target} PROPERTIES CXX_EXTENSIONS OFF)
        target_include_directories(${agent_core_target} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
        target_compile_options(${agent_core_target} PRIVATE -Wall -Wextra -Wpedantic -Werror
            "$<$<CONFIG:Release>:-ffunction-sections;-fdata-sections>")
        target_link_libraries(${agent_core_target} PRIVATE ZLIB::ZLIB
            PUBLIC ultraterimal::protocol ruvia::core OpenSSL::SSL OpenSSL::Crypto pthread)
    endforeach()
    add_executable(ultraterimal-agent main.cpp)
    target_compile_options(ultraterimal-agent PRIVATE -Wall -Wextra -Wpedantic -Werror
        "$<$<CONFIG:Release>:-ffunction-sections;-fdata-sections>")
    target_link_options(ultraterimal-agent PRIVATE "$<$<CONFIG:Release>:-Wl,--gc-sections,-s>")
    target_link_libraries(ultraterimal-agent PRIVATE ultraterimal-agent-core)
    set_target_properties(ultraterimal-agent PROPERTIES OUTPUT_NAME agent)
endif()
