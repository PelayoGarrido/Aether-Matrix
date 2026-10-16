find_package(Git REQUIRED)
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${ROOT}" rev-parse HEAD
    OUTPUT_VARIABLE revision OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
if(NOT revision STREQUAL "ea77444750de45e036e0279bb0291144bf8e3688")
    message(FATAL_ERROR "JUCE source does not match the pinned security release commit")
endif()
