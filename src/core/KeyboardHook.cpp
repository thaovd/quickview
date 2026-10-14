#include "KeyboardHook.h"
#include "ShellIntegration.h"
#include "Utils.h"

#include <windows.h>

HHOOK         KeyboardHook::s_hKeyHook          = nullptr;
HWINEVENTHOOK KeyboardHook::s_hWinEventHook     = nullptr;
HWND          KeyboardHook::s_targetHwnd        = nullptr;
HWND          KeyboardHook::s_lastShellWnd       = nullptr;
ULONGLONG     KeyboardHook::s_lastTypingTick     = 0;
HWND          KeyboardHook::s_activeRenameHwnd   = nullptr;
bool          KeyboardHook::s_hasEditedInRename  = false;

// Thời gian trễ sau khi gõ phím chữ/số (500ms) để coi là đang nhập văn bản
static const ULONGLONG TYPING_DELAY_MS = 500;
static bool s_spaceDownBlocked = false;

static bool IsTextEditingKey(DWORD vkCode) {
    if (vkCode == VK_BACK || vkCode == VK_DELETE || vkCode == VK_HOME || vkCode == VK_END) {
        return true;
    }
    // Chữ và số (0-9, A-Z)
    if (vkCode >= 0x30 && vkCode <= 0x5A) {
        return true;
    }
    // Phím Numpad
    if (vkCode >= VK_NUMPAD0 && vkCode <= VK_DIVIDE) {
        return true;
    }
    // Các phím dấu câu / ký tự OEM
    if ((vkCode >= VK_OEM_1 && vkCode <= VK_OEM_8) || vkCode == VK_OEM_102) {
        return true;
    }
    return false;
}

bool KeyboardHook::Install(HWND targetHwnd) {
    if (s_hKeyHook) return true; // Đã cài đặt

    s_targetHwnd        = targetHwnd;
    s_lastShellWnd      = nullptr;
    s_lastTypingTick    = 0;
    s_activeRenameHwnd  = nullptr;
    s_hasEditedInRename = false;

    // Cài đặt Low-level Keyboard Hook
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    s_hKeyHook = SetWindowsHookExW(
        WH_KEYBOARD_LL,
        LowLevelKeyboardProc,
        hInst,
        0
    );
    if (!s_hKeyHook) {
        Utils::Log(L"[KeyboardHook] SetWindowsHookEx failed, err=%lu", GetLastError());
        return false;
    }
    Utils::Log(L"[KeyboardHook] Hook installed successfully. hHook=0x%p, targetHwnd=0x%p", s_hKeyHook, targetHwnd);

    // Cài đặt WinEventHook theo dõi thay đổi cửa sổ Foreground (EVENT_SYSTEM_FOREGROUND)
    // Khi người dùng chuyển cửa sổ (ví dụ Alt+Tab vào Explorer), reset ngay bộ đếm typing delay
    s_hWinEventHook = SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND,
        EVENT_SYSTEM_FOREGROUND,
        nullptr,
        WinEventProc,
        0,
        0,
        WINEVENT_OUTOFCONTEXT
    );

    return true;
}

void KeyboardHook::Uninstall() {
    if (s_hKeyHook) {
        UnhookWindowsHookEx(s_hKeyHook);
        s_hKeyHook = nullptr;
    }
    if (s_hWinEventHook) {
        UnhookWinEvent(s_hWinEventHook);
        s_hWinEventHook = nullptr;
    }
    s_targetHwnd        = nullptr;
    s_lastShellWnd      = nullptr;
    s_activeRenameHwnd  = nullptr;
    s_hasEditedInRename = false;
}

bool KeyboardHook::IsInstalled() {
    return s_hKeyHook != nullptr;
}

bool KeyboardHook::IsUserEditingRename() {
    if (s_hasEditedInRename) return true;
    if (s_lastTypingTick != 0 && (GetTickCount64() - s_lastTypingTick) <= TYPING_DELAY_MS) {
        return true;
    }
    return false;
}

