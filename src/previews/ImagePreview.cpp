#include "ImagePreview.h"
#include "StbImageLoader.h"
#include "MpvImageLoader.h"
#include "RawThumbnailExtractor.h"

bool ImagePreview::LoadFile(const std::wstring& path, IWICImagingFactory* wicFactory) {
    Reset();
    if (!wicFactory) return false;

    // Check if this file is a Camera RAW format (CR2, NEF, ARW, DNG, RAF, RW2, ORF, etc.)
    std::wstring ext = Utils::GetFileExtension(path);
    Utils::Log(L"[ImagePreview] LoadFile: %s, ext: '%s', isMpv: %d", path.c_str(), ext.c_str(), MpvImageLoader::IsMpvImageExtension(ext) ? 1 : 0);

    if (RawThumbnailExtractor::IsRawExtension(ext)) {
        std::vector<uint8_t> rawJpegBytes;
        if (RawThumbnailExtractor::Extract(path, rawJpegBytes)) {
            if (LoadMemory(rawJpegBytes.data(), rawJpegBytes.size(), wicFactory)) {
                return true;
            }
        }
    }

    // HEIC, HEIF, AVIF: Windows WIC thường thiếu codec HEVC (0xC00D5212) -> Ưu tiên giải mã qua libmpv
    if (MpvImageLoader::IsMpvImageExtension(ext)) {
        m_converter = MpvImageLoader::Load(path, wicFactory, m_width, m_height);
        if (m_converter) {
            return true;
        }
    }

    // Standard image files (JPG, PNG, WebP, etc.) -> Try WIC decoder first
    ComPtr<IWICBitmapDecoder> decoder;
    HRESULT hr = wicFactory->CreateDecoderFromFilename(
        path.c_str(), nullptr, GENERIC_READ,
        WICDecodeMetadataCacheOnLoad, &decoder);
    if (SUCCEEDED(hr)) {
        // Get first frame
        ComPtr<IWICBitmapFrameDecode> frame;
        hr = decoder->GetFrame(0, &frame);
        if (SUCCEEDED(hr)) {
            // Get dimensions
            frame->GetSize(&m_width, &m_height);

            // Convert to 32bppPBGRA (premultiplied BGRA) for Direct2D
            hr = wicFactory->CreateFormatConverter(&m_converter);
            if (SUCCEEDED(hr)) {
                hr = m_converter->Initialize(
                    frame.Get(),
                    GUID_WICPixelFormat32bppPBGRA,
                    WICBitmapDitherTypeNone,
                    nullptr, 0.0,
                    WICBitmapPaletteTypeMedianCut);
                if (SUCCEEDED(hr)) {
                    // Kiểm tra thực tế xem WIC có thể giải mã pixel hay không
                    // Đối với file HEIC/HEIF, nếu Windows chưa cài codec HEVC, CopyPixels sẽ báo lỗi 0xC00D5212
                    WICRect rc = { 0, 0, 1, 1 };
                    BYTE testPixel[4];
                    if (SUCCEEDED(m_converter->CopyPixels(&rc, 4, 4, testPixel))) {
                        return true;
                    }
                }
                m_converter.Reset();
            }
        }
    }

    // Fallback: Sử dụng MpvImageLoader giải mã bằng libmpv/ffmpeg (hỗ trợ HEIC, HEIF, AVIF khi thiếu codec hệ thống)
    m_converter = MpvImageLoader::Load(path, wicFactory, m_width, m_height);
    if (m_converter) {
        return true;
    }

    // Try RawThumbnailExtractor as fallback (in case a RAW file had an unusual extension)
    std::vector<uint8_t> fallbackJpegBytes;
    if (RawThumbnailExtractor::Extract(path, fallbackJpegBytes)) {
        if (LoadMemory(fallbackJpegBytes.data(), fallbackJpegBytes.size(), wicFactory)) {
            return true;
        }
    }

    // Fallback to stb_image (PSD, TGA, HDR, PIC, PNM, etc.)
    m_converter = StbImageLoader::Load(path, wicFactory, m_width, m_height);
    return m_converter != nullptr;
}

#include <shlwapi.h>
#pragma comment(lib, "shlwapi.lib")

