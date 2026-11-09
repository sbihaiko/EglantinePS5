cmake_minimum_required(VERSION 3.24)
project(eglantine_nx2_core_check LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(FetchContent)
file(READ "${EDEN_SOURCE}/cpmfile.json" packages)
string(JSON catch_version GET "${packages}" catch2 version)
string(JSON catch_hash GET "${packages}" catch2 hash)
set(CATCH_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(CATCH_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
set(CATCH_INSTALL_EXTRAS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(Catch2
    URL "https://github.com/catchorg/Catch2/archive/refs/tags/${catch_version}.tar.gz"
    URL_HASH "SHA512=${catch_hash}")
FetchContent_MakeAvailable(Catch2)
file(GLOB ampere_tests CONFIGURE_DEPENDS "${EDEN_SOURCE}/src/tests/video_core/ampere_*.cpp")
# These two integration tests link the complete shader compiler and run in upstream tests.
# This host check exercises the header-only native capture/lowering and memory helpers.
list(FILTER ampere_tests EXCLUDE REGEX "ampere_(bindless_tex|sured|geometry)\\.cpp$")
add_executable(nx2-core-check ${ampere_tests}
    logging.cpp
    "${EDEN_SOURCE}/src/tests/common/host_command_completion.cpp"
    "${EDEN_SOURCE}/src/tests/common/host_command_queue.cpp"
    "${EDEN_SOURCE}/src/tests/core/read_traps.cpp"
    "${EDEN_SOURCE}/src/tests/core/nx2_readback_cache.cpp"
    "${EDEN_SOURCE}/src/tests/core/nx2_core_readback_capacity.cpp"
    "${EDEN_SOURCE}/src/tests/video_core/nx2_gpu_mappings.cpp"
    "${EDEN_SOURCE}/src/tests/video_core/nx2_command_ranges.cpp"
    "${EDEN_SOURCE}/src/tests/video_core/nx2_command_sync.cpp"
    "${EDEN_SOURCE}/src/tests/video_core/nx2_image_written_pages.cpp"
    "${EDEN_SOURCE}/src/tests/video_core/nx2_sparse_region.cpp"
    "${EDEN_SOURCE}/src/tests/video_core/nx2_sparse_tiles.cpp"
    "${EDEN_SOURCE}/src/tests/video_core/nx2_sparse_images.cpp"
    "${EDEN_SOURCE}/src/tests/video_core/nx2_sparse_image_write.cpp"
    "${EDEN_SOURCE}/src/tests/video_core/memory_budget.cpp"
    "${EDEN_SOURCE}/src/tests/video_core/query_region_visit.cpp")
file(GLOB boost_includes "${DEPENDENCY_SOURCE}/.cache/cpm/boost/*/libs/*/include")
# Eden's pinned Vulkan headers (sparse_image_policy.h), not whatever the host has installed.
file(GLOB vulkan_includes "${DEPENDENCY_SOURCE}/.cache/cpm/vulkanheaders/*/include")
if(NOT vulkan_includes)
    message(FATAL_ERROR "Eden's Vulkan headers are not in ${DEPENDENCY_SOURCE}/.cache/cpm/vulkanheaders")
endif()
target_include_directories(nx2-core-check PRIVATE "${EDEN_SOURCE}/src" "${FMT_INCLUDE}" ${boost_includes}
    ${vulkan_includes})
target_link_libraries(nx2-core-check PRIVATE Catch2::Catch2WithMain)
