cmake_minimum_required(VERSION 3.15)
project(golden_model LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(MSVC)
    set(CMAKE_C_FLAGS_RELEASE "/O2 /EHsc /W4 /WX /MT /permissive-")
    set(CMAKE_C_FLAGS_DEBUG "/Od /Zi /EHsc /W4 /MTd /permissive-")
    set(CMAKE_C_FLAGS "/O2 /EHsc /W4 /WX /MT /permissive-")
    add_link_options(/INCREMENTAL:NO)
endif()

add_library(golden_model STATIC
    golden_common.h
    golden_quant.c
    golden_analyze.c
    golden_resolve.c
    golden_packing.c
    golden_packetize.c
    golden_pipeline.c
    golden_pipeline.h
)
target_include_directories(golden_model PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(golden_test golden_test.c)
target_link_libraries(golden_test PRIVATE golden_model)

install(TARGETS golden_model ARCHIVE DESTINATION lib)
install(FILES golden_common.h golden_pipeline.h DESTINATION include)
