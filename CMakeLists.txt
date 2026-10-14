cmake_minimum_required(VERSION 3.20)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
project(QuickView VERSION 1.0.3 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Unicode & UTF-8 source/execution character set
add_definitions(-DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX)
if(MSVC)
    add_compile_options(/utf-8)
endif()

# OpenXLSX Subdirectory
set(OPENXLSX_CREATE_DOCS OFF CACHE BOOL "" FORCE)
set(OPENXLSX_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
set(OPENXLSX_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(OPENXLSX_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/OpenXLSX)

# Source files
set(SOURCES
    src/main.cpp
    # Core (System, Shell & Input Hook)
    src/core/Utils.cpp
    src/core/FileTypeDetector.cpp
    src/core/ShellIntegration.cpp
    src/core/KeyboardHook.cpp
    # UI (Main Preview Window & Info View)
    src/ui/PreviewWindow.cpp
    src/ui/FileInfoView.cpp
    # Previews (Format-Specific Renderers)
    src/previews/ImagePreview.cpp
    src/previews/VideoPreview.cpp
    src/previews/MarkdownPreview.cpp
    src/previews/PdfPreview.cpp
    src/previews/ArchivePreview.cpp
    src/previews/SvgPreview.cpp
    src/previews/FontPreview.cpp
    src/previews/ExcelPreview.cpp
    # Loaders (Decoders & Thumbnail Extractors)
    src/loaders/StbImageLoader.cpp
    src/loaders/RawThumbnailExtractor.cpp
    src/loaders/MpvImageLoader.cpp
    # Third-party single-file sources
    third_party/md4c/md4c.c
)

set(HEADERS
    # Resources
    src/resources/Resource.h
    # Core
    src/core/Utils.h
    src/core/FileTypeDetector.h
    src/core/ShellIntegration.h
    src/core/KeyboardHook.h
    # UI
    src/ui/PreviewWindow.h
    src/ui/FileInfoView.h
    # Previews
    src/previews/ImagePreview.h
    src/previews/VideoPreview.h
    src/previews/MarkdownPreview.h
    src/previews/PdfPreview.h
    src/previews/ArchivePreview.h
    src/previews/SvgPreview.h
    src/previews/FontPreview.h
    src/previews/ExcelPreview.h
    # Loaders
    src/loaders/StbImageLoader.h
    src/loaders/RawThumbnailExtractor.h
    src/loaders/MpvImageLoader.h
)

# Preserve directory tree in Visual Studio / IDE solution explorer
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src" PREFIX "src" FILES
    src/main.cpp
    src/core/Utils.cpp
    src/core/FileTypeDetector.cpp
    src/core/ShellIntegration.cpp
    src/core/KeyboardHook.cpp
    src/ui/PreviewWindow.cpp
    src/ui/FileInfoView.cpp
    src/previews/ImagePreview.cpp
    src/previews/VideoPreview.cpp
    src/previews/MarkdownPreview.cpp
    src/previews/PdfPreview.cpp
    src/previews/ArchivePreview.cpp
    src/previews/SvgPreview.cpp
    src/previews/FontPreview.cpp
    src/previews/ExcelPreview.cpp
    src/loaders/StbImageLoader.cpp
    src/loaders/RawThumbnailExtractor.cpp
    src/loaders/MpvImageLoader.cpp
    ${HEADERS}
    src/resources/Resource.rc
    src/resources/app.manifest
)

# Win32 GUI executable
add_executable(QuickView WIN32 ${SOURCES} ${HEADERS} src/resources/Resource.rc)

# Include directories
target_include_directories(QuickView PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui
    ${CMAKE_CURRENT_SOURCE_DIR}/src/previews
    ${CMAKE_CURRENT_SOURCE_DIR}/src/loaders
    ${CMAKE_CURRENT_SOURCE_DIR}/src/resources
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/mpv/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/md4c
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/pdfium/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniz
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/OpenXLSX/OpenXLSX
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/OpenXLSX/OpenXLSX/headers
    ${CMAKE_CURRENT_BINARY_DIR}/third_party/OpenXLSX/OpenXLSX
)

# Link Windows system libraries and third party libs
target_link_libraries(QuickView PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/pdfium/lib/pdfium.dll.lib
    OpenXLSX::OpenXLSX
    miniz
    d2d1
    dwrite
    windowscodecs
    shlwapi
    shell32
    comdlg32
    ole32
    oleaut32
    comctl32
    dwmapi
    uxtheme
    propsys
    uuid
)




# Embed manifest for DPI awareness
target_sources(QuickView PRIVATE src/resources/app.manifest)
set_target_properties(QuickView PROPERTIES
    WIN32_EXECUTABLE TRUE
    VS_DPI_AWARE "PerMonitor"
)

# Set manifest
if(MSVC)
    target_link_options(QuickView PRIVATE "/MANIFEST:EMBED" "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/src/resources/app.manifest")
endif()

# Copy libmpv-2.dll next to QuickView.exe
add_custom_command(TARGET QuickView POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/mpv/libmpv-2.dll"
        "$<TARGET_FILE_DIR:QuickView>/libmpv-2.dll"
    COMMENT "Copying libmpv-2.dll to output directory"
)

# Copy pdfium.dll next to QuickView.exe
add_custom_command(TARGET QuickView POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/pdfium/bin/pdfium.dll"
        "$<TARGET_FILE_DIR:QuickView>/pdfium.dll"
    COMMENT "Copying pdfium.dll to output directory"
)


