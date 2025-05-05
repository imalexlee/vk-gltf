cmake_minimum_required(VERSION 3.29)
include(FetchContent)
project(vk_gltf)

find_package(Vulkan REQUIRED)

set(CMAKE_CXX_STANDARD 20)


add_library(vk-gltf STATIC src/loader.cpp)

option(VK_GLTF_USE_VOLK_OPT "Whether vk_gltf should use volk function definitions over vulkan.h" OFF)
option(VK_GLTF_BUILD_TEST_VIEWER_OPT "Whether vk_gltf should build the test gltf viewer exe" OFF)


set(KTX_FEATURE_TESTS OFF CACHE BOOL "Disable KTX tests" FORCE)
set(KTX_FEATURE_TOOLS OFF CACHE BOOL "Disable KTX tools" FORCE)
set(KTX_FEATURE_TOOLS_CTS OFF CACHE BOOL "Disable KTX CTS tests" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build static libraries" FORCE)
FetchContent_Declare(
        ktx
        GIT_REPOSITORY https://github.com/KhronosGroup/KTX-Software.git
        GIT_TAG beef80159525d9fb7abb8645ea85f4c4f6842e8f
        GIT_SUBMODULES ""
)

FetchContent_Declare(
        vk-lib
        GIT_REPOSITORY https://github.com/imalexlee/vk-lib.git
        GIT_TAG 1d03e2bc32f51d072856f71c59ff88b62d4fa5bd
)

if (VK_GLTF_BUILD_TEST_VIEWER_OPT)
    add_subdirectory(test)
    set(VK_GLTF_USE_VOLK_OPT ON)
endif ()

if (VK_GLTF_USE_VOLK_OPT)
    FetchContent_Declare(
            volk
            URL https://github.com/zeux/volk/archive/refs/tags/1.4.304.zip
    )
    FetchContent_MakeAvailable(volk)
    target_compile_definitions(vk-gltf PUBLIC VK_GLTF_USE_VOLK)
    target_link_libraries(vk-gltf PUBLIC volk)
endif ()

FetchContent_MakeAvailable(vk-lib ktx)

target_link_libraries(vk-gltf PUBLIC vk-lib ktx)

target_include_directories(vk-gltf PUBLIC include vendor)

