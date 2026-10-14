#include "Utils.h"
#include <sstream>
#include <iomanip>
#include <propsys.h>
#include <propkey.h>
#include <propvarutil.h>
#pragma comment(lib, "propsys.lib")

namespace Utils {

// ─────────────────────────────────────────────
// String / Format Helpers
// ─────────────────────────────────────────────

std::wstring FormatFileSize(uint64_t bytes) {
    const wchar_t* units[] = { L"B", L"KB", L"MB", L"GB", L"TB" };
    int idx = 0;
    double size = static_cast<double>(bytes);
    while (size >= 1024.0 && idx < 4) {
        size /= 1024.0;
        idx++;
    }
    wchar_t buf[64];
    if (idx == 0)
        swprintf_s(buf, L"%.0f %s", size, units[idx]);
    else
        swprintf_s(buf, L"%.1f %s", size, units[idx]);
    return buf;
}

std::wstring FormatDuration(double seconds) {
    if (seconds < 0.0) seconds = 0.0;
    int totalSec = static_cast<int>(seconds);
    int h = totalSec / 3600;
    int m = (totalSec % 3600) / 60;
    int s = totalSec % 60;
    wchar_t buf[32];
    if (h > 0) {
        swprintf_s(buf, L"%d:%02d:%02d", h, m, s);
    } else {
        swprintf_s(buf, L"%02d:%02d", m, s);
    }
    return buf;
}

std::wstring FormatDateTime(const FILETIME& ft) {
    SYSTEMTIME st, local;
    FileTimeToSystemTime(&ft, &st);
    SystemTimeToTzSpecificLocalTime(nullptr, &st, &local);
    wchar_t buf[128];
    swprintf_s(buf, L"%02d/%02d/%04d  %02d:%02d",
        local.wDay, local.wMonth, local.wYear,
        local.wHour, local.wMinute);
    return buf;
}

std::wstring GetFileExtension(const std::wstring& path) {
    size_t dot = path.rfind(L'.');
    if (dot == std::wstring::npos) return L"";
    std::wstring ext = path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
    return ext;
}

std::wstring GetFileName(const std::wstring& path) {
    size_t sep = path.rfind(L'\\');
    if (sep == std::wstring::npos) sep = path.rfind(L'/');
    return (sep == std::wstring::npos) ? path : path.substr(sep + 1);
}

std::wstring GetDirectory(const std::wstring& path) {
    size_t sep = path.rfind(L'\\');
    if (sep == std::wstring::npos) sep = path.rfind(L'/');
    return (sep == std::wstring::npos) ? L"" : path.substr(0, sep);
}

// ─────────────────────────────────────────────
// Shell Helpers
// ─────────────────────────────────────────────

HICON GetFileIcon(const std::wstring& path, bool largeIcon) {
    SHFILEINFOW sfi = {};
    UINT flags = SHGFI_ICON | (largeIcon ? SHGFI_LARGEICON : SHGFI_SMALLICON);
    if (SHGetFileInfoW(path.c_str(), 0, &sfi, sizeof(sfi), flags)) {
        return sfi.hIcon;
    }
    return nullptr;
}

std::wstring GetFileTypeName(const std::wstring& path) {
    SHFILEINFOW sfi = {};
    if (SHGetFileInfoW(path.c_str(), 0, &sfi, sizeof(sfi), SHGFI_TYPENAME)) {
        return sfi.szTypeName;
    }
    return L"File";
}

float GetDpiScale(HWND hwnd) {
    HDC hdc = GetDC(hwnd);
    float dpi = static_cast<float>(GetDeviceCaps(hdc, LOGPIXELSX));
    ReleaseDC(hwnd, hdc);
    return dpi / 96.0f;
}

void CenterWindow(HWND hwnd, int width, int height) {
    HMONITOR mon = MonitorFromWindow(GetForegroundWindow(), MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfo(mon, &mi);
    int x = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - width) / 2;
    int y = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - height) / 2;
    SetWindowPos(hwnd, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE);
}

