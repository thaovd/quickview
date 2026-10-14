#include "VideoPreview.h"
#include "Resource.h"
#include <algorithm>
#include <vector>
#include <cmath>

HMODULE VideoPreview::s_hMpvDll       = nullptr;
bool    VideoPreview::s_dllLoaded     = false;
bool    VideoPreview::s_dllLoadFailed = false;

VideoPreview::pfn_mpv_create             VideoPreview::fn_mpv_create             = nullptr;
VideoPreview::pfn_mpv_initialize         VideoPreview::fn_mpv_initialize         = nullptr;
VideoPreview::pfn_mpv_destroy            VideoPreview::fn_mpv_destroy            = nullptr;
VideoPreview::pfn_mpv_terminate_destroy  VideoPreview::fn_mpv_terminate_destroy  = nullptr;
VideoPreview::pfn_mpv_command            VideoPreview::fn_mpv_command            = nullptr;
VideoPreview::pfn_mpv_command_string     VideoPreview::fn_mpv_command_string     = nullptr;
VideoPreview::pfn_mpv_set_option         VideoPreview::fn_mpv_set_option         = nullptr;
VideoPreview::pfn_mpv_set_option_string  VideoPreview::fn_mpv_set_option_string  = nullptr;
VideoPreview::pfn_mpv_get_property       VideoPreview::fn_mpv_get_property       = nullptr;
VideoPreview::pfn_mpv_get_property_string VideoPreview::fn_mpv_get_property_string = nullptr;
VideoPreview::pfn_mpv_set_property       VideoPreview::fn_mpv_set_property       = nullptr;
VideoPreview::pfn_mpv_set_property_string VideoPreview::fn_mpv_set_property_string = nullptr;
VideoPreview::pfn_mpv_wait_event         VideoPreview::fn_mpv_wait_event         = nullptr;
VideoPreview::pfn_mpv_free               VideoPreview::fn_mpv_free               = nullptr;

static const wchar_t* VIDEO_HOST_CLASS = L"QuickViewVideoHost";

static LRESULT CALLBACK VideoHostWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_LBUTTONDOWN:
        // Gửi thông điệp nhấp chuột lên cửa sổ cha để bật/tắt Play/Pause
        SendMessageW(GetParent(hwnd), WM_APP_VIDEO_CLICK, 0, 0);
        return 0;
    case WM_LBUTTONDBLCLK:
        // Gửi thông điệp nhấp đúp chuột lên cửa sổ cha để bật/tắt Full Screen
        SendMessageW(GetParent(hwnd), WM_LBUTTONDBLCLK, wp, lp);
        return 0;
    case WM_RBUTTONUP:
        // Gửi thông điệp nhấp chuột phải lên cửa sổ cha để mở context menu (khi pause)
        SendMessageW(GetParent(hwnd), WM_APP_VIDEO_RCLICK, wp, lp);
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        // Chuyển tiếp phím bấm lên cửa sổ cha (phím Space, Esc, F11, mũi tên, v.v.)
        SendMessageW(GetParent(hwnd), msg, wp, lp);
        return 0;
    case WM_ERASEBKGND:
        return 1; // Ngăn chớp nháy nền
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

VideoPreview::VideoPreview() = default;

VideoPreview::~VideoPreview() {
    Reset();
    if (m_videoWnd) {
        DestroyWindow(m_videoWnd);
        m_videoWnd = nullptr;
    }
}

