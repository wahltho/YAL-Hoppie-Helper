cmake_minimum_required(VERSION 3.15)
project(YAL_hoppiehelper)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(XPLANE_SDK_PATH "" CACHE PATH "Path to X-Plane SDK root")
if (NOT XPLANE_SDK_PATH)
    message(FATAL_ERROR "Set XPLANE_SDK_PATH to the X-Plane SDK root (contains CHeaders).")
endif()

find_package(CURL REQUIRED)

add_library(HoppieHelper MODULE src/HoppieHelper.cpp)

target_include_directories(HoppieHelper PRIVATE
    ${XPLANE_SDK_PATH}/CHeaders/XPLM
    ${XPLANE_SDK_PATH}/CHeaders/Widgets
)

if (APPLE)
    target_compile_definitions(HoppieHelper PRIVATE APL=1)
elseif (WIN32)
    target_compile_definitions(HoppieHelper PRIVATE IBM=1)
else()
    target_compile_definitions(HoppieHelper PRIVATE LIN=1)
endif()

target_link_libraries(HoppieHelper PRIVATE CURL::libcurl)

set_target_properties(HoppieHelper PROPERTIES
    PREFIX ""
    SUFFIX ".xpl"
)

if (WIN32)
    set_target_properties(HoppieHelper PROPERTIES OUTPUT_NAME "win")
elseif (APPLE)
    set_target_properties(HoppieHelper PROPERTIES OUTPUT_NAME "mac")
else()
    set_target_properties(HoppieHelper PROPERTIES OUTPUT_NAME "lin")
endif()
