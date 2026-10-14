# ONNX Runtime: محرك تشغيل نماذج الذكاء الاصطناعي محلياً.
# إذا لم يُحدَّد ONNXRUNTIME_ROOT تُنزَّل النسخة الجاهزة الرسمية مرة واحدة وقت الإعداد.

set(ONNXRUNTIME_VERSION 1.20.1)
set(ONNXRUNTIME_ROOT "" CACHE PATH "Folder of an extracted ONNX Runtime release (contains include/ and lib/)")

if(NOT ONNXRUNTIME_ROOT)
    if(WIN32)
        set(_ort_package "onnxruntime-win-x64-${ONNXRUNTIME_VERSION}")
        set(_ort_archive "${_ort_package}.zip")
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(_ort_package "onnxruntime-linux-x64-${ONNXRUNTIME_VERSION}")
        set(_ort_archive "${_ort_package}.tgz")
    else()
        message(FATAL_ERROR "Set ONNXRUNTIME_ROOT to an ONNX Runtime release for this platform")
    endif()

    set(ONNXRUNTIME_ROOT "${DEPS_DIR}/${_ort_package}")
    if(NOT EXISTS "${ONNXRUNTIME_ROOT}/include/onnxruntime_cxx_api.h")
        set(_ort_url "https://github.com/microsoft/onnxruntime/releases/download/v${ONNXRUNTIME_VERSION}/${_ort_archive}")
        message(STATUS "Downloading ${_ort_url}")
        file(DOWNLOAD "${_ort_url}" "${DEPS_DIR}/${_ort_archive}" SHOW_PROGRESS STATUS _ort_status)
        list(GET _ort_status 0 _ort_code)
        if(NOT _ort_code EQUAL 0)
            message(FATAL_ERROR "Failed to download ONNX Runtime: ${_ort_status}")
        endif()
        file(ARCHIVE_EXTRACT INPUT "${DEPS_DIR}/${_ort_archive}" DESTINATION "${DEPS_DIR}")
    endif()
endif()

add_library(onnxruntime SHARED IMPORTED)
target_include_directories(onnxruntime INTERFACE "${ONNXRUNTIME_ROOT}/include")
if(WIN32)
    set_target_properties(onnxruntime PROPERTIES
        IMPORTED_IMPLIB "${ONNXRUNTIME_ROOT}/lib/onnxruntime.lib"
        IMPORTED_LOCATION "${ONNXRUNTIME_ROOT}/lib/onnxruntime.dll"
    )
else()
    set_target_properties(onnxruntime PROPERTIES
        IMPORTED_LOCATION "${ONNXRUNTIME_ROOT}/lib/libonnxruntime.so"
    )
endif()
