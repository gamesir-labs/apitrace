cmake_minimum_required(VERSION 3.20)

project(apitrace_test_demo LANGUAGES CXX)

include(GNUInstallDirs)

option(APITRACE_BUILD_TEST_DEMO "Build the multi-scene D3D test demo" ON)
option(APITRACE_BUILD_METAL_DEMO "Build the native Metal validation demo" OFF)

set(APITRACE_ROOT_BUILD_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/../build/macos-native"
    CACHE PATH
    "Path to the root apitrace build directory that provides libapitrace_core.a and libapitrace_platform_apple_metal.a"
)

set(APITRACE_TEST_DEMO_SOURCES
    src/demo_common.hpp
    src/main.cpp
    src/app/include/app/cli.hpp
    src/app/include/app/demo_app.hpp
    src/app/src/cli.cpp
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

set(APITRACE_METAL_DEMO_SOURCES
    src/metal_main.mm
    src/runtime/metal/include/runtime/metal/runtime.hpp
    src/runtime/metal/src/runtime.mm
    src/scenes/metal/include/scenes/metal/scenes.hpp
    src/scenes/metal/src/scenes.mm
)

if(APITRACE_BUILD_TEST_DEMO AND WIN32)
    add_executable(apitrace_test_demo ${APITRACE_TEST_DEMO_SOURCES})
    target_compile_features(apitrace_test_demo PRIVATE cxx_std_17)
    target_compile_definitions(apitrace_test_demo PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    target_include_directories(apitrace_test_demo PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/app/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/dx11/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/dx12/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/scenes/dx11/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/scenes/dx12/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/scenes/shared/include
    )
    target_link_libraries(apitrace_test_demo PRIVATE
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
        target_link_options(apitrace_test_demo PRIVATE -static-libgcc -static-libstdc++)
    endif()
    install(TARGETS apitrace_test_demo RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/assets/ DESTINATION ${CMAKE_INSTALL_BINDIR}/assets)
endif()

if(APITRACE_BUILD_METAL_DEMO)
    if(NOT APPLE)
        message(FATAL_ERROR "APITRACE_BUILD_METAL_DEMO requires APPLE")
    endif()

    enable_language(OBJCXX)

    set(APITRACE_CORE_STATIC "${APITRACE_ROOT_BUILD_DIR}/libapitrace_core.a")
    set(APITRACE_METAL_STATIC "${APITRACE_ROOT_BUILD_DIR}/libapitrace_platform_apple_metal.a")
    foreach(required_lib IN ITEMS "${APITRACE_CORE_STATIC}" "${APITRACE_METAL_STATIC}")
        if(NOT EXISTS "${required_lib}")
            message(FATAL_ERROR "missing root build artifact required by APITRACE_BUILD_METAL_DEMO: ${required_lib}")
        endif()
    endforeach()

    add_executable(apitrace_metal_demo ${APITRACE_METAL_DEMO_SOURCES})
    target_compile_features(apitrace_metal_demo PRIVATE cxx_std_17)
    target_include_directories(apitrace_metal_demo PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/metal/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/scenes/metal/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/metal/include
    )
    target_link_libraries(apitrace_metal_demo PRIVATE
        "${APITRACE_CORE_STATIC}"
        "${APITRACE_METAL_STATIC}"
        "-framework Foundation"
        "-framework Metal"
        "-framework QuartzCore"
        "-framework CoreGraphics"
    )
    install(TARGETS apitrace_metal_demo RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