bool KeyboardHook::IsModifierPressed() {
    return (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 ||
           (GetAsyncKeyState(VK_MENU)    & 0x8000) != 0 ||
           (GetAsyncKeyState(VK_LWIN)    & 0x8000) != 0 ||
           (GetAsyncKeyState(VK_RWIN)    & 0x8000) != 0;
}

bool KeyboardHook::IsNavigationalKey(DWORD vkCode) {
    switch (vkCode) {
        case VK_SPACE:
        case VK_ESCAPE:
        case VK_UP:
        case VK_DOWN:
        case VK_LEFT:
        case VK_RIGHT:
        case VK_RETURN:
        case VK_F5:
        case VK_F11:
            return true;
        default:
            return false;
    }
}

void CALLBACK KeyboardHook::WinEventProc(
    HWINEVENTHOOK /*hWinEventHook*/,
    DWORD /*event*/,
    HWND hwnd,
    LONG /*idObject*/,
    LONG /*idChild*/,
    DWORD /*idEventThread*/,
    DWORD /*dwmsEventTime*/)
{
    // Khi cửa sổ foreground thay đổi, xóa ngay độ trễ gõ phím của ngữ cảnh cũ
    s_lastTypingTick    = 0;
    s_activeRenameHwnd  = nullptr;
    s_hasEditedInRename = false;
    if (hwnd) {
        ShellWindowType t = ShellIntegration::GetForegroundWindowType(s_targetHwnd);
        if (t == ShellWindowType::Explorer || t == ShellWindowType::Desktop) {
            s_lastShellWnd = hwnd;
        }
    }
}

LRESULT CALLBACK KeyboardHook::LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode < 0 || !s_targetHwnd) {
        return CallNextHookEx(s_hKeyHook, nCode, wParam, lParam);
    }

    auto* pKbd = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
    DWORD vkCode = pKbd->vkCode;
    bool isKeyDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
    bool isKeyUp   = (wParam == WM_KEYUP   || wParam == WM_SYSKEYUP);

    bool isPreviewVisible = IsWindowVisible(s_targetHwnd) != FALSE;

    // ── 1. Xử lý phím Space ──
    if (vkCode == VK_SPACE && isKeyDown) {
        // Bỏ qua ngay nếu có Alt hoặc Win (tránh xung đột Alt+Space hệ thống hay Win+Space đổi bàn phím)
        bool isAlt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
        bool isWin = (GetAsyncKeyState(VK_LWIN) & 0x8000) != 0 || (GetAsyncKeyState(VK_RWIN) & 0x8000) != 0;
        if (isAlt || isWin) {
            return CallNextHookEx(s_hKeyHook, nCode, wParam, lParam);
        }

        HWND fg = GetForegroundWindow();
        ShellWindowType wndType = ShellIntegration::GetForegroundWindowType(s_targetHwnd);

        // Lớp phòng thủ 1: Nếu người dùng đang ở ứng dụng khác (trình duyệt, chat app, IDE, v.v.):
        // Tuyệt đối KHÔNG chặn phím Space, KHÔNG đóng preview và KHÔNG ghi log gây trễ!
        if (wndType != ShellWindowType::Explorer &&
            wndType != ShellWindowType::Desktop &&
            wndType != ShellWindowType::QuickView &&
            fg != s_targetHwnd) {
            return CallNextHookEx(s_hKeyHook, nCode, wParam, lParam);
        }

        // Lớp phòng thủ 2: Nếu người dùng đang focus vào cửa sổ QuickView:
        if (wndType == ShellWindowType::QuickView || fg == s_targetHwnd) {
            if (isPreviewVisible) {
                Utils::Log(L"[KeyboardHook] Space on focused QuickView -> closing preview");
                PostMessageW(s_targetHwnd, WM_APP_QUICKVIEW_CLOSE, 0, 0);
                s_spaceDownBlocked = true;
                return 1;
            }
            return CallNextHookEx(s_hKeyHook, nCode, wParam, lParam);
        }

        // Lớp phòng thủ 3: Người dùng đang ở File Explorer hoặc Desktop
        s_lastShellWnd = fg;

        // Kiểm tra xem Explorer/Desktop có đang ở chế độ đổi tên file (inline Rename) không
        HWND hRenameEdit = nullptr;
        if (ShellIntegration::IsExplorerRenamingFile(fg, &hRenameEdit)) {
            if (hRenameEdit != s_activeRenameHwnd) {
                s_activeRenameHwnd  = hRenameEdit;
                s_hasEditedInRename = false;
            }
            bool isEditingThisRename = IsUserEditingRename();

            // Nếu tên file đang bị bôi đen toàn bộ (do click chậm kích hoạt nhầm Rename)
            // hoặc người dùng chưa hề gõ chữ nào vào ô Rename -> Hủy Rename ngay và kích hoạt QuickView!
            if (ShellIntegration::ShouldProtectRenameOnSpace(hRenameEdit, isEditingThisRename)) {
                Utils::Log(L"[KeyboardHook] Space on Explorer rename edit 0x%p (isVisible=%d) -> Cancelling rename and toggling preview",
                           hRenameEdit, isPreviewVisible);
                ShellIntegration::CancelExplorerRename(fg, hRenameEdit);
                s_activeRenameHwnd  = nullptr;
                s_hasEditedInRename = false;
                PostMessageW(s_targetHwnd, WM_APP_QUICKVIEW_TOGGLE, reinterpret_cast<WPARAM>(fg), 0);
                s_spaceDownBlocked = true;
                return 1;
            }

            // Người dùng đang chủ động gõ tên file mới -> cho phép phím Space đi qua để đặt tên có dấu cách
            return CallNextHookEx(s_hKeyHook, nCode, wParam, lParam);
        } else {
            s_activeRenameHwnd  = nullptr;
            s_hasEditedInRename = false;
            if (ShellIntegration::IsCursorOrInputActive()) {
                return CallNextHookEx(s_hKeyHook, nCode, wParam, lParam);
            }
        }

        // Khi ở trong Explorer/Desktop: Kích hoạt xem trước / đóng xem trước
        Utils::Log(L"[KeyboardHook] Space in Explorer/Desktop: isVisible=%d, fg=0x%p", isPreviewVisible, fg);
        PostMessageW(s_targetHwnd, WM_APP_QUICKVIEW_TOGGLE, reinterpret_cast<WPARAM>(fg), 0);
        s_spaceDownBlocked = true;
        return 1;
    }

    // Chặn phím Space UP nếu Space DOWN trước đó đã bị chặn
    if (vkCode == VK_SPACE && isKeyUp) {
        if (s_spaceDownBlocked) {
            s_spaceDownBlocked = false;
            return 1; // Chặn phím Space UP
        }
    }

    // ── 2. Xử lý phím Escape / Enter ──
    if (isKeyDown && (vkCode == VK_ESCAPE || vkCode == VK_RETURN)) {
        s_activeRenameHwnd  = nullptr;
        s_hasEditedInRename = false;
    }

    if (vkCode == VK_ESCAPE && isKeyDown) {
        if (isPreviewVisible) {
            HWND fg = GetForegroundWindow();
            ShellWindowType wndType = ShellIntegration::GetForegroundWindowType(s_targetHwnd);
            if (wndType == ShellWindowType::QuickView ||
                wndType == ShellWindowType::Explorer ||
                wndType == ShellWindowType::Desktop ||
                fg == s_targetHwnd) {
                PostMessageW(s_targetHwnd, WM_APP_QUICKVIEW_CLOSE, 0, 0);
                return 1; // Chặn phím Esc
            }
        }
    }

    // ── 3. Xử lý phím mũi tên (Up/Down/Left/Right) khi QuickView đang mở ──
    if (isPreviewVisible && isKeyUp) {
        if (vkCode == VK_UP || vkCode == VK_DOWN || vkCode == VK_LEFT || vkCode == VK_RIGHT) {
            HWND fg = GetForegroundWindow();
            ShellWindowType wndType = ShellIntegration::GetForegroundWindowType(s_targetHwnd);
            if (wndType == ShellWindowType::Explorer || wndType == ShellWindowType::Desktop) {
                // Không chặn phím (để Explorer cập nhật mục chọn),
                // nhưng gửi thông điệp yêu cầu QuickView cập nhật sang file mới kèm HWND
                PostMessageW(s_targetHwnd, WM_APP_QUICKVIEW_NAVIGATE, reinterpret_cast<WPARAM>(fg), 0);
            }
        }
    }

    // ── 4. Ghi nhận thời gian gõ phím văn bản / chỉnh sửa khi không giữ Ctrl/Alt/Win ──
    if (isKeyDown && IsTextEditingKey(vkCode) && !IsModifierPressed()) {
        s_lastTypingTick    = GetTickCount64();
        s_hasEditedInRename = true;
        GUITHREADINFO gti = { sizeof(GUITHREADINFO) };
        if (GetGUIThreadInfo(0, &gti) && gti.hwndFocus) {
            s_activeRenameHwnd = gti.hwndFocus;
        }
    }

    return CallNextHookEx(s_hKeyHook, nCode, wParam, lParam);
}
