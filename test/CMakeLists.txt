cmake_minimum_required(VERSION 3.20)

project(apitrace_triangle_demos LANGUAGES CXX)

option(APITRACE_BUILD_D3D11_DEMO "Build the D3D11 triangle demo" ON)
option(APITRACE_BUILD_D3D12_DEMO "Build the D3D12 triangle demo" ON)

set(APITRACE_TRIANGLE_DEMO_COMMON
    src/demo_common.hpp
)

function(apitrace_add_triangle_demo target_name source_file)
    add_executable(${target_name} ${source_file} ${APITRACE_TRIANGLE_DEMO_COMMON})
    target_compile_features(${target_name} PRIVATE cxx_std_17)
    target_compile_definitions(${target_name} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    target_include_directories(${target_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(${target_name} PRIVATE
        d3d11
        d3d12
        dxgi
        user32
        gdi32
        ole32
        uuid
    )
    install(TARGETS ${target_name} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endfunction()

if(APITRACE_BUILD_D3D11_DEMO)
    apitrace_add_triangle_demo(apitrace_triangle_d3d11 src/d3d11_triangle.cpp)
endif()

if(APITRACE_BUILD_D3D12_DEMO)
    apitrace_add_triangle_demo(apitrace_triangle_d3d12 src/d3d12_triangle.cpp)
endif()
