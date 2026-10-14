#pragma once

#include "Utils.h"
#include <mpv/client.h>
#include <string>
#include <memory>

/// Quản lý phát video/media sử dụng thư viện libmpv nhúng trực tiếp (Hardware Accelerated).
class VideoPreview {
public:
    VideoPreview();
    ~VideoPreview();

    /// Khởi tạo thư viện libmpv (dynamic loading) và tạo cửa sổ host
    bool Init(HWND parentHwnd, HINSTANCE hInst);

    /// Tải và bắt đầu phát file video
    bool Load(const std::wstring& filePath);

    /// Dừng phát và dọn dẹp tài nguyên
    void Reset();

    /// Điều khiển phát lại
    void Play();
    void Pause();
    void TogglePause();
    void Seek(double targetSeconds);
    void SeekPercent(double percent01);
    void ToggleMute();
    void SetMute(bool mute);

    /// Trạng thái phát lại
    bool   IsLoaded() const { return m_mpv != nullptr; }
    bool   IsPlaying() const;
    bool   IsMuted() const;
    double GetPosition() const;
    double GetDuration() const;
    std::wstring GetTimecode() const;
    void   GetVideoResolution(int& outW, int& outH) const;
    SIZE   GetPreferredSize(int maxW, int maxH) const;

    /// Cập nhật kích thước vùng hiển thị video
    void Resize(const RECT& rect);

    /// Điều khiển hiển thị cửa sổ con video
    void Show();
    void Hide();

    HWND GetHwnd() const { return m_videoWnd; }
    bool IsAudioOnly() const { return m_isAudio; }

    /// Chụp và lưu khung hình hiện tại thành file ảnh (hỗ trợ .png, .jpg, v.v.)
    bool SaveScreenshot(const std::wstring& filePath);

    /// Kiểm tra thư viện libmpv có khả dụng không
    static bool IsAvailable();

private:
    static bool LoadMpvLibrary();
    static HMODULE s_hMpvDll;
    static bool    s_dllLoaded;
    static bool    s_dllLoadFailed;

    // ── Function pointers cho libmpv ──
    typedef mpv_handle* (*pfn_mpv_create)();
    typedef int         (*pfn_mpv_initialize)(mpv_handle*);
    typedef void        (*pfn_mpv_destroy)(mpv_handle*);
    typedef void        (*pfn_mpv_terminate_destroy)(mpv_handle*);
    typedef int         (*pfn_mpv_command)(mpv_handle*, const char**);
    typedef int         (*pfn_mpv_command_string)(mpv_handle*, const char*);
    typedef int         (*pfn_mpv_set_option)(mpv_handle*, const char*, mpv_format, void*);
    typedef int         (*pfn_mpv_set_option_string)(mpv_handle*, const char*, const char*);
    typedef int         (*pfn_mpv_get_property)(mpv_handle*, const char*, mpv_format, void*);
    typedef char*       (*pfn_mpv_get_property_string)(mpv_handle*, const char*);
    typedef int         (*pfn_mpv_set_property)(mpv_handle*, const char*, mpv_format, void*);
    typedef int         (*pfn_mpv_set_property_string)(mpv_handle*, const char*, const char*);
    typedef mpv_event*  (*pfn_mpv_wait_event)(mpv_handle*, double);
    typedef void        (*pfn_mpv_free)(void*);

    static pfn_mpv_create             fn_mpv_create;
    static pfn_mpv_initialize         fn_mpv_initialize;
    static pfn_mpv_destroy            fn_mpv_destroy;
    static pfn_mpv_terminate_destroy  fn_mpv_terminate_destroy;
    static pfn_mpv_command            fn_mpv_command;
    static pfn_mpv_command_string     fn_mpv_command_string;
    static pfn_mpv_set_option         fn_mpv_set_option;
    static pfn_mpv_set_option_string  fn_mpv_set_option_string;
    static pfn_mpv_get_property       fn_mpv_get_property;
    static pfn_mpv_get_property_string fn_mpv_get_property_string;
    static pfn_mpv_set_property       fn_mpv_set_property;
    static pfn_mpv_set_property_string fn_mpv_set_property_string;
    static pfn_mpv_wait_event         fn_mpv_wait_event;
    static pfn_mpv_free               fn_mpv_free;

    // ── Window & MPV instance ──
    HWND        m_parentWnd = nullptr;
    HWND        m_videoWnd  = nullptr;
    HINSTANCE   m_hInst     = nullptr;
    mpv_handle* m_mpv       = nullptr;

    std::wstring m_currentPath;
    int         m_vidWidth  = 0;
    int         m_vidHeight = 0;
    bool        m_isAudio   = false;
};
