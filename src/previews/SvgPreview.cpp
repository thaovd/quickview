#include "SvgPreview.h"
#include "Utils.h"
#include <shlwapi.h>
#include <vector>
#include <algorithm>
#include <cmath>
#include <sstream>
#include <iomanip>

SvgPreview::SvgPreview() {}
SvgPreview::~SvgPreview() {
    Unload();
}

void SvgPreview::Unload() {
    m_svgDoc.Reset();
    m_filePath.clear();
    m_fileName.clear();
    m_zoom = 1.0f;
    m_panX = 0.0f;
    m_panY = 0.0f;
    m_isDragging = false;
    DiscardResources();
}

bool SvgPreview::Load(const std::wstring& path, ID2D1DeviceContext5* dc5) {
    Unload();
    if (!dc5) return false;

    m_filePath = path;
    m_fileName = Utils::GetFileName(path);

    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER fileSize;
    if (!GetFileSizeEx(hFile, &fileSize) || fileSize.QuadPart == 0 || fileSize.QuadPart > 50 * 1024 * 1024) {
        CloseHandle(hFile);
        return false;
    }

    std::vector<BYTE> buffer(static_cast<size_t>(fileSize.QuadPart));
    DWORD bytesRead = 0;
    if (!ReadFile(hFile, buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead, nullptr) || bytesRead == 0) {
        CloseHandle(hFile);
        return false;
    }
    CloseHandle(hFile);

    Microsoft::WRL::ComPtr<IStream> stream;
    stream.Attach(SHCreateMemStream(buffer.data(), static_cast<UINT>(bytesRead)));
    if (!stream) return false;

    // Create SVG document with initial viewport
    D2D1_SIZE_F initialViewport = D2D1::SizeF(800.0f, 600.0f);
    HRESULT hr = dc5->CreateSvgDocument(stream.Get(), initialViewport, &m_svgDoc);
    if (FAILED(hr) || !m_svgDoc) return false;

    // Read intrinsic viewport size if available
    D2D1_SIZE_F vp = m_svgDoc->GetViewportSize();
    if (vp.width > 1.0f && vp.height > 1.0f) {
        m_nativeSize = vp;
    } else {
        m_nativeSize = initialViewport;
    }

    ResetView();
    return true;
}

void SvgPreview::ResetView() {
    m_zoom = 1.0f;
    m_panX = 0.0f;
    m_panY = 0.0f;
    m_isDragging = false;
}

void SvgPreview::OnMouseWheel(int delta, float mouseX, float mouseY) {
    (void)mouseX;
    (void)mouseY;
    float factor = (delta > 0) ? 1.2f : (1.0f / 1.2f);
    m_zoom *= factor;
    if (m_zoom < 0.05f) m_zoom = 0.05f;
    if (m_zoom > 50.0f) m_zoom = 50.0f;
}

void SvgPreview::OnLButtonDown(float x, float y) {
    m_isDragging = true;
    m_lastMouseX = x;
    m_lastMouseY = y;
}

void SvgPreview::OnMouseMove(float x, float y) {
    if (m_isDragging) {
        m_panX += (x - m_lastMouseX);
        m_panY += (y - m_lastMouseY);
        m_lastMouseX = x;
        m_lastMouseY = y;
    }
}

void SvgPreview::OnLButtonUp() {
    m_isDragging = false;
}

void SvgPreview::CreateResources(ID2D1RenderTarget* rt) {
    if (m_badgeBgBrush) return;
    rt->CreateSolidColorBrush(D2D1::ColorF(0.08f, 0.10f, 0.15f, 0.85f), &m_badgeBgBrush);
    rt->CreateSolidColorBrush(D2D1::ColorF(0.85f, 0.88f, 0.92f, 1.0f), &m_badgeTextBrush);
    rt->CreateSolidColorBrush(D2D1::ColorF(0.25f, 0.30f, 0.40f, 0.8f), &m_borderBrush);
}

void SvgPreview::DiscardResources() {
    m_badgeBgBrush.Reset();
    m_badgeTextBrush.Reset();
    m_borderBrush.Reset();
}

void SvgPreview::Render(ID2D1DeviceContext5* dc5, IDWriteFactory* dwriteFactory, const D2D1_RECT_F& area) {
    if (!m_svgDoc || !dc5) return;
    CreateResources(dc5);

    float areaW = area.right - area.left;
    float areaH = area.bottom - area.top;
    if (areaW <= 0 || areaH <= 0) return;

    // Calculate base fit scale
    float pad = 32.0f;
    float availW = (std::max)(10.0f, areaW - pad * 2.0f);
    float availH = (std::max)(10.0f, areaH - pad * 2.0f);

    float fitScale = (std::min)(availW / m_nativeSize.width, availH / m_nativeSize.height);
    if (fitScale <= 0.001f) fitScale = 1.0f;

    float totalScale = fitScale * m_zoom;

    float renderW = m_nativeSize.width * totalScale;
    float renderH = m_nativeSize.height * totalScale;

    float originX = area.left + (areaW - renderW) * 0.5f + m_panX;
    float originY = area.top + (areaH - renderH) * 0.5f + m_panY;

    // Save previous transform
    D2D1_MATRIX_3X2_F oldTransform;
    dc5->GetTransform(&oldTransform);

    // Set SVG document viewport
    m_svgDoc->SetViewportSize(m_nativeSize);

    // Apply transform and render SVG document
    D2D1_MATRIX_3X2_F svgMatrix = D2D1::Matrix3x2F::Scale(totalScale, totalScale) *
                                  D2D1::Matrix3x2F::Translation(originX, originY) *
                                  oldTransform;
    dc5->SetTransform(svgMatrix);
    dc5->DrawSvgDocument(m_svgDoc.Get());

    // Restore previous transform for UI overlay
    dc5->SetTransform(oldTransform);

    // Render subtle info badge in top right
    if (dwriteFactory) {
        Microsoft::WRL::ComPtr<IDWriteTextFormat> badgeFormat;
        dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_MEDIUM,
                                       DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                       12.0f, L"en-us", &badgeFormat);

        if (badgeFormat) {
            std::wstringstream ss;
            ss << L"SVG Vector  •  " << static_cast<int>(m_zoom * 100.0f) << L"%"
               << L"  •  " << static_cast<int>(m_nativeSize.width) << L" × " << static_cast<int>(m_nativeSize.height);
            std::wstring badgeText = ss.str();

            float badgeW = 230.0f;
            float badgeH = 26.0f;
            D2D1_RECT_F badgeRect = D2D1::RectF(area.right - badgeW - 16.0f, area.top + 12.0f,
                                                area.right - 16.0f, area.top + 12.0f + badgeH);

            D2D1_ROUNDED_RECT rounded = D2D1::RoundedRect(badgeRect, 6.0f, 6.0f);
            dc5->FillRoundedRectangle(rounded, m_badgeBgBrush.Get());
            dc5->DrawRoundedRectangle(rounded, m_borderBrush.Get(), 1.0f);

            badgeFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            badgeFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            dc5->DrawTextW(badgeText.c_str(), static_cast<UINT32>(badgeText.length()),
                           badgeFormat.Get(), badgeRect, m_badgeTextBrush.Get());
        }
    }
}
