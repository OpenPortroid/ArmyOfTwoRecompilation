# Downloads and extracts the ReXGlue SDK release archive for the current
# platform into rexglue/<platform>. Idempotent: if rexglue/<platform> already
# exists, nothing is downloaded or extracted.
#
# Usage:
#   cmake -DREXSDK_VERSION=<ver> -DREXSDK_PLATFORM=<platform> -P fetch-rexglue-sdk.cmake
#
# REXSDK_PLATFORM must be one of: win-amd64, linux-amd64, linux-arm64,
# mac-amd64, mac-arm64. (No win-arm64 release is published.)

if(NOT DEFINED REXSDK_VERSION)
    message(FATAL_ERROR "REXSDK_VERSION must be passed via -DREXSDK_VERSION=<version>")
endif()
if(NOT DEFINED REXSDK_PLATFORM)
    message(FATAL_ERROR "REXSDK_PLATFORM must be passed via -DREXSDK_PLATFORM=<platform>")
endif()

set(REXSDK_ROOT "${CMAKE_CURRENT_LIST_DIR}/../rexglue")
if(NOT EXISTS "${REXSDK_ROOT}")
    file(MAKE_DIRECTORY "${REXSDK_ROOT}")
endif()

set(REXSDK_DIR "${REXSDK_ROOT}/${REXSDK_PLATFORM}")

if(EXISTS "${REXSDK_DIR}")
    message(STATUS "[fetch-rexglue-sdk] ${REXSDK_PLATFORM} already present at ${REXSDK_DIR}; skipping")
    return()
endif()

set(REXSDK_ZIP_NAME "rexglue-sdk-${REXSDK_VERSION}-${REXSDK_PLATFORM}.zip")
set(REXSDK_ZIP_URL "https://github.com/rexglue/rexglue-sdk/releases/download/nightly-20260915-1406e1b7/${REXSDK_ZIP_NAME}")
set(REXSDK_ZIP_PATH "${REXSDK_ROOT}/${REXSDK_ZIP_NAME}")

message(STATUS "[fetch-rexglue-sdk] Downloading ${REXSDK_ZIP_URL}")
file(DOWNLOAD "${REXSDK_ZIP_URL}" "${REXSDK_ZIP_PATH}"
    STATUS download_status)
list(GET download_status 0 result)
list(GET download_status 1 error_message)
if(NOT result EQUAL 0)
    file(REMOVE "${REXSDK_ZIP_PATH}")
    message(FATAL_ERROR "[fetch-rexglue-sdk] download failed: ${error_message}")
endif()

execute_process(
    COMMAND ${CMAKE_COMMAND} -E tar xzf "${REXSDK_ZIP_PATH}"
    WORKING_DIRECTORY "${REXSDK_ROOT}"
    RESULT_VARIABLE extract_result
    ERROR_VARIABLE extract_error
)
file(REMOVE "${REXSDK_ZIP_PATH}")
if(NOT extract_result EQUAL 0)
    message(FATAL_ERROR "[fetch-rexglue-sdk] extraction failed: ${extract_error}")
endif()

message(STATUS "[fetch-rexglue-sdk] Extracted ${REXSDK_PLATFORM} SDK to ${REXSDK_DIR}")
