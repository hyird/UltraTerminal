find_package(Protobuf CONFIG REQUIRED)
add_library(ultraterimal-protocol STATIC src/message.protocol.cpp src/desktop.protocol.cpp src/desktop_ipc.protocol.cpp src/stream.protocol.cpp src/native.protocol.cpp)
add_library(ultraterimal::protocol ALIAS ultraterimal-protocol)
target_compile_features(ultraterimal-protocol PUBLIC cxx_std_23)
target_include_directories(ultraterimal-protocol PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_options(ultraterimal-protocol PRIVATE
    "$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Release>>:/Gy;/Gw;/GF;/GL>"
    "$<$<AND:$<NOT:$<CXX_COMPILER_ID:MSVC>>,$<CONFIG:Release>>:-ffunction-sections;-fdata-sections>")
set_target_properties(ultraterimal-protocol PROPERTIES
    STATIC_LIBRARY_OPTIONS "$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Release>>:/LTCG>")
target_link_options(ultraterimal-protocol INTERFACE "$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Release>>:/LTCG>")
target_link_libraries(ultraterimal-protocol PUBLIC protobuf::libprotobuf-lite)
protobuf_generate(TARGET ultraterimal-protocol LANGUAGE cpp PROTOS schemas/session.proto)
