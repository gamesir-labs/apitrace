cmake_minimum_required(VERSION 3.20)

project(apitrace_test LANGUAGES CXX)

include(GNUInstallDirs)

option(APITRACE_BUILD_TEST_DEMO "Build the multi-scene D3D test demo" ON)
option(APITRACE_BUILD_METAL_DEMO "Build the native Metal validation demo" OFF)

set(APITRACE_ROOT_BUILD_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/../build/macos-native"
    CACHE PATH
    "Path to the root apitrace build directory that provides libapitrace_core.a and libapitrace_platform_apple_metal.a"
)

set(APITRACE_D3D_DEMO_COMMON_SOURCES
    src/demo_common.hpp
    src/app/include/app/demo_app.hpp
    src/app/src/demo_app.cpp
    src/runtime/dx11/include/runtime/dx11/runtime.hpp
    src/runtime/dx11/src/runtime.cpp
    src/runtime/dx12/include/runtime/dx12/runtime.hpp
    src/runtime/dx12/src/runtime.cpp
    src/scenes/dx11/include/scenes/dx11/scenes.hpp
    src/scenes/dx11/src/scenes.cpp
    src/scenes/dx12/include/scenes/dx12/scenes.hpp
    src/scenes/dx12/src/scenes.cpp
    src/scenes/shared/include/scenes/shared/scene_matrix.hpp
    src/scenes/shared/src/scene_matrix.cpp
)

set(APITRACE_D3D11_DEMO_SOURCES
    src/d3d11_main.cpp
    ${APITRACE_D3D_DEMO_COMMON_SOURCES}
)

set(APITRACE_D3D12_DEMO_SOURCES
    src/d3d12_main.cpp
    ${APITRACE_D3D_DEMO_COMMON_SOURCES}
)

set(APITRACE_D3D12_ASSET_DUMP_SOURCES
    src/d3d12_asset_dump.cpp
    src/demo_common.hpp
)

set(APITRACE_METAL_DEMO_SOURCES
    src/metal_main.mm
    src/runtime/metal/include/runtime/metal/runtime.hpp
    src/runtime/metal/src/runtime.mm
    src/scenes/metal/include/scenes/metal/scenes.hpp
    src/scenes/metal/src/scenes.mm
)

if(APITRACE_BUILD_TEST_DEMO AND WIN32)
    function(apitrace_configure_d3d_demo target_name source_list)
        add_executable(${target_name} ${${source_list}})
        target_compile_features(${target_name} PRIVATE cxx_std_17)
        target_compile_definitions(${target_name} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        target_include_directories(${target_name} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/src/app/include
            ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/dx11/include
            ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/dx12/include
            ${CMAKE_CURRENT_SOURCE_DIR}/src/scenes/dx11/include
            ${CMAKE_CURRENT_SOURCE_DIR}/src/scenes/dx12/include
            ${CMAKE_CURRENT_SOURCE_DIR}/src/scenes/shared/include
        )
        target_link_libraries(${target_name} PRIVATE
            d3d12
            d3d11
            dxgi
            dxguid
            user32
            gdi32
            ole32
            uuid
        )
        if(MINGW)
            target_link_options(${target_name} PRIVATE -static-libgcc -static-libstdc++)
        endif()
        install(TARGETS ${target_name} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    endfunction()

    apitrace_configure_d3d_demo(apitrace_test_d3d11 APITRACE_D3D11_DEMO_SOURCES)
    apitrace_configure_d3d_demo(apitrace_test_d3d12 APITRACE_D3D12_DEMO_SOURCES)
    apitrace_configure_d3d_demo(apitrace_d3d12_asset_dump APITRACE_D3D12_ASSET_DUMP_SOURCES)
    install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/assets/ DESTINATION ${CMAKE_INSTALL_BINDIR}/assets)
endif()

if(APITRACE_BUILD_METAL_DEMO)
    if(NOT APPLE)
        message(FATAL_ERROR "APITRACE_BUILD_METAL_DEMO requires APPLE")
    endif()

    enable_language(OBJCXX)

    set(APITRACE_CORE_STATIC "${APITRACE_ROOT_BUILD_DIR}/libapitrace_core.a")
    set(APITRACE_METAL_STATIC "${APITRACE_ROOT_BUILD_DIR}/libapitrace_platform_apple_metal.a")
    set(APITRACE_PLATFORM_MACOS_STATIC "${APITRACE_ROOT_BUILD_DIR}/libapitrace_platform_macos.a")
    foreach(required_lib IN ITEMS "${APITRACE_CORE_STATIC}" "${APITRACE_METAL_STATIC}" "${APITRACE_PLATFORM_MACOS_STATIC}")
        if(NOT EXISTS "${required_lib}")
            message(FATAL_ERROR "missing root build artifact required by APITRACE_BUILD_METAL_DEMO: ${required_lib}")
        endif()
    endforeach()

    add_executable(apitrace_test_metal ${APITRACE_METAL_DEMO_SOURCES})
    target_compile_features(apitrace_test_metal PRIVATE cxx_std_17)
    target_include_directories(apitrace_test_metal PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/metal/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/scenes/metal/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/metal/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/platform/macos/include
    )
    target_link_libraries(apitrace_test_metal PRIVATE
        "${APITRACE_CORE_STATIC}"
        "${APITRACE_METAL_STATIC}"
        "${APITRACE_PLATFORM_MACOS_STATIC}"
        "-framework Cocoa"
        "-framework Foundation"
        "-framework Metal"
        "-framework QuartzCore"
        "-framework CoreGraphics"
    )
    install(TARGETS apitrace_test_metal RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
