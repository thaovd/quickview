#include "PdfPreview.h"
#include <algorithm>
#include <vector>

bool PdfPreview::s_libraryInitialized = false;

void PdfPreview::InitLibrary() {
    if (!s_libraryInitialized) {
        FPDF_InitLibrary();
        s_libraryInitialized = true;
    }
}

void PdfPreview::DestroyLibrary() {
    if (s_libraryInitialized) {
        FPDF_DestroyLibrary();
        s_libraryInitialized = false;
    }
}

PdfPreview::PdfPreview() {
    InitLibrary();
}

PdfPreview::~PdfPreview() {
    Reset();
}

bool PdfPreview::Load(const std::wstring& filePath) {
    Reset();
    InitLibrary();

    // Convert file path to UTF-8
    int u8Len = WideCharToMultiByte(CP_UTF8, 0, filePath.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (u8Len <= 1) return false;

    std::string u8Path(u8Len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, filePath.c_str(), -1, &u8Path[0], u8Len, nullptr, nullptr);

    m_doc = FPDF_LoadDocument(u8Path.c_str(), nullptr);
    if (!m_doc) {
        return false;
    }

    m_pageCount = FPDF_GetPageCount(m_doc);
    if (m_pageCount <= 0) {
        Reset();
        return false;
    }

    m_currentPage = 0;
    FPDF_GetPageSizeByIndex(m_doc, 0, &m_pagePtW, &m_pagePtH);
    return true;
}

bool PdfPreview::AttachLoadedDocument(
    FPDF_DOCUMENT doc,
    int pageCount,
    double pagePtW,
    double pagePtH,
    const uint8_t* pixelBuffer,
    int renderW,
    int renderH,
    int stride,
    ID2D1RenderTarget* rt)
{
    Reset();
    InitLibrary();
    if (!doc || pageCount <= 0) return false;

    m_doc = doc;
    m_pageCount = pageCount;
    m_currentPage = 0;
    m_pagePtW = pagePtW;
    m_pagePtH = pagePtH;

    if (pixelBuffer && renderW > 0 && renderH > 0 && rt) {
        D2D1_BITMAP_PROPERTIES props;
        props.pixelFormat.format    = DXGI_FORMAT_B8G8R8A8_UNORM;
        props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_IGNORE;
        props.dpiX                  = 96.0f;
        props.dpiY                  = 96.0f;

        HRESULT hr = rt->CreateBitmap(D2D1::SizeU(renderW, renderH), pixelBuffer, stride, &props, &m_bitmap);
        if (SUCCEEDED(hr) && m_bitmap) {
            m_bmpWidth  = renderW;
            m_bmpHeight = renderH;
        }
    }
    return true;
}

bool PdfPreview::RenderCurrentPageToBitmap(ID2D1RenderTarget* rt, float targetWidth, float targetHeight) {
    m_bitmap.Reset();
    if (!m_doc || !rt || m_currentPage < 0 || m_currentPage >= m_pageCount) {
        return false;
    }

    FPDF_GetPageSizeByIndex(m_doc, m_currentPage, &m_pagePtW, &m_pagePtH);
    if (m_pagePtW <= 0 || m_pagePtH <= 0) {
        m_pagePtW = 595.0;
        m_pagePtH = 842.0;
    }

    // High quality render scale (2.0x for crisp text on high DPI)
    float scale = 2.0f;
    if (targetWidth > 0 && targetHeight > 0) {
        float sx = (targetWidth * 1.5f) / static_cast<float>(m_pagePtW);
        float sy = (targetHeight * 1.5f) / static_cast<float>(m_pagePtH);
        scale = (std::max)(1.5f, (std::min)(sx, sy));
    }

    int renderW = static_cast<int>(m_pagePtW * scale);
    int renderH = static_cast<int>(m_pagePtH * scale);
    renderW = (std::max)(600, (std::min)(renderW, 3200));
    renderH = (std::max)(800, (std::min)(renderH, 4200));

    FPDF_PAGE page = FPDF_LoadPage(m_doc, m_currentPage);
    if (!page) return false;

    FPDF_BITMAP fpdfBmp = FPDFBitmap_CreateEx(renderW, renderH, FPDFBitmap_BGRA, nullptr, 0);
    if (!fpdfBmp) {
        FPDF_ClosePage(page);
        return false;
    }

    // White background
    FPDFBitmap_FillRect(fpdfBmp, 0, 0, renderW, renderH, 0xFFFFFFFF);

    // Render page with LCD anti-aliased text and annotations
    FPDF_RenderPageBitmap(fpdfBmp, page, 0, 0, renderW, renderH, 0, FPDF_ANNOT | FPDF_LCD_TEXT);

    void* buffer = FPDFBitmap_GetBuffer(fpdfBmp);
    int stride   = FPDFBitmap_GetStride(fpdfBmp);

    D2D1_BITMAP_PROPERTIES props;
    props.pixelFormat.format    = DXGI_FORMAT_B8G8R8A8_UNORM;
    props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_IGNORE;
    props.dpiX                  = 96.0f;
    props.dpiY                  = 96.0f;

    HRESULT hr = rt->CreateBitmap(D2D1::SizeU(renderW, renderH), buffer, stride, &props, &m_bitmap);

    FPDFBitmap_Destroy(fpdfBmp);
    FPDF_ClosePage(page);

    if (SUCCEEDED(hr) && m_bitmap) {
        m_bmpWidth  = renderW;
        m_bmpHeight = renderH;
        return true;
    }

    return false;
}

bool PdfPreview::CreateBitmap(ID2D1RenderTarget* rt, float targetWidth, float targetHeight) {
    return RenderCurrentPageToBitmap(rt, targetWidth, targetHeight);
}

void PdfPreview::Render(ID2D1RenderTarget* rt, const D2D1_RECT_F& area) {
    if (!rt || !m_bitmap) return;

    auto bmpSize = m_bitmap->GetSize();
    float areaW = area.right - area.left;
    float areaH = area.bottom - area.top;

    float scaledW = bmpSize.width * m_zoom;
    float scaledH = bmpSize.height * m_zoom;
    float cx = area.left + (areaW - scaledW) / 2.0f + m_panX;
    float cy = area.top  + (areaH - scaledH) / 2.0f + m_panY;

    D2D1_RECT_F dest = D2D1::RectF(cx, cy, cx + scaledW, cy + scaledH);

    // Subtle drop shadow behind the PDF page
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> shadowBrush;
    rt->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.35f), &shadowBrush);
    if (shadowBrush) {
        D2D1_ROUNDED_RECT shadowRect = D2D1::RoundedRect(
            D2D1::RectF(dest.left + 4.0f, dest.top + 4.0f, dest.right + 6.0f, dest.bottom + 6.0f),
            4.0f, 4.0f);
        rt->FillRoundedRectangle(shadowRect, shadowBrush.Get());
    }

    // Draw PDF page bitmap
    auto interp = (m_zoom > 3.0f)
        ? D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR
        : D2D1_BITMAP_INTERPOLATION_MODE_LINEAR;

    rt->DrawBitmap(m_bitmap.Get(), dest, 1.0f, interp);

    // 1px subtle page border
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> borderBrush;
    rt->CreateSolidColorBrush(D2D1::ColorF(0.3f, 0.3f, 0.35f, 0.4f), &borderBrush);
    if (borderBrush) {
        rt->DrawRectangle(dest, borderBrush.Get(), 1.0f);
    }
}

