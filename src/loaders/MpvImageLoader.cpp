#include "MpvImageLoader.h"
#include "Utils.h"
#include <mpv/client.h>
#include <vector>
#include <algorithm>

namespace MpvImageLoader {

typedef mpv_handle* (*pfn_mpv_create)();
typedef int         (*pfn_mpv_initialize)(mpv_handle*);
typedef void        (*pfn_mpv_terminate_destroy)(mpv_handle*);
typedef int         (*pfn_mpv_command)(mpv_handle*, const char**);
typedef int         (*pfn_mpv_command_node)(mpv_handle*, mpv_node*, mpv_node*);
typedef void        (*pfn_mpv_free_node_contents)(mpv_node*);
typedef int         (*pfn_mpv_set_option)(mpv_handle*, const char*, mpv_format, void*);
typedef int         (*pfn_mpv_set_option_string)(mpv_handle*, const char*, const char*);
typedef int         (*pfn_mpv_get_property)(mpv_handle*, const char*, mpv_format, void*);
typedef mpv_event*  (*pfn_mpv_wait_event)(mpv_handle*, double);

static HMODULE s_hMpvDll = nullptr;
static pfn_mpv_create             fn_mpv_create             = nullptr;
static pfn_mpv_initialize         fn_mpv_initialize         = nullptr;
static pfn_mpv_terminate_destroy  fn_mpv_terminate_destroy  = nullptr;
static pfn_mpv_command            fn_mpv_command            = nullptr;
static pfn_mpv_command_node       fn_mpv_command_node       = nullptr;
static pfn_mpv_free_node_contents fn_mpv_free_node_contents = nullptr;
static pfn_mpv_set_option         fn_mpv_set_option         = nullptr;
static pfn_mpv_set_option_string  fn_mpv_set_option_string  = nullptr;
static pfn_mpv_get_property       fn_mpv_get_property       = nullptr;
static pfn_mpv_wait_event         fn_mpv_wait_event         = nullptr;

static bool InitMpvLibrary() {
    if (s_hMpvDll) return true;
    static bool s_dllLoadFailed = false;
    if (s_dllLoadFailed) return false;

    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring exeDir = Utils::GetDirectory(exePath);

    std::vector<std::wstring> searchPaths = {
        exeDir + L"\\libmpv-2.dll",
        exeDir + L"\\mpv-2.dll",
        exeDir + L"\\third_party\\mpv\\libmpv-2.dll",
        L"libmpv-2.dll",
        L"mpv-2.dll"
    };

    for (const auto& p : searchPaths) {
        s_hMpvDll = LoadLibraryW(p.c_str());
        if (s_hMpvDll) break;
    }

    if (!s_hMpvDll) {
        s_dllLoadFailed = true;
        return false;
    }

    fn_mpv_create             = reinterpret_cast<pfn_mpv_create>(GetProcAddress(s_hMpvDll, "mpv_create"));
    fn_mpv_initialize         = reinterpret_cast<pfn_mpv_initialize>(GetProcAddress(s_hMpvDll, "mpv_initialize"));
    fn_mpv_terminate_destroy  = reinterpret_cast<pfn_mpv_terminate_destroy>(GetProcAddress(s_hMpvDll, "mpv_terminate_destroy"));
    fn_mpv_command            = reinterpret_cast<pfn_mpv_command>(GetProcAddress(s_hMpvDll, "mpv_command"));
    fn_mpv_command_node       = reinterpret_cast<pfn_mpv_command_node>(GetProcAddress(s_hMpvDll, "mpv_command_node"));
    fn_mpv_free_node_contents = reinterpret_cast<pfn_mpv_free_node_contents>(GetProcAddress(s_hMpvDll, "mpv_free_node_contents"));
    fn_mpv_set_option         = reinterpret_cast<pfn_mpv_set_option>(GetProcAddress(s_hMpvDll, "mpv_set_option"));
    fn_mpv_set_option_string  = reinterpret_cast<pfn_mpv_set_option_string>(GetProcAddress(s_hMpvDll, "mpv_set_option_string"));
    fn_mpv_get_property       = reinterpret_cast<pfn_mpv_get_property>(GetProcAddress(s_hMpvDll, "mpv_get_property"));
    fn_mpv_wait_event         = reinterpret_cast<pfn_mpv_wait_event>(GetProcAddress(s_hMpvDll, "mpv_wait_event"));

    if (!fn_mpv_create || !fn_mpv_initialize || !fn_mpv_terminate_destroy ||
        !fn_mpv_command || !fn_mpv_command_node || !fn_mpv_free_node_contents ||
        !fn_mpv_set_option || !fn_mpv_set_option_string) {
        FreeLibrary(s_hMpvDll);
        s_hMpvDll = nullptr;
        s_dllLoadFailed = true;
        return false;
    }

    return true;
}

bool IsMpvImageExtension(const std::wstring& ext) {
    return ext == L".heic" || ext == L".heif" || ext == L".avif";
}

Microsoft::WRL::ComPtr<IWICFormatConverter> Load(
    const std::wstring& path,
    IWICImagingFactory* wicFactory,
    UINT& outWidth,
    UINT& outHeight)
{
    outWidth  = 0;
    outHeight = 0;
    if (!wicFactory || path.empty()) return nullptr;
    Utils::Log(L"[MpvImageLoader] Load called for '%s'", path.c_str());

    if (!InitMpvLibrary()) {
        Utils::Log(L"[MpvImageLoader] libmpv-2.dll not available");
        return nullptr;
    }

    // Tạo cửa sổ tĩnh ẩn làm WID host cho mpv GPU context
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"", WS_POPUP,
                                0, 0, 64, 64, nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
    if (!hwnd) {
        Utils::Log(L"[MpvImageLoader] Failed to create host window");
        return nullptr;
    }

    mpv_handle* mpv = fn_mpv_create();
    if (!mpv) {
        DestroyWindow(hwnd);
        return nullptr;
    }

    int64_t wid = reinterpret_cast<intptr_t>(hwnd);
    fn_mpv_set_option(mpv, "wid", MPV_FORMAT_INT64, &wid);
    fn_mpv_set_option_string(mpv, "vo", "gpu");
    fn_mpv_set_option_string(mpv, "hwdec", "auto-safe");
    fn_mpv_set_option_string(mpv, "image-display-duration", "inf");
    fn_mpv_set_option_string(mpv, "loop-file", "inf");
    fn_mpv_set_option_string(mpv, "keep-open", "yes");
    fn_mpv_set_option_string(mpv, "input-default-bindings", "no");
    fn_mpv_set_option_string(mpv, "input-vo-keyboard", "no");

    int initRes = fn_mpv_initialize(mpv);
    if (initRes < 0) {
        Utils::Log(L"[MpvImageLoader] mpv_initialize failed: %d", initRes);
        fn_mpv_terminate_destroy(mpv);
        DestroyWindow(hwnd);
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        return nullptr;
    }

    // Chuyển đường dẫn sang UTF-8
    int u8Len = WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string u8Path(u8Len > 1 ? u8Len - 1 : 0, '\0');
    if (u8Len > 1) {
        WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, &u8Path[0], u8Len, nullptr, nullptr);
    }