bool ImagePreview::LoadMemory(const void* data, size_t size, IWICImagingFactory* wicFactory) {
    Reset();
    if (!wicFactory || !data || size == 0) return false;

    // Use SHCreateMemStream which allocates and copies memory, managing lifetime safely via IStream refcounting
    m_stream.Attach(SHCreateMemStream(reinterpret_cast<const BYTE*>(data), static_cast<UINT>(size)));
    if (!m_stream) return false;

    ComPtr<IWICBitmapDecoder> decoder;
    HRESULT hr = wicFactory->CreateDecoderFromStream(m_stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder);
    if (FAILED(hr)) {
        m_stream.Reset();
        return false;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr)) {
        m_stream.Reset();
        return false;
    }

    frame->GetSize(&m_width, &m_height);
    hr = wicFactory->CreateFormatConverter(&m_converter);
    if (FAILED(hr)) {
        m_stream.Reset();
        return false;
    }

    hr = m_converter->Initialize(
        frame.Get(),
        GUID_WICPixelFormat32bppPBGRA,
        WICBitmapDitherTypeNone,
        nullptr, 0.0,
        WICBitmapPaletteTypeMedianCut);

    if (FAILED(hr)) {
        m_converter.Reset();
        m_stream.Reset();
        return false;
    }

    // Kiểm tra thực tế xem WIC stream có giải mã được pixel hay không
    WICRect rc = { 0, 0, 1, 1 };
    BYTE testPix[4];
    if (FAILED(m_converter->CopyPixels(&rc, 4, 4, testPix))) {
        m_converter.Reset();
        m_stream.Reset();
        return false;
    }

    return true;
}

bool ImagePreview::CreateBitmap(ID2D1RenderTarget* rt) {
    m_bitmap.Reset();
    if (!m_converter || !rt) return false;

    HRESULT hr = rt->CreateBitmapFromWicBitmap(m_converter.Get(), &m_bitmap);
    if (FAILED(hr)) {
        Utils::Log(L"[ImagePreview] CreateBitmapFromWicBitmap failed: 0x%08X (dims: %u x %u)", hr, m_width, m_height);
        return false;
    }
    return true;
}

void ImagePreview::Render(ID2D1RenderTarget* rt, const D2D1_RECT_F& area) {
    if (!rt || !m_bitmap) return;

    auto bmpSize = m_bitmap->GetSize();
    float areaW = area.right - area.left;
    float areaH = area.bottom - area.top;

    // Calculate centered position with zoom and pan
    float scaledW = bmpSize.width * m_zoom;
    float scaledH = bmpSize.height * m_zoom;
    float cx = area.left + (areaW - scaledW) / 2.0f + m_panX;
    float cy = area.top  + (areaH - scaledH) / 2.0f + m_panY;

    D2D1_RECT_F dest = D2D1::RectF(cx, cy, cx + scaledW, cy + scaledH);

    // Draw checkerboard background behind image (for transparency)
    ComPtr<ID2D1SolidColorBrush> checkBrush;
    rt->CreateSolidColorBrush(D2D1::ColorF(0.15f, 0.15f, 0.18f), &checkBrush);
    if (checkBrush) {
        float checkSize = 12.0f;
        float startX = (std::max)(dest.left, area.left);
        float startY = (std::max)(dest.top, area.top);
        float endX   = (std::min)(dest.right, area.right);
        float endY   = (std::min)(dest.bottom, area.bottom);

        D2D1_COLOR_F c1 = { 0.12f, 0.12f, 0.14f, 1.0f };
        D2D1_COLOR_F c2 = { 0.16f, 0.16f, 0.18f, 1.0f };
        int col = 0;
        for (float x = startX; x < endX; x += checkSize, col++) {
            int row = col;
            for (float y = startY; y < endY; y += checkSize, row++) {
                checkBrush->SetColor((row % 2 == 0) ? c1 : c2);
                rt->FillRectangle(D2D1::RectF(
                    x, y,
                    (std::min)(x + checkSize, endX),
                    (std::min)(y + checkSize, endY)), checkBrush.Get());
            }
        }
    }

    // Choose interpolation mode based on zoom level
    auto interp = (m_zoom > 3.0f)
        ? D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR
        : D2D1_BITMAP_INTERPOLATION_MODE_LINEAR;

    rt->DrawBitmap(m_bitmap.Get(), dest, 1.0f, interp);
}

