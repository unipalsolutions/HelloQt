cmake_minimum_required(VERSION 3.21)

project(HelloQt VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(DEPS_DIR "${CMAKE_BINARY_DIR}/_deps" CACHE PATH "Folder for downloaded dependencies (ONNX Runtime, face models)")

find_package(Qt6 REQUIRED COMPONENTS Widgets Concurrent)
find_package(OpenCV 4.8 REQUIRED COMPONENTS core imgproc imgcodecs videoio)

include(cmake/onnxruntime.cmake)
include(cmake/models.cmake)

qt_standard_project_setup()

qt_add_executable(HelloQt
    src/main.cpp
    src/mainwindow.h
    src/mainwindow.cpp
    src/faceengine.h
    src/faceengine.cpp
    src/facedatabase.h
    src/facedatabase.cpp
    src/facetypes.h
    src/carddelegates.h
    src/carddelegates.cpp
    src/icons.h
    src/icons.cpp
    src/theme.h
    src/theme.cpp
    src/widgets.h
    src/widgets.cpp
    src/imageio.h
    src/imageio.cpp
    src/imageview.h
    src/imageview.cpp
    src/persondialog.h
    src/persondialog.cpp
    src/videoworker.h
    src/videoworker.cpp
)

target_link_libraries(HelloQt PRIVATE
    Qt6::Widgets
    Qt6::Concurrent
    ${OpenCV_LIBS}
    onnxruntime
)

# ترميز UTF-8 للملفات المصدرية مع MSVC
if(MSVC)
    target_compile_options(HelloQt PRIVATE /utf-8)
endif()

set_target_properties(HelloQt PROPERTIES
    WIN32_EXECUTABLE ON
)

# نسخ النماذج بجانب الملف التنفيذي
add_custom_command(TARGET HelloQt POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:HelloQt>/models"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${FACE_MODELS_DIR}/det_10g.onnx"
            "${FACE_MODELS_DIR}/w600k_r50.onnx"
            "$<TARGET_FILE_DIR:HelloQt>/models"
)

# نسخ مكتبات OpenCV و ONNX Runtime بجانب الملف التنفيذي على Windows
if(WIN32)
    file(GLOB EXTRA_RUNTIME_DLLS
        "${ONNXRUNTIME_ROOT}/lib/onnxruntime_providers_shared.dll"
        "${OpenCV_DIR}/../bin/opencv_videoio_ffmpeg*.dll"
    )
    add_custom_command(TARGET HelloQt POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:HelloQt> ${EXTRA_RUNTIME_DLLS}
                "$<TARGET_FILE_DIR:HelloQt>"
        COMMAND_EXPAND_LISTS
    )
endif()
