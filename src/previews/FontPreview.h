#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <dwrite_3.h>
#include <wrl/client.h>
#include <string>
#include <vector>

class FontPreview {
public:
    FontPreview();
    ~FontPreview();

    bool Load(const std::wstring& path, IDWriteFactory* dwriteFactory);
    void Unload();

    void Render(ID2D1RenderTarget* rt, IDWriteFactory* dwriteFactory, const D2D1_RECT_F& area);
    void OnMouseWheel(int delta);

    bool IsLoaded() const { return m_fontFace != nullptr; }

private:
    std::wstring m_filePath;
    std::wstring m_fileName;
    std::wstring m_familyName;
    std::wstring m_faceName;
    std::wstring m_formatName;
    UINT32 m_glyphCount = 0;
    DWRITE_FONT_METRICS m_metrics = {};
    DWRITE_FONT_WEIGHT m_weight = DWRITE_FONT_WEIGHT_NORMAL;

    Microsoft::WRL::ComPtr<IDWriteFontFile> m_fontFile;
    Microsoft::WRL::ComPtr<IDWriteFontFace> m_fontFace;
    Microsoft::WRL::ComPtr<IDWriteFontFace3> m_fontFace3;

    float m_scrollY = 0.0f;
    float m_contentHeight = 0.0f;

    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_textBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_dimBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_accentBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_badgeBgBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_cardBgBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_borderBrush;

    void CreateResources(ID2D1RenderTarget* rt);
    void DiscardResources();

    void DrawGlyphString(ID2D1RenderTarget* rt, float fontSize, const std::wstring& text,
                         D2D1_POINT_2F origin, ID2D1Brush* brush, float maxWidth = 0.0f);
};
