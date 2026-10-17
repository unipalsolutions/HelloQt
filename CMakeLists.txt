cmake_minimum_required(VERSION 3.21)

project(Malamih VERSION 3.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# رقم الإصدار الكامل؛ CI يمرر رقم البناء (3.0.N) فيظهر داخل البرنامج ومعلومات الملف والمثبّت معاً
set(MALAMIH_VERSION "${PROJECT_VERSION}" CACHE STRING "Version shown in the app and the Windows file properties")

set(DEPS_DIR "${CMAKE_BINARY_DIR}/_deps" CACHE PATH "Folder for downloaded dependencies (ONNX Runtime, face models)")

find_package(Qt6 REQUIRED COMPONENTS Widgets Concurrent Sql Network)
find_package(OpenCV 4.5 REQUIRED COMPONENTS core imgproc imgcodecs videoio)

include(cmake/onnxruntime.cmake)
include(cmake/models.cmake)

qt_standard_project_setup()

qt_add_executable(Malamih
    src/main.cpp
    src/applog.h
    src/applog.cpp
    src/alertsound.h
    src/alertsound.cpp
    src/camerarules.h
    src/camerarules.cpp
    src/camerarulesdialog.h
    src/camerarulesdialog.cpp
    src/clipexport.h
    src/clipexport.cpp
    src/appsettings.h
    src/appsettings.cpp
    src/settingsdialog.h
    src/settingsdialog.cpp
    src/maintenance.h
    src/maintenance.cpp
    src/camerawatch.h
    src/camerawatch.cpp
    src/visitorsdialog.h
    src/visitorsdialog.cpp
    src/recordingsdialog.h
    src/recordingsdialog.cpp
    src/dailyreport.h
    src/dailyreport.cpp
    src/gpucheck.h
    src/gpucheck.cpp
    src/mainwindow.h
    src/mainwindow.cpp
    src/mainwindow_p.h
    src/mainwindow_camera.cpp
    src/mainwindow_records.cpp
    src/mainwindow_persons.cpp
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
    src/videoplayer.h
    src/videoplayer.cpp
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
    src/alertlog.h
    src/alertlog.cpp
    src/importdialog.h
    src/importdialog.cpp
    src/performance.h
    src/performance.cpp
    src/videoexport.h
    src/videoexport.cpp
    src/videofilters.h
    src/videofilters.cpp
    src/videofiltersdialog.h
    src/videofiltersdialog.cpp
    src/cameras.h
    src/cameras.cpp
    src/cameradialog.h
    src/cameradialog.cpp
    src/recorder.h
    src/recorder.cpp
    src/multicamera.h
    src/multicamera.cpp
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
    string(REPLACE "." "," MALAMIH_VERSION_COMMAS "${MALAMIH_VERSION}")
    set(APP_ICON "${CMAKE_CURRENT_SOURCE_DIR}/resources/app.ico")
    configure_file(resources/app.rc.in "${CMAKE_CURRENT_BINARY_DIR}/app.rc" @ONLY)
    target_sources(Malamih PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/app.rc")
endif()
target_compile_definitions(Malamih PRIVATE MALAMIH_VERSION="${MALAMIH_VERSION}")
qt_add_resources(Malamih app_resources
    PREFIX "/"
    FILES resources/app-256.png resources/app-512.png
)

target_link_libraries(Malamih PRIVATE
    Qt6::Widgets
    Qt6::Network
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
    target_link_libraries(Malamih PRIVATE psapi strmiids ole32 oleaut32 crypt32 mfplat mf mfuuid dxgi dbghelp winmm)
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
        "${OpenCV_DIR}/../bin/opencv_videoio_ffmpeg*.dll"
    )
    list(APPEND EXTRA_RUNTIME_DLLS ${ONNXRUNTIME_EXTRA_DLLS})
    add_custom_command(TARGET Malamih POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:Malamih> ${EXTRA_RUNTIME_DLLS}
                "$<TARGET_FILE_DIR:Malamih>"
        COMMAND_EXPAND_LISTS
    )
endif()
