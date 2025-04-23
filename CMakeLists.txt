cmake_minimum_required(VERSION 3.29)
include(FetchContent)
project(vk_gltf)

set(CMAKE_CXX_STANDARD 20)

add_library(vk-gltf STATIC src/loader.cpp)

if (NOT TARGET ktx)
    set(KTX_FEATURE_TESTS OFF CACHE BOOL "Disable KTX tests" FORCE)
    set(KTX_FEATURE_TOOLS OFF CACHE BOOL "Disable KTX tools" FORCE)
    set(KTX_FEATURE_TOOLS_CTS OFF CACHE BOOL "Disable KTX CTS tests" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build static libraries" FORCE)
    FetchContent_Declare(
            ktx-beef
            GIT_REPOSITORY https://github.com/KhronosGroup/KTX-Software.git
            GIT_TAG beef80159525d9fb7abb8645ea85f4c4f6842e8f
            GIT_SUBMODULES ""
    )
    FetchContent_MakeAvailable(ktx-beef) # lol beef
    target_link_libraries(vk-gltf PRIVATE ktx)
endif ()

if (NOT TARGET vk-lib)
    FetchContent_Declare(
            vk-lib-8d70
            GIT_REPOSITORY https://github.com/imalexlee/vk-lib.git
            GIT_TAG 8d7030ee258442d974f3d9905f8a919b68726712
    )

    FetchContent_MakeAvailable(vk-lib-8d70)

    target_link_libraries(vk-gltf PRIVATE vk-lib)
endif ()

target_include_directories(vk-gltf PRIVATE vendor)

target_include_directories(vk-gltf PUBLIC include)
