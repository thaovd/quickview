#include "ShellIntegration.h"
#include "Utils.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <exdisp.h>
#include <wrl/client.h>
#include <string>
#include <vector>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "shell32.lib")

using Microsoft::WRL::ComPtr;

// Service ID for top-level browser
// {4C96BE40-915C-11CF-99D3-00AA004AE837}
static const GUID SID_STopLevelBrowser_Local =
    { 0x4C96BE40, 0x915C, 0x11CF, { 0x99, 0xD3, 0x00, 0xAA, 0x00, 0x4A, 0xE8, 0x37 } };

static bool IsExplorerWindowClass(const wchar_t* cls) {
    if (!cls || !cls[0]) return false;
    if (wcscmp(cls, L"CabinetWClass") == 0 ||
        wcscmp(cls, L"ExploreWClass") == 0 ||
        wcscmp(cls, L"ShellTabWindowClass") == 0 ||
        wcsstr(cls, L"Clover") != nullptr ||
        wcscmp(cls, L"TablacusExplorer") == 0) {
        return true;
    }
    return false;
}

ShellWindowType ShellIntegration::GetForegroundWindowType(HWND quickviewHwnd) {
    HWND fgWnd = GetForegroundWindow();
    if (!fgWnd) return ShellWindowType::Invalid;

    // Kiểm tra nếu chính là cửa sổ QuickView hoặc process của QuickView
    if (quickviewHwnd && fgWnd == quickviewHwnd) {
        return ShellWindowType::QuickView;
    }
    DWORD fgPid = 0;
    GetWindowThreadProcessId(fgWnd, &fgPid);
    if (fgPid == GetCurrentProcessId()) {
        return ShellWindowType::QuickView;
    }

    wchar_t className[256] = {};
    if (GetClassNameW(fgWnd, className, 256) == 0) {
        return ShellWindowType::Invalid;
    }

    // Windows Desktop (WorkerW hoặc Progman)
    if (wcscmp(className, L"WorkerW") == 0 ||
        wcscmp(className, L"Progman") == 0) {
        if (FindWindowExW(fgWnd, nullptr, L"SHELLDLL_DefView", nullptr) != nullptr) {
            return ShellWindowType::Desktop;
        }
        // Trường hợp WorkerW phụ bao phủ toàn màn hình desktop
        HWND progman = FindWindowW(L"Progman", nullptr);
        if (progman && FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr)) {
            return ShellWindowType::Desktop;
        }
        return ShellWindowType::Desktop;
    }

    // File Explorer (CabinetWClass, ExploreWClass, ShellTabWindowClass, Clover, v.v.)
    if (IsExplorerWindowClass(className)) {
        return ShellWindowType::Explorer;
    }

    // Kiểm tra GA_ROOT của fgWnd (cho trường hợp tab/container lồng nhau như Clover)
    HWND root = GetAncestor(fgWnd, GA_ROOT);
    if (root && root != fgWnd) {
        wchar_t rootCls[256] = {};
        if (GetClassNameW(root, rootCls, 256) > 0) {
            if (IsExplorerWindowClass(rootCls)) {
                return ShellWindowType::Explorer;
            }
        }
    }

    // Bỏ qua nhanh các cửa sổ trình duyệt / Electron / IDE phổ biến để tránh EnumChildWindows trong WH_KEYBOARD_LL
    if (wcsncmp(className, L"Chrome_WidgetWin_", 17) == 0 ||
        wcscmp(className, L"MozillaWindowClass") == 0 ||
        wcscmp(className, L"ApplicationFrameWindow") == 0 ||
        wcscmp(className, L"ConsoleWindowClass") == 0 ||
        wcscmp(className, L"CASCADIA_HOSTING_WINDOW_CLASS") == 0) {
        return ShellWindowType::Invalid;
    }

    // Kiểm tra xem bên trong fgWnd hoặc root có chứa SHELLDLL_DefView không (giới hạn số lượng child để không gây trễ hook)
    struct SearchCtx {
        bool found = false;
        int checked = 0;
    };
    SearchCtx ctx;
    EnumChildWindows(root ? root : fgWnd, [](HWND child, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<SearchCtx*>(lp);
        if (++c->checked > 48) return FALSE;
        wchar_t cCls[64] = {};
        if (GetClassNameW(child, cCls, 64) > 0) {
            if (wcscmp(cCls, L"SHELLDLL_DefView") == 0) {
                c->found = true;
                return FALSE;
            }
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));

    if (ctx.found) {
        return ShellWindowType::Explorer;
    }

    return ShellWindowType::Invalid;
}

