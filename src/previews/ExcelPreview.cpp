#include "ExcelPreview.h"
#include "Utils.h"
#include <OpenXLSX.hpp>
#include <algorithm>
#include <sstream>

namespace {
std::string WideToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return "";
    std::string str(len, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &str[0], len, nullptr, nullptr);
    if (!str.empty() && str.back() == '\0') str.pop_back();
    return str;
}

std::wstring Utf8ToWide(const std::string& str) {
    if (str.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
    if (len <= 0) return L"";
    std::wstring wstr(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &wstr[0], len);
    if (!wstr.empty() && wstr.back() == L'\0') wstr.pop_back();
    return wstr;
}
}

ExcelPreview::ExcelPreview() {}
ExcelPreview::~ExcelPreview() {
    Unload();
}

std::wstring ExcelPreview::GetColLetter(uint16_t col) {
    std::wstring result;
    while (col > 0) {
        uint16_t rem = (col - 1) % 26;
        result.insert(result.begin(), static_cast<wchar_t>(L'A' + rem));
        col = (col - 1) / 26;
    }
    return result.empty() ? L"A" : result;
}

void ExcelPreview::Unload() {
    m_isLoaded = false;
    m_filePath.clear();
    m_fileName.clear();
    m_sheets.clear();
    m_currentSheet = 0;
    m_scrollX = 0.0f;
    m_scrollY = 0.0f;
    m_hoveredRow = 0;
    m_hoveredCol = 0;
    DiscardResources();
}

bool ExcelPreview::ParseWorkbook(const std::wstring& path, std::vector<ExcelSheetData>& outSheets) {
    outSheets.clear();
    try {
        OpenXLSX::XLDocument doc;
        doc.open(WideToUtf8(path));

        auto sheetNames = doc.workbook().worksheetNames();
        if (sheetNames.empty()) return false;

        for (const auto& sname : sheetNames) {
            ExcelSheetData sdata;
            sdata.name = Utf8ToWide(sname);
            auto wks = doc.workbook().worksheet(sname);

            uint32_t rCount = wks.rowCount();
            uint16_t cCount = wks.columnCount();
            if (rCount == 0) rCount = 30;
            if (cCount == 0) cCount = 10;

            uint32_t maxR = (std::min)(rCount, 600u);
            uint16_t maxC = (std::min)(cCount, static_cast<uint16_t>(60));

            sdata.rows = (std::max)(maxR, 30u);
            sdata.cols = (std::max)(maxC, static_cast<uint16_t>(10));

            for (uint32_t r = 1; r <= maxR; ++r) {
                for (uint16_t c = 1; c <= maxC; ++c) {
                    try {
                        auto cell = wks.cell(r, c);
                        auto valType = cell.value().type();
                        if (valType != OpenXLSX::XLValueType::Empty) {
                            std::string valStr = cell.getString();
                            if (!valStr.empty()) {
                                ExcelCellData cdata;
                                cdata.text = Utf8ToWide(valStr);
                                cdata.isNumber = (valType == OpenXLSX::XLValueType::Integer ||
                                                  valType == OpenXLSX::XLValueType::Float);
                                uint64_t key = (static_cast<uint64_t>(r) << 32) | c;
                                sdata.cells[key] = std::move(cdata);
                            }
                        }
                    } catch (...) {
                        // Gracefully ignore cell read errors
                    }
                }
            }

            outSheets.push_back(std::move(sdata));
        }

        doc.close();
    } catch (...) {
        return false;
    }
    return !outSheets.empty();
}

bool ExcelPreview::Load(const std::wstring& path) {
    Unload();
    m_filePath = path;
    m_fileName = Utils::GetFileName(path);
    if (ParseWorkbook(path, m_sheets)) {
        m_isLoaded = true;
        return true;
    }
    return false;
}

void ExcelPreview::SetSheets(const std::wstring& path, std::vector<ExcelSheetData>&& sheets) {
    Unload();
    m_filePath = path;
    m_fileName = Utils::GetFileName(path);
    m_sheets = std::move(sheets);
    m_isLoaded = !m_sheets.empty();
}

