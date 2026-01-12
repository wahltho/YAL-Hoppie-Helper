cmake_minimum_required(VERSION 3.15)
project(YAL_hoppiehelper)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(XPLANE_SDK_PATH "" CACHE PATH "Path to X-Plane SDK root")
if (NOT XPLANE_SDK_PATH OR NOT EXISTS "${XPLANE_SDK_PATH}/CHeaders/XPLM/XPLMPlugin.h")
    message(FATAL_ERROR "Set XPLANE_SDK_PATH to the X-Plane SDK root (contains CHeaders).")
endif()

set(IS_WINDOWS_BUILD OFF)
if (WIN32 OR MINGW OR (CMAKE_SYSTEM_NAME STREQUAL "Windows") OR (CMAKE_C_COMPILER MATCHES "mingw"))
    set(IS_WINDOWS_BUILD ON)
endif()

option(YAL_USE_CURL "Use libcurl for HTTP (non-Windows)" ON)
if (IS_WINDOWS_BUILD)
    set(YAL_USE_CURL OFF)
endif()

if (YAL_USE_CURL)
    find_package(CURL REQUIRED)
endif()

add_library(HoppieHelper MODULE src/YAL_hoppiehelper.cpp)

target_include_directories(HoppieHelper PRIVATE
    "${XPLANE_SDK_PATH}/CHeaders"
    "${XPLANE_SDK_PATH}/CHeaders/XPLM"
    "${XPLANE_SDK_PATH}/CHeaders/Widgets"
)

if(APPLE)
    target_compile_definitions(HoppieHelper PRIVATE APL=1 IBM=0 LIN=0)
    target_link_options(HoppieHelper PRIVATE "-F${XPLANE_SDK_PATH}/Libraries/Mac")
    target_link_libraries(HoppieHelper PRIVATE "-framework XPLM" "-framework XPWidgets")
elseif(IS_WINDOWS_BUILD)
    target_compile_definitions(HoppieHelper PRIVATE APL=0 IBM=1 LIN=0)
    target_link_libraries(HoppieHelper PRIVATE
        "${XPLANE_SDK_PATH}/Libraries/Win/XPLM_64.lib"
        "${XPLANE_SDK_PATH}/Libraries/Win/XPWidgets_64.lib"
        ws2_32
        winhttp
    )
    if(MINGW OR (CMAKE_C_COMPILER MATCHES "mingw"))
        target_compile_options(HoppieHelper PRIVATE "-mthreads")
        target_link_libraries(HoppieHelper PRIVATE winpthread)
        target_link_options(HoppieHelper PRIVATE "-mthreads" "-static" "-static-libgcc" "-static-libstdc++")
    endif()
else()
    target_compile_definitions(HoppieHelper PRIVATE APL=0 IBM=0 LIN=1)
    target_link_libraries(HoppieHelper PRIVATE
        "${XPLANE_SDK_PATH}/Libraries/Lin/XPLM_64.so"
        "${XPLANE_SDK_PATH}/Libraries/Lin/XPWidgets_64.so"
    )
endif()

target_compile_definitions(HoppieHelper PRIVATE XPLM200=1 XPLM210=1 XPLM300=1 XPLM301=1 XPLM303=1 XPLM400=1)

if (YAL_USE_CURL)
    target_link_libraries(HoppieHelper PRIVATE CURL::libcurl)
endif()

set_target_properties(HoppieHelper PROPERTIES
    PREFIX ""
    SUFFIX ".xpl"
)

if (IS_WINDOWS_BUILD)
    set_target_properties(HoppieHelper PROPERTIES OUTPUT_NAME "win")
elseif (APPLE)
    set_target_properties(HoppieHelper PROPERTIES OUTPUT_NAME "mac")
else()
    set_target_properties(HoppieHelper PROPERTIES OUTPUT_NAME "lin")
endif()
