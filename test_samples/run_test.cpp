#include <windows.h>
#include <iostream>

int main() {
    const wchar_t* target = L"\\\\mediaivf\\NAS 4 - File Quay chụp\\Tu lieu ads\\2026\\T9\\16.9.26\\META\\Hình ảnh HEIF-4D46-B8C2-30-0.heic";

    HWND hwnd = FindWindowW(L"QuickViewPreviewWindow", nullptr);
    if (!hwnd) hwnd = (HWND)GetPropW(GetDesktopWindow(), L"QuickView_MainHwnd");

    if (hwnd) {
        COPYDATASTRUCT cds = {};
        cds.dwData = 1;
        cds.cbData = (DWORD)((wcslen(target) + 1) * sizeof(wchar_t));
        cds.lpData = (void*)target;
        LRESULT res = SendMessageW(hwnd, WM_COPYDATA, 0, (LPARAM)&cds);
        std::cout << "Sent WM_COPYDATA to hwnd " << hwnd << " res: " << res << std::endl;
    } else {
        std::cout << "HWND not found" << std::endl;
    }
    return 0;
}
