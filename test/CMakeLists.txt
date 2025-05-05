
file(GLOB_RECURSE project_sources "./*.cpp")
add_executable(test_viewer ${project_sources})

add_compile_definitions(VK_NO_PROTOTYPES)

set(GLFW_BUILD_DOCS OFF CACHE BOOL "Disable GLFW docs" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "Disable GLFW tests" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "Disable GLFW examples" FORCE)
FetchContent_Declare(
        GLFW
        URL https://github.com/glfw/glfw/releases/download/3.4/glfw-3.4.zip
)

FetchContent_Declare(
        glm
        URL https://github.com/g-truc/glm/archive/refs/tags/1.0.1.zip
)

FetchContent_Declare(
        vk-lib
        GIT_REPOSITORY https://github.com/imalexlee/vk-lib.git
        GIT_TAG 8d7030ee258442d974f3d9905f8a919b68726712
)

FetchContent_Declare(
        volk
        URL https://github.com/zeux/volk/archive/refs/tags/1.4.304.zip
)

FetchContent_MakeAvailable(vk-lib GLFW glm volk)

target_link_libraries(test_viewer PUBLIC vk-lib glfw glm::glm vk-gltf volk)

target_include_directories(test_viewer PUBLIC test)