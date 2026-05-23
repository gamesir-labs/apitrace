cmake_minimum_required(VERSION 3.20)

project(apitrace_test_demo LANGUAGES CXX)

include(GNUInstallDirs)

option(APITRACE_BUILD_TEST_DEMO "Build the multi-scene D3D test demo" ON)

set(APITRACE_TEST_DEMO_SOURCES
    src/demo_common.hpp
    src/main.cpp
    src/app/include/app/cli.hpp
    src/app/include/app/demo_app.hpp
    src/app/src/cli.cpp
    src/app/src/demo_app.cpp
    src/runtime/dx11/include/runtime/dx11/runtime.hpp
    src/runtime/dx11/src/runtime.cpp
    src/scenes/dx11/include/scenes/dx11/scenes.hpp
    src/scenes/dx11/src/scenes.cpp
)

if(APITRACE_BUILD_TEST_DEMO)
    add_executable(apitrace_test_demo ${APITRACE_TEST_DEMO_SOURCES})
    target_compile_features(apitrace_test_demo PRIVATE cxx_std_17)
    target_compile_definitions(apitrace_test_demo PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    target_include_directories(apitrace_test_demo PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/app/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/dx11/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/scenes/dx11/include
    )
    target_link_libraries(apitrace_test_demo PRIVATE
        d3d11
        dxgi
        user32
        gdi32
        ole32
        uuid
    )
    install(TARGETS apitrace_test_demo RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/assets/ DESTINATION ${CMAKE_INSTALL_BINDIR}/assets)
endif()
