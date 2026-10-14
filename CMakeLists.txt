cmake_minimum_required(VERSION 3.21)

project(Malamih VERSION 3.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(DEPS_DIR "${CMAKE_BINARY_DIR}/_deps" CACHE PATH "Folder for downloaded dependencies (ONNX Runtime, face models)")

find_package(Qt6 REQUIRED COMPONENTS Widgets Concurrent Sql)
find_package(OpenCV 4.8 REQUIRED COMPONENTS core imgproc imgcodecs videoio)

include(cmake/onnxruntime.cmake)
include(cmake/models.cmake)

qt_standard_project_setup()

qt_add_executable(Malamih
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
    src/diagnostics.h
    src/diagnostics.cpp
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
    src/personmodel.h
    src/personmodel.cpp
    src/videoworker.h
    src/videoworker.cpp
    src/shotviewer.h
    src/shotviewer.cpp
    src/timelineview.h
    src/timelineview.cpp
    src/matchpage.h
    src/matchpage.cpp
    src/personprofile.h
    src/personprofile.cpp
    src/harvestdialog.h
    src/harvestdialog.cpp
    src/performance.h
    src/performance.cpp
    src/videoexport.h
    src/videoexport.cpp
    src/imagelab.h
    src/imagelab.cpp
    src/enhancedialog.h
    src/enhancedialog.cpp
    src/folderscan.h
    src/folderscan.cpp
    src/report.h
    src/report.cpp
)

# أيقونة التطبيق: داخل الملف التنفيذي على Windows وللنافذة عبر موارد Qt
if(WIN32)
    target_sources(Malamih PRIVATE resources/app.rc)
endif()
qt_add_resources(Malamih app_resources
    PREFIX "/"
    FILES resources/app-256.png resources/app-512.png
)

target_link_libraries(Malamih PRIVATE
    Qt6::Widgets
    Qt6::Concurrent
    Qt6::Sql
    ${OpenCV_LIBS}
    onnxruntime
)

# ترميز UTF-8 للملفات المصدرية مع MSVC
if(MSVC)
    target_compile_options(Malamih PRIVATE /utf-8)
endif()
if(WIN32)
    target_link_libraries(Malamih PRIVATE psapi)
endif()

set_target_properties(Malamih PROPERTIES
    WIN32_EXECUTABLE ON
)

# نسخ النماذج بجانب الملف التنفيذي
add_custom_command(TARGET Malamih POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:Malamih>/models"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${FACE_MODELS_DIR}/det_10g.onnx"
            "${FACE_MODELS_DIR}/w600k_r50.onnx"
            "$<TARGET_FILE_DIR:Malamih>/models"
)

# نسخ مكتبات OpenCV و ONNX Runtime بجانب الملف التنفيذي على Windows
if(WIN32)
    file(GLOB EXTRA_RUNTIME_DLLS
        "${ONNXRUNTIME_ROOT}/lib/onnxruntime_providers_shared.dll"
        "${OpenCV_DIR}/../bin/opencv_videoio_ffmpeg*.dll"
    )
    add_custom_command(TARGET Malamih POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:Malamih> ${EXTRA_RUNTIME_DLLS}
                "$<TARGET_FILE_DIR:Malamih>"
        COMMAND_EXPAND_LISTS
    )
endif()
