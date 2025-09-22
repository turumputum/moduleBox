# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
include($ENV{ADF_PATH}/CMakeLists.txt)
get_filename_component(ProjectId ${CMAKE_CURRENT_LIST_DIR} NAME)
string(REPLACE " " "_" ProjectId ${ProjectId})
project(${ProjectId})
#spiffs_create_partition_image(storage font FLASH_IN_PROJECT)

#execute_process(COMMAND sbin/wichbranch.sh OUTPUT_VARIABLE BRANCH)
#execute_process(COMMAND git branch OUTPUT_VARIABLE REPO_TAGS)
#string (REGEX MATCH "^\\*\\s+\\K\\S+$" BRANCH ${REPO_TAGS})

idf_build_set_property(COMPILE_OPTIONS "-fdiagnostics-color=always" APPEND)
idf_build_set_property(COMPILE_OPTIONS -DBRANCH="${BRANCH}" APPEND)
