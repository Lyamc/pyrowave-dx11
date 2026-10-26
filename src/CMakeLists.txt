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
find_package(Python3 COMPONENTS Interpreter REQUIRED)

function(precompile_shader name src_file)
    set(SRC "${SHADER_DIR}/${src_file}")
    set(Hdr "${SHADER_OUT}/${name}_dxbc.h")

    add_custom_command(
        OUTPUT "${Hdr}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${SHADER_OUT}"
        COMMAND fxc /nologo /T cs_5_0 /E main /O3 /Ges /Gis
                /Fh "${Hdr}" /Vn g_${name}_dxbc "${SRC}"
        COMMAND "${Python3_EXECUTABLE}"
                "${EMBED_SCRIPT}" "${Hdr}" "g_${name}_dxbc"
        DEPENDS "${SRC}" "${EMBED_SCRIPT}"
        COMMENT "[fxc] ${src_file} -> ${name}_dxbc.h"
        VERBATIM
    )

    set(${name}_Hdr "${Hdr}" PARENT_SCOPE)
endfunction()

precompile_shader(dwt dwt.hlsl)
precompile_shader(probe probe.hlsl)
precompile_shader(quant quant.hlsl)
precompile_shader(analyze analyze.hlsl)
precompile_shader(analyze_finalize analyze_finalize.hlsl)
precompile_shader(resolve resolve.hlsl)
precompile_shader(pack pack.hlsl)

add_custom_target(shaders ALL DEPENDS "${dwt_Hdr}" "${probe_Hdr}")

# ------------------------------------------------------------------
# Library: pyrowave_d3d11.dll
# ------------------------------------------------------------------
add_library(pyrowave_d3d11 SHARED
    device.cpp
    c_api.cpp
    debug_dump.cpp
    dwt.cpp
    frame.cpp
    quant.cpp
    analyze.cpp
    resolve.cpp
    ${dwt_Hdr}
    ${probe_Hdr}
    ${quant_Hdr}
    ${analyze_Hdr}
    ${analyze_finalize_Hdr}
    ${resolve_Hdr}
    ${pack_Hdr}
)
target_include_directories(pyrowave_d3d11 PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}"
)
target_compile_definitions(pyrowave_d3d11 PRIVATE PWD3D11_BUILD_DLL)
target_link_libraries(pyrowave_d3d11 PRIVATE d3d11 dxgi crypt32 d3dcompiler)
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
    debug_dump.cpp
    dwt.cpp
    frame.cpp
    quant.cpp
    analyze.cpp
    resolve.cpp
    ${dwt_Hdr}
    ${probe_Hdr}
    ${quant_Hdr}
    ${analyze_Hdr}
    ${analyze_finalize_Hdr}
    ${resolve_Hdr}
    ${pack_Hdr}
)
target_include_directories(pyrowave_d3d11_static PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}"
)
target_link_libraries(pyrowave_d3d11_static PRIVATE d3d11 dxgi crypt32 d3dcompiler)

# ------------------------------------------------------------------
# Tests
# ------------------------------------------------------------------
add_executable(test_t0 tests/test_t0.cpp)
target_include_directories(test_t0 PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_BINARY_DIR}"
)
target_link_libraries(test_t0 PRIVATE d3d11 dxgi d3dcompiler crypt32)
target_sources(test_t0 PRIVATE device.cpp c_api.cpp debug_dump.cpp)
set_target_properties(test_t0 PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)

add_executable(test_t1 tests/test_t1.cpp)
target_include_directories(test_t1 PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_BINARY_DIR}"
)
target_link_libraries(test_t1 PRIVATE d3d11 dxgi d3dcompiler crypt32)
target_sources(test_t1 PRIVATE device.cpp c_api.cpp debug_dump.cpp)
set_target_properties(test_t1 PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)

# ------------------------------------------------------------------
# T2b test: quantization
# ------------------------------------------------------------------
add_executable(test_t2_quant tests/test_t2_quant.cpp)
target_include_directories(test_t2_quant PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_BINARY_DIR}"
)
target_link_libraries(test_t2_quant PRIVATE d3d11 dxgi d3dcompiler crypt32)
target_sources(test_t2_quant PRIVATE device.cpp c_api.cpp debug_dump.cpp quant.cpp)
set_target_properties(test_t2_quant PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)

# ------------------------------------------------------------------
# T5b test: full encode pipeline
# ------------------------------------------------------------------
add_executable(test_t5b tests/test_t5b.cpp)
target_include_directories(test_t5b PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_BINARY_DIR}"
)
target_link_libraries(test_t5b PRIVATE d3d11 dxgi d3dcompiler crypt32)
target_sources(test_t5b PRIVATE device.cpp c_api.cpp debug_dump.cpp dwt.cpp frame.cpp quant.cpp analyze.cpp resolve.cpp)
add_dependencies(test_t5b pyrowave_d3d11)
set_target_properties(test_t5b PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)

# Time encode_cpu + compute_num_packets + packetize on the shipped entry points.
add_executable(bench_time tests/bench_time.cpp)
target_include_directories(bench_time PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_BINARY_DIR}"
)
target_link_libraries(bench_time PRIVATE d3d11 dxgi d3dcompiler crypt32)
target_sources(bench_time PRIVATE device.cpp c_api.cpp debug_dump.cpp dwt.cpp frame.cpp quant.cpp analyze.cpp resolve.cpp)
add_dependencies(bench_time pyrowave_d3d11)
set_target_properties(bench_time PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)

# ------------------------------------------------------------------
# T6c test: debug dump hooks + pwd3d11_debug_run_stage
# ------------------------------------------------------------------
add_executable(test_t6c tests/test_t6c.cpp)
target_include_directories(test_t6c PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_BINARY_DIR}"
)
target_link_libraries(test_t6c PRIVATE d3d11 dxgi d3dcompiler crypt32)
target_sources(test_t6c PRIVATE device.cpp c_api.cpp debug_dump.cpp dwt.cpp frame.cpp quant.cpp analyze.cpp resolve.cpp)
set_target_properties(test_t6c PROPERTIES
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
