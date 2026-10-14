#pragma once

#include <windows.h>

// ─────────────────────────────────────────────
// Custom Window Messages gửi tới PreviewWindow
// ─────────────────────────────────────────────
#define WM_APP_QUICKVIEW_TOGGLE    (WM_APP + 101)  // Bật/Tắt xem trước (phím Space)
#define WM_APP_QUICKVIEW_CLOSE     (WM_APP + 102)  // Đóng xem trước (phím Esc)
#define WM_APP_QUICKVIEW_NAVIGATE  (WM_APP + 103)  // Chuyển file (phím mũi tên Up/Down/Left/Right)

/// Quản lý Low-Level Keyboard Hook (WH_KEYBOARD_LL) và Shell Event Hook
/// với kiến trúc phát hiện nhập liệu (Typing Detection Filter) tương tự QuickLook.
class KeyboardHook {
public:
    /// Khởi tạo và cài đặt hook bàn phím toàn cục
    /// @param targetHwnd Cửa sổ nhận các thông điệp WM_APP_QUICKVIEW_*
    static bool Install(HWND targetHwnd);

    /// Gỡ bỏ hook bàn phím và shell event hook
    static void Uninstall();

    /// Kiểm tra hook đã được cài đặt chưa
    static bool IsInstalled();

    /// Lấy handle của cửa sổ Explorer/Desktop gần nhất
    static HWND GetLastShellWindow() { return s_lastShellWnd; }

    /// Kiểm tra người dùng có đang chủ động gõ phím sửa tên trong phiên Rename hiện tại không
    static bool IsUserEditingRename();

private:
    static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);
    static void CALLBACK WinEventProc(
        HWINEVENTHOOK hWinEventHook,
        DWORD event,
        HWND hwnd,
        LONG idObject,
        LONG idChild,
        DWORD idEventThread,
        DWORD dwmsEventTime
    );

    static bool IsModifierPressed();
    static bool IsNavigationalKey(DWORD vkCode);

    static HHOOK         s_hKeyHook;
    static HWINEVENTHOOK s_hWinEventHook;
    static HWND          s_targetHwnd;
    static HWND          s_lastShellWnd;
    static ULONGLONG     s_lastTypingTick;
    static HWND          s_activeRenameHwnd;
    static bool          s_hasEditedInRename;
};
