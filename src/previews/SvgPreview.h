#pragma once

#include <windows.h>
#include <d2d1.h>
#include <d2d1_3.h>
#include <wrl/client.h>
#include <string>

class SvgPreview {
public:
    SvgPreview();
    ~SvgPreview();

    bool Load(const std::wstring& path, ID2D1DeviceContext5* dc5);
    void Unload();

    void Render(ID2D1DeviceContext5* dc5, IDWriteFactory* dwriteFactory, const D2D1_RECT_F& area);

    void OnMouseWheel(int delta, float mouseX, float mouseY);
    void OnLButtonDown(float x, float y);
    void OnMouseMove(float x, float y);
    void OnLButtonUp();
    void ResetView();

    bool IsLoaded() const { return m_svgDoc != nullptr; }
    D2D1_SIZE_F GetNativeSize() const { return m_nativeSize; }

private:
    Microsoft::WRL::ComPtr<ID2D1SvgDocument> m_svgDoc;
    std::wstring m_filePath;
    std::wstring m_fileName;
    D2D1_SIZE_F m_nativeSize = { 800.0f, 600.0f };

    float m_zoom = 1.0f;
    float m_panX = 0.0f;
    float m_panY = 0.0f;

    bool m_isDragging = false;
    float m_lastMouseX = 0.0f;
    float m_lastMouseY = 0.0f;

    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_badgeBgBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_badgeTextBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_borderBrush;

    void CreateResources(ID2D1RenderTarget* rt);
    void DiscardResources();
};
