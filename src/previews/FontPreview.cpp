#include "FontPreview.h"
#include "Utils.h"
#include <algorithm>
#include <sstream>

FontPreview::FontPreview() {}
FontPreview::~FontPreview() {
    Unload();
}

void FontPreview::Unload() {
    m_fontFace3.Reset();
    m_fontFace.Reset();
    m_fontFile.Reset();
    m_filePath.clear();
    m_fileName.clear();
    m_familyName.clear();
    m_faceName.clear();
    m_formatName.clear();
    m_glyphCount = 0;
    m_metrics = {};
    m_scrollY = 0.0f;
    DiscardResources();
}

bool FontPreview::Load(const std::wstring& path, IDWriteFactory* dwriteFactory) {
    Unload();
    if (!dwriteFactory) return false;

    m_filePath = path;
    m_fileName = Utils::GetFileName(path);

    HRESULT hr = dwriteFactory->CreateFontFileReference(path.c_str(), nullptr, &m_fontFile);
    if (FAILED(hr) || !m_fontFile) return false;

    BOOL isSupported = FALSE;
    DWRITE_FONT_FILE_TYPE fileType;
    DWRITE_FONT_FACE_TYPE faceType;
    UINT32 numberOfFaces = 0;
    hr = m_fontFile->Analyze(&isSupported, &fileType, &faceType, &numberOfFaces);
    if (FAILED(hr) || !isSupported || numberOfFaces == 0) return false;

    switch (faceType) {
        case DWRITE_FONT_FACE_TYPE_CFF: m_formatName = L"OpenType (CFF)"; break;
        case DWRITE_FONT_FACE_TYPE_TRUETYPE: m_formatName = L"TrueType"; break;
        case DWRITE_FONT_FACE_TYPE_TRUETYPE_COLLECTION: m_formatName = L"TrueType Collection"; break;
        case DWRITE_FONT_FACE_TYPE_TYPE1: m_formatName = L"Type 1"; break;
        case DWRITE_FONT_FACE_TYPE_VECTOR: m_formatName = L"Vector Font"; break;
        case DWRITE_FONT_FACE_TYPE_BITMAP: m_formatName = L"Bitmap Font"; break;
        default: m_formatName = L"OpenType Font"; break;
    }

    IDWriteFontFile* fontFiles[] = { m_fontFile.Get() };
    hr = dwriteFactory->CreateFontFace(faceType, 1, fontFiles, 0, DWRITE_FONT_SIMULATIONS_NONE, &m_fontFace);
    if (FAILED(hr) || !m_fontFace) return false;

    m_glyphCount = m_fontFace->GetGlyphCount();
    m_fontFace->GetMetrics(&m_metrics);
    m_weight = DWRITE_FONT_WEIGHT_NORMAL;

    // Query IDWriteFontFace3 for family and face localized names
    hr = m_fontFace.As(&m_fontFace3);
    if (SUCCEEDED(hr) && m_fontFace3) {
        m_weight = m_fontFace3->GetWeight();

        Microsoft::WRL::ComPtr<IDWriteLocalizedStrings> familyNames;
        if (SUCCEEDED(m_fontFace3->GetFamilyNames(&familyNames)) && familyNames) {
            UINT32 len = 0;
            familyNames->GetStringLength(0, &len);
            m_familyName.resize(len + 1);
            familyNames->GetString(0, &m_familyName[0], len + 1);
            if (!m_familyName.empty() && m_familyName.back() == L'\0') m_familyName.pop_back();
        }

        Microsoft::WRL::ComPtr<IDWriteLocalizedStrings> faceNames;
        if (SUCCEEDED(m_fontFace3->GetFaceNames(&faceNames)) && faceNames) {
            UINT32 len = 0;
            faceNames->GetStringLength(0, &len);
            m_faceName.resize(len + 1);
            faceNames->GetString(0, &m_faceName[0], len + 1);
            if (!m_faceName.empty() && m_faceName.back() == L'\0') m_faceName.pop_back();
        }
    }

    if (m_familyName.empty()) {
        std::wstring fn = Utils::GetFileName(path);
        size_t dot = fn.rfind(L'.');
        if (dot != std::wstring::npos) fn = fn.substr(0, dot);
        m_familyName = fn;
    }
    if (m_faceName.empty()) {
        m_faceName = L"Regular";
    }

    return true;
}

