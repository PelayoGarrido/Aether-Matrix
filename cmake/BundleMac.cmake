if(NOT EXISTS "${BUNDLE}/Contents/MacOS")
    message(FATAL_ERROR "Missing plugin bundle: ${BUNDLE}")
endif()
file(MAKE_DIRECTORY "${BUNDLE}/Contents/Resources/licenses")
file(COPY "${MODEL}" DESTINATION "${BUNDLE}/Contents/Resources")
file(COPY "${SOURCE_DIR}/docs/licenses/" DESTINATION "${BUNDLE}/Contents/Resources/licenses")
file(COPY "${SOURCE_DIR}/LICENSE" "${SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
    DESTINATION "${BUNDLE}/Contents/Resources/licenses")
if(ENABLE_ONNX)
    file(MAKE_DIRECTORY "${BUNDLE}/Contents/Frameworks")
    set(library "${BUNDLE}/Contents/Frameworks/libonnxruntime.dylib")
    file(COPY_FILE "${ORT_ROOT}/lib/libonnxruntime.1.31.0.dylib" "${library}" ONLY_IF_DIFFERENT)
    # Absolute runtime loading does not require rewriting the upstream install name.
    execute_process(COMMAND /usr/bin/codesign --force --sign - "${library}"
        COMMAND_ERROR_IS_FATAL ANY)
    file(COPY "${ORT_ROOT}/LICENSE" "${ORT_ROOT}/ThirdPartyNotices.txt"
        DESTINATION "${BUNDLE}/Contents/Resources/licenses")
endif()
execute_process(COMMAND /usr/bin/codesign --force --deep --sign - "${BUNDLE}"
    COMMAND_ERROR_IS_FATAL ANY)
