cmake_minimum_required(VERSION 3.15)
project(YAL_hoppiehelper)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(XPLANE_SDK_PATH "" CACHE PATH "Path to X-Plane SDK root")
if (NOT XPLANE_SDK_PATH OR NOT EXISTS "${XPLANE_SDK_PATH}/CHeaders/XPLM/XPLMPlugin.h")
    message(FATAL_ERROR "Set XPLANE_SDK_PATH to the X-Plane SDK root (contains CHeaders).")
endif()

if (NOT WIN32)
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
elseif(WIN32)
    target_compile_definitions(HoppieHelper PRIVATE APL=0 IBM=1 LIN=0)
    target_link_libraries(HoppieHelper PRIVATE
        "${XPLANE_SDK_PATH}/Libraries/Win/XPLM_64.lib"
        "${XPLANE_SDK_PATH}/Libraries/Win/XPWidgets_64.lib"
        ws2_32
        winhttp
    )
    if(MINGW)
        target_link_options(HoppieHelper PRIVATE "-static" "-static-libgcc" "-static-libstdc++")
    endif()
else()
    target_compile_definitions(HoppieHelper PRIVATE APL=0 IBM=0 LIN=1)
    target_link_libraries(HoppieHelper PRIVATE
        "${XPLANE_SDK_PATH}/Libraries/Lin/XPLM_64.so"
        "${XPLANE_SDK_PATH}/Libraries/Lin/XPWidgets_64.so"
    )
endif()

target_compile_definitions(HoppieHelper PRIVATE XPLM200=1 XPLM210=1 XPLM300=1 XPLM301=1 XPLM303=1 XPLM400=1)

if (NOT WIN32)
    target_link_libraries(HoppieHelper PRIVATE CURL::libcurl)
endif()

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
