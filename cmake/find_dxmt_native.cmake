# Locate the DXMT native build directory and expose imported targets for the
# d3d11 / d3d12 / dxgi / winemetal dylibs that the macOS native D3D retrace
# backend links against.
#
# Inputs (cache variables / options):
#   APITRACE_DXMT_NATIVE_BUILD_DIR  — path to dxmt/build-gs-native
#
# Outputs (imported targets):
#   apitrace_dxmt::d3d12
#   apitrace_dxmt::d3d11
#   apitrace_dxmt::dxgi
#   apitrace_dxmt::winemetal
#
# Sets APITRACE_DXMT_NATIVE_FOUND to TRUE/FALSE.

set(APITRACE_DXMT_NATIVE_FOUND FALSE)

if(NOT APITRACE_DXMT_NATIVE_BUILD_DIR)
    message(STATUS "apitrace: APITRACE_DXMT_NATIVE_BUILD_DIR is not set; DXMT imported targets disabled.")
    return()
endif()

if(NOT IS_DIRECTORY "${APITRACE_DXMT_NATIVE_BUILD_DIR}")
    message(WARNING "apitrace: APITRACE_DXMT_NATIVE_BUILD_DIR='${APITRACE_DXMT_NATIVE_BUILD_DIR}' is not a directory.")
    return()
endif()

set(_dxmt_d3d12   "${APITRACE_DXMT_NATIVE_BUILD_DIR}/src/d3d12/d3d12.dylib")
set(_dxmt_d3d11   "${APITRACE_DXMT_NATIVE_BUILD_DIR}/src/d3d11/d3d11.dylib")
set(_dxmt_dxgi    "${APITRACE_DXMT_NATIVE_BUILD_DIR}/src/dxgi/dxgi.dylib")
set(_dxmt_winemtl "${APITRACE_DXMT_NATIVE_BUILD_DIR}/src/nativemetal/winemetal.dylib")

foreach(_p IN ITEMS "${_dxmt_d3d12}" "${_dxmt_d3d11}" "${_dxmt_dxgi}" "${_dxmt_winemtl}")
    if(NOT EXISTS "${_p}")
        message(WARNING "apitrace: required DXMT dylib not found: ${_p}")
        return()
    endif()
endforeach()

function(_apitrace_add_dxmt_lib name path)
    if(NOT TARGET apitrace_dxmt::${name})
        add_library(apitrace_dxmt::${name} SHARED IMPORTED GLOBAL)
        set_target_properties(apitrace_dxmt::${name} PROPERTIES
            IMPORTED_LOCATION "${path}"
            IMPORTED_NO_SONAME TRUE
        )
    endif()
endfunction()

_apitrace_add_dxmt_lib(d3d12    "${_dxmt_d3d12}")
_apitrace_add_dxmt_lib(d3d11    "${_dxmt_d3d11}")
_apitrace_add_dxmt_lib(dxgi     "${_dxmt_dxgi}")
_apitrace_add_dxmt_lib(winemetal "${_dxmt_winemtl}")

set(APITRACE_DXMT_NATIVE_FOUND TRUE)
message(STATUS "apitrace: DXMT native build dir = ${APITRACE_DXMT_NATIVE_BUILD_DIR}")