SIZE GetVideoDimensions(const std::wstring& path) {
    SIZE sz = { 0, 0 };
    ComPtr<IPropertyStore> store;
    HRESULT hr = SHGetPropertyStoreFromParsingName(
        path.c_str(), nullptr, GPS_DEFAULT, IID_PPV_ARGS(&store));
    if (SUCCEEDED(hr) && store) {
        PROPVARIANT propW = {}, propH = {};
        PropVariantInit(&propW);
        PropVariantInit(&propH);
        if (SUCCEEDED(store->GetValue(PKEY_Video_FrameWidth, &propW)) &&
            SUCCEEDED(store->GetValue(PKEY_Video_FrameHeight, &propH))) {
            ULONG w = 0, h = 0;
            if (SUCCEEDED(PropVariantToUInt32(propW, &w)) &&
                SUCCEEDED(PropVariantToUInt32(propH, &h))) {
                sz.cx = static_cast<LONG>(w);
                sz.cy = static_cast<LONG>(h);
            }
        }
        PropVariantClear(&propW);
        PropVariantClear(&propH);
    }
    return sz;
}

SIZE GetImageDimensions(const std::wstring& path, IWICImagingFactory* wicFactory) {
    SIZE sz = { 0, 0 };
    // 1. Try Windows Shell Property Store first (<0.2ms)
    ComPtr<IPropertyStore> store;
    if (SUCCEEDED(SHGetPropertyStoreFromParsingName(path.c_str(), nullptr, GPS_DEFAULT, IID_PPV_ARGS(&store))) && store) {
        PROPVARIANT propW = {}, propH = {};
        PropVariantInit(&propW);
        PropVariantInit(&propH);
        if (SUCCEEDED(store->GetValue(PKEY_Image_HorizontalSize, &propW)) &&
            SUCCEEDED(store->GetValue(PKEY_Image_VerticalSize, &propH))) {
            ULONG w = 0, h = 0;
            if (SUCCEEDED(PropVariantToUInt32(propW, &w)) &&
                SUCCEEDED(PropVariantToUInt32(propH, &h))) {
                sz.cx = static_cast<LONG>(w);
                sz.cy = static_cast<LONG>(h);
            }
        }
        PropVariantClear(&propW);
        PropVariantClear(&propH);
        if (sz.cx > 0 && sz.cy > 0) return sz;
    }

    // 2. Fallback: WIC metadata-only decoder (<0.2ms, does NOT decode pixels)
    if (wicFactory) {
        ComPtr<IWICBitmapDecoder> decoder;
        if (SUCCEEDED(wicFactory->CreateDecoderFromFilename(
                path.c_str(), nullptr, GENERIC_READ,
                WICDecodeMetadataCacheOnDemand, &decoder))) {
            ComPtr<IWICBitmapFrameDecode> frame;
            if (SUCCEEDED(decoder->GetFrame(0, &frame))) {
                UINT w = 0, h = 0;
                if (SUCCEEDED(frame->GetSize(&w, &h))) {
                    sz.cx = static_cast<LONG>(w);
                    sz.cy = static_cast<LONG>(h);
                }
            }
        }
    }
    return sz;
}

// ─────────────────────────────────────────────
// Window Effects
// ─────────────────────────────────────────────

// Undocumented SetWindowCompositionAttribute structures
enum ACCENT_STATE {
    ACCENT_DISABLED = 0,
    ACCENT_ENABLE_GRADIENT = 1,
    ACCENT_ENABLE_TRANSPARENTGRADIENT = 2,
    ACCENT_ENABLE_BLURBEHIND = 3,
    ACCENT_ENABLE_ACRYLICBLURBEHIND = 4,
    ACCENT_ENABLE_HOSTBACKDROP = 5,
};

struct ACCENT_POLICY {
    ACCENT_STATE AccentState;
    DWORD AccentFlags;
    DWORD GradientColor;
    DWORD AnimationId;
};

