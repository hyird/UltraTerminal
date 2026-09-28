find_package(ruvia CONFIG REQUIRED COMPONENTS web)
find_package(libpqxx CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(Boost CONFIG REQUIRED COMPONENTS json)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/migrations/001_identity_devices.sql" ULTRATERIMAL_INITIAL_MIGRATION)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/migrations/002_remote_sessions.sql" ULTRATERIMAL_SESSION_MIGRATION)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/migrations/003_agent_execution_account.sql" ULTRATERIMAL_ACCOUNT_MIGRATION)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/migrations/004_system_restart.sql" ULTRATERIMAL_RESTART_MIGRATION)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/migrations/005_restart_resolution.sql" ULTRATERIMAL_RESTART_RESOLUTION_MIGRATION)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/migrations/006_restart_direct.sql" ULTRATERIMAL_RESTART_DIRECT_MIGRATION)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/migrations/007_device_provisioning.sql" ULTRATERIMAL_DEVICE_PROVISIONING_MIGRATION)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS migrations/007_device_provisioning.sql)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS migrations/006_restart_direct.sql)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS migrations/003_agent_execution_account.sql migrations/004_system_restart.sql migrations/005_restart_resolution.sql)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS migrations/001_identity_devices.sql migrations/002_remote_sessions.sql)
configure_file(migrations/migration.schema.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/migration.schema.h" @ONLY)
add_library(ultraterimal-server-core STATIC
    config/server.config.cpp
    modules/database/database.service.cpp
    modules/identity/identity.service.cpp
    modules/devices/device.service.cpp
    modules/sessions/session.service.cpp)
target_include_directories(ultraterimal-server-core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_features(ultraterimal-server-core PUBLIC cxx_std_23)
set_target_properties(ultraterimal-server-core PROPERTIES CXX_EXTENSIONS OFF)
target_compile_options(ultraterimal-server-core PRIVATE
    "$<$<CXX_COMPILER_ID:MSVC>:/W4;/WX;/permissive-;/utf-8;/EHsc;/bigobj>"
    "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall;-Wextra;-Wpedantic;-Werror>")
target_link_libraries(ultraterimal-server-core PUBLIC libpqxx::pqxx ultraterimal::protocol PRIVATE Boost::json OpenSSL::Crypto)
add_library(ultraterimal-control-runtime STATIC features/control/control.runtime.cpp)
target_include_directories(ultraterimal-control-runtime PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(ultraterimal-control-runtime PUBLIC cxx_std_23)
set_target_properties(ultraterimal-control-runtime PROPERTIES CXX_EXTENSIONS OFF)
target_compile_options(ultraterimal-control-runtime PRIVATE
    "$<$<CXX_COMPILER_ID:MSVC>:/W4;/WX;/permissive-;/utf-8;/EHsc>"
    "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall;-Wextra;-Wpedantic;-Werror>")
target_link_libraries(ultraterimal-control-runtime PUBLIC ruvia::core ultraterimal::protocol)
add_library(ultraterimal-desktop-runtime STATIC features/desktop/desktop.runtime.cpp)
target_include_directories(ultraterimal-desktop-runtime PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(ultraterimal-desktop-runtime PUBLIC cxx_std_23)
set_target_properties(ultraterimal-desktop-runtime PROPERTIES CXX_EXTENSIONS OFF)
target_compile_options(ultraterimal-desktop-runtime PRIVATE
    "$<$<CXX_COMPILER_ID:MSVC>:/W4;/WX;/permissive-;/utf-8;/EHsc>"
    "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall;-Wextra;-Wpedantic;-Werror>")
target_link_libraries(ultraterimal-desktop-runtime PUBLIC ruvia::core ultraterimal::protocol PRIVATE spdlog::spdlog)
add_executable(ultraterimal-server main.cpp
    features/native/native.runtime.cpp
    features/native/stream.transport.cpp
    features/native/quic.transport.cpp
    config/gateway.runtime.cpp
    features/status/status.runtime.cpp
    modules/identity/identity.controller.cpp
    modules/devices/device.controller.cpp
    modules/sessions/session.controller.cpp
    features/desktop/desktop.controller.cpp)
target_compile_features(ultraterimal-server PRIVATE cxx_std_23)
set_target_properties(ultraterimal-server PROPERTIES CXX_EXTENSIONS OFF OUTPUT_NAME gateway)
target_compile_options(ultraterimal-server PRIVATE
    "$<$<CXX_COMPILER_ID:MSVC>:/W4;/WX;/permissive-;/utf-8;/EHsc;/bigobj>"
    "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall;-Wextra;-Wpedantic;-Werror>")
target_link_libraries(ultraterimal-server PRIVATE ultraterimal-server-core ultraterimal-control-runtime ultraterimal-desktop-runtime ruvia::web Boost::json spdlog::spdlog)

install(TARGETS ultraterimal-server RUNTIME DESTINATION bin)
