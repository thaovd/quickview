#pragma once

#include <string>
#include <windows.h>
#include <unknwn.h>

/// Kiểu cửa sổ đang active
enum class ShellWindowType {
    Invalid,
    Explorer,
    Desktop,
    QuickView
};

/// Quản lý tương tác với Windows Shell (File Explorer, Desktop)
/// và nhận diện trạng thái người dùng có đang nhập văn bản/đổi tên hay không.
class ShellIntegration {
public:
    /// Xác định kiểu của cửa sổ Foreground hiện tại
    static ShellWindowType GetForegroundWindowType(HWND quickviewHwnd = nullptr);

    /// Kiểm tra xem người dùng có đang trong trạng thái gõ phím / nhập liệu hay không
    /// (Ví dụ: con trỏ văn bản caret đang nhấp nháy, đang đổi tên file F2,
    ///  thanh tìm kiếm search box, thanh địa chỉ address bar, ô nhập văn bản).
    static bool IsCursorOrInputActive();

    /// Kiểm tra xem Explorer/Desktop có đang trong chế độ đổi tên file (inline rename edit box) hay không
    static bool IsExplorerRenamingFile(HWND fgWnd = nullptr, HWND* outEditWnd = nullptr);

    /// Kiểm tra xem có cần bảo vệ tên file (chặn phím Space xóa tên và hủy rename) hay không
    static bool ShouldProtectRenameOnSpace(HWND hEdit, bool hasUserEditedRename);

    /// Tự động hủy chế độ đổi tên file trong Explorer/Desktop (gửi ESC để bảo vệ tên file)
    static void CancelExplorerRename(HWND fgWnd = nullptr, HWND hEdit = nullptr);

    /// Lấy đường dẫn đầy đủ của file đang được chọn trong File Explorer hoặc Desktop
    /// @param targetWnd Cửa sổ shell mục tiêu (nếu nullptr sẽ dùng GetForegroundWindow)
    static std::wstring GetSelectedFilePath(HWND targetWnd = nullptr);

    /// Kiểm tra xem cửa sổ hiện tại có phải là File Explorer hoặc Desktop không
    static bool IsShellWindowFocused(HWND quickviewHwnd = nullptr);

    /// Lấy đường dẫn file được chọn từ IShellView
    static std::wstring GetPathFromShellView(IUnknown* pShellViewUnk);

private:
    static std::wstring GetSelectedFromExplorer(HWND targetWnd = nullptr);
    static std::wstring GetSelectedFromDesktop();
};
