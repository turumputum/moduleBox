# This script runs at BUILD time (not configure time),
# so CMakeCache.txt already contains the complete MODULE_FUNCTIONS list.
#
# Usage: cmake -DCACHE_FILE=<path_to_CMakeCache.txt> -DOUTPUT_FILE=<path_to_output.h> -P gen_manifest_modules.cmake

# Read CMakeCache.txt
file(READ "${CACHE_FILE}" CACHE_CONTENTS)

# Extract MODULE_FUNCTIONS value
string(REGEX MATCH "MODULE_FUNCTIONS:INTERNAL=(.*)" _match "${CACHE_CONTENTS}")
set(MF "${CMAKE_MATCH_1}")

# Extract MODULE_FUNCTIONS_DEFS value  
string(REGEX MATCH "MODULE_FUNCTIONS_DEFS:INTERNAL=(.*)" _match "${CACHE_CONTENTS}")
set(MFD "${CMAKE_MATCH_1}")

# Strip leading/trailing whitespace and trailing single quotes
string(STRIP "${MF}" MF)
string(REGEX REPLACE "^'" "" MF "${MF}")
string(REGEX REPLACE "'$" "" MF "${MF}")
string(STRIP "${MF}" MF)

string(STRIP "${MFD}" MFD)
string(REGEX REPLACE "^'" "" MFD "${MFD}")
string(REGEX REPLACE "'$" "" MFD "${MFD}")
string(STRIP "${MFD}" MFD)

# Generate header content
set(HEADER_CONTENT "#pragma once\n// Auto-generated at build time. Do not edit.\n#define MODULE_FUNCTIONS ${MF}\n#define MODULE_FUNCTIONS_DEFS ${MFD}\n")

# Only write if content changed (avoid unnecessary rebuilds)
if(EXISTS "${OUTPUT_FILE}")
    file(READ "${OUTPUT_FILE}" OLD_CONTENT)
    if("${OLD_CONTENT}" STREQUAL "${HEADER_CONTENT}")
        return()
    endif()
endif()

file(WRITE "${OUTPUT_FILE}" "${HEADER_CONTENT}")
message(STATUS "Generated ${OUTPUT_FILE}")