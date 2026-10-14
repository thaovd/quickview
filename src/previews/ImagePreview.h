#pragma once

#include "Utils.h"

/// Loads and renders image files using WIC (decode) + Direct2D (render).
/// Supports zoom/pan interaction via mouse.
class ImagePreview {
public:
    ImagePreview() = default;
    ~ImagePreview() = default;

    /// Load image from file into WIC format converter (CPU decode)
    bool LoadFile(const std::wstring& path, IWICImagingFactory* wicFactory);

    /// Load image from memory buffer into WIC format converter
    bool LoadMemory(const void* data, size_t size, IWICImagingFactory* wicFactory);

    /// Create GPU-side D2D bitmap from the loaded WIC data
    bool CreateBitmap(ID2D1RenderTarget* rt);

    /// Render the image into the given rectangle on the render target
    void Render(ID2D1RenderTarget* rt, const D2D1_RECT_F& area);

    /// Handle mouse wheel for zoom (centered on cursor)
    void OnMouseWheel(int delta, int mx, int my, const D2D1_RECT_F& area);

    /// Handle mouse button down for pan
    void OnMouseDown(int mx, int my);

    /// Handle mouse move for pan
    void OnMouseMove(int mx, int my, HWND hwnd);

    /// Handle mouse button up
    void OnMouseUp();

    /// Reset view to fit-to-window
    void ResetView(const D2D1_RECT_F& area);

    /// Get preferred window size for this image
    SIZE GetPreferredSize() const;

    /// Image dimensions info string (e.g. "1920 × 1080")
    std::wstring GetDimensionsString() const;

    UINT GetWidth()  const { return m_width; }
    UINT GetHeight() const { return m_height; }
    bool IsLoaded()  const { return m_converter != nullptr; }

    void Reset();

private:
    ComPtr<IWICFormatConverter> m_converter;
    ComPtr<ID2D1Bitmap>         m_bitmap;
    ComPtr<IStream>             m_stream;

    UINT  m_width  = 0;
    UINT  m_height = 0;
    float m_zoom   = 1.0f;
    float m_panX   = 0.0f;
    float m_panY   = 0.0f;

    bool  m_dragging    = false;
    POINT m_dragStart   = {};
    float m_dragPanX    = 0.0f;
    float m_dragPanY    = 0.0f;
};