void FontPreview::OnMouseWheel(int delta) {
    float step = 60.0f;
    if (delta > 0) {
        m_scrollY = (std::max)(0.0f, m_scrollY - step);
    } else {
        m_scrollY += step;
    }
}

void FontPreview::CreateResources(ID2D1RenderTarget* rt) {
    if (m_textBrush) return;
    rt->CreateSolidColorBrush(D2D1::ColorF(0.94f, 0.96f, 0.98f, 1.0f), &m_textBrush);
    rt->CreateSolidColorBrush(D2D1::ColorF(0.55f, 0.60f, 0.68f, 1.0f), &m_dimBrush);
    rt->CreateSolidColorBrush(D2D1::ColorF(0.35f, 0.65f, 1.0f, 1.0f), &m_accentBrush);
    rt->CreateSolidColorBrush(D2D1::ColorF(0.14f, 0.18f, 0.24f, 0.9f), &m_badgeBgBrush);
    rt->CreateSolidColorBrush(D2D1::ColorF(0.10f, 0.13f, 0.18f, 0.7f), &m_cardBgBrush);
    rt->CreateSolidColorBrush(D2D1::ColorF(0.20f, 0.25f, 0.32f, 1.0f), &m_borderBrush);
}

void FontPreview::DiscardResources() {
    m_textBrush.Reset();
    m_dimBrush.Reset();
    m_accentBrush.Reset();
    m_badgeBgBrush.Reset();
    m_cardBgBrush.Reset();
    m_borderBrush.Reset();
}

void FontPreview::DrawGlyphString(ID2D1RenderTarget* rt, float fontSize, const std::wstring& text,
                                  D2D1_POINT_2F origin, ID2D1Brush* brush, float maxWidth) {
    if (!m_fontFace || text.empty() || !rt || m_metrics.designUnitsPerEm == 0) return;

    UINT32 len = static_cast<UINT32>(text.length());
    std::vector<UINT32> codePoints(len);
    for (UINT32 i = 0; i < len; ++i) {
        codePoints[i] = static_cast<UINT32>(text[i]);
    }

    std::vector<UINT16> glyphIndices(len);
    if (FAILED(m_fontFace->GetGlyphIndicesW(codePoints.data(), len, glyphIndices.data()))) {
        return;
    }

    std::vector<DWRITE_GLYPH_METRICS> glyphMetrics(len);
    if (FAILED(m_fontFace->GetDesignGlyphMetrics(glyphIndices.data(), len, glyphMetrics.data()))) {
        return;
    }

    float scale = fontSize / static_cast<float>(m_metrics.designUnitsPerEm);
    std::vector<FLOAT> advances(len);
    float totalW = 0.0f;
    UINT32 actualCount = 0;

    for (UINT32 i = 0; i < len; ++i) {
        float adv = static_cast<float>(glyphMetrics[i].advanceWidth) * scale;
        if (maxWidth > 0.0f && (totalW + adv) > maxWidth) {
            break;
        }
        advances[i] = adv;
        totalW += adv;
        actualCount++;
    }

    if (actualCount == 0) return;

    DWRITE_GLYPH_RUN glyphRun = {};
    glyphRun.fontFace = m_fontFace.Get();
    glyphRun.fontEmSize = fontSize;
    glyphRun.glyphCount = actualCount;
    glyphRun.glyphIndices = glyphIndices.data();
    glyphRun.glyphAdvances = advances.data();
    glyphRun.glyphOffsets = nullptr;
    glyphRun.isSideways = FALSE;
    glyphRun.bidiLevel = 0;

    rt->DrawGlyphRun(origin, &glyphRun, brush);
}

