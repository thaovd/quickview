#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>

struct ExcelCellData {
    std::wstring text;
    bool isNumber = false;
};

struct ExcelSheetData {
    std::wstring name;
    uint32_t rows = 0;
    uint32_t cols = 0;
    std::unordered_map<uint64_t, ExcelCellData> cells;
    D2D1_RECT_F tabRect = {};
};

class ExcelPreview {
public:
    ExcelPreview();
    ~ExcelPreview();

    bool Load(const std::wstring& path);
    void Unload();

    /// Parse workbook asynchronously (thread-safe, can run in background)
    static bool ParseWorkbook(const std::wstring& path, std::vector<ExcelSheetData>& outSheets);

    /// Attach pre-parsed sheets
    void SetSheets(const std::wstring& path, std::vector<ExcelSheetData>&& sheets);

    void Render(ID2D1RenderTarget* rt, IDWriteFactory* dwriteFactory, const D2D1_RECT_F& area);

    void OnMouseWheel(int delta, bool isShiftDown);
    void OnLButtonDown(float x, float y);
    void OnMouseMove(float x, float y);

    bool IsLoaded() const { return m_isLoaded; }
    size_t GetSheetCount() const { return m_sheets.size(); }

    static std::wstring GetColLetter(uint16_t col);

private:
    bool m_isLoaded = false;
    std::wstring m_filePath;
    std::wstring m_fileName;
    std::vector<ExcelSheetData> m_sheets;
    size_t m_currentSheet = 0;

    float m_scrollX = 0.0f;
    float m_scrollY = 0.0f;
    float m_mouseX = -1.0f;
    float m_mouseY = -1.0f;

    uint32_t m_hoveredRow = 0;
    uint16_t m_hoveredCol = 0;

    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_textBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_dimBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_gridBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_headerBgBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_cellBgBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_cellAltBgBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_excelGreenBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_hoverBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_tabBgBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_tabActiveBgBrush;

    void CreateResources(ID2D1RenderTarget* rt);
    void DiscardResources();
};