void ExcelPreview::OnMouseWheel(int delta, bool isShiftDown) {
    if (isShiftDown) {
        float step = 100.0f;
        if (delta > 0) {
            m_scrollX = (std::max)(0.0f, m_scrollX - step);
        } else {
            m_scrollX += step;
        }
    } else {
        float step = 72.0f;
        if (delta > 0) {
            m_scrollY = (std::max)(0.0f, m_scrollY - step);
        } else {
            m_scrollY += step;
        }
    }
}

void ExcelPreview::OnMouseMove(float x, float y) {
    m_mouseX = x;
    m_mouseY = y;
}

void ExcelPreview::OnLButtonDown(float x, float y) {
    // Check if clicked on a sheet tab
    for (size_t i = 0; i < m_sheets.size(); ++i) {
        const auto& rect = m_sheets[i].tabRect;
        if (x >= rect.left && x <= rect.right && y >= rect.top && y <= rect.bottom) {
            if (m_currentSheet != i) {
                m_currentSheet = i;
                m_scrollX = 0.0f;
                m_scrollY = 0.0f;
            }
            break;
        }
    }
}

void ExcelPreview::CreateResources(ID2D1RenderTarget* rt) {
    if (m_textBrush) return;
    rt->CreateSolidColorBrush(D2D1::ColorF(0.92f, 0.94f, 0.96f, 1.0f), &m_textBrush);
    rt->CreateSolidColorBrush(D2D1::ColorF(0.55f, 0.60f, 0.68f, 1.0f), &m_dimBrush);
    rt->CreateSolidColorBrush(D2D1::ColorF(0.18f, 0.22f, 0.28f, 1.0f), &m_gridBrush);
    rt->CreateSolidColorBrush(D2D1::ColorF(0.12f, 0.15f, 0.20f, 1.0f), &m_headerBgBrush);
    rt->CreateSolidColorBrush(D2D1::ColorF(0.08f, 0.10f, 0.13f, 1.0f), &m_cellBgBrush);
    rt->CreateSolidColorBrush(D2D1::ColorF(0.09f, 0.11f, 0.15f, 1.0f), &m_cellAltBgBrush);
    rt->CreateSolidColorBrush(D2D1::ColorF(0.06f, 0.50f, 0.25f, 1.0f), &m_excelGreenBrush);
    rt->CreateSolidColorBrush(D2D1::ColorF(0.18f, 0.28f, 0.40f, 0.5f), &m_hoverBrush);
    rt->CreateSolidColorBrush(D2D1::ColorF(0.14f, 0.17f, 0.22f, 1.0f), &m_tabBgBrush);
    rt->CreateSolidColorBrush(D2D1::ColorF(0.10f, 0.48f, 0.24f, 1.0f), &m_tabActiveBgBrush);
}

void ExcelPreview::DiscardResources() {
    m_textBrush.Reset();
    m_dimBrush.Reset();
    m_gridBrush.Reset();
    m_headerBgBrush.Reset();
    m_cellBgBrush.Reset();
    m_cellAltBgBrush.Reset();
    m_excelGreenBrush.Reset();
    m_hoverBrush.Reset();
    m_tabBgBrush.Reset();
    m_tabActiveBgBrush.Reset();
}

