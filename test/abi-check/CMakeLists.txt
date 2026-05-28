# ABI smoke test: link apitrace-vendored standalone D3D headers against DXMT-built dylibs.
if(NOT (APPLE AND APITRACE_BUILD_D3D_NATIVE_RETRACE))
    return()
endif()

add_executable(apitrace_native_d3d12_smoke native_d3d12_smoke.cpp)
target_link_libraries(apitrace_native_d3d12_smoke PRIVATE apitrace_d3d_headers)
target_compile_features(apitrace_native_d3d12_smoke PRIVATE cxx_std_17)
target_compile_definitions(apitrace_native_d3d12_smoke PRIVATE
    APITRACE_DXMT_NATIVE_BUILD_DIR="${APITRACE_DXMT_NATIVE_BUILD_DIR}"
)
# We dlopen the DXMT dylibs explicitly so that we control load order, but
# linking against them too gives more useful error messages early.
target_link_libraries(apitrace_native_d3d12_smoke PRIVATE
    apitrace_dxmt::winemetal
    apitrace_dxmt::dxgi
    apitrace_dxmt::d3d12
)
set_target_properties(apitrace_native_d3d12_smoke PROPERTIES
    BUILD_RPATH "${APITRACE_DXMT_NATIVE_BUILD_DIR}/src/nativemetal;${APITRACE_DXMT_NATIVE_BUILD_DIR}/src/dxgi;${APITRACE_DXMT_NATIVE_BUILD_DIR}/src/d3d12"
)
