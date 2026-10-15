cmake_minimum_required(VERSION 3.20)
project(vkref LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)

# Compiler flags
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    set(CXX_FLAGS -Wshadow -Wall -Wextra -Wno-comment -Wno-missing-field-initializers -Wno-empty-body -fvisibility=hidden -msse3)
    if (${CMAKE_CXX_COMPILER_ID} MATCHES "Clang")
        set(CXX_FLAGS ${CXX_FLAGS} -Wno-backslash-newline-escape)
    endif()
endif()

# PyroWave sources (only the ones we have in pwd3d11-ref)
set(PYROWAVE_SOURCES
    pyrowave_config.hpp
    shaders/slangmosh.hpp
    pyrowave_encoder.hpp pyrowave_encoder.cpp
    pyrowave_decoder.hpp pyrowave_decoder.cpp
    pyrowave_common.hpp pyrowave_common.cpp
    pyrowave_c.cpp
    yuv4mpeg.cpp yuv4mpeg.hpp
)

# Build as static library
add_library(pyrowave STATIC ${PYROWAVE_SOURCES})
target_include_directories(pyrowave PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(pyrowave PRIVATE ${CXX_FLAGS})
set_target_properties(pyrowave PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_definitions(pyrowave PRIVATE PYROWAVE_PRECISION=1)

# Driver executable
add_executable(vkref_driver vkref_driver.cpp)
target_link_libraries(vkref_driver PRIVATE pyrowave)
target_compile_options(vkref_driver PRIVATE ${CXX_FLAGS})

# Find Vulkan
find_package(Vulkan REQUIRED)
target_link_libraries(vkref_driver PRIVATE ${Vulkan_LIBRARIES})