void FontPreview::Render(ID2D1RenderTarget* rt, IDWriteFactory* dwriteFactory, const D2D1_RECT_F& area) {
    if (!m_fontFace || !rt || !dwriteFactory) return;
    CreateResources(rt);

    float areaW = area.right - area.left;
    float areaH = area.bottom - area.top;
    if (areaW <= 0 || areaH <= 0) return;

    // 1. Header Card (Hero)
    float headerH = 110.0f;
    D2D1_RECT_F headerRect = D2D1::RectF(area.left + 24.0f, area.top + 16.0f, area.right - 24.0f, area.top + 16.0f + headerH);

    D2D1_ROUNDED_RECT roundedHeader = D2D1::RoundedRect(headerRect, 8.0f, 8.0f);
    rt->FillRoundedRectangle(roundedHeader, m_cardBgBrush.Get());
    rt->DrawRoundedRectangle(roundedHeader, m_borderBrush.Get(), 1.0f);

    // Hero Character Box (Aa)
    float heroBoxW = 90.0f;
    D2D1_RECT_F heroBox = D2D1::RectF(headerRect.left + 12.0f, headerRect.top + 10.0f,
                                      headerRect.left + 12.0f + heroBoxW, headerRect.bottom - 10.0f);
    D2D1_ROUNDED_RECT roundedHero = D2D1::RoundedRect(heroBox, 6.0f, 6.0f);
    rt->FillRoundedRectangle(roundedHero, m_badgeBgBrush.Get());
    rt->DrawRoundedRectangle(roundedHero, m_borderBrush.Get(), 0.8f);

    // Draw "Aa" inside hero box with native font face
    float heroFontSize = 54.0f;
    float heroBaseline = heroBox.top + (heroBox.bottom - heroBox.top) * 0.72f;
    DrawGlyphString(rt, heroFontSize, L"Aa", D2D1::Point2F(heroBox.left + 14.0f, heroBaseline), m_accentBrush.Get(), heroBoxW);

    // Typography Labels next to hero box
    float textX = heroBox.right + 18.0f;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> nameFormat;
    dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD,
                                   DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                   22.0f, L"en-us", &nameFormat);

    Microsoft::WRL::ComPtr<IDWriteTextFormat> pillFormat;
    dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_MEDIUM,
                                   DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                   11.0f, L"en-us", &pillFormat);

    if (nameFormat) {
        D2D1_RECT_F nameRect = D2D1::RectF(textX, headerRect.top + 14.0f, headerRect.right - 20.0f, headerRect.top + 44.0f);
        rt->DrawTextW(m_familyName.c_str(), static_cast<UINT32>(m_familyName.length()), nameFormat.Get(), nameRect, m_textBrush.Get());
    }

    // Badges / pills row
    if (pillFormat) {
        std::vector<std::wstring> badges = {
            m_faceName,
            m_formatName,
            std::to_wstring(m_glyphCount) + L" glyphs",
            L"Weight " + std::to_wstring(m_weight)
        };

        float badgeX = textX;
        float badgeY = headerRect.top + 52.0f;
        float badgeHeight = 22.0f;

        for (const auto& b : badges) {
            float badgeWidth = b.length() * 7.5f + 16.0f;
            D2D1_RECT_F bRect = D2D1::RectF(badgeX, badgeY, badgeX + badgeWidth, badgeY + badgeHeight);
            D2D1_ROUNDED_RECT bRounded = D2D1::RoundedRect(bRect, 4.0f, 4.0f);

            rt->FillRoundedRectangle(bRounded, m_badgeBgBrush.Get());
            rt->DrawRoundedRectangle(bRounded, m_borderBrush.Get(), 0.8f);

            pillFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            pillFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            rt->DrawTextW(b.c_str(), static_cast<UINT32>(b.length()), pillFormat.Get(), bRect, m_dimBrush.Get());

            badgeX += badgeWidth + 8.0f;
        }
    }

    // 2. Size Ramp Section (Scrollable viewport)
    float rampTop = headerRect.bottom + 16.0f;
    float rampBottom = area.bottom - 16.0f;
    float viewHeight = rampBottom - rampTop;
    if (viewHeight <= 0) return;

    D2D1_RECT_F clipRect = D2D1::RectF(area.left + 24.0f, rampTop, area.right - 24.0f, rampBottom);
    rt->PushAxisAlignedClip(clipRect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    struct SampleItem {
        float size;
        std::wstring label;
        std::wstring text;
    };

    std::vector<SampleItem> samples = {
        { 36.0f, L"36 pt", L"The quick brown fox jumps over the lazy dog" },
        { 26.0f, L"26 pt", L"Pack my box with five dozen liquor jugs" },
        { 20.0f, L"20 pt", L"Quick wafting zephyrs vex bold Jim" },
        { 16.0f, L"16 pt", L"ABCDEFGHIJKLMNOPQRSTUVWXYZ" },
        { 16.0f, L"16 pt", L"abcdefghijklmnopqrstuvwxyz" },
        { 16.0f, L"16 pt", L"0123456789  •  !@#$%^&*()_+-=[]{}|;:,.<>?" },
        { 14.0f, L"14 pt", L"Sphinx of black quartz, judge my vow  •  1234567890" },
        { 12.0f, L"12 pt", L"To be or not to be, that is the question: Whether 'tis nobler in the mind to suffer the slings and arrows of outrageous fortune." },
        { 10.0f, L"10 pt", L"Typography is the art and technique of arranging type to make written language legible, readable, and appealing when displayed." }
    };

    float curY = rampTop - m_scrollY;
    float maxContentW = clipRect.right - clipRect.left;

    Microsoft::WRL::ComPtr<IDWriteTextFormat> sizeLabelFormat;
    dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                   DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                   11.0f, L"en-us", &sizeLabelFormat);

    for (const auto& sample : samples) {
        float itemH = sample.size * 1.5f + 24.0f;

        // Label above sample
        if (sizeLabelFormat && curY + 14.0f >= rampTop && curY <= rampBottom) {
            D2D1_RECT_F lblRect = D2D1::RectF(clipRect.left, curY, clipRect.left + 80.0f, curY + 14.0f);
            rt->DrawTextW(sample.label.c_str(), static_cast<UINT32>(sample.label.length()), sizeLabelFormat.Get(), lblRect, m_accentBrush.Get());
        }

        // Font text glyph run
        float textY = curY + 18.0f;
        float baseline = textY + (sample.size * static_cast<float>(m_metrics.ascent) / static_cast<float>(m_metrics.designUnitsPerEm));

        if (baseline >= rampTop - 20.0f && baseline <= rampBottom + 20.0f) {
            DrawGlyphString(rt, sample.size, sample.text, D2D1::Point2F(clipRect.left, baseline), m_textBrush.Get(), maxContentW);
        }

        // Subtle divider line
        float lineY = curY + itemH - 4.0f;
        if (lineY >= rampTop && lineY <= rampBottom) {
            rt->DrawLine(D2D1::Point2F(clipRect.left, lineY), D2D1::Point2F(clipRect.right, lineY), m_borderBrush.Get(), 0.5f);
        }

        curY += itemH;
    }

    m_contentHeight = curY - (rampTop - m_scrollY);
    float maxScroll = (std::max)(0.0f, m_contentHeight - viewHeight);
    if (m_scrollY > maxScroll) m_scrollY = maxScroll;

    rt->PopAxisAlignedClip();

    // Scrollbar if needed
    if (m_contentHeight > viewHeight) {
        float scrollbarWidth = 6.0f;
        float scrollbarX = clipRect.right - scrollbarWidth;
        float trackHeight = viewHeight;
        float thumbHeight = (std::max)(24.0f, (viewHeight / m_contentHeight) * trackHeight);
        float thumbY = rampTop + (m_scrollY / (m_contentHeight - viewHeight)) * (trackHeight - thumbHeight);

        D2D1_ROUNDED_RECT thumbRect = D2D1::RoundedRect(
            D2D1::RectF(scrollbarX, thumbY, scrollbarX + scrollbarWidth, thumbY + thumbHeight),
            3.0f, 3.0f);
        rt->FillRoundedRectangle(thumbRect, m_dimBrush.Get());
    }
}
