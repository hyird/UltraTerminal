find_package(spdlog CONFIG REQUIRED)
set(ULTRATERIMAL_INSTALL_SCRIPT_FILE "${CMAKE_CURRENT_SOURCE_DIR}/modules/devices/install.ps1")
file(READ "${ULTRATERIMAL_INSTALL_SCRIPT_FILE}" ULTRATERIMAL_INSTALL_SCRIPT)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${ULTRATERIMAL_INSTALL_SCRIPT_FILE}")
configure_file(config/installer.script.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/installer.script.h" @ONLY)
add_library(ultraterimal-server-core STATIC
    config/server.config.cpp
    config/storage.cpp
    modules/identity/identity.service.cpp
    modules/identity/security.protocol.cpp
    modules/telegram/telegram.service.cpp
    modules/devices/device.service.cpp
    modules/sessions/session.service.cpp)
target_include_directories(ultraterimal-server-core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(ultraterimal-server-core PUBLIC cxx_std_23)
set_target_properties(ultraterimal-server-core PROPERTIES CXX_EXTENSIONS OFF)
target_compile_options(ultraterimal-server-core PRIVATE
    "$<$<CXX_COMPILER_ID:MSVC>:/W4;/WX;/permissive-;/utf-8;/EHsc;/bigobj>"
    "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall;-Wextra;-Wpedantic;-Werror>"
    "$<$<AND:$<NOT:$<CXX_COMPILER_ID:MSVC>>,$<CONFIG:Release>>:-ffunction-sections;-fdata-sections>")
target_link_libraries(ultraterimal-server-core PUBLIC ruvia::web ultraterimal::protocol PRIVATE OpenSSL::Crypto)
add_library(ultraterimal-control-runtime STATIC features/control/control.runtime.cpp)
target_include_directories(ultraterimal-control-runtime PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(ultraterimal-control-runtime PUBLIC cxx_std_23)
set_target_properties(ultraterimal-control-runtime PROPERTIES CXX_EXTENSIONS OFF)
target_compile_options(ultraterimal-control-runtime PRIVATE
    "$<$<CXX_COMPILER_ID:MSVC>:/W4;/WX;/permissive-;/utf-8;/EHsc>"
    "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall;-Wextra;-Wpedantic;-Werror>"
    "$<$<AND:$<NOT:$<CXX_COMPILER_ID:MSVC>>,$<CONFIG:Release>>:-ffunction-sections;-fdata-sections>")
target_link_libraries(ultraterimal-control-runtime PUBLIC ruvia::core ultraterimal::protocol)
add_library(ultraterimal-desktop-runtime STATIC features/desktop/desktop.runtime.cpp)
target_include_directories(ultraterimal-desktop-runtime PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(ultraterimal-desktop-runtime PUBLIC cxx_std_23)
set_target_properties(ultraterimal-desktop-runtime PROPERTIES CXX_EXTENSIONS OFF)
target_compile_options(ultraterimal-desktop-runtime PRIVATE
    "$<$<CXX_COMPILER_ID:MSVC>:/W4;/WX;/permissive-;/utf-8;/EHsc>"
    "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall;-Wextra;-Wpedantic;-Werror>"
    "$<$<AND:$<NOT:$<CXX_COMPILER_ID:MSVC>>,$<CONFIG:Release>>:-ffunction-sections;-fdata-sections>")
target_link_libraries(ultraterimal-desktop-runtime PUBLIC ruvia::core ultraterimal::protocol PRIVATE spdlog::spdlog)
add_executable(ultraterimal-server main.cpp
    config/server.runtime.cpp
    features/native/native.runtime.cpp
    features/native/stream.transport.cpp
    features/native/quic.transport.cpp
    config/gateway.runtime.cpp
    features/status/status.runtime.cpp
    features/status/status.controller.cpp
    modules/telegram/telegram.controller.cpp
    modules/identity/identity.controller.cpp
    modules/devices/device.controller.cpp
    modules/sessions/session.controller.cpp
    features/desktop/desktop.controller.cpp
    features/file/file.runtime.cpp
    features/file/file.controller.cpp)
target_compile_features(ultraterimal-server PRIVATE cxx_std_23)
target_include_directories(ultraterimal-server PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
set_target_properties(ultraterimal-server PROPERTIES CXX_EXTENSIONS OFF OUTPUT_NAME gateway)
target_compile_options(ultraterimal-server PRIVATE
    "$<$<CXX_COMPILER_ID:MSVC>:/W4;/WX;/permissive-;/utf-8;/EHsc;/bigobj>"
    "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall;-Wextra;-Wpedantic;-Werror>"
    "$<$<AND:$<NOT:$<CXX_COMPILER_ID:MSVC>>,$<CONFIG:Release>>:-ffunction-sections;-fdata-sections>")
target_link_options(ultraterimal-server PRIVATE
    "$<$<AND:$<NOT:$<CXX_COMPILER_ID:MSVC>>,$<CONFIG:Release>>:-Wl,--gc-sections,--icf=safe,-s>")
target_link_libraries(ultraterimal-server PRIVATE ultraterimal-server-core ultraterimal-control-runtime ultraterimal-desktop-runtime ruvia::web spdlog::spdlog)

install(TARGETS ultraterimal-server RUNTIME DESTINATION bin)