    const char* cmd[] = { "loadfile", u8Path.c_str(), nullptr };
    fn_mpv_command(mpv, cmd);

    // Chờ tối đa ~1.5 giây để mpv demux và giải mã frame đầu tiên
    int64_t imgW = 0, imgH = 0;
    for (int i = 0; i < 75; i++) {
        fn_mpv_wait_event(mpv, 0.02);
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (fn_mpv_get_property &&
            fn_mpv_get_property(mpv, "video-params/w", MPV_FORMAT_INT64, &imgW) >= 0 && imgW > 0 &&
            fn_mpv_get_property(mpv, "video-params/h", MPV_FORMAT_INT64, &imgH) >= 0 && imgH > 0) {
            if (i >= 5) break; // Đảm bảo D3D context render xong ít nhất 1 frame
        }
    }

    if (imgW <= 0 || imgH <= 0) {
        Utils::Log(L"[MpvImageLoader] Timed out waiting for image resolution: %s", path.c_str());
        fn_mpv_terminate_destroy(mpv);
        DestroyWindow(hwnd);
        return nullptr;
    }

    // Gọi lệnh screenshot-raw video để lấy mảng raw pixel trực tiếp
    mpv_node_list args;
    mpv_node argNodes[2];
    argNodes[0].format = MPV_FORMAT_STRING;
    argNodes[0].u.string = const_cast<char*>("screenshot-raw");
    argNodes[1].format = MPV_FORMAT_STRING;
    argNodes[1].u.string = const_cast<char*>("video");
    args.num = 2;
    args.values = argNodes;
    args.keys = nullptr;