bool ShellIntegration::IsShellWindowFocused(HWND quickviewHwnd) {
    ShellWindowType type = GetForegroundWindowType(quickviewHwnd);
    return type == ShellWindowType::Explorer ||
           type == ShellWindowType::Desktop ||
           type == ShellWindowType::QuickView;
}

static bool IsRenameEditWindow(HWND hWnd) {
    if (!hWnd || !IsWindow(hWnd) || !IsWindowVisible(hWnd)) return false;

    wchar_t cls[64] = {};
    if (GetClassNameW(hWnd, cls, 64) <= 0 || _wcsicmp(cls, L"Edit") != 0) {
        return false;
    }

    // Duyệt chuỗi cửa sổ cha (tối đa 6 cấp) để đảm bảo Edit nằm trong vùng danh sách file
    // (SysListView32, DirectUIHWND, SHELLDLL_DefView, UIItemsView) chứ không phải thanh địa chỉ hay ô tìm kiếm
    bool inFileView = false;
    HWND cur = GetParent(hWnd);
    for (int depth = 0; cur && depth < 6; ++depth, cur = GetParent(cur)) {
        wchar_t pcls[128] = {};
        if (GetClassNameW(cur, pcls, 128) <= 0) continue;

        if (_wcsicmp(pcls, L"ComboBox") == 0 ||
            _wcsicmp(pcls, L"ComboBoxEx32") == 0 ||
            _wcsicmp(pcls, L"Address Band Root") == 0 ||
            _wcsicmp(pcls, L"UniversalSearchBand") == 0 ||
            _wcsicmp(pcls, L"SearchBox") == 0 ||
            _wcsicmp(pcls, L"SearchEditBoxWrapperClass") == 0 ||
            _wcsicmp(pcls, L"Breadcrumb Parent") == 0) {
            return false;
        }

        if (_wcsicmp(pcls, L"SysListView32") == 0 ||
            _wcsicmp(pcls, L"DirectUIHWND") == 0 ||
            _wcsicmp(pcls, L"SHELLDLL_DefView") == 0 ||
            _wcsicmp(pcls, L"UIItemsView") == 0) {
            inFileView = true;
        }
    }
    return inFileView;
}

