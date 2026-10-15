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

# Precompile the probe shader (cs_5_0, /O3 /WX /Ges /Gis)
set(PROBE_SRC "${SHADER_DIR}/probe.hlsl")
set(PROBE_Dxbc "${SHADER_OUT}/probe.dxbc")
set(PROBE_HDR "${SHADER_OUT}/probe_dxbc.h")

add_custom_command(
    OUTPUT "${PROBE_Dxbc}" "${PROBE_HDR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${SHADER_OUT}"
    COMMAND fxc /nologo /T cs_5_0 /E main /O3 /WX /Ges /Gis
            /Fc "${PROBE_Dxbc}" /Vn g_probe_dxbc "${PROBE_SRC}"
    COMMAND ${CMAKE_COMMAND} -E echo "// Embedded probe shader bytecode" > "${PROBE_HDR}"
    COMMAND ${CMAKE_COMMAND} -E echo "#pragma once" >> "${PROBE_HDR}"
    COMMAND ${CMAKE_COMMAND} -E cat "${PROBE_Dxbc}" >> "${PROBE_HDR}"
    DEPENDS "${PROBE_SRC}"
    COMMENT "[fxc] probe.hlsl -> probe.dxbc"
    VERBATIM
)

add_custom_target(shaders ALL DEPENDS "${PROBE_Dxbc}" "${PROBE_HDR}")

# ------------------------------------------------------------------
# Library: pyrowave_d3d11.dll
# ------------------------------------------------------------------
add_library(pyrowave_d3d11 SHARED
    device.cpp
    c_api.cpp
    ${PROBE_HDR}
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

# Embed shader bytecode as a resource
add_custom_command(TARGET pyrowave_d3d11 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROBE_Dxbc}" "$<TARGET_FILE_DIR:pyrowave_d3d11>"
    COMMENT "[copy] probe.dxbc -> output dir"
)

# ------------------------------------------------------------------
# Static lib for tests (no DLL export)
# ------------------------------------------------------------------
add_library(pyrowave_d3d11_static STATIC
    device.cpp
    c_api.cpp
    ${PROBE_HDR}
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
# Add device.cpp and c_api.cpp to the test so it can call pwd3d11_* functions
target_sources(test_t0 PRIVATE device.cpp c_api.cpp)
set_target_properties(test_t0 PROPERTIES
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