enum WINDOWCOMPOSITIONATTRIB {
    WCA_ACCENT_POLICY = 19
};

struct WINDOWCOMPOSITIONATTRIBDATA {
    WINDOWCOMPOSITIONATTRIB Attrib;
    PVOID pvData;
    SIZE_T cbData;
};

typedef BOOL(WINAPI* pfnSetWindowCompositionAttribute)(HWND, WINDOWCOMPOSITIONATTRIBDATA*);

bool EnableAcrylicBlur(HWND hwnd) {
    // Try Windows 11 system backdrop (Mica / Acrylic)
    // DWMWA_SYSTEMBACKDROP_TYPE = 38
    int backdropType = 3; // Acrylic
    HRESULT hr = DwmSetWindowAttribute(hwnd, 38, &backdropType, sizeof(backdropType));
    if (SUCCEEDED(hr)) return true;

    // Fallback: Windows 10 SetWindowCompositionAttribute
    HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
    if (!hUser32) return false;

    auto pSetWCA = reinterpret_cast<pfnSetWindowCompositionAttribute>(
        GetProcAddress(hUser32, "SetWindowCompositionAttribute"));
    if (!pSetWCA) return false;

    ACCENT_POLICY accent = {};
    accent.AccentState = ACCENT_ENABLE_ACRYLICBLURBEHIND;
    accent.GradientColor = 0xCC1A1A1E; // ABGR: dark translucent

    WINDOWCOMPOSITIONATTRIBDATA data = {};
    data.Attrib = WCA_ACCENT_POLICY;
    data.pvData = &accent;
    data.cbData = sizeof(accent);

    return pSetWCA(hwnd, &data) != FALSE;
}

void EnableRoundedCorners(HWND hwnd) {
    // DWMWA_WINDOW_CORNER_PREFERENCE = 33
    int cornerPref = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(hwnd, 33, &cornerPref, sizeof(cornerPref));
}

// ─────────────────────────────────────────────
// Text File Reading (Win32 Memory-Mapped File)
// ─────────────────────────────────────────────

std::wstring ReadTextFile(const std::wstring& path, size_t maxBytes) {
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return L"(Cannot read file)";

    LARGE_INTEGER fileSize;
    if (!GetFileSizeEx(hFile, &fileSize)) {
        CloseHandle(hFile);
        return L"(Cannot read file size)";
    }
    if (fileSize.QuadPart == 0) {
        CloseHandle(hFile);
        return L"(Empty file)";
    }

    size_t mapSize = (std::min)(static_cast<size_t>(fileSize.QuadPart), maxBytes);

    HANDLE hMapping = CreateFileMappingW(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!hMapping) {
        CloseHandle(hFile);
        return L"(Cannot create file mapping)";
    }

    const char* pData = static_cast<const char*>(MapViewOfFile(hMapping, FILE_MAP_READ, 0, 0, mapSize));
    if (!pData) {
        CloseHandle(hMapping);
        CloseHandle(hFile);
        return L"(Cannot map view of file)";
    }

    auto u = [](char c) -> unsigned char { return static_cast<unsigned char>(c); };

    std::wstring result;
    // UTF-8 BOM
    if (mapSize >= 3 && u(pData[0]) == 0xEF && u(pData[1]) == 0xBB && u(pData[2]) == 0xBF) {
        int wLen = MultiByteToWideChar(CP_UTF8, 0, pData + 3, static_cast<int>(mapSize - 3), nullptr, 0);
        if (wLen > 0) {
            result.resize(wLen);
            MultiByteToWideChar(CP_UTF8, 0, pData + 3, static_cast<int>(mapSize - 3), result.data(), wLen);
        }
    }
    // UTF-16 LE BOM
    else if (mapSize >= 2 && u(pData[0]) == 0xFF && u(pData[1]) == 0xFE) {
        result.assign(reinterpret_cast<const wchar_t*>(pData + 2), (mapSize - 2) / 2);
    }
    // UTF-16 BE BOM
    else if (mapSize >= 2 && u(pData[0]) == 0xFE && u(pData[1]) == 0xFF) {
        size_t chars = (mapSize - 2) / 2;
        result.resize(chars);
        const unsigned char* src = reinterpret_cast<const unsigned char*>(pData + 2);
        for (size_t i = 0; i < chars; ++i) {
            result[i] = static_cast<wchar_t>((src[i * 2] << 8) | src[i * 2 + 1]);
        }
    }
    else {
        // Try UTF-8 without BOM
        int wLen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, pData, static_cast<int>(mapSize), nullptr, 0);
        if (wLen > 0) {
            result.resize(wLen);
            MultiByteToWideChar(CP_UTF8, 0, pData, static_cast<int>(mapSize), result.data(), wLen);
        } else {
            // Fallback to ANSI / CP_ACP
            wLen = MultiByteToWideChar(CP_ACP, 0, pData, static_cast<int>(mapSize), nullptr, 0);
            if (wLen > 0) {
                result.resize(wLen);
                MultiByteToWideChar(CP_ACP, 0, pData, static_cast<int>(mapSize), result.data(), wLen);
            }
        }
    }

    if (fileSize.QuadPart > static_cast<LONGLONG>(maxBytes)) {
        result += L"\r\n\r\n[... File truncated. Showing first " + Utils::FormatFileSize(maxBytes) + L" of " + Utils::FormatFileSize(fileSize.QuadPart) + L" ...]";
    }

    UnmapViewOfFile(pData);
    CloseHandle(hMapping);
    CloseHandle(hFile);

    return result.empty() ? L"(Empty file)" : result;
}