bool ShellIntegration::IsExplorerRenamingFile(HWND fgWnd, HWND* outEditWnd) {
    if (outEditWnd) *outEditWnd = nullptr;
    if (!fgWnd) fgWnd = GetForegroundWindow();
    if (!fgWnd) return false;

    // 1. Kiểm tra qua GetGUIThreadInfo(0) (luồng active thực tế trên hệ thống, không cần AttachThreadInput)
    GUITHREADINFO gti = { sizeof(gti) };
    if (GetGUIThreadInfo(0, &gti)) {
        if (IsRenameEditWindow(gti.hwndFocus)) {
            if (outEditWnd) *outEditWnd = gti.hwndFocus;
            return true;
        }
        if (IsRenameEditWindow(gti.hwndCaret)) {
            if (outEditWnd) *outEditWnd = gti.hwndCaret;
            return true;
        }
    }

    // 2. Kiểm tra qua thread của fgWnd
    DWORD tid = GetWindowThreadProcessId(fgWnd, nullptr);
    if (tid != 0) {
        GUITHREADINFO gtiFg = { sizeof(gtiFg) };
        if (GetGUIThreadInfo(tid, &gtiFg)) {
            if (IsRenameEditWindow(gtiFg.hwndFocus)) {
                if (outEditWnd) *outEditWnd = gtiFg.hwndFocus;
                return true;
            }
            if (IsRenameEditWindow(gtiFg.hwndCaret)) {
                if (outEditWnd) *outEditWnd = gtiFg.hwndCaret;
                return true;
            }
        }
    }

    // 3. Quét trực tiếp cây cửa sổ con của Explorer/Desktop (xử lý triệt để Windows 11 Multi-Tab Explorer
    //    khi luồng của CabinetWClass khác với luồng của ShellTabWindowClass / DirectUIHWND)
    HWND root = GetAncestor(fgWnd, GA_ROOT);
    if (!root) root = fgWnd;

    struct FindRenameCtx {
        HWND foundEdit = nullptr;
    } ctx;

    EnumChildWindows(root, [](HWND child, LPARAM lp) -> BOOL {
        if (IsRenameEditWindow(child)) {
            reinterpret_cast<FindRenameCtx*>(lp)->foundEdit = child;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));

    if (ctx.foundEdit) {
        if (outEditWnd) *outEditWnd = ctx.foundEdit;
        return true;
    }

    return false;
}

bool ShellIntegration::ShouldProtectRenameOnSpace(HWND hEdit, bool hasUserEditedRename) {
    if (!hEdit || !IsWindow(hEdit)) return false;

    // Kiểm tra vùng bôi đen (selection) trong ô Edit với timeout ngắn (15ms) để tránh treo hook
    DWORD selStart = 0, selEnd = 0;
    DWORD_PTR msgRes = 0;
    if (SendMessageTimeoutW(hEdit, EM_GETSEL, reinterpret_cast<WPARAM>(&selStart),
                            reinterpret_cast<LPARAM>(&selEnd), SMTO_ABORTIFHUNG, 15, &msgRes)) {
        // Khi Windows tự bật mode Rename (do click chậm), tên file đang bị bôi đen (selEnd > selStart).
        // Luôn bảo vệ khi có vùng văn bản đang bị bôi đen để phím Space không bao giờ xóa mất tên file!
        if (selEnd > selStart) {
            return true;
        }
        // Nếu người dùng đã chủ động gõ phím trong phiên đổi tên này thì cho phép gõ dấu cách bình thường
        if (hasUserEditedRename) {
            return false;
        }
        // Nếu con trỏ ở đầu tên file (selStart == 0, ví dụ file không có phần tên trước dấu chấm như .gitignore)
        // và người dùng chưa gõ phím nào -> bảo vệ
        if (selStart == 0) {
            return true;
        }
        // Nếu người dùng đã dùng chuột/phím di chuyển con trỏ vào giữa/cuối tên file (selStart > 0, không bôi đen)
        // thì cho phép chèn dấu cách
        return false;
    }

    // Nếu không lấy được EM_GETSEL, bảo vệ tên file trừ khi người dùng đã chủ động gõ phím
    return !hasUserEditedRename;
}

void ShellIntegration::CancelExplorerRename(HWND fgWnd, HWND hEdit) {
    if (!hEdit || !IsWindow(hEdit)) {
        IsExplorerRenamingFile(fgWnd, &hEdit);
    }
    if (hEdit && IsWindow(hEdit)) {
        Utils::Log(L"[ShellIntegration] Cancelling Explorer rename on Edit 0x%p to protect filename", hEdit);
        // Dùng PostMessageW (bất đồng bộ) thay vì SendMessageW để tuyệt đối không làm nghẽn WH_KEYBOARD_LL hook
        UINT scan = MapVirtualKeyW(VK_ESCAPE, MAPVK_VK_TO_VSC);
        PostMessageW(hEdit, WM_KEYDOWN, VK_ESCAPE, (scan << 16) | 1);
        PostMessageW(hEdit, WM_CHAR, VK_ESCAPE, (scan << 16) | 1);
        PostMessageW(hEdit, WM_KEYUP, VK_ESCAPE, (scan << 16) | 0xC0000001);
    }
}

bool ShellIntegration::IsCursorOrInputActive() {
    HWND fgWnd = GetForegroundWindow();
    if (!fgWnd) return false;

    auto checkGuiInfo = [](const GUITHREADINFO& gti) -> bool {
        if (gti.flags & (GUI_INMENUMODE | GUI_INMOVESIZE | GUI_POPUPMENUMODE | GUI_SYSTEMMENUMODE)) {
            return true;
        }
        // Nếu focus/caret là ô rename file trong Explorer thì để IsExplorerRenamingFile xử lý riêng
        if (IsRenameEditWindow(gti.hwndFocus) || IsRenameEditWindow(gti.hwndCaret)) {
            return false;
        }
        if (gti.hwndCaret != nullptr || (gti.flags & GUI_CARETBLINKING)) {
            return true;
        }
        if (gti.hwndFocus) {
            wchar_t cls[128] = {};
            if (GetClassNameW(gti.hwndFocus, cls, 128) > 0) {
                if (_wcsicmp(cls, L"Edit") == 0 ||
                    _wcsicmp(cls, L"RichEdit") == 0 ||
                    _wcsicmp(cls, L"RichEdit20W") == 0 ||
                    _wcsicmp(cls, L"RICHEDIT50W") == 0 ||
                    _wcsicmp(cls, L"RichEditD2DPT") == 0 ||
                    _wcsicmp(cls, L"SearchEditBox") == 0 ||
                    _wcsicmp(cls, L"UniversalSearchBand") == 0) {
                    return true;
                }
            }
        }
        return false;
    };

    // 1. Kiểm tra trên luồng foreground thực tế (idThread = 0)
    GUITHREADINFO gti = { sizeof(gti) };
    if (GetGUIThreadInfo(0, &gti) && checkGuiInfo(gti)) {
        return true;
    }

    // 2. Kiểm tra trên luồng của cửa sổ fgWnd
    DWORD tid = GetWindowThreadProcessId(fgWnd, nullptr);
    if (tid != 0) {
        GUITHREADINFO gtiFg = { sizeof(gtiFg) };
        if (GetGUIThreadInfo(tid, &gtiFg) && checkGuiInfo(gtiFg)) {
            return true;
        }
    }

    return false;
}

std::wstring ShellIntegration::GetSelectedFilePath(HWND targetWnd) {
    ShellWindowType type = ShellWindowType::Invalid;
    if (targetWnd) {
        wchar_t className[256] = {};
        GetClassNameW(targetWnd, className, 256);
        if (IsExplorerWindowClass(className)) {
            type = ShellWindowType::Explorer;
        } else if (wcscmp(className, L"WorkerW") == 0 || wcscmp(className, L"Progman") == 0) {
            type = ShellWindowType::Desktop;
        } else {
            HWND root = GetAncestor(targetWnd, GA_ROOT);
            if (root && root != targetWnd) {
                wchar_t rootCls[256] = {};
                if (GetClassNameW(root, rootCls, 256) > 0 && IsExplorerWindowClass(rootCls)) {
                    type = ShellWindowType::Explorer;
                }
            }
        }
    }
    if (type == ShellWindowType::Invalid) {
        type = GetForegroundWindowType();
    }

    std::wstring result;
    if (type == ShellWindowType::Explorer) {
        result = GetSelectedFromExplorer(targetWnd);
    } else if (type == ShellWindowType::Desktop) {
        result = GetSelectedFromDesktop();
    }

    return result;
}

std::wstring ShellIntegration::GetPathFromShellView(IUnknown* pShellViewUnk) {
    if (!pShellViewUnk) return L"";

    ComPtr<IShellView> shellView;
    if (FAILED(pShellViewUnk->QueryInterface(IID_PPV_ARGS(&shellView)))) {
        return L"";
    }

    // Cách 1: Sử dụng IFolderView2 (Hiện đại, nhanh, chuẩn Windows Vista+)
    ComPtr<IFolderView2> folderView;
    if (SUCCEEDED(shellView.As(&folderView)) && folderView) {
        int selCount = 0;
        if (SUCCEEDED(folderView->ItemCount(SVGIO_SELECTION, &selCount))) {
            if (selCount <= 0) {
                // Người dùng không chọn file nào (click ra vùng trống trong Explorer)
                return L"";
            }
            ComPtr<IShellItemArray> itemArray;
            if (SUCCEEDED(folderView->Items(SVGIO_SELECTION, IID_PPV_ARGS(&itemArray)))) {
                ComPtr<IShellItem> item;
                if (SUCCEEDED(itemArray->GetItemAt(0, &item))) {
                    PWSTR displayName = nullptr;
                    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &displayName)) && displayName) {
                        std::wstring result(displayName);
                        CoTaskMemFree(displayName);
                        return result;
                    }
                }
            }
        }
    }

    // Cách 2: Sử dụng IDataObject với CF_HDROP (tương thích cao theo phong cách QuickLook)
    ComPtr<IDataObject> dataObject;
    if (SUCCEEDED(shellView->GetItemObject(SVGIO_SELECTION, IID_PPV_ARGS(&dataObject))) && dataObject) {
        FORMATETC fe = {};
        fe.cfFormat = CF_HDROP;
        fe.ptd      = nullptr;
        fe.dwAspect = DVASPECT_CONTENT;
        fe.lindex   = -1;
        fe.tymed    = TYMED_HGLOBAL;

        STGMEDIUM stg = {};
        if (SUCCEEDED(dataObject->GetData(&fe, &stg))) {
            HDROP hDrop = static_cast<HDROP>(stg.hGlobal);
            UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
            if (count > 0) {
                wchar_t filePath[MAX_PATH] = {};
                if (DragQueryFileW(hDrop, 0, filePath, MAX_PATH) > 0) {
                    wchar_t longPath[MAX_PATH] = {};
                    if (GetLongPathNameW(filePath, longPath, MAX_PATH) > 0) {
                        ReleaseStgMedium(&stg);
                        return std::wstring(longPath);
                    }
                    ReleaseStgMedium(&stg);
                    return std::wstring(filePath);
                }
            }
            ReleaseStgMedium(&stg);
        }

        // Cách 3: CFSTR_SHELLIDLIST cho các icon desktop đặc biệt
        static const CLIPFORMAT cfShellIDList = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_SHELLIDLIST));
        fe.cfFormat = cfShellIDList;
        if (SUCCEEDED(dataObject->GetData(&fe, &stg))) {
            auto* pida = static_cast<CIDA*>(GlobalLock(stg.hGlobal));
            if (pida) {
                auto* pidlFolder = reinterpret_cast<ITEMIDLIST*>(reinterpret_cast<BYTE*>(pida) + pida->aoffset[0]);
                auto* pidlItem   = reinterpret_cast<ITEMIDLIST*>(reinterpret_cast<BYTE*>(pida) + pida->aoffset[1]);
                PIDLIST_ABSOLUTE pidlFull = ILCombine(pidlFolder, pidlItem);
                GlobalUnlock(stg.hGlobal);
                ReleaseStgMedium(&stg);

                if (pidlFull) {
                    ComPtr<IShellItem> shellItem;
                    if (SUCCEEDED(SHCreateItemFromIDList(pidlFull, IID_PPV_ARGS(&shellItem)))) {
                        PWSTR displayName = nullptr;
                        if (SUCCEEDED(shellItem->GetDisplayName(SIGDN_FILESYSPATH, &displayName)) && displayName) {
                            std::wstring res(displayName);
                            CoTaskMemFree(displayName);
                            ILFree(pidlFull);
                            return res;
                        }
                    }
                    ILFree(pidlFull);
                }
            } else {
                ReleaseStgMedium(&stg);
            }
        }
    }

    return L"";
}

