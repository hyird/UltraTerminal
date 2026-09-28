add_executable(protocol-message-tests message.cpp)
target_compile_features(protocol-message-tests PRIVATE cxx_std_23)
target_link_libraries(protocol-message-tests PRIVATE ultraterimal::protocol)
add_test(NAME protocol.message COMMAND protocol-message-tests)
add_executable(protocol-stream-tests stream.cpp)
target_compile_features(protocol-stream-tests PRIVATE cxx_std_23)
target_link_libraries(protocol-stream-tests PRIVATE ultraterimal::protocol)
add_test(NAME protocol.stream COMMAND protocol-stream-tests)
add_executable(protocol-native-tests native.cpp)
target_compile_features(protocol-native-tests PRIVATE cxx_std_23)
target_link_libraries(protocol-native-tests PRIVATE ultraterimal::protocol)
add_test(NAME protocol.native COMMAND protocol-native-tests)
add_executable(protocol-desktop-tests desktop.cpp)
target_link_libraries(protocol-desktop-tests PRIVATE ultraterimal::protocol)
target_compile_options(protocol-desktop-tests PRIVATE
    "$<$<CXX_COMPILER_ID:MSVC>:/W4;/WX;/permissive-;/utf-8;/EHsc>"
    "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall;-Wextra;-Wpedantic;-Werror>")
add_test(NAME protocol.desktop COMMAND protocol-desktop-tests)
add_executable(protocol-desktop-ipc-tests desktop_ipc.cpp)
target_link_libraries(protocol-desktop-ipc-tests PRIVATE ultraterimal::protocol)
target_compile_options(protocol-desktop-ipc-tests PRIVATE
    "$<$<CXX_COMPILER_ID:MSVC>:/W4;/WX;/permissive-;/utf-8;/EHsc>"
    "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall;-Wextra;-Wpedantic;-Werror>")
add_test(NAME protocol.desktop_ipc COMMAND protocol-desktop-ipc-tests)

set_tests_properties(protocol.message protocol.stream protocol.native protocol.desktop protocol.desktop_ipc PROPERTIES LABELS unit)