bool VideoPreview::LoadMpvLibrary() {
    if (s_dllLoaded) return true;
    if (s_dllLoadFailed) return false;

    // Các đường dẫn tìm kiếm DLL ưu tiên:
    // 1. Cùng thư mục với file exe đang chạy
    // 2. Thư mục third_party/mpv
    // 3. Đường dẫn hệ thống
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring exeDir = Utils::GetDirectory(exePath);

    std::vector<std::wstring> searchPaths = {
        exeDir + L"\\libmpv-2.dll",
        exeDir + L"\\mpv-2.dll",
        exeDir + L"\\third_party\\mpv\\libmpv-2.dll",
        L"C:\\Users\\Vuthao\\Desktop\\quickview\\third_party\\mpv\\libmpv-2.dll",
        L"libmpv-2.dll",
        L"mpv-2.dll"
    };

    for (const auto& path : searchPaths) {
        s_hMpvDll = LoadLibraryW(path.c_str());
        if (s_hMpvDll) {
            Utils::Log(L"[VideoPreview] Successfully loaded libmpv from: %s", path.c_str());
            break;
        }
    }

    if (!s_hMpvDll) {
        Utils::Log(L"[VideoPreview] Failed to load libmpv-2.dll (GetLastError=%lu)", GetLastError());
        s_dllLoadFailed = true;
        return false;
    }

    // Resolve function pointers
    fn_mpv_create             = reinterpret_cast<pfn_mpv_create>(GetProcAddress(s_hMpvDll, "mpv_create"));
    fn_mpv_initialize         = reinterpret_cast<pfn_mpv_initialize>(GetProcAddress(s_hMpvDll, "mpv_initialize"));
    fn_mpv_destroy            = reinterpret_cast<pfn_mpv_destroy>(GetProcAddress(s_hMpvDll, "mpv_destroy"));
    fn_mpv_terminate_destroy  = reinterpret_cast<pfn_mpv_terminate_destroy>(GetProcAddress(s_hMpvDll, "mpv_terminate_destroy"));
    fn_mpv_command            = reinterpret_cast<pfn_mpv_command>(GetProcAddress(s_hMpvDll, "mpv_command"));
    fn_mpv_command_string     = reinterpret_cast<pfn_mpv_command_string>(GetProcAddress(s_hMpvDll, "mpv_command_string"));
    fn_mpv_set_option         = reinterpret_cast<pfn_mpv_set_option>(GetProcAddress(s_hMpvDll, "mpv_set_option"));
    fn_mpv_set_option_string  = reinterpret_cast<pfn_mpv_set_option_string>(GetProcAddress(s_hMpvDll, "mpv_set_option_string"));
    fn_mpv_get_property       = reinterpret_cast<pfn_mpv_get_property>(GetProcAddress(s_hMpvDll, "mpv_get_property"));
    fn_mpv_get_property_string = reinterpret_cast<pfn_mpv_get_property_string>(GetProcAddress(s_hMpvDll, "mpv_get_property_string"));
    fn_mpv_set_property       = reinterpret_cast<pfn_mpv_set_property>(GetProcAddress(s_hMpvDll, "mpv_set_property"));
    fn_mpv_set_property_string = reinterpret_cast<pfn_mpv_set_property_string>(GetProcAddress(s_hMpvDll, "mpv_set_property_string"));
    fn_mpv_wait_event         = reinterpret_cast<pfn_mpv_wait_event>(GetProcAddress(s_hMpvDll, "mpv_wait_event"));
    fn_mpv_free               = reinterpret_cast<pfn_mpv_free>(GetProcAddress(s_hMpvDll, "mpv_free"));

    if (!fn_mpv_create || !fn_mpv_initialize || !fn_mpv_command ||
        !fn_mpv_set_option || !fn_mpv_set_option_string) {
        Utils::Log(L"[VideoPreview] Failed to resolve required libmpv exports.");
        FreeLibrary(s_hMpvDll);
        s_hMpvDll = nullptr;
        s_dllLoadFailed = true;
        return false;
    }

    s_dllLoaded = true;
    return true;
}

bool VideoPreview::IsAvailable() {
    return LoadMpvLibrary();
}

bool VideoPreview::Init(HWND parentHwnd, HINSTANCE hInst) {
    m_parentWnd = parentHwnd;
    m_hInst     = hInst;

    if (!LoadMpvLibrary()) return false;

    // Đăng ký window class cho cửa sổ hiển thị video nếu chưa có
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc   = VideoHostWndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = VIDEO_HOST_CLASS;
    RegisterClassExW(&wc);

    // Tạo cửa sổ con hiển thị video
    if (!m_videoWnd) {
        m_videoWnd = CreateWindowExW(
            0, VIDEO_HOST_CLASS, L"",
            WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
            0, 0, 100, 100,
            m_parentWnd, nullptr, m_hInst, nullptr);
    }

    return m_videoWnd != nullptr;
}