static std::wstring GetPathFromFolderViewDual(IWebBrowser2* browser) {
    if (!browser) return L"";
    ComPtr<IDispatch> dispDoc;
    if (SUCCEEDED(browser->get_Document(&dispDoc)) && dispDoc) {
        ComPtr<IShellFolderViewDual> folderViewDual;
        if (SUCCEEDED(dispDoc.As(&folderViewDual)) && folderViewDual) {
            ComPtr<FolderItems> items;
            if (SUCCEEDED(folderViewDual->SelectedItems(&items)) && items) {
                long selCnt = 0;
                if (SUCCEEDED(items->get_Count(&selCnt)) && selCnt > 0) {
                    VARIANT vIdx;
                    VariantInit(&vIdx);
                    vIdx.vt = VT_I4;
                    vIdx.lVal = 0;
                    ComPtr<FolderItem> fi;
                    if (SUCCEEDED(items->Item(vIdx, &fi)) && fi) {
                        BSTR bPath = nullptr;
                        if (SUCCEEDED(fi->get_Path(&bPath)) && bPath) {
                            std::wstring res(bPath);
                            SysFreeString(bPath);
                            return res;
                        }
                    }
                }
            }
        }
    }
    return L"";
}

static std::wstring GetSelectionFromBrowserItem(IWebBrowser2* browser, IShellBrowser* shellBrowser) {
    if (!browser) return L"";
    if (shellBrowser) {
        ComPtr<IShellView> shellView;
        if (SUCCEEDED(shellBrowser->QueryActiveShellView(&shellView)) && shellView) {
            std::wstring path = ShellIntegration::GetPathFromShellView(shellView.Get());
            if (!path.empty()) {
                return path;
            }
        }
    }
    return GetPathFromFolderViewDual(browser);
}