void ImagePreview::OnMouseWheel(int delta, int mx, int my, const D2D1_RECT_F& area) {
    float factor = (delta > 0) ? 1.12f : 0.89f;
    float newZoom = m_zoom * factor;
    newZoom = (std::max)(0.05f, (std::min)(newZoom, 20.0f));

    // Zoom towards cursor position
    float areaCX = (area.left + area.right) / 2.0f;
    float areaCY = (area.top + area.bottom) / 2.0f;
    float relX = mx - areaCX;
    float relY = my - areaCY;

    m_panX = relX + (m_panX - relX) * (newZoom / m_zoom);
    m_panY = relY + (m_panY - relY) * (newZoom / m_zoom);
    m_zoom = newZoom;
}

void ImagePreview::OnMouseDown(int mx, int my) {
    m_dragging  = true;
    m_dragStart = { mx, my };
    m_dragPanX  = m_panX;
    m_dragPanY  = m_panY;
}

void ImagePreview::OnMouseMove(int mx, int my, HWND hwnd) {
    if (!m_dragging) return;
    m_panX = m_dragPanX + (mx - m_dragStart.x);
    m_panY = m_dragPanY + (my - m_dragStart.y);
    if (hwnd) InvalidateRect(hwnd, nullptr, FALSE);
}

void ImagePreview::OnMouseUp() {
    m_dragging = false;
    ReleaseCapture();
}

void ImagePreview::ResetView(const D2D1_RECT_F& area) {
    if (m_width == 0 || m_height == 0) return;

    float areaW = area.right - area.left;
    float areaH = area.bottom - area.top;
    if (areaW <= 0 || areaH <= 0) return;

    float scaleX = areaW / static_cast<float>(m_width);
    float scaleY = areaH / static_cast<float>(m_height);
    m_zoom = (std::min)(scaleX, scaleY);
    if (m_zoom > 1.0f) m_zoom = 1.0f; // Don't upscale small images

    m_panX = 0.0f;
    m_panY = 0.0f;
}

SIZE ImagePreview::GetPreferredSize() const {
    if (m_width == 0 || m_height == 0) {
        return { PREVIEW_MIN_WIDTH, PREVIEW_MIN_HEIGHT };
    }

    float aspect = static_cast<float>(m_width) / static_cast<float>(m_height);
    int w = static_cast<int>(m_width);
    int h = static_cast<int>(m_height);

    // Clamp to max
    if (w > PREVIEW_MAX_WIDTH) {
        w = PREVIEW_MAX_WIDTH;
        h = static_cast<int>(w / aspect);
    }
    if (h > PREVIEW_MAX_HEIGHT - TITLEBAR_HEIGHT - TOOLBAR_HEIGHT) {
        h = PREVIEW_MAX_HEIGHT - TITLEBAR_HEIGHT - TOOLBAR_HEIGHT;
        w = static_cast<int>(h * aspect);
    }

    // Enforce minimums
    w = (std::max)(w, static_cast<int>(PREVIEW_MIN_WIDTH));
    h = (std::max)(h, static_cast<int>(PREVIEW_MIN_HEIGHT - TITLEBAR_HEIGHT - TOOLBAR_HEIGHT));

    return { w, h + TITLEBAR_HEIGHT + TOOLBAR_HEIGHT };
}

std::wstring ImagePreview::GetDimensionsString() const {
    if (m_width == 0 || m_height == 0) return L"";
    wchar_t buf[64];
    swprintf_s(buf, L"%u \u00D7 %u", m_width, m_height);
    return buf;
}

void ImagePreview::Reset() {
    m_converter.Reset();
    m_bitmap.Reset();
    m_stream.Reset();
    m_width    = 0;
    m_height   = 0;
    m_zoom     = 1.0f;
    m_panX     = 0.0f;
    m_panY     = 0.0f;
    m_dragging = false;
}
