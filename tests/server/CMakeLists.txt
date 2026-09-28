set(ULTRATERIMAL_POSTGRES_BIN "" CACHE PATH "PostgreSQL bin directory for isolated gateway integration tests")
add_executable(server-control-tests control.cpp)
target_link_libraries(server-control-tests PRIVATE ultraterimal-control-runtime)
target_compile_options(server-control-tests PRIVATE
    "$<$<CXX_COMPILER_ID:MSVC>:/W4;/WX;/permissive-;/utf-8;/EHsc>"
    "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall;-Wextra;-Wpedantic;-Werror>")
add_test(NAME server.control COMMAND server-control-tests)
set_tests_properties(server.control PROPERTIES TIMEOUT 15)
add_executable(server-desktop-tests desktop.cpp)
target_link_libraries(server-desktop-tests PRIVATE ultraterimal-desktop-runtime)
target_compile_options(server-desktop-tests PRIVATE
    "$<$<CXX_COMPILER_ID:MSVC>:/W4;/WX;/permissive-;/utf-8;/EHsc>"
    "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall;-Wextra;-Wpedantic;-Werror>")
add_test(NAME server.desktop COMMAND server-desktop-tests)
set_tests_properties(server.desktop PROPERTIES TIMEOUT 15)
if(ULTRATERIMAL_BUILD_AGENT)
    find_package(Boost CONFIG REQUIRED COMPONENTS json)
    find_package(spdlog CONFIG REQUIRED)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_executable(server-shutdown-tests shutdown.cpp
        "${PROJECT_SOURCE_DIR}/server/features/status/status.runtime.cpp"
        "${PROJECT_SOURCE_DIR}/server/features/native/native.runtime.cpp"
        "${PROJECT_SOURCE_DIR}/server/features/native/quic.transport.cpp"
        "${PROJECT_SOURCE_DIR}/server/features/native/stream.transport.cpp")
    target_link_libraries(server-shutdown-tests PRIVATE ultraterimal-server-core ultraterimal-control-runtime ultraterimal-desktop-runtime ultraterimal-agent-core ruvia::web Boost::json spdlog::spdlog)
    target_compile_features(server-shutdown-tests PRIVATE cxx_std_23)
    target_compile_options(server-shutdown-tests PRIVATE
        "$<$<CXX_COMPILER_ID:MSVC>:/W4;/WX;/permissive-;/utf-8;/EHsc;/bigobj>"
        "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall;-Wextra;-Wpedantic;-Werror>")
    add_test(NAME server.shutdown COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/shutdown_fixture.py" "$<TARGET_FILE:server-shutdown-tests>")
    set_tests_properties(server.shutdown PROPERTIES TIMEOUT 20)
endif()
if(ULTRATERIMAL_POSTGRES_BIN AND ULTRATERIMAL_BUILD_AGENT)
    add_executable(server-session-tests session.cpp)
    target_link_libraries(server-session-tests PRIVATE ultraterimal-server-core)
    target_compile_options(server-session-tests PRIVATE
        "$<$<CXX_COMPILER_ID:MSVC>:/W4;/WX;/permissive-;/utf-8;/EHsc>"
        "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall;-Wextra;-Wpedantic;-Werror>")
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME server.registration COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/registration_fixture.py"
        "$<TARGET_FILE:ultraterimal-server>" "$<TARGET_FILE:ultraterimal-agent>"
        "${ULTRATERIMAL_POSTGRES_BIN}" "${CMAKE_CURRENT_BINARY_DIR}/fixtures" "$<TARGET_FILE:server-session-tests>" "$<TARGET_FILE:agent-desktop-tests>" "$<TARGET_FILE:agent-native-tests>")
    set_tests_properties(server.registration PROPERTIES TIMEOUT 180 LABELS "postgresql;integration")
    if(ULTRATERIMAL_BROWSER_EXECUTABLE)
        set(DESKTOP_FIXTURE_COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/registration_fixture.py"
            "$<TARGET_FILE:ultraterimal-server>" "$<TARGET_FILE:ultraterimal-agent>"
            "${ULTRATERIMAL_POSTGRES_BIN}" "${CMAKE_CURRENT_BINARY_DIR}/fixtures" "$<TARGET_FILE:server-session-tests>" "$<TARGET_FILE:agent-desktop-tests>" "$<TARGET_FILE:agent-native-tests>"
            "${ULTRATERIMAL_NODE_EXECUTABLE}" "${ULTRATERIMAL_BROWSER_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/build/web" "$<TARGET_FILE:agent-input-observer>")
        add_test(NAME web.desktop COMMAND ${DESKTOP_FIXTURE_COMMAND})
        add_test(NAME web.desktop_shaped COMMAND "${CMAKE_COMMAND}" -E env "ULTRATERIMAL_DESKTOP_SHAPED=1" ${DESKTOP_FIXTURE_COMMAND})
        add_test(NAME web.desktop_synthetic COMMAND ${DESKTOP_FIXTURE_COMMAND} --synthetic)
        set_tests_properties(web.desktop web.desktop_shaped PROPERTIES TIMEOUT 240 RUN_SERIAL TRUE LABELS "browser;postgresql;interactive-desktop")
        set_tests_properties(web.desktop_shaped PROPERTIES LABELS "browser;postgresql;interactive-desktop;weak-network")
        set_tests_properties(web.desktop_synthetic PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE LABELS "browser;postgresql;weak-network;synthetic-peer")
    endif()
endif()
