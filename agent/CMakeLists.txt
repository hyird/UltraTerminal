find_package(Boost CONFIG REQUIRED COMPONENTS json)
find_package(OpenSSL REQUIRED)
add_library(ultraterimal-login-serialization STATIC
    platform/windows/login_serialization.transport.cpp
    platform/windows/login_provider.transport.cpp
    platform/windows/login_peer.transport.cpp
    platform/windows/login_inbox.transport.cpp
    platform/windows/login_credential.transport.cpp)
target_compile_features(ultraterimal-login-serialization PUBLIC cxx_std_23)
target_include_directories(ultraterimal-login-serialization PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(ultraterimal-login-serialization PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
target_compile_options(ultraterimal-login-serialization PRIVATE /W4 /WX /permissive- /utf-8 /EHsc)
target_link_libraries(ultraterimal-login-serialization PUBLIC ultraterimal::protocol PRIVATE advapi32 secur32 ole32 user32)
add_library(ultraterimal-login-provider SHARED platform/windows/login_module.runtime.cpp platform/windows/login_module.def)
target_compile_features(ultraterimal-login-provider PRIVATE cxx_std_23)
target_compile_options(ultraterimal-login-provider PRIVATE /W4 /WX /permissive- /utf-8 /EHsc)
target_link_libraries(ultraterimal-login-provider PRIVATE ultraterimal-login-serialization)
set_target_properties(ultraterimal-login-provider PROPERTIES OUTPUT_NAME ultraterimal_login)
# The vcpkg OpenH264 port exports pkg-config metadata but no CMake target.
# Select both CRT configurations from the manifest installation, never a system library.
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
add_library(ultraterimal-agent-core STATIC
    config/command_line.config.cpp
    features/connection/connection.config.cpp
    features/connection/control.runtime.cpp
    features/connection/tls.transport.cpp
    features/connection/quic.transport.cpp
    features/connection/native.transport.cpp
    features/connection/connection.transport.cpp
    features/desktop/software_video.transport.cpp
    features/desktop/video.transport.cpp
    features/desktop/avc.protocol.cpp
    features/desktop/video_color.protocol.cpp
    features/desktop/desktop.transport.cpp
    features/desktop/video.runtime.cpp
    features/desktop/worker.runtime.cpp
    features/desktop/bridge.runtime.cpp
    features/desktop/session.runtime.cpp
    features/enrollment/enrollment.runtime.cpp
    features/enrollment/enrollment.transport.cpp
    platform/windows/credential.transport.cpp
    platform/windows/windows_login.transport.cpp
    platform/windows/system_restart.transport.cpp
    platform/windows/login_broker.transport.cpp
    platform/windows/login_install.transport.cpp
    platform/windows/capability.transport.cpp
    platform/windows/capture.transport.cpp
    platform/windows/cursor.transport.cpp
    platform/windows/pipe.transport.cpp
    platform/windows/desktop_process.transport.cpp
    platform/windows/desktop_launch.transport.cpp
    platform/windows/desktop.transport.cpp
    platform/windows/input.transport.cpp
    platform/windows/media_events.transport.cpp
    platform/windows/media_encoder.transport.cpp
    platform/windows/service.runtime.cpp
    platform/windows/terminal.transport.cpp
)
target_compile_features(ultraterimal-agent-core PUBLIC cxx_std_23)
set_target_properties(ultraterimal-agent-core PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(ultraterimal-agent-core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(ultraterimal-agent-core PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
target_compile_options(ultraterimal-agent-core PUBLIC /bigobj /W4 /WX /permissive- /utf-8 /EHsc)
target_link_libraries(ultraterimal-agent-core PRIVATE ultraterimal-login-serialization advapi32 wtsapi32 userenv dxgi d3d11 gdi32 crypt32 bcrypt ole32 mfuuid strmiids OpenH264::OpenH264
    PUBLIC ultraterimal::protocol ruvia::core Boost::json OpenSSL::SSL OpenSSL::Crypto)
add_executable(ultraterimal-agent main.cpp)
target_link_libraries(ultraterimal-agent PRIVATE ultraterimal-agent-core)
set_target_properties(ultraterimal-agent PROPERTIES OUTPUT_NAME agent)