std::wstring ShellIntegration::GetSelectedFromExplorer(HWND targetWnd) {
    HWND fgWnd = targetWnd ? targetWnd : GetForegroundWindow();
    if (!fgWnd) return L"";

    HWND topFg = GetAncestor(fgWnd, GA_ROOT);
    if (!topFg) topFg = fgWnd;

    // Re-initialize COM in case mpv_terminate_destroy unbalanced it
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    ComPtr<IShellWindows> shellWindows;
    HRESULT hr = CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(&shellWindows));
    if (FAILED(hr)) {
        Utils::Log(L"[GetSelectedFromExplorer] CoCreateInstance(CLSID_ShellWindows) FAILED hr=0x%08X", hr);
        return L"";
    }

    long count = 0;
    shellWindows->get_Count(&count);

    struct TabCandidate {
        long index = -1;
        ComPtr<IWebBrowser2> browser;
        ComPtr<IShellBrowser> shellBrowser;
        HWND itemHwnd = nullptr;
        HWND browserHwnd = nullptr;
        HWND svWnd = nullptr;
        bool isDirectFocus = false;
        bool belongsToTopFg = false;
        bool isSvVisible = false;
    };

    std::vector<TabCandidate> candidates;
    candidates.reserve(count);

    for (long i = 0; i < count; i++) {
        VARIANT vi;
        V_VT(&vi) = VT_I4;
        V_I4(&vi) = i;

        ComPtr<IDispatch> disp;
        hr = shellWindows->Item(vi, &disp);
        if (FAILED(hr) || !disp) continue;

        ComPtr<IWebBrowser2> browser;
        hr = disp.As(&browser);
        if (FAILED(hr) || !browser) continue;

        ComPtr<IServiceProvider> sp;
        hr = browser.As(&sp);
        if (FAILED(hr) || !sp) continue;

        ComPtr<IShellBrowser> shellBrowser;
        hr = sp->QueryService(SID_STopLevelBrowser_Local, IID_PPV_ARGS(&shellBrowser));
        if (FAILED(hr)) {
            hr = sp->QueryService(IID_IShellBrowser, IID_PPV_ARGS(&shellBrowser));
        }

        SHANDLE_PTR hwndVal = 0;
        browser->get_HWND(&hwndVal);
        HWND itemHwnd = reinterpret_cast<HWND>(hwndVal);

        HWND browserHwnd = nullptr;
        if (shellBrowser) {
            shellBrowser->GetWindow(&browserHwnd);
        }

        HWND svWnd = nullptr;
        if (shellBrowser) {
            ComPtr<IShellView> shellView;
            if (SUCCEEDED(shellBrowser->QueryActiveShellView(&shellView)) && shellView) {
                shellView->GetWindow(&svWnd);
            }
        }

        HWND itemRoot = itemHwnd ? GetAncestor(itemHwnd, GA_ROOT) : nullptr;
        HWND sbRoot   = browserHwnd ? GetAncestor(browserHwnd, GA_ROOT) : nullptr;
        HWND svRoot   = svWnd ? GetAncestor(svWnd, GA_ROOT) : nullptr;

        bool isDirectFocus = false;
        if (itemHwnd && (itemHwnd == fgWnd || IsChild(itemHwnd, fgWnd))) isDirectFocus = true;
        if (!isDirectFocus && browserHwnd && (browserHwnd == fgWnd || IsChild(browserHwnd, fgWnd))) isDirectFocus = true;
        if (!isDirectFocus && svWnd && (svWnd == fgWnd || IsChild(svWnd, fgWnd))) isDirectFocus = true;

        bool belongsToTopFg = false;
        if (itemHwnd && (itemHwnd == topFg || itemRoot == topFg || IsChild(topFg, itemHwnd))) belongsToTopFg = true;
        if (!belongsToTopFg && browserHwnd && (browserHwnd == topFg || sbRoot == topFg || IsChild(topFg, browserHwnd))) belongsToTopFg = true;
        if (!belongsToTopFg && svWnd && (svWnd == topFg || svRoot == topFg || IsChild(topFg, svWnd))) belongsToTopFg = true;

        bool isSvVisible = svWnd && (IsWindowVisible(svWnd) != FALSE);

        TabCandidate cand;
        cand.index = i;
        cand.browser = browser;
        cand.shellBrowser = shellBrowser;
        cand.itemHwnd = itemHwnd;
        cand.browserHwnd = browserHwnd;
        cand.svWnd = svWnd;
        cand.isDirectFocus = isDirectFocus;
        cand.belongsToTopFg = belongsToTopFg;
        cand.isSvVisible = isSvVisible;

        candidates.push_back(cand);
    }

    // Ưu tiên 1: Tab có focus trực tiếp (chuột hoặc bàn phím đang thao tác trên tab này)
    for (const auto& cand : candidates) {
        if (cand.isDirectFocus) {
            return GetSelectionFromBrowserItem(cand.browser.Get(), cand.shellBrowser.Get());
        }
    }

    // Ưu tiên 2: Tab thuộc topFg và đang hiển thị (Active Tab của Windows 11 Explorer khi focus ở title bar/ribbon)
    for (const auto& cand : candidates) {
        if (cand.belongsToTopFg && cand.isSvVisible) {
            return GetSelectionFromBrowserItem(cand.browser.Get(), cand.shellBrowser.Get());
        }
    }

    // Ưu tiên 3: Bất kỳ tab nào thuộc topFg
    for (const auto& cand : candidates) {
        if (cand.belongsToTopFg) {
            return GetSelectionFromBrowserItem(cand.browser.Get(), cand.shellBrowser.Get());
        }
    }

    // Tuyệt đối KHÔNG quét tìm sang các cửa sổ hoặc tab khác đang chạy ngầm!
    return L"";
}

