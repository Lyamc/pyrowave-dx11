cmake_minimum_required(VERSION 3.15)
project(pyrowave_d3d11 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# MSVC, static CRT (/MT), warnings as errors
if(MSVC)
    add_compile_options(/O2 /EHsc /W4 /WX /MT /permissive- /DUNICODE /D_UNICODE)
    add_link_options(/INCREMENTAL:NO)
endif()

# ------------------------------------------------------------------
# fxc build step: compile placeholder shader to prove the pipeline
# ------------------------------------------------------------------
set(SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
set(SHADER_OUT "${CMAKE_CURRENT_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${SHADER_OUT}")

# Precompile shaders (cs_5_0, /O3 /WX /Ges /Gis)
# /Fh outputs a C header with the bytecode as a byte array.
# We then add a size constant using a Python post-processor.
# ------------------------------------------------------------------
set(EMBED_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/../tools/embed_shader.py")

function(precompile_shader name src_file)
    set(SRC "${SHADER_DIR}/${src_file}")
    set(Hdr "${SHADER_OUT}/${name}_dxbc.h")

    add_custom_command(
        OUTPUT "${Hdr}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${SHADER_OUT}"
        COMMAND fxc /nologo /T cs_5_0 /E main /O3 /WX /Ges /Gis
                /Fh "${Hdr}" /Vn g_${name}_dxbc "${SRC}"
        COMMAND "C:\\Users\\Lyam\\AppData\\Local\\Programs\\Python\\Python313\\python.exe"
                "${EMBED_SCRIPT}" "${Hdr}" "g_${name}_dxbc"
        DEPENDS "${SRC}" "${EMBED_SCRIPT}"
        COMMENT "[fxc] ${src_file} -> ${name}_dxbc.h"
        VERBATIM
    )

    set(${name}_Hdr "${Hdr}" PARENT_SCOPE)
endfunction()

precompile_shader(dwt dwt.hlsl)
precompile_shader(probe probe.hlsl)

add_custom_target(shaders ALL DEPENDS "${dwt_Hdr}" "${probe_Hdr}")

# ------------------------------------------------------------------
# Library: pyrowave_d3d11.dll
# ------------------------------------------------------------------
add_library(pyrowave_d3d11 SHARED
    device.cpp
    c_api.cpp
    ${dwt_Hdr}
    ${probe_Hdr}
)
target_include_directories(pyrowave_d3d11 PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_BINARY_DIR}"
)
target_link_libraries(pyrowave_d3d11 PRIVATE d3d11 dxgi)
set_target_properties(pyrowave_d3d11 PROPERTIES
    OUTPUT_NAME "pyrowave_d3d11"
    SUFFIX ".dll"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)

# ------------------------------------------------------------------
# Static lib for tests (no DLL export)
# ------------------------------------------------------------------
add_library(pyrowave_d3d11_static STATIC
    device.cpp
    c_api.cpp
    ${dwt_Hdr}
    ${probe_Hdr}
)
target_include_directories(pyrowave_d3d11_static PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_BINARY_DIR}"
)
target_link_libraries(pyrowave_d3d11_static PRIVATE d3d11 dxgi)

# ------------------------------------------------------------------
# Tests
# ------------------------------------------------------------------
add_executable(test_t0 tests/test_t0.cpp)
target_include_directories(test_t0 PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_BINARY_DIR}"
)
target_link_libraries(test_t0 PRIVATE d3d11 dxgi d3dcompiler)
target_sources(test_t0 PRIVATE device.cpp c_api.cpp)
set_target_properties(test_t0 PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)

add_executable(test_t1 tests/test_t1.cpp)
target_include_directories(test_t1 PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_BINARY_DIR}"
)
target_link_libraries(test_t1 PRIVATE d3d11 dxgi d3dcompiler)
set_target_properties(test_t1 PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)

# ------------------------------------------------------------------
# Install
# ------------------------------------------------------------------
install(TARGETS pyrowave_d3d11
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)
install(FILES include/pyrowave_d3d11.h DESTINATION include)