bool VideoPreview::Load(const std::wstring& filePath) {
    if (!m_videoWnd || !s_dllLoaded) return false;

    // Nếu đang phát video khác thì dọn dẹp trước
    Reset();

    m_currentPath = filePath;
    m_vidWidth    = 0;
    m_vidHeight   = 0;
    m_isAudio     = false;

    // Tạo instance MPV mới
    m_mpv = fn_mpv_create();
    if (!m_mpv) return false;

    // Cấu hình nhúng HWND vào cửa sổ con
    int64_t wid = reinterpret_cast<intptr_t>(m_videoWnd);
    fn_mpv_set_option(m_mpv, "wid", MPV_FORMAT_INT64, &wid);

    // Bật card đồ họa GPU giải mã phần cứng siêu tốc
    fn_mpv_set_option_string(m_mpv, "vo", "gpu");
    fn_mpv_set_option_string(m_mpv, "hwdec", "auto-safe");

    // Lặp vô tận (Auto-loop) chuẩn Quick Look
    fn_mpv_set_option_string(m_mpv, "loop-file", "inf");
    fn_mpv_set_option_string(m_mpv, "keep-open", "yes");

    // Tắt bắt phím/chuột của mpv để nhường toàn quyền cho QuickView
    fn_mpv_set_option_string(m_mpv, "input-default-bindings", "no");
    fn_mpv_set_option_string(m_mpv, "input-vo-keyboard", "no");
    fn_mpv_set_option_string(m_mpv, "force-window", "immediate");

    int initRes = fn_mpv_initialize(m_mpv);
    if (initRes < 0) {
        Utils::Log(L"[VideoPreview] mpv_initialize failed: %d", initRes);
        fn_mpv_destroy(m_mpv);
        m_mpv = nullptr;
        return false;
    }

    // Chuyển đường dẫn sang UTF-8
    int u8Len = WideCharToMultiByte(CP_UTF8, 0, filePath.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string u8Path(u8Len > 1 ? u8Len - 1 : 0, '\0');
    if (u8Len > 1) {
        WideCharToMultiByte(CP_UTF8, 0, filePath.c_str(), -1, &u8Path[0], u8Len, nullptr, nullptr);
    }

    const char* cmd[] = { "loadfile", u8Path.c_str(), nullptr };
    fn_mpv_command(m_mpv, cmd);

    // Đợi tối đa ~150ms để đọc độ phân giải thực của video
    bool hasVideo = false;
    for (int i = 0; i < 15; i++) {
        fn_mpv_wait_event(m_mpv, 0.01);
        int64_t w = 0, h = 0;
        if (fn_mpv_get_property(m_mpv, "video-params/w", MPV_FORMAT_INT64, &w) >= 0 && w > 0 &&
            fn_mpv_get_property(m_mpv, "video-params/h", MPV_FORMAT_INT64, &h) >= 0 && h > 0) {
            m_vidWidth  = static_cast<int>(w);
            m_vidHeight = static_cast<int>(h);
            hasVideo = true;
            break;
        }
    }

    if (!hasVideo) {
        m_isAudio = true;
        m_vidWidth  = 520;
        m_vidHeight = 360;
        ShowWindow(m_videoWnd, SW_HIDE);
    } else {
        m_isAudio = false;
        ShowWindow(m_videoWnd, SW_SHOW);
    }

    Utils::Log(L"[VideoPreview] Load succeeded for '%s', isAudio=%d, res=%dx%d",
               filePath.c_str(), m_isAudio ? 1 : 0, m_vidWidth, m_vidHeight);
    return true;
}

void VideoPreview::Reset() {
    if (m_mpv) {
        if (fn_mpv_terminate_destroy) {
            fn_mpv_terminate_destroy(m_mpv);
        } else if (fn_mpv_destroy) {
            fn_mpv_destroy(m_mpv);
        }
        m_mpv = nullptr;

        // mpv_terminate_destroy may call CoUninitialize internally,
        // which unbalances the COM ref count on the STA thread.
        // Re-initialize to restore COM availability for IShellWindows queries.
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    }
    if (m_videoWnd) {
        ShowWindow(m_videoWnd, SW_HIDE);
    }
    m_currentPath.clear();
    m_vidWidth  = 0;
    m_vidHeight = 0;
    m_isAudio   = false;
}

void VideoPreview::Play() {
    if (m_mpv) {
        int pause = 0;
        fn_mpv_set_property(m_mpv, "pause", MPV_FORMAT_FLAG, &pause);
    }
}

void VideoPreview::Pause() {
    if (m_mpv) {
        int pause = 1;
        fn_mpv_set_property(m_mpv, "pause", MPV_FORMAT_FLAG, &pause);
    }
}

void VideoPreview::TogglePause() {
    if (m_mpv) {
        const char* cmd[] = { "cycle", "pause", nullptr };
        fn_mpv_command(m_mpv, cmd);
    }
}

void VideoPreview::Seek(double targetSeconds) {
    if (m_mpv) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.2f", targetSeconds);
        const char* cmd[] = { "seek", buf, "absolute", nullptr };
        fn_mpv_command(m_mpv, cmd);
    }
}

void VideoPreview::SeekPercent(double percent01) {
    if (m_mpv) {
        percent01 = (std::max)(0.0, (std::min)(percent01, 1.0));
        double dur = GetDuration();
        if (dur > 0.0) {
            Seek(dur * percent01);
        }
    }
}

void VideoPreview::ToggleMute() {
    if (m_mpv) {
        const char* cmd[] = { "cycle", "mute", nullptr };
        fn_mpv_command(m_mpv, cmd);
    }
}

void VideoPreview::SetMute(bool mute) {
    if (m_mpv) {
        int m = mute ? 1 : 0;
        fn_mpv_set_property(m_mpv, "mute", MPV_FORMAT_FLAG, &m);
    }
}