std::wstring ShellIntegration::GetSelectedFromDesktop() {
    // Re-initialize COM in case mpv_terminate_destroy unbalanced it
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    ComPtr<IShellWindows> shellWindows;
    HRESULT hr = CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(&shellWindows));
    if (FAILED(hr)) {
        Utils::Log(L"[GetSelectedFromDesktop] CoCreateInstance(CLSID_ShellWindows) FAILED hr=0x%08X", hr);
        return L"";
    }

    VARIANT vtLoc;
    VariantInit(&vtLoc);
    V_VT(&vtLoc) = VT_I4;
    V_I4(&vtLoc) = CSIDL_DESKTOP;

    VARIANT vtEmpty;
    VariantInit(&vtEmpty);

    long hwndVal = 0;
    ComPtr<IDispatch> disp;

    // SWC_DESKTOP: tìm browser của Desktop với CSIDL_DESKTOP
    hr = shellWindows->FindWindowSW(&vtLoc, &vtEmpty, SWC_DESKTOP, &hwndVal, SWFO_NEEDDISPATCH, &disp);
    if (FAILED(hr) || !disp) {
        VARIANT vtEmpty2;
        VariantInit(&vtEmpty2);
        hr = shellWindows->FindWindowSW(&vtEmpty2, &vtEmpty, SWC_DESKTOP, &hwndVal, SWFO_NEEDDISPATCH, &disp);
        if (FAILED(hr) || !disp) return L"";
    }

    ComPtr<IServiceProvider> sp;
    hr = disp.As(&sp);
    if (FAILED(hr) || !sp) return L"";

    ComPtr<IShellBrowser> shellBrowser;
    hr = sp->QueryService(SID_STopLevelBrowser_Local, IID_PPV_ARGS(&shellBrowser));
    if (FAILED(hr) || !shellBrowser) {
        hr = sp->QueryService(IID_IShellBrowser, IID_PPV_ARGS(&shellBrowser));
        if (FAILED(hr) || !shellBrowser) return L"";
    }

    ComPtr<IShellView> shellView;
    hr = shellBrowser->QueryActiveShellView(&shellView);
    if (FAILED(hr) || !shellView) return L"";

    return GetPathFromShellView(shellView.Get());
}