bool PdfPreview::NextPage(ID2D1RenderTarget* rt) {
    if (m_currentPage + 1 < m_pageCount) {
        m_currentPage++;
        if (RenderCurrentPageToBitmap(rt, 0, 0)) {
            return true;
        }
    }
    return false;
}

bool PdfPreview::PrevPage(ID2D1RenderTarget* rt) {
    if (m_currentPage > 0) {
        m_currentPage--;
        if (RenderCurrentPageToBitmap(rt, 0, 0)) {
            return true;
        }
    }
    return false;
}

bool PdfPreview::GoToPage(int pageIndex, ID2D1RenderTarget* rt) {
    if (pageIndex >= 0 && pageIndex < m_pageCount && pageIndex != m_currentPage) {
        m_currentPage = pageIndex;
        return RenderCurrentPageToBitmap(rt, 0, 0);
    }
    return false;
}

void PdfPreview::OnMouseWheel(int delta, int mx, int my, const D2D1_RECT_F& area) {
    float factor = (delta > 0) ? 1.12f : 0.89f;
    float newZoom = m_zoom * factor;
    newZoom = (std::max)(0.05f, (std::min)(newZoom, 20.0f));

    float areaCX = (area.left + area.right) / 2.0f;
    float areaCY = (area.top + area.bottom) / 2.0f;
    float relX = mx - areaCX;
    float relY = my - areaCY;

    m_panX = relX + (m_panX - relX) * (newZoom / m_zoom);
    m_panY = relY + (m_panY - relY) * (newZoom / m_zoom);
    m_zoom = newZoom;
}

