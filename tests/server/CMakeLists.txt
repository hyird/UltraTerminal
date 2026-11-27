add_executable(server-control-tests control.cpp)
target_link_libraries(server-control-tests PRIVATE ultraterimal-control-runtime)
target_compile_options(server-control-tests PRIVATE
    "$<$<CXX_COMPILER_ID:MSVC>:/W4;/WX;/permissive-;/utf-8;/EHsc>"
    "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall;-Wextra;-Wpedantic;-Werror>")
add_test(NAME server.control COMMAND server-control-tests)
set_tests_properties(server.control PROPERTIES TIMEOUT 15)
add_executable(server-desktop-tests desktop.cpp)
target_link_libraries(server-desktop-tests PRIVATE ultraterimal-desktop-runtime ruvia::web)
target_compile_options(server-desktop-tests PRIVATE
    "$<$<CXX_COMPILER_ID:MSVC>:/W4;/WX;/permissive-;/utf-8;/EHsc>"
    "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall;-Wextra;-Wpedantic;-Werror>")
add_test(NAME server.desktop COMMAND server-desktop-tests)
set_tests_properties(server.desktop PROPERTIES TIMEOUT 15)
add_executable(server-desktop-diagnostics-tests desktop_diagnostics.cpp)
target_link_libraries(server-desktop-diagnostics-tests PRIVATE ultraterimal-desktop-runtime)
target_compile_options(server-desktop-diagnostics-tests PRIVATE
    "$<$<CXX_COMPILER_ID:MSVC>:/W4;/WX;/permissive-;/utf-8;/EHsc>"
    "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall;-Wextra;-Wpedantic;-Werror>")
add_test(NAME server.desktop_diagnostics COMMAND server-desktop-diagnostics-tests)
set_tests_properties(server.desktop_diagnostics PROPERTIES TIMEOUT 15)
set_tests_properties(server.control server.desktop server.desktop_diagnostics PROPERTIES LABELS unit)
add_executable(server-security-tests security.cpp)
target_link_libraries(server-security-tests PRIVATE ultraterimal-server-core)
add_test(NAME server.security COMMAND server-security-tests)
set_tests_properties(server.security PROPERTIES LABELS unit TIMEOUT 15)
# Manual only: requires a disposable MariaDB database, never run by CI.
add_executable(server-access-integration EXCLUDE_FROM_ALL access_integration.cpp)
target_link_libraries(server-access-integration PRIVATE ultraterimal-server-core)

# In-memory HTTP contract test: no sockets, database, or external services.
add_executable(server-http-tests http.cpp)
target_link_libraries(server-http-tests PRIVATE ultraterimal-server-core ruvia::web)
add_test(NAME server.http COMMAND server-http-tests)
set_tests_properties(server.http PROPERTIES LABELS unit TIMEOUT 15)

add_executable(server-terminal-tests terminal.cpp)
target_link_libraries(server-terminal-tests PRIVATE ultraterimal-terminal-runtime ultraterimal-server-core ruvia::web)
target_compile_options(server-terminal-tests PRIVATE
    "$<$<CXX_COMPILER_ID:MSVC>:/W4;/WX;/permissive-;/utf-8;/EHsc>"
    "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall;-Wextra;-Wpedantic;-Werror>")
add_test(NAME server.terminal COMMAND server-terminal-tests)
set_tests_properties(server.terminal PROPERTIES LABELS unit TIMEOUT 15)
