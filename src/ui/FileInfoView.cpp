#include "FileInfoView.h"

void FileInfoView::SetFileInfo(const Utils::FileInfo& info, const std::wstring& filePath,
                                IWICImagingFactory* wic, ID2D1RenderTarget* rt) {
    m_info = info;
    m_filePath = filePath;

    // Load file icon as D2D bitmap
    HICON hIcon = Utils::GetFileIcon(filePath, true);
    if (hIcon) {
        m_iconBitmap = Utils::CreateBitmapFromIcon(hIcon, rt, wic);
        DestroyIcon(hIcon);
    }
}

void FileInfoView::DrawInfoRow(ID2D1RenderTarget* rt, IDWriteFactory* dwrite,
                                ID2D1SolidColorBrush* brush,
                                const wchar_t* label, const std::wstring& value,
                                float x, float& y, float width) {
    if (value.empty()) return;

    ComPtr<IDWriteTextFormat> labelFmt, valueFmt;
    dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"en-US", &labelFmt);
    dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"en-US", &valueFmt);

    if (!labelFmt || !valueFmt) return;

    valueFmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    DWRITE_TRIMMING trimming = { DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
    ComPtr<IDWriteInlineObject> ellipsis;
    if (SUCCEEDED(dwrite->CreateEllipsisTrimmingSign(valueFmt.Get(), &ellipsis))) {
        valueFmt->SetTrimming(&trimming, ellipsis.Get());
    }

    float labelWidth = 100.0f;
    float rowHeight  = 22.0f;

    // Label
    brush->SetColor(Colors::TextDim);
    D2D1_RECT_F labelRect = D2D1::RectF(x, y, x + labelWidth, y + rowHeight);
    rt->DrawText(label, static_cast<UINT32>(wcslen(label)), labelFmt.Get(), labelRect, brush,
                 D2D1_DRAW_TEXT_OPTIONS_CLIP);

    // Value
    brush->SetColor(Colors::Text);
    D2D1_RECT_F valueRect = D2D1::RectF(x + labelWidth, y, x + width, y + rowHeight);
    rt->DrawText(value.c_str(), static_cast<UINT32>(value.length()), valueFmt.Get(), valueRect, brush,
                 D2D1_DRAW_TEXT_OPTIONS_CLIP);

    y += rowHeight + 4.0f;
}