bool VideoPreview::IsPlaying() const {
    if (!m_mpv) return false;
    int pause = 0;
    if (fn_mpv_get_property(m_mpv, "pause", MPV_FORMAT_FLAG, &pause) >= 0) {
        return pause == 0;
    }
    return true;
}

bool VideoPreview::IsMuted() const {
    if (!m_mpv) return false;
    int mute = 0;
    if (fn_mpv_get_property(m_mpv, "mute", MPV_FORMAT_FLAG, &mute) >= 0) {
        return mute != 0;
    }
    return false;
}

double VideoPreview::GetPosition() const {
    if (!m_mpv) return 0.0;
    double pos = 0.0;
    if (fn_mpv_get_property(m_mpv, "time-pos", MPV_FORMAT_DOUBLE, &pos) >= 0) {
        return pos;
    }
    return 0.0;
}

double VideoPreview::GetDuration() const {
    if (!m_mpv) return 0.0;
    double dur = 0.0;
    if (fn_mpv_get_property(m_mpv, "duration", MPV_FORMAT_DOUBLE, &dur) >= 0) {
        return dur;
    }
    return 0.0;
}

std::wstring VideoPreview::GetTimecode() const {
    if (!m_mpv) return L"";

    double pos = GetPosition();
    if (pos < 0.0) pos = 0.0;
    double dur = GetDuration();

    // Với video, audio dài trên 1 giờ thì timecode là hh:mm:ss, nhỏ hơn là mm:ss
    bool isOverOneHour = (dur >= 3600.0) || (pos >= 3600.0);

    int totalSec = static_cast<int>(pos);
    int h = totalSec / 3600;
    int m = (totalSec % 3600) / 60;
    int s = totalSec % 60;

    wchar_t buf[32];
    if (isOverOneHour) {
        swprintf_s(buf, L"%02d:%02d:%02d", h, m, s);
    } else {
        swprintf_s(buf, L"%02d:%02d", m, s);
    }
    return buf;
}

void VideoPreview::GetVideoResolution(int& outW, int& outH) const {
    outW = m_vidWidth;
    outH = m_vidHeight;
}

SIZE VideoPreview::GetPreferredSize(int maxW, int maxH) const {
    SIZE sz = { 800, 450 };
    if (m_vidWidth > 0 && m_vidHeight > 0) {
        sz.cx = m_vidWidth;
        sz.cy = m_vidHeight;
    }

    // Co dãn vừa vặn trong giới hạn maxW, maxH
    float aspect = static_cast<float>(sz.cx) / static_cast<float>(sz.cy);
    if (sz.cx > maxW) {
        sz.cx = maxW;
        sz.cy = static_cast<LONG>(maxW / aspect);
    }
    if (sz.cy > maxH) {
        sz.cy = maxH;
        sz.cx = static_cast<LONG>(maxH * aspect);
    }

    sz.cx = (std::max)(static_cast<LONG>(PREVIEW_MIN_WIDTH), sz.cx);
    sz.cy = (std::max)(static_cast<LONG>(PREVIEW_MIN_HEIGHT), sz.cy);

    return sz;
}

void VideoPreview::Resize(const RECT& rect) {
    if (m_videoWnd) {
        if (m_isAudio) {
            ShowWindow(m_videoWnd, SW_HIDE);
        } else {
            MoveWindow(m_videoWnd, rect.left, rect.top,
                       rect.right - rect.left, rect.bottom - rect.top, TRUE);
            ShowWindow(m_videoWnd, SW_SHOW);
        }
    }
}

void VideoPreview::Show() {
    if (m_videoWnd && !m_isAudio) {
        ShowWindow(m_videoWnd, SW_SHOW);
    }
}

void VideoPreview::Hide() {
    if (m_videoWnd) {
        ShowWindow(m_videoWnd, SW_HIDE);
    }
}

bool VideoPreview::SaveScreenshot(const std::wstring& filePath) {
    if (!m_mpv || filePath.empty() || !fn_mpv_command) return false;

    int u8Len = WideCharToMultiByte(CP_UTF8, 0, filePath.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (u8Len <= 1) return false;
    std::string u8Path(u8Len - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, filePath.c_str(), -1, &u8Path[0], u8Len, nullptr, nullptr);

    // Lệnh mpv screenshot-to-file <filename> video (chụp khung hình gốc không dính phụ đề/OSD)
    const char* cmd[] = { "screenshot-to-file", u8Path.c_str(), "video", nullptr };
    int res = fn_mpv_command(m_mpv, cmd);
    Utils::Log(L"[VideoPreview] screenshot-to-file '%s' result: %d", filePath.c_str(), res);
    return res >= 0;
}
