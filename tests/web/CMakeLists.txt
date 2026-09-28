set(ULTRATERIMAL_BROWSER_EXECUTABLE "" CACHE FILEPATH "Chromium, Chrome, or Edge executable for WebCodecs integration tests")
if(ULTRATERIMAL_BROWSER_EXECUTABLE)
    find_program(ULTRATERIMAL_NODE_EXECUTABLE node REQUIRED)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/node_modules/playwright/package.json")
        message(FATAL_ERROR "Run npm ci --prefix tests/web before enabling browser tests.")
    endif()
    add_test(NAME web.software_video
        COMMAND "${ULTRATERIMAL_NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/software_video.cjs"
        "$<TARGET_FILE:agent-software-video-tests>" "${CMAKE_CURRENT_BINARY_DIR}/video-fixtures"
        "${ULTRATERIMAL_BROWSER_EXECUTABLE}")
    set_tests_properties(web.software_video PROPERTIES TIMEOUT 45 LABELS browser)
    add_test(NAME web.hardware_video
        COMMAND "${ULTRATERIMAL_NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/software_video.cjs"
        "$<TARGET_FILE:agent-software-video-tests>" "${CMAKE_CURRENT_BINARY_DIR}/hardware-video-fixtures"
        "${ULTRATERIMAL_BROWSER_EXECUTABLE}" --automatic)
    set_tests_properties(web.hardware_video PROPERTIES TIMEOUT 45 LABELS browser)
    add_test(NAME web.backpressure COMMAND "${ULTRATERIMAL_NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/backpressure.cjs" "${ULTRATERIMAL_BROWSER_EXECUTABLE}")
    set_tests_properties(web.backpressure PROPERTIES TIMEOUT 30 LABELS browser)
    add_test(NAME web.desktop_network COMMAND "${ULTRATERIMAL_NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/desktop_network.cjs")
    set_tests_properties(web.desktop_network PROPERTIES TIMEOUT 15 LABELS integration)
    add_test(NAME web.outbound COMMAND "${ULTRATERIMAL_NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/outbound.cjs" "${ULTRATERIMAL_BROWSER_EXECUTABLE}")
    set_tests_properties(web.outbound PROPERTIES TIMEOUT 30 LABELS browser)
    add_test(NAME web.workspace COMMAND "${ULTRATERIMAL_NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/workspace.cjs" "${ULTRATERIMAL_BROWSER_EXECUTABLE}")
    set_tests_properties(web.workspace PROPERTIES TIMEOUT 30 LABELS browser)
    add_test(NAME web.restart COMMAND "${ULTRATERIMAL_NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/restart.cjs" "${ULTRATERIMAL_BROWSER_EXECUTABLE}")
    set_tests_properties(web.restart PROPERTIES TIMEOUT 30 LABELS browser)
    add_test(NAME web.desktop_probe COMMAND "${ULTRATERIMAL_NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/desktop_probe.cjs" "${ULTRATERIMAL_BROWSER_EXECUTABLE}")
    set_tests_properties(web.desktop_probe PROPERTIES TIMEOUT 45 LABELS browser)
    add_test(NAME web.desktop_pixels COMMAND "${ULTRATERIMAL_NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/desktop_pixels.cjs" "${ULTRATERIMAL_BROWSER_EXECUTABLE}")
    set_tests_properties(web.desktop_pixels PROPERTIES TIMEOUT 30 LABELS browser)
    add_test(NAME web.desktop_bitrate COMMAND "${ULTRATERIMAL_NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/desktop_bitrate.cjs" "${ULTRATERIMAL_BROWSER_EXECUTABLE}")
    set_tests_properties(web.desktop_bitrate PROPERTIES TIMEOUT 30 LABELS browser)
endif()
