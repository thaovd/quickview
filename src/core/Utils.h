#pragma once

#include <windows.h>
#include <wrl/client.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <commdlg.h>
#include <string>
#include <cstdint>
#include <algorithm>
#include <memory>
#include <vector>

using Microsoft::WRL::ComPtr;

// Link libraries
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "propsys.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "uxtheme.lib")

// ─────────────────────────────────────────────
// Layout Constants
// ─────────────────────────────────────────────
constexpr int PREVIEW_MAX_WIDTH  = 960;
constexpr int PREVIEW_MAX_HEIGHT = 720;
constexpr int PREVIEW_MIN_WIDTH  = 420;
constexpr int PREVIEW_MIN_HEIGHT = 320;
constexpr int TOOLBAR_HEIGHT     = 48;
constexpr int TITLEBAR_HEIGHT    = 44;
constexpr int CORNER_RADIUS      = 12;
constexpr int CLOSE_BTN_SIZE     = 32;
constexpr int CONTENT_PADDING    = 1;

// ─────────────────────────────────────────────
// Dark Theme Color Palette
// ─────────────────────────────────────────────
namespace Colors {
    constexpr D2D1_COLOR_F Background      = { 0.11f, 0.11f, 0.13f, 1.0f };
    constexpr D2D1_COLOR_F TitleBar        = { 0.09f, 0.09f, 0.11f, 1.0f };
    constexpr D2D1_COLOR_F Footer          = { 0.09f, 0.09f, 0.11f, 1.0f };
    constexpr D2D1_COLOR_F ContentBg       = { 0.06f, 0.06f, 0.08f, 1.0f };
    constexpr D2D1_COLOR_F Text            = { 0.93f, 0.93f, 0.95f, 1.0f };
    constexpr D2D1_COLOR_F TextDim         = { 0.55f, 0.55f, 0.60f, 1.0f };
    constexpr D2D1_COLOR_F TextMuted       = { 0.40f, 0.40f, 0.45f, 1.0f };
    constexpr D2D1_COLOR_F Accent          = { 0.35f, 0.55f, 1.00f, 1.0f };
    constexpr D2D1_COLOR_F AccentDim       = { 0.25f, 0.40f, 0.80f, 1.0f };
    constexpr D2D1_COLOR_F Border          = { 0.22f, 0.22f, 0.26f, 0.6f };
    constexpr D2D1_COLOR_F CloseHover      = { 0.90f, 0.22f, 0.22f, 1.0f };
    constexpr D2D1_COLOR_F Separator       = { 0.18f, 0.18f, 0.22f, 1.0f };
    constexpr D2D1_COLOR_F IconBg          = { 0.14f, 0.14f, 0.18f, 1.0f };
    constexpr D2D1_COLOR_F Badge           = { 0.20f, 0.20f, 0.25f, 1.0f };
    constexpr D2D1_COLOR_F CodeBg          = { 0.08f, 0.08f, 0.10f, 1.0f };
    constexpr D2D1_COLOR_F LineNumber      = { 0.30f, 0.30f, 0.35f, 1.0f };
    constexpr D2D1_COLOR_F ScrollTrack     = { 0.12f, 0.12f, 0.15f, 1.0f };
    constexpr D2D1_COLOR_F ScrollThumb     = { 0.30f, 0.30f, 0.35f, 0.8f };
}

// ─────────────────────────────────────────────
// Utility Functions
// ─────────────────────────────────────────────
namespace Utils {

    /// Format file attributes (e.g., "R H S A")
    std::wstring FormatAttributes(DWORD attributes);

    /// Ghi log debug ra file quickview.log để chẩn đoán phím và focus
    void Log(const wchar_t* fmt, ...);

    // Format file size to human readable (e.g. "2.4 MB")
    std::wstring FormatFileSize(uint64_t bytes);

    // Format seconds to time duration string (e.g. "01:23" or "1:02:15")
    std::wstring FormatDuration(double seconds);

    // Format FILETIME to readable string
    std::wstring FormatDateTime(const FILETIME& ft);

    // Get file extension (lowercase, with dot)
    std::wstring GetFileExtension(const std::wstring& path);

    // Get file name from full path
    std::wstring GetFileName(const std::wstring& path);

    // Get parent directory from path
    std::wstring GetDirectory(const std::wstring& path);

    // Get shell file icon (caller must DestroyIcon)
    HICON GetFileIcon(const std::wstring& path, bool largeIcon = true);

    // Get shell file type description
    std::wstring GetFileTypeName(const std::wstring& path);

    // Get DPI scale factor for a window
    float GetDpiScale(HWND hwnd);

    // Center a window on its monitor
    void CenterWindow(HWND hwnd, int width, int height);

    // Read video native dimensions via Shell property store in <1ms
    SIZE GetVideoDimensions(const std::wstring& path);

    // Read image native dimensions via Shell property store / WIC in <1ms
    SIZE GetImageDimensions(const std::wstring& path, IWICImagingFactory* wicFactory = nullptr);

    // Enable acrylic/blur background effect
    bool EnableAcrylicBlur(HWND hwnd);

    // Enable rounded corners (Windows 11)
    void EnableRoundedCorners(HWND hwnd);

    // Read text file with encoding detection using Win32 Memory-Mapped File (up to maxBytes)
    std::wstring ReadTextFile(const std::wstring& path, size_t maxBytes = 524288);

    // Create D2D bitmap from HICON
    ComPtr<ID2D1Bitmap> CreateBitmapFromIcon(
        HICON hIcon, ID2D1RenderTarget* rt, IWICImagingFactory* wic);

    // File information structure
    struct FileInfo {
        std::wstring name;
        std::wstring extension;
        std::wstring directory;
        std::wstring fullPath;
        std::wstring typeName;
        uint64_t     size = 0;
        FILETIME     creationTime = {};
        FILETIME     modifiedTime = {};
        FILETIME     accessTime   = {};
        DWORD        attributes   = 0;
        bool         isReadOnly   = false;
        bool         isHidden     = false;
        bool         isSystem     = false;
    };

    FileInfo GetFileInfo(const std::wstring& path);
    
    // Windows startup registry helpers
    bool IsStartWithWindowsEnabled();
    bool SetStartWithWindows(bool enable);
}