void ExcelPreview::Render(ID2D1RenderTarget* rt, IDWriteFactory* dwriteFactory, const D2D1_RECT_F& area) {
    if (!m_isLoaded || m_sheets.empty() || !rt || !dwriteFactory) return;
    CreateResources(rt);

    float areaW = area.right - area.left;
    float areaH = area.bottom - area.top;
    if (areaW <= 0 || areaH <= 0) return;

    const auto& sheet = m_sheets[m_currentSheet];

    // 1. Top Bar (Formula & Cell Address)
    float topBarH = 36.0f;
    D2D1_RECT_F topBarRect = D2D1::RectF(area.left, area.top, area.right, area.top + topBarH);
    rt->FillRectangle(topBarRect, m_headerBgBrush.Get());
    rt->DrawLine(D2D1::Point2F(topBarRect.left, topBarRect.bottom),
                 D2D1::Point2F(topBarRect.right, topBarRect.bottom), m_gridBrush.Get(), 1.0f);

    Microsoft::WRL::ComPtr<IDWriteTextFormat> barFormat;
    dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                   DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                   12.0f, L"en-us", &barFormat);

    Microsoft::WRL::ComPtr<IDWriteTextFormat> valFormat;
    dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                   DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                   12.0f, L"en-us", &valFormat);

    // Excel Badge
    float badgeW = 44.0f;
    D2D1_RECT_F badgeRect = D2D1::RectF(topBarRect.left + 12.0f, topBarRect.top + 7.0f,
                                        topBarRect.left + 12.0f + badgeW, topBarRect.bottom - 7.0f);
    D2D1_ROUNDED_RECT roundedBadge = D2D1::RoundedRect(badgeRect, 3.0f, 3.0f);
    rt->FillRoundedRectangle(roundedBadge, m_excelGreenBrush.Get());
    if (barFormat) {
        barFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        barFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        rt->DrawTextW(L"XLSX", 4, barFormat.Get(), badgeRect, m_textBrush.Get());
    }

    // Active Cell Info
    std::wstring cellAddress = L"A1";
    std::wstring cellContent = L"";
    if (m_hoveredRow > 0 && m_hoveredCol > 0) {
        cellAddress = GetColLetter(m_hoveredCol) + std::to_wstring(m_hoveredRow);
        uint64_t key = (static_cast<uint64_t>(m_hoveredRow) << 32) | m_hoveredCol;
        auto it = sheet.cells.find(key);
        if (it != sheet.cells.end()) {
            cellContent = it->second.text;
        }
    }

    if (barFormat && valFormat) {
        barFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        D2D1_RECT_F addrRect = D2D1::RectF(badgeRect.right + 12.0f, topBarRect.top + 8.0f,
                                           badgeRect.right + 80.0f, topBarRect.bottom - 8.0f);
        rt->DrawTextW(cellAddress.c_str(), static_cast<UINT32>(cellAddress.length()), barFormat.Get(), addrRect, m_excelGreenBrush.Get());

        D2D1_RECT_F fxRect = D2D1::RectF(addrRect.right + 4.0f, topBarRect.top + 8.0f,
                                         topBarRect.right - 12.0f, topBarRect.bottom - 8.0f);
        std::wstring formulaDisplay = cellContent.empty() ? L"" : (L"fx  " + cellContent);
        valFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        rt->DrawTextW(formulaDisplay.c_str(), static_cast<UINT32>(formulaDisplay.length()), valFormat.Get(), fxRect, m_textBrush.Get());
    }

    // 2. Bottom Tab Bar
    float bottomBarH = 32.0f;
    D2D1_RECT_F bottomBarRect = D2D1::RectF(area.left, area.bottom - bottomBarH, area.right, area.bottom);
    rt->FillRectangle(bottomBarRect, m_headerBgBrush.Get());
    rt->DrawLine(D2D1::Point2F(bottomBarRect.left, bottomBarRect.top),
                 D2D1::Point2F(bottomBarRect.right, bottomBarRect.top), m_gridBrush.Get(), 1.0f);

    float tabX = bottomBarRect.left + 16.0f;
    float tabY = bottomBarRect.top + 4.0f;
    float tabH = 24.0f;

    Microsoft::WRL::ComPtr<IDWriteTextFormat> tabFormat;
    dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_MEDIUM,
                                   DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                   11.0f, L"en-us", &tabFormat);
    if (tabFormat) {
        tabFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        tabFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    for (size_t i = 0; i < m_sheets.size(); ++i) {
        float tabW = m_sheets[i].name.length() * 8.0f + 24.0f;
        if (tabW < 60.0f) tabW = 60.0f;
        D2D1_RECT_F tRect = D2D1::RectF(tabX, tabY, tabX + tabW, tabY + tabH);
        m_sheets[i].tabRect = tRect;

        bool isActive = (i == m_currentSheet);
        D2D1_ROUNDED_RECT roundedTab = D2D1::RoundedRect(tRect, 4.0f, 4.0f);
        rt->FillRoundedRectangle(roundedTab, isActive ? m_tabActiveBgBrush.Get() : m_tabBgBrush.Get());
        rt->DrawRoundedRectangle(roundedTab, m_gridBrush.Get(), 0.8f);

        if (tabFormat) {
            rt->DrawTextW(m_sheets[i].name.c_str(), static_cast<UINT32>(m_sheets[i].name.length()),
                          tabFormat.Get(), tRect, isActive ? m_textBrush.Get() : m_dimBrush.Get());
        }

        tabX += tabW + 8.0f;
    }

    // 3. Main Spreadsheet Grid
    float gridTop = topBarRect.bottom;
    float gridBottom = bottomBarRect.top;
    float gridW = area.right - area.left;
    float gridH = gridBottom - gridTop;
    if (gridW <= 0 || gridH <= 0) return;

    float rowHeaderW = 46.0f;
    float colHeaderH = 26.0f;
    float cellW = 100.0f;
    float cellH = 24.0f;

    float cellsViewportW = gridW - rowHeaderW;
    float cellsViewportH = gridH - colHeaderH;

    float totalGridW = sheet.cols * cellW;
    float totalGridH = sheet.rows * cellH;

    float maxScrollX = (std::max)(0.0f, totalGridW - cellsViewportW);
    float maxScrollY = (std::max)(0.0f, totalGridH - cellsViewportH);
    if (m_scrollX > maxScrollX) m_scrollX = maxScrollX;
    if (m_scrollY > maxScrollY) m_scrollY = maxScrollY;

    // Corner Header Cell (top-left)
    D2D1_RECT_F cornerRect = D2D1::RectF(area.left, gridTop, area.left + rowHeaderW, gridTop + colHeaderH);
    rt->FillRectangle(cornerRect, m_headerBgBrush.Get());
    rt->DrawRectangle(cornerRect, m_gridBrush.Get(), 1.0f);

    // Visible Column Range
    int firstCol = static_cast<int>(m_scrollX / cellW) + 1;
    if (firstCol < 1) firstCol = 1;
    int lastCol = static_cast<int>((m_scrollX + cellsViewportW) / cellW) + 2;
    if (lastCol > static_cast<int>(sheet.cols)) lastCol = static_cast<int>(sheet.cols);

    // Visible Row Range
    int firstRow = static_cast<int>(m_scrollY / cellH) + 1;
    if (firstRow < 1) firstRow = 1;
    int lastRow = static_cast<int>((m_scrollY + cellsViewportH) / cellH) + 2;
    if (lastRow > static_cast<int>(sheet.rows)) lastRow = static_cast<int>(sheet.rows);

    Microsoft::WRL::ComPtr<IDWriteTextFormat> headerFormat;
    dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                   DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                   11.0f, L"en-us", &headerFormat);
    if (headerFormat) {
        headerFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        headerFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    Microsoft::WRL::ComPtr<IDWriteTextFormat> cellTextFormat;
    dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                   DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                   12.0f, L"en-us", &cellTextFormat);
    if (cellTextFormat) {
        cellTextFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        cellTextFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    Microsoft::WRL::ComPtr<IDWriteTextFormat> cellNumFormat;
    dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                   DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                   12.0f, L"en-us", &cellNumFormat);
    if (cellNumFormat) {
        cellNumFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        cellNumFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        cellNumFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    // Cells Viewport Clip
    D2D1_RECT_F cellsClip = D2D1::RectF(area.left + rowHeaderW, gridTop + colHeaderH, area.right, gridBottom);
    rt->PushAxisAlignedClip(cellsClip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    // Render Data Cells
    for (int r = firstRow; r <= lastRow; ++r) {
        float curY = gridTop + colHeaderH + (r - 1) * cellH - m_scrollY;
        for (int c = firstCol; c <= lastCol; ++c) {
            float curX = area.left + rowHeaderW + (c - 1) * cellW - m_scrollX;
            D2D1_RECT_F cellRect = D2D1::RectF(curX, curY, curX + cellW, curY + cellH);

            bool isHovered = (m_mouseX >= cellRect.left && m_mouseX < cellRect.right &&
                              m_mouseY >= cellRect.top && m_mouseY < cellRect.bottom);
            if (isHovered) {
                m_hoveredRow = r;
                m_hoveredCol = static_cast<uint16_t>(c);
                rt->FillRectangle(cellRect, m_hoverBrush.Get());
            } else if (r % 2 == 1) {
                rt->FillRectangle(cellRect, m_cellAltBgBrush.Get());
            } else {
                rt->FillRectangle(cellRect, m_cellBgBrush.Get());
            }

            rt->DrawRectangle(cellRect, m_gridBrush.Get(), 0.6f);

            uint64_t key = (static_cast<uint64_t>(r) << 32) | c;
            auto it = sheet.cells.find(key);
            if (it != sheet.cells.end()) {
                const auto& cdata = it->second;
                D2D1_RECT_F textRect = D2D1::RectF(cellRect.left + 6.0f, cellRect.top,
                                                   cellRect.right - 6.0f, cellRect.bottom);
                if (cdata.isNumber && cellNumFormat) {
                    rt->DrawTextW(cdata.text.c_str(), static_cast<UINT32>(cdata.text.length()),
                                  cellNumFormat.Get(), textRect, m_textBrush.Get());
                } else if (cellTextFormat) {
                    rt->DrawTextW(cdata.text.c_str(), static_cast<UINT32>(cdata.text.length()),
                                  cellTextFormat.Get(), textRect, m_textBrush.Get());
                }
            }
        }
    }
    rt->PopAxisAlignedClip();

    // Render Column Headers (A, B, C...) sticky at top
    D2D1_RECT_F colClip = D2D1::RectF(area.left + rowHeaderW, gridTop, area.right, gridTop + colHeaderH);
    rt->PushAxisAlignedClip(colClip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    for (int c = firstCol; c <= lastCol; ++c) {
        float curX = area.left + rowHeaderW + (c - 1) * cellW - m_scrollX;
        D2D1_RECT_F colRect = D2D1::RectF(curX, gridTop, curX + cellW, gridTop + colHeaderH);
        rt->FillRectangle(colRect, m_headerBgBrush.Get());
        rt->DrawRectangle(colRect, m_gridBrush.Get(), 0.8f);

        if (headerFormat) {
            std::wstring colLetter = GetColLetter(static_cast<uint16_t>(c));
            rt->DrawTextW(colLetter.c_str(), static_cast<UINT32>(colLetter.length()),
                          headerFormat.Get(), colRect,
                          (c == m_hoveredCol) ? m_excelGreenBrush.Get() : m_dimBrush.Get());
        }
    }
    rt->PopAxisAlignedClip();

    // Render Row Headers (1, 2, 3...) sticky at left
    D2D1_RECT_F rowClip = D2D1::RectF(area.left, gridTop + colHeaderH, area.left + rowHeaderW, gridBottom);
    rt->PushAxisAlignedClip(rowClip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    for (int r = firstRow; r <= lastRow; ++r) {
        float curY = gridTop + colHeaderH + (r - 1) * cellH - m_scrollY;
        D2D1_RECT_F rowRect = D2D1::RectF(area.left, curY, area.left + rowHeaderW, curY + cellH);
        rt->FillRectangle(rowRect, m_headerBgBrush.Get());
        rt->DrawRectangle(rowRect, m_gridBrush.Get(), 0.8f);

        if (headerFormat) {
            std::wstring rowNum = std::to_wstring(r);
            rt->DrawTextW(rowNum.c_str(), static_cast<UINT32>(rowNum.length()),
                          headerFormat.Get(), rowRect,
                          (r == static_cast<int>(m_hoveredRow)) ? m_excelGreenBrush.Get() : m_dimBrush.Get());
        }
    }
    rt->PopAxisAlignedClip();
}
