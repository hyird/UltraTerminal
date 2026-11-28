set(ULTRATERIMAL_WDK_ROOT "" CACHE PATH "Path to Windows WDK root (containing Include/wdf/umdf and Lib/wdf/umdf)")
set(ULTRATERIMAL_IDD_SIGN_SHA1 "" CACHE STRING "Optional certificate SHA1 thumbprint to sign ultraterimal_idd.dll and .cat")

set(_wdk_candidates)
if(ULTRATERIMAL_WDK_ROOT)
    list(APPEND _wdk_candidates "${ULTRATERIMAL_WDK_ROOT}")
endif()
list(APPEND _wdk_candidates
    "${CMAKE_SOURCE_DIR}/build/idd-proto/wdk/c"
    "C:/Program Files (x86)/Windows Kits/10")

set(_wdk_root "")
set(_wdk_um_ver "")
foreach(_cand IN LISTS _wdk_candidates)
    if(EXISTS "${_cand}/Include/wdf/umdf/2.31/wdf.h")
        file(GLOB _idd_headers "${_cand}/Include/10.0.*/um/iddcx/1.6/IddCx.h")
        if(_idd_headers)
            list(SORT _idd_headers ORDER DESCENDING)
            list(GET _idd_headers 0 _idd_header)
            get_filename_component(_idd_dir "${_idd_header}" DIRECTORY)
            get_filename_component(_iddcx_dir "${_idd_dir}" DIRECTORY)
            get_filename_component(_um_dir "${_iddcx_dir}" DIRECTORY)
            get_filename_component(_ver_dir "${_um_dir}" DIRECTORY)
            get_filename_component(_wdk_um_ver "${_ver_dir}" NAME)
            set(_wdk_root "${_cand}")
            break()
        endif()
    endif()
endforeach()

if(NOT _wdk_root)
    message(STATUS "WDK (UMDF 2.31 + IddCx 1.6) not found; skipping ultraterimal-idd-driver target (set -DULTRATERIMAL_WDK_ROOT=... to enable)")
    return()
endif()

message(STATUS "Building Ultraterimal IDD driver using WDK at ${_wdk_root} (SDK ${_wdk_um_ver})")

add_library(ultraterimal-idd-driver SHARED driver.cpp)
set_target_properties(ultraterimal-idd-driver PROPERTIES
    OUTPUT_NAME ultraterimal_idd
    CXX_EXTENSIONS OFF
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
target_compile_features(ultraterimal-idd-driver PRIVATE cxx_std_23)
target_compile_definitions(ultraterimal-idd-driver PRIVATE
    UNICODE _UNICODE _AMD64_
    UMDF_VERSION_MAJOR=2 UMDF_VERSION_MINOR=31
    IDDCX_VERSION_MAJOR=1 IDDCX_VERSION_MINOR=6)
target_compile_options(ultraterimal-idd-driver PRIVATE /W3 /EHsc "$<$<CONFIG:Release>:/Gy;/Gw;/GF>")
target_include_directories(ultraterimal-idd-driver PRIVATE
    "${_wdk_root}/Include/wdf/umdf/2.31"
    "${_wdk_root}/Include/${_wdk_um_ver}/um/iddcx/1.6"
    "${_wdk_root}/Include/${_wdk_um_ver}/um"
    "${_wdk_root}/Include/${_wdk_um_ver}/shared"
    "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_directories(ultraterimal-idd-driver PRIVATE
    "${_wdk_root}/Lib/wdf/umdf/x64/2.31"
    "${_wdk_root}/Lib/${_wdk_um_ver}/um/x64/iddcx/1.6"
    "${_wdk_root}/Lib/${_wdk_um_ver}/um/x64")
target_link_libraries(ultraterimal-idd-driver PRIVATE
    WdfDriverStubUm.lib iddcxstub.lib d3d11.lib dxgi.lib ole32.lib advapi32.lib kernel32.lib user32.lib ntdll.lib)
target_link_options(ultraterimal-idd-driver PRIVATE "$<$<CONFIG:Release>:/OPT:REF;/OPT:ICF>")

set(_idd_stage_dir "${CMAKE_CURRENT_BINARY_DIR}/package")
find_program(_inf2cat_exe NAMES Inf2Cat.exe inf2cat.exe PATHS
    "${_wdk_root}/bin/${_wdk_um_ver}/x86"
    "${_wdk_root}/bin/x86"
    NO_DEFAULT_PATH
    REQUIRED)

file(GLOB _sdk_x64_bin_dirs "C:/Program Files (x86)/Windows Kits/10/bin/10.0.*/x64")
if(_sdk_x64_bin_dirs)
    list(SORT _sdk_x64_bin_dirs ORDER DESCENDING)
endif()
find_program(_signtool_exe signtool.exe PATHS
    "${_wdk_root}/bin/${_wdk_um_ver}/x64"
    "C:/Program Files (x86)/Windows Kits/10/bin/${_wdk_um_ver}/x64"
    ${_sdk_x64_bin_dirs}
    REQUIRED)
find_program(_powershell_exe NAMES pwsh powershell REQUIRED)

set(_idd_sign_args)
if(ULTRATERIMAL_IDD_SIGN_SHA1)
    set(_idd_sign_args -SignSha1 "${ULTRATERIMAL_IDD_SIGN_SHA1}")
endif()

add_custom_command(TARGET ultraterimal-idd-driver POST_BUILD
    COMMAND "${_powershell_exe}" -NoProfile -NonInteractive -ExecutionPolicy Bypass
        -File "${CMAKE_CURRENT_SOURCE_DIR}/stage_idd_package.ps1"
        -DllPath "$<TARGET_FILE:ultraterimal-idd-driver>"
        -InfPath "${CMAKE_CURRENT_SOURCE_DIR}/ultraterimal_idd.inf"
        -StageDir "${_idd_stage_dir}"
        -Inf2CatExe "${_inf2cat_exe}"
        -SignToolExe "${_signtool_exe}"
        ${_idd_sign_args}
    COMMENT "Staging and signing Ultraterimal IDD driver package")