void FileInfoView::Render(ID2D1RenderTarget* rt, IDWriteFactory* dwrite,
                           const D2D1_RECT_F& area, ID2D1SolidColorBrush* brush) {
    if (!rt || !dwrite || !brush) return;

    float areaW = area.right - area.left;
    float areaH = area.bottom - area.top;
    float centerX = area.left + areaW / 2.0f;

    // ── Draw file icon ──
    float iconSize = 80.0f;
    float iconY = area.top + 28.0f;

    // Icon background circle
    D2D1_ELLIPSE iconBgCircle = D2D1::Ellipse(
        D2D1::Point2F(centerX, iconY + iconSize / 2.0f),
        iconSize / 2.0f + 16.0f, iconSize / 2.0f + 16.0f);
    brush->SetColor(Colors::IconBg);
    rt->FillEllipse(iconBgCircle, brush);

    if (m_iconBitmap) {
        auto bmpSize = m_iconBitmap->GetSize();
        float scale = iconSize / (std::max)(bmpSize.width, bmpSize.height);
        float drawW = bmpSize.width * scale;
        float drawH = bmpSize.height * scale;
        D2D1_RECT_F iconRect = D2D1::RectF(
            centerX - drawW / 2.0f, iconY + (iconSize - drawH) / 2.0f,
            centerX + drawW / 2.0f, iconY + (iconSize + drawH) / 2.0f);
        rt->DrawBitmap(m_iconBitmap.Get(), iconRect);
    }

    // ── File name with word wrapping ──
    float textY = iconY + iconSize + 22.0f;
    ComPtr<IDWriteTextFormat> nameFont;
    dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 16.0f, L"en-US", &nameFont);
    if (nameFont) {
        nameFont->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        nameFont->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);

        float maxTextW = areaW - 48.0f;
        if (maxTextW < 120.0f) maxTextW = 120.0f;

        ComPtr<IDWriteTextLayout> textLayout;
        HRESULT hr = dwrite->CreateTextLayout(
            m_info.name.c_str(),
            static_cast<UINT32>(m_info.name.length()),
            nameFont.Get(),
            maxTextW,
            120.0f,
            &textLayout);

        if (SUCCEEDED(hr) && textLayout) {
            textLayout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            textLayout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);

            DWRITE_TRIMMING trimming = { DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
            ComPtr<IDWriteInlineObject> ellipsisSign;
            if (SUCCEEDED(dwrite->CreateEllipsisTrimmingSign(nameFont.Get(), &ellipsisSign))) {
                textLayout->SetTrimming(&trimming, ellipsisSign.Get());
            }

            DWRITE_TEXT_METRICS metrics = {};
            textLayout->GetMetrics(&metrics);

            brush->SetColor(Colors::Text);
            rt->DrawTextLayout(
                D2D1::Point2F(area.left + 24.0f, textY),
                textLayout.Get(),
                brush);

            textY += metrics.height + 14.0f;
        } else {
            brush->SetColor(Colors::Text);
            D2D1_RECT_F nameRect = D2D1::RectF(area.left + 24.0f, textY, area.right - 24.0f, textY + 36.0f);
            rt->DrawText(m_info.name.c_str(), static_cast<UINT32>(m_info.name.length()),
                         nameFont.Get(), nameRect, brush);
            textY += 40.0f;
        }
    }

    // ── File type badge ──
    ComPtr<IDWriteTextFormat> badgeFont;
    dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 11.0f, L"en-US", &badgeFont);
    if (badgeFont) {
        badgeFont->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);

        std::wstring typeStr = m_info.typeName;
        float badgeW = (std::max)(static_cast<float>(typeStr.length()) * 7.5f + 24.0f, 80.0f);
        float badgeH = 24.0f;
        D2D1_ROUNDED_RECT badgeBg = D2D1::RoundedRect(
            D2D1::RectF(centerX - badgeW / 2.0f, textY,
                        centerX + badgeW / 2.0f, textY + badgeH),
            badgeH / 2.0f, badgeH / 2.0f);
        brush->SetColor(Colors::Badge);
        rt->FillRoundedRectangle(badgeBg, brush);

        brush->SetColor(Colors::TextDim);
        D2D1_RECT_F badgeRect = D2D1::RectF(
            centerX - badgeW / 2.0f, textY + 3.0f,
            centerX + badgeW / 2.0f, textY + badgeH);
        rt->DrawText(typeStr.c_str(), static_cast<UINT32>(typeStr.length()),
                     badgeFont.Get(), badgeRect, brush);
        textY += badgeH + 20.0f;
    }

    // ── Separator ──
    brush->SetColor(Colors::Separator);
    float sepX = area.left + 36.0f;
    float sepW = areaW - 72.0f;
    rt->FillRectangle(D2D1::RectF(sepX, textY, sepX + sepW, textY + 1.0f), brush);
    textY += 14.0f;

    // ── Info rows ──
    float infoX = area.left + 36.0f;
    float infoW = areaW - 72.0f;

    DrawInfoRow(rt, dwrite, brush, L"Size", Utils::FormatFileSize(m_info.size),
                infoX, textY, infoW);
    DrawInfoRow(rt, dwrite, brush, L"Created", Utils::FormatDateTime(m_info.creationTime),
                infoX, textY, infoW);
    DrawInfoRow(rt, dwrite, brush, L"Modified", Utils::FormatDateTime(m_info.modifiedTime),
                infoX, textY, infoW);
    DrawInfoRow(rt, dwrite, brush, L"Location", m_info.directory,
                infoX, textY, infoW);

    // Attributes
    std::wstring attrs;
    if (m_info.isReadOnly) attrs += L"Read-Only  ";
    if (m_info.isHidden)   attrs += L"Hidden  ";
    if (m_info.isSystem)   attrs += L"System";
    if (!attrs.empty()) {
        DrawInfoRow(rt, dwrite, brush, L"Attributes", attrs, infoX, textY, infoW);
    }
}

void FileInfoView::Reset() {
    m_iconBitmap.Reset();
    m_info = {};
    m_filePath.clear();
}
