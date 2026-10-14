#pragma once

#include "Utils.h"

/// Renders a "file info card" for unsupported file types.
/// Displays: icon, name, type, size, dates, path, attributes.
class FileInfoView {
public:
    FileInfoView() = default;
    ~FileInfoView() = default;

    /// Set file information to display
    void SetFileInfo(const Utils::FileInfo& info, const std::wstring& filePath,
                     IWICImagingFactory* wic, ID2D1RenderTarget* rt);

    /// Render the info card
    void Render(ID2D1RenderTarget* rt, IDWriteFactory* dwrite,
                const D2D1_RECT_F& area, ID2D1SolidColorBrush* brush);

    void Reset();

private:
    void DrawInfoRow(ID2D1RenderTarget* rt, IDWriteFactory* dwrite,
                     ID2D1SolidColorBrush* brush,
                     const wchar_t* label, const std::wstring& value,
                     float x, float& y, float width);

    Utils::FileInfo      m_info;
    ComPtr<ID2D1Bitmap>  m_iconBitmap;
    std::wstring         m_filePath;
};
