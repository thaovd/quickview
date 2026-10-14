#pragma once

#include "Utils.h"
#include <fpdfview.h>
#include <string>

class PdfPreview {
public:
    PdfPreview();
    ~PdfPreview();

    /// Global PDFium library init/cleanup
    static void InitLibrary();
    static void DestroyLibrary();

    /// Load PDF file
    bool Load(const std::wstring& filePath);

    /// Attach document loaded and rendered in background thread
    bool AttachLoadedDocument(
        FPDF_DOCUMENT doc,
        int pageCount,
        double pagePtW,
        double pagePtH,
        const uint8_t* pixelBuffer,
        int renderW,
        int renderH,
        int stride,
        ID2D1RenderTarget* rt);

    /// D2D Bitmap creation for current page
    bool CreateBitmap(ID2D1RenderTarget* rt, float targetWidth = 0.0f, float targetHeight = 0.0f);

    /// Render current page onto Direct2D render target
    void Render(ID2D1RenderTarget* rt, const D2D1_RECT_F& area);

    /// Multi-page navigation
    bool NextPage(ID2D1RenderTarget* rt);
    bool PrevPage(ID2D1RenderTarget* rt);
    bool GoToPage(int pageIndex, ID2D1RenderTarget* rt);

    /// Mouse interactions (Zoom & Pan)
    void OnMouseWheel(int delta, int mx, int my, const D2D1_RECT_F& area);
    void OnMouseDown(int mx, int my);
    void OnMouseMove(int mx, int my, HWND hwnd);
    void OnMouseUp();
    void ResetView(const D2D1_RECT_F& area);

    int  GetCurrentPage() const { return m_currentPage; } // 0-based
    int  GetPageCount()   const { return m_pageCount; }
    bool IsLoaded()       const { return m_doc != nullptr; }

    SIZE GetPreferredSize() const;
    void Reset();

private:
    static bool s_libraryInitialized;

    FPDF_DOCUMENT m_doc        = nullptr;
    FPDF_PAGE     m_curPageDoc = nullptr;
    int           m_currentPage = 0;
    int           m_pageCount   = 0;
    double        m_pagePtW     = 0.0;
    double        m_pagePtH     = 0.0;

    ComPtr<ID2D1Bitmap> m_bitmap;
    UINT  m_bmpWidth  = 0;
    UINT  m_bmpHeight = 0;

    float m_zoom = 1.0f;
    float m_panX = 0.0f;
    float m_panY = 0.0f;

    bool  m_dragging  = false;
    POINT m_dragStart = {};
    float m_dragPanX  = 0.0f;
    float m_dragPanY  = 0.0f;

    bool RenderCurrentPageToBitmap(ID2D1RenderTarget* rt, float targetWidth, float targetHeight);
};