// ─────────────────────────────────────────────
// Icon → D2D Bitmap
// ─────────────────────────────────────────────

ComPtr<ID2D1Bitmap> CreateBitmapFromIcon(
    HICON hIcon, ID2D1RenderTarget* rt, IWICImagingFactory* wic)
{
    if (!hIcon || !rt || !wic) return nullptr;

    ICONINFO iconInfo = {};
    if (!GetIconInfo(hIcon, &iconInfo)) return nullptr;

    BITMAP bm = {};
    GetObject(iconInfo.hbmColor ? iconInfo.hbmColor : iconInfo.hbmMask, sizeof(bm), &bm);

    int width  = bm.bmWidth;
    int height = bm.bmHeight;
    if (width <= 0 || height <= 0) {
        if (iconInfo.hbmColor) DeleteObject(iconInfo.hbmColor);
        if (iconInfo.hbmMask)  DeleteObject(iconInfo.hbmMask);
        return nullptr;
    }

    // Draw icon into a 32-bit DIB
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = width;
    bmi.bmiHeader.biHeight      = -height; // top-down
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HDC hdc = CreateCompatibleDC(nullptr);
    HBITMAP hDib = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ hOld = SelectObject(hdc, hDib);

    // Clear to transparent
    memset(bits, 0, width * height * 4);
    DrawIconEx(hdc, 0, 0, hIcon, width, height, 0, nullptr, DI_NORMAL);

    SelectObject(hdc, hOld);

    // Pre-multiply alpha (fix for GDI non-premultiplied output)
    auto* pixels = static_cast<BYTE*>(bits);
    for (int i = 0; i < width * height; i++) {
        BYTE a = pixels[i * 4 + 3];
        if (a == 0 && (pixels[i * 4] || pixels[i * 4 + 1] || pixels[i * 4 + 2])) {
            pixels[i * 4 + 3] = 255; // opaque if any color but no alpha
        }
    }

    // Create D2D bitmap directly from pixel data
    D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));

    ComPtr<ID2D1Bitmap> bitmap;
    rt->CreateBitmap(
        D2D1::SizeU(width, height),
        bits, width * 4,
        props, &bitmap);

    DeleteObject(hDib);
    DeleteDC(hdc);
    if (iconInfo.hbmColor) DeleteObject(iconInfo.hbmColor);
    if (iconInfo.hbmMask)  DeleteObject(iconInfo.hbmMask);

    return bitmap;
}

