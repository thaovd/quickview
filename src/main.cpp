#include "PreviewWindow.h"
#include "ShellIntegration.h"
#include "KeyboardHook.h"
#include "Resource.h"

#include <windows.h>
#include <commctrl.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// ─────────────────────────────────────────────
// Entry Point
// ─────────────────────────────────────────────

int WINAPI wWinMain(
    _In_     HINSTANCE hInstance,
    _In_opt_ HINSTANCE /*hPrevInstance*/,
    _In_     PWSTR     pCmdLine,
    _In_     int       /*nCmdShow*/)
{
    // ── Single instance check ──
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, L"QuickView_SingleInstance_Mutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = (HWND)GetPropW(GetDesktopWindow(), L"QuickView_MainHwnd");
        if (existing && IsWindow(existing)) {
            wchar_t cls[64] = {};
            if (GetClassNameW(existing, cls, 64) <= 0 || wcscmp(cls, L"QuickViewPreviewWindow") != 0) {
                existing = nullptr;
            }
        } else {
            existing = nullptr;
        }
        if (!existing) {
            existing = FindWindowW(L"QuickViewPreviewWindow", L"QuickView");
            if (!existing) existing = FindWindowW(L"QuickViewPreviewWindow", nullptr);
            if (!existing) existing = FindWindowExW(nullptr, nullptr, L"QuickViewPreviewWindow", nullptr);
        }
        Utils::Log(L"[Main] Another instance detected: existing=0x%p, pCmdLine='%s'", existing, pCmdLine ? pCmdLine : L"");
        if (existing) {
            if (pCmdLine && *pCmdLine) {
                std::wstring cmd(pCmdLine);
                size_t first = cmd.find_first_not_of(L" \t\r\n\"");
                if (first != std::wstring::npos) {
                    size_t last = cmd.find_last_not_of(L" \t\r\n\"");
                    cmd = cmd.substr(first, last - first + 1);
                }
                if (cmd.length() > 3 && (cmd.back() == L'\\' || cmd.back() == L'/') && cmd[1] != L':') {
                    cmd.pop_back();
                }
                COPYDATASTRUCT cds = {};
                cds.dwData = 1;
                cds.cbData = static_cast<DWORD>((cmd.length() + 1) * sizeof(wchar_t));
                cds.lpData = const_cast<wchar_t*>(cmd.c_str());
                SendMessageW(existing, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cds));
            } else {
                SetForegroundWindow(existing);
            }
            if (hMutex) CloseHandle(hMutex);
            return 0;
        }
        Utils::Log(L"[Main] Existing window not found despite mutex. Continuing as primary instance.");
    }

    // ── Initialize COM (STA) ──
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hr)) return 1;

    // ── Initialize Common Controls ──
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    // ── Create preview window ──
    PreviewWindow preview;
    if (!preview.Create(hInstance)) {
        CoUninitialize();
        CloseHandle(hMutex);
        return 1;
    }

    // Register HWND with desktop window for cross-process IPC
    SetPropW(GetDesktopWindow(), L"QuickView_MainHwnd", preview.GetHwnd());

    // ── If a file path was passed on command line, preview it immediately ──
    if (pCmdLine && *pCmdLine) {
        std::wstring cmd(pCmdLine);
        size_t first = cmd.find_first_not_of(L" \t\r\n\"");
        if (first != std::wstring::npos) {
            size_t last = cmd.find_last_not_of(L" \t\r\n\"");
            cmd = cmd.substr(first, last - first + 1);
        }
        if (cmd.length() > 3 && (cmd.back() == L'\\' || cmd.back() == L'/') && cmd[1] != L':') {
            cmd.pop_back();
        }
        if (!cmd.empty()) {
            preview.ShowPreview(cmd);
        }
    }

    // ── Install low-level keyboard hook (Space key with smart typing detection) ──
    if (!KeyboardHook::Install(preview.GetHwnd())) {
        MessageBoxW(nullptr,
            L"Failed to install keyboard hook.\n"
            L"QuickView preview may not respond to Space key.",
            L"QuickView", MB_OK | MB_ICONWARNING);
    }

    // ── Main message loop ──
    Utils::Log(L"[Main] Entering message loop. hwnd=0x%p", preview.GetHwnd());
    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    Utils::Log(L"[Main] Exiting message loop. last msg=%u, wParam=%llu", msg.message, static_cast<unsigned long long>(msg.wParam));

    // ── Cleanup ──
    RemovePropW(GetDesktopWindow(), L"QuickView_MainHwnd");
    KeyboardHook::Uninstall();
    CoUninitialize();

    if (hMutex) CloseHandle(hMutex);
    return static_cast<int>(msg.wParam);
}
