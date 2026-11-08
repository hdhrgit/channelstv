cmake_minimum_required(VERSION 3.16)
# release builds pass -DPROJECT_VER=<the tag without its v>
if(NOT DEFINED PROJECT_VER)
    set(PROJECT_VER "0.4.0")
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(channelstv)
