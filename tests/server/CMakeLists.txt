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
add_executable(server-desktop-diagnostics-tests desktop_diagnostics.cpp)
target_link_libraries(server-desktop-diagnostics-tests PRIVATE ultraterimal-desktop-runtime)
target_compile_options(server-desktop-diagnostics-tests PRIVATE
    "$<$<CXX_COMPILER_ID:MSVC>:/W4;/WX;/permissive-;/utf-8;/EHsc>"
    "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall;-Wextra;-Wpedantic;-Werror>")
add_test(NAME server.desktop_diagnostics COMMAND server-desktop-diagnostics-tests)
set_tests_properties(server.desktop_diagnostics PROPERTIES TIMEOUT 15)
set_tests_properties(server.control server.desktop server.desktop_diagnostics PROPERTIES LABELS unit)
