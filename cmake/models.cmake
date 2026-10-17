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

# كشف أخف من "buffalo_m" (det_2.5g.onnx) لتحليل الفيديو على الأجهزة الضعيفة دون كرت شاشة.
# بصمة "buffalo_m" هي w600k_r50 نفسها، فلا يتغير شيء في قاعدة الأشخاص
set(FACE_FAST_MODELS_URL "https://github.com/deepinsight/insightface/releases/download/v0.7/buffalo_m.zip")
set(FACE_FAST_MODELS_SHA256 "d98264bd8f2dc75cbc2ddce2a14e636e02bb857b3051c234b737bf3b614edca9")
if(NOT EXISTS "${FACE_MODELS_DIR}/det_2.5g.onnx")
    set(_fast_zip "${DEPS_DIR}/buffalo_m.zip")
    if(NOT EXISTS "${_fast_zip}")
        message(STATUS "Downloading fast face detector (~265 MB archive): ${FACE_FAST_MODELS_URL}")
        file(DOWNLOAD "${FACE_FAST_MODELS_URL}" "${_fast_zip}.part"
            EXPECTED_HASH SHA256=${FACE_FAST_MODELS_SHA256}
            SHOW_PROGRESS
        )
        file(RENAME "${_fast_zip}.part" "${_fast_zip}")
    endif()
    # الملف داخل مجلد buffalo_m في الأرشيف
    set(_fast_dir "${DEPS_DIR}/buffalo_m_extract")
    file(ARCHIVE_EXTRACT INPUT "${_fast_zip}" DESTINATION "${_fast_dir}"
        PATTERNS buffalo_m/det_2.5g.onnx
    )
    file(MAKE_DIRECTORY "${FACE_MODELS_DIR}")
    file(RENAME "${_fast_dir}/buffalo_m/det_2.5g.onnx" "${FACE_MODELS_DIR}/det_2.5g.onnx")
    file(REMOVE_RECURSE "${_fast_dir}")
    # الأرشيف لا يلزم بعد استخراج الملف الصغير، فلا يبقى في مجلد التبعيات (وذاكرة CI المؤقتة)
    file(REMOVE "${_fast_zip}")
endif()