    mpv_node cmdNode;
    cmdNode.format = MPV_FORMAT_NODE_ARRAY;
    cmdNode.u.list = &args;

    mpv_node result;
    int nodeRes = fn_mpv_command_node(mpv, &cmdNode, &result);

    Microsoft::WRL::ComPtr<IWICFormatConverter> converter;

    if (nodeRes >= 0 && result.format == MPV_FORMAT_NODE_MAP) {
        uint8_t* rawBytes = nullptr;
        size_t sz = 0;
        int64_t w = 0, h = 0, stride = 0;

        for (int k = 0; k < result.u.list->num; k++) {
            const char* key = result.u.list->keys[k];
            if (!key) continue;
            if (strcmp(key, "data") == 0) {
                rawBytes = reinterpret_cast<uint8_t*>(result.u.list->values[k].u.ba->data);
                sz = result.u.list->values[k].u.ba->size;
            } else if (strcmp(key, "w") == 0) {
                w = result.u.list->values[k].u.int64;
            } else if (strcmp(key, "h") == 0) {
                h = result.u.list->values[k].u.int64;
            } else if (strcmp(key, "stride") == 0) {
                stride = result.u.list->values[k].u.int64;
            }
        }

        if (rawBytes && sz > 0 && w > 0 && h > 0 && stride > 0) {
            // Định dạng trả về là bgr0 (kênh alpha = 0). Cần đặt alpha = 255 (0xFF) cho PBGRA
            for (size_t i = 3; i < sz; i += 4) {
                rawBytes[i] = 0xFF;
            }

            Microsoft::WRL::ComPtr<IWICBitmap> wicBitmap;
            HRESULT hr = wicFactory->CreateBitmapFromMemory(
                static_cast<UINT>(w), static_cast<UINT>(h),
                GUID_WICPixelFormat32bppPBGRA,
                static_cast<UINT>(stride),
                static_cast<UINT>(sz),
                rawBytes,
                &wicBitmap);

            if (SUCCEEDED(hr) && wicBitmap) {
                hr = wicFactory->CreateFormatConverter(&converter);
                if (SUCCEEDED(hr) && converter) {
                    hr = converter->Initialize(
                        wicBitmap.Get(),
                        GUID_WICPixelFormat32bppPBGRA,
                        WICBitmapDitherTypeNone,
                        nullptr, 0.0,
                        WICBitmapPaletteTypeMedianCut);

                    if (SUCCEEDED(hr)) {
                        outWidth  = static_cast<UINT>(w);
                        outHeight = static_cast<UINT>(h);
                        Utils::Log(L"[MpvImageLoader] Successfully decoded image '%s' via libmpv (%ux%u)",
                                   path.c_str(), outWidth, outHeight);
                    } else {
                        converter.Reset();
                    }
                }
            }
        }

        fn_mpv_free_node_contents(&result);
    } else {
        Utils::Log(L"[MpvImageLoader] screenshot-raw failed: %d", nodeRes);
    }

    fn_mpv_terminate_destroy(mpv);
    DestroyWindow(hwnd);

    // Phục hồi lại COM nếu mpv_terminate_destroy vô tình làm giảm COM ref
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    return converter;
}

} // namespace MpvImageLoader