void PdfPreview::OnMouseDown(int mx, int my) {
    m_dragging  = true;
    m_dragStart = { mx, my };
    m_dragPanX  = m_panX;
    m_dragPanY  = m_panY;
}

void PdfPreview::OnMouseMove(int mx, int my, HWND hwnd) {
    if (!m_dragging) return;
    m_panX = m_dragPanX + (mx - m_dragStart.x);
    m_panY = m_dragPanY + (my - m_dragStart.y);
    if (hwnd) InvalidateRect(hwnd, nullptr, FALSE);
}

void PdfPreview::OnMouseUp() {
    m_dragging = false;
    ReleaseCapture();
}

void PdfPreview::ResetView(const D2D1_RECT_F& area) {
    if (m_bmpWidth == 0 || m_bmpHeight == 0) return;

    float areaW = area.right - area.left - 24.0f;
    float areaH = area.bottom - area.top - 24.0f;
    if (areaW <= 0 || areaH <= 0) return;

    float scaleX = areaW / static_cast<float>(m_bmpWidth);
    float scaleY = areaH / static_cast<float>(m_bmpHeight);
    m_zoom = (std::min)(scaleX, scaleY);

    m_panX = 0.0f;
    m_panY = 0.0f;
}

SIZE PdfPreview::GetPreferredSize() const {
    if (m_pagePtW <= 0 || m_pagePtH <= 0) {
        return { 720, 800 };
    }

    float aspect = static_cast<float>(m_pagePtW) / static_cast<float>(m_pagePtH);
    int h = PREVIEW_MAX_HEIGHT - TITLEBAR_HEIGHT - TOOLBAR_HEIGHT;
    int w = static_cast<int>(h * aspect);

    w = (std::max)(static_cast<int>(PREVIEW_MIN_WIDTH), (std::min)(w, PREVIEW_MAX_WIDTH));
    h = (std::max)(static_cast<int>(PREVIEW_MIN_HEIGHT - TITLEBAR_HEIGHT - TOOLBAR_HEIGHT), h);

    return { w, h + TITLEBAR_HEIGHT + TOOLBAR_HEIGHT };
}

void PdfPreview::Reset() {
    m_bitmap.Reset();
    if (m_doc) {
        FPDF_CloseDocument(m_doc);
        m_doc = nullptr;
    }
    m_curPageDoc  = nullptr;
    m_currentPage = 0;
    m_pageCount   = 0;
    m_pagePtW     = 0.0;
    m_pagePtH     = 0.0;
    m_bmpWidth    = 0;
    m_bmpHeight   = 0;
    m_zoom        = 1.0f;
    m_panX        = 0.0f;
    m_panY        = 0.0f;
    m_dragging    = false;
}