// ─────────────────────────────────────────────
// File Information
// ─────────────────────────────────────────────

FileInfo GetFileInfo(const std::wstring& path) {
    FileInfo info;
    info.fullPath  = path;
    info.name      = GetFileName(path);
    info.extension = GetFileExtension(path);
    info.directory = GetDirectory(path);
    info.typeName  = GetFileTypeName(path);

    WIN32_FILE_ATTRIBUTE_DATA fad = {};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) {
        info.size         = (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
        info.creationTime = fad.ftCreationTime;
        info.modifiedTime = fad.ftLastWriteTime;
        info.accessTime   = fad.ftLastAccessTime;
        info.attributes   = fad.dwFileAttributes;
        info.isReadOnly   = (fad.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0;
        info.isHidden     = (fad.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) != 0;
        info.isSystem     = (fad.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM) != 0;
    }
    return info;
}

void Log(const wchar_t* fmt, ...) {
    wchar_t buf[1024];
    va_list args;
    va_start(args, fmt);
    vswprintf_s(buf, fmt, args);
    va_end(args);

    OutputDebugStringW(buf);

    static const wchar_t* kLogPath = L"C:\\Users\\Vuthao\\Desktop\\quickview\\quickview.log";
    static unsigned int s_logCallCount = 0;
    const wchar_t* openMode = L"a, ccs=UTF-8";

    if ((s_logCallCount++ & 0xFF) == 0) {
        WIN32_FILE_ATTRIBUTE_DATA fad = {};
        if (GetFileAttributesExW(kLogPath, GetFileExInfoStandard, &fad)) {
            if (fad.nFileSizeHigh > 0 || fad.nFileSizeLow > 512 * 1024) {
                openMode = L"w, ccs=UTF-8";
            }
        }
    }

    FILE* f = nullptr;
    if (_wfopen_s(&f, kLogPath, openMode) == 0 && f) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fwprintf(f, L"[%02d:%02d:%02d.%03d] %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, buf);
        fclose(f);
    }
}

bool IsStartWithWindowsEnabled() {
    wchar_t startupPath[MAX_PATH] = {};
    if (SHGetSpecialFolderPathW(nullptr, startupPath, CSIDL_STARTUP, FALSE)) {
        std::wstring linkPath = std::wstring(startupPath) + L"\\QuickView.lnk";
        if (GetFileAttributesW(linkPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
            return true;
        }
    }

    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        return false;
    }
    DWORD type = 0;
    DWORD size = 0;
    LONG res = RegQueryValueExW(hKey, L"QuickView", nullptr, &type, nullptr, &size);
    RegCloseKey(hKey);
    return (res == ERROR_SUCCESS && size > 0);
}

bool SetStartWithWindows(bool enable) {
    wchar_t startupPath[MAX_PATH] = {};
    if (SHGetSpecialFolderPathW(nullptr, startupPath, CSIDL_STARTUP, FALSE)) {
        std::wstring linkPath = std::wstring(startupPath) + L"\\QuickView.lnk";
        if (!enable) {
            DeleteFileW(linkPath.c_str());
        }
    }

    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &hKey) != ERROR_SUCCESS) {
        return false;
    }

    LONG res = ERROR_SUCCESS;
    if (enable) {
        wchar_t exePath[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        std::wstring quotedPath = L"\"" + std::wstring(exePath) + L"\"";
        res = RegSetValueExW(hKey, L"QuickView", 0, REG_SZ,
                             reinterpret_cast<const BYTE*>(quotedPath.c_str()),
                             static_cast<DWORD>((quotedPath.length() + 1) * sizeof(wchar_t)));
    } else {
        res = RegDeleteValueW(hKey, L"QuickView");
        RegDeleteValueW(hKey, L"quickview");
    }
    RegCloseKey(hKey);
    return (res == ERROR_SUCCESS || (!enable && res == ERROR_FILE_NOT_FOUND));
}

} // namespace Utils
