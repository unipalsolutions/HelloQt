# نماذج InsightFace "buffalo_l":
#   det_10g.onnx   كشف الوجوه ونقاطها الخمس (SCRFD)
#   w600k_r50.onnx بصمة الوجه للتعرف (ArcFace ResNet-50)
# ترخيص الأوزان: للاستخدام غير التجاري فقط (راجع README).

set(FACE_MODELS_URL "https://github.com/deepinsight/insightface/releases/download/v0.7/buffalo_l.zip")
set(FACE_MODELS_SHA256 "80ffe37d8a5940d59a7384c201a2a38d4741f2f3c51eef46ebb28218a7b0ca2f")
set(FACE_MODELS_DIR "${DEPS_DIR}/models" CACHE PATH "Folder containing det_10g.onnx and w600k_r50.onnx")

if(NOT EXISTS "${FACE_MODELS_DIR}/det_10g.onnx" OR NOT EXISTS "${FACE_MODELS_DIR}/w600k_r50.onnx")
    set(_models_zip "${DEPS_DIR}/buffalo_l.zip")
    if(NOT EXISTS "${_models_zip}")
        message(STATUS "Downloading face models (~290 MB): ${FACE_MODELS_URL}")
        file(DOWNLOAD "${FACE_MODELS_URL}" "${_models_zip}.part"
            EXPECTED_HASH SHA256=${FACE_MODELS_SHA256}
            SHOW_PROGRESS
        )
        file(RENAME "${_models_zip}.part" "${_models_zip}")
    endif()
    file(ARCHIVE_EXTRACT INPUT "${_models_zip}" DESTINATION "${FACE_MODELS_DIR}"
        PATTERNS det_10g.onnx w600k_r50.onnx
    )
endif()
