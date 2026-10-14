#include "ArchivePreview.h"
#include "Utils.h"
#include <miniz.h>
#include <algorithm>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace {
std::wstring Utf8ToWide(const char* utf8Str) {
    if (!utf8Str || *utf8Str == '\0') return L"";
    // First try strict UTF-8
    int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8Str, -1, nullptr, 0);
    if (len > 0) {
        std::wstring wstr(len, 0);
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8Str, -1, &wstr[0], len);
        if (!wstr.empty() && wstr.back() == L'\0') wstr.pop_back();
        return wstr;
    }
    // Fallback to OEM/CP437 (standard DOS/legacy ZIPs)
    len = MultiByteToWideChar(CP_OEMCP, 0, utf8Str, -1, nullptr, 0);
    if (len > 0) {
        std::wstring wstr(len, 0);
        MultiByteToWideChar(CP_OEMCP, 0, utf8Str, -1, &wstr[0], len);
        if (!wstr.empty() && wstr.back() == L'\0') wstr.pop_back();
        return wstr;
    }
    // Fallback to ANSI
    len = MultiByteToWideChar(CP_ACP, 0, utf8Str, -1, nullptr, 0);
    if (len <= 0) return L"";
    std::wstring wstr(len, 0);
    MultiByteToWideChar(CP_ACP, 0, utf8Str, -1, &wstr[0], len);
    if (!wstr.empty() && wstr.back() == L'\0') wstr.pop_back();
    return wstr;
}

std::wstring FormatDosOrUnixTime(time_t rawTime) {
    if (rawTime <= 0) return L"-";
    tm t;
    if (localtime_s(&t, &rawTime) != 0) return L"-";
    wchar_t buf[64];
    swprintf_s(buf, L"%04d-%02d-%02d %02d:%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min);
    return buf;
}

bool CaseInsensitiveEquals(std::string a, std::string b) {
    std::replace(a.begin(), a.end(), '\\', '/');
    std::replace(b.begin(), b.end(), '\\', '/');
    if (a.length() != b.length()) return false;
    return std::equal(a.begin(), a.end(), b.begin(), [](char c1, char c2) {
        return std::tolower(static_cast<unsigned char>(c1)) == std::tolower(static_cast<unsigned char>(c2));
    });
}

std::wstring TruncateMiddle(
    const std::wstring& text,
    float maxWidth,
    IDWriteTextFormat* format,
    IDWriteFactory* dwriteFactory)
{
    if (text.empty() || maxWidth <= 0.0f || !format || !dwriteFactory) {
        return text;
    }

    // Measure full text first
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    HRESULT hr = dwriteFactory->CreateTextLayout(
        text.c_str(), static_cast<UINT32>(text.length()),
        format, 10000.0f, 100.0f, &layout);
    if (FAILED(hr) || !layout) return text;

    DWRITE_TEXT_METRICS tm = {};
    layout->GetMetrics(&tm);
    if (tm.width <= maxWidth) {
        return text; // Already fits
    }

    const std::wstring ellipsis = L"...";
    Microsoft::WRL::ComPtr<IDWriteTextLayout> elLayout;
    dwriteFactory->CreateTextLayout(ellipsis.c_str(), static_cast<UINT32>(ellipsis.length()),
                                    format, 10000.0f, 100.0f, &elLayout);
    DWRITE_TEXT_METRICS elTm = {};
    if (elLayout) elLayout->GetMetrics(&elTm);

    float targetWidth = maxWidth - elTm.width;
    if (targetWidth <= 10.0f) {
        return ellipsis;
    }

    // Binary search on the total number of characters to keep
    int low = 2;
    int high = static_cast<int>(text.length()) - 1;
    int bestK = 2;

    while (low <= high) {
        int mid = (low + high) / 2;
        // Allocate 40% to left (directory root), 60% to right (filename/extension)
        int leftLen = (std::max)(1, mid * 4 / 10);
        int rightLen = mid - leftLen;
        if (leftLen + rightLen >= static_cast<int>(text.length())) {
            high = mid - 1;
            continue;
        }

        std::wstring candidate = text.substr(0, leftLen) + ellipsis + text.substr(text.length() - rightLen);
        Microsoft::WRL::ComPtr<IDWriteTextLayout> candLayout;
        hr = dwriteFactory->CreateTextLayout(
            candidate.c_str(), static_cast<UINT32>(candidate.length()),
            format, 10000.0f, 100.0f, &candLayout);

        if (SUCCEEDED(hr) && candLayout) {
            DWRITE_TEXT_METRICS ctm = {};
            candLayout->GetMetrics(&ctm);
            if (ctm.width <= maxWidth) {
                bestK = mid;
                low = mid + 1; // Can fit more text
            } else {
                high = mid - 1; // Need more truncation
            }
        } else {
            high = mid - 1;
        }
    }

    int leftLen = (std::max)(1, bestK * 4 / 10);
    int rightLen = bestK - leftLen;
    if (leftLen + rightLen >= static_cast<int>(text.length())) return text;
    return text.substr(0, leftLen) + ellipsis + text.substr(text.length() - rightLen);
}
}

ArchivePreview::ArchivePreview() {}
ArchivePreview::~ArchivePreview() {
    Unload();
}

void ArchivePreview::Unload() {
    m_isLoaded = false;
    m_entries.clear();
    m_filePath.clear();
    m_fileName.clear();
    m_totalUncompressed = 0;
    m_totalCompressed = 0;
    m_fileCount = 0;
    m_folderCount = 0;
    m_scrollY = 0.0f;
    DiscardResources();
}

bool ArchivePreview::ParseArchive(const std::wstring& path, ArchiveData& outData) {
    outData.entries.clear();
    outData.totalUncompressed = 0;
    outData.totalCompressed = 0;
    outData.fileCount = 0;
    outData.folderCount = 0;

    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size;
    if (!GetFileSizeEx(hFile, &size) || size.QuadPart == 0) {
        CloseHandle(hFile);
        return false;
    }

    HANDLE hMap = CreateFileMappingW(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!hMap) {
        CloseHandle(hFile);
        return false;
    }

    const void* pMem = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
    if (!pMem) {
        CloseHandle(hMap);
        CloseHandle(hFile);
        return false;
    }

    mz_zip_archive zip = {};
    if (!mz_zip_reader_init_mem(&zip, pMem, static_cast<size_t>(size.QuadPart), 0)) {
        UnmapViewOfFile(pMem);
        CloseHandle(hMap);
        CloseHandle(hFile);
        return false;
    }

    mz_uint numFiles = mz_zip_reader_get_num_files(&zip);
    outData.entries.reserve(numFiles);

    for (mz_uint i = 0; i < numFiles; ++i) {
        mz_zip_archive_file_stat stat = {};
        if (!mz_zip_reader_file_stat(&zip, i, &stat)) continue;

        ArchiveEntry entry;
        entry.name = Utf8ToWide(stat.m_filename);
        entry.isDirectory = mz_zip_reader_is_file_a_directory(&zip, i) != 0;
        entry.uncompressedSize = stat.m_uncomp_size;
        entry.compressedSize = stat.m_comp_size;
        entry.ratio = (entry.uncompressedSize > 0)
            ? (100.0 - (static_cast<double>(entry.compressedSize) * 100.0 / entry.uncompressedSize))
            : 0.0;
        if (entry.ratio < 0.0) entry.ratio = 0.0;
        entry.dateModified = FormatDosOrUnixTime(stat.m_time);

        if (entry.isDirectory) {
            outData.folderCount++;
        } else {
            outData.fileCount++;
            outData.totalUncompressed += entry.uncompressedSize;
            outData.totalCompressed += entry.compressedSize;
        }

        outData.entries.push_back(std::move(entry));
    }

    mz_zip_reader_end(&zip);
    UnmapViewOfFile(pMem);
    CloseHandle(hMap);
    CloseHandle(hFile);

    return true;
}

bool ArchivePreview::Load(const std::wstring& path) {
    Unload();
    ArchiveData data;
    if (ParseArchive(path, data)) {
        SetData(path, std::move(data));
        return true;
    }
    return false;
}

void ArchivePreview::SetData(const std::wstring& path, ArchiveData&& data) {
    Unload();
    m_filePath = path;
    m_fileName = Utils::GetFileName(path);
    m_entries = std::move(data.entries);
    m_totalUncompressed = data.totalUncompressed;
    m_totalCompressed = data.totalCompressed;
    m_fileCount = data.fileCount;
    m_folderCount = data.folderCount;
    m_isLoaded = !m_entries.empty();
}

bool ArchivePreview::ExtractFile(const std::wstring& zipPath, const std::string& entryName, std::vector<uint8_t>& outBytes) {
    HANDLE hFile = CreateFileW(zipPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size;
    if (!GetFileSizeEx(hFile, &size) || size.QuadPart == 0) {
        CloseHandle(hFile);
        return false;
    }

    HANDLE hMap = CreateFileMappingW(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!hMap) {
        CloseHandle(hFile);
        return false;
    }

    const void* pMem = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
    if (!pMem) {
        CloseHandle(hMap);
        CloseHandle(hFile);
        return false;
    }

    mz_zip_archive zip = {};
    if (!mz_zip_reader_init_mem(&zip, pMem, static_cast<size_t>(size.QuadPart), 0)) {
        UnmapViewOfFile(pMem);
        CloseHandle(hMap);
        CloseHandle(hFile);
        return false;
    }

    mz_uint numFiles = mz_zip_reader_get_num_files(&zip);
    int targetIndex = -1;

    for (mz_uint i = 0; i < numFiles; ++i) {
        mz_zip_archive_file_stat stat = {};
        if (!mz_zip_reader_file_stat(&zip, i, &stat)) continue;
        if (CaseInsensitiveEquals(stat.m_filename, entryName)) {
            targetIndex = static_cast<int>(i);
            break;
        }
    }

    bool success = false;
    if (targetIndex >= 0) {
        size_t uncompSize = 0;
        void* pData = mz_zip_reader_extract_to_heap(&zip, targetIndex, &uncompSize, 0);
        if (pData) {
            outBytes.resize(uncompSize);
            memcpy(outBytes.data(), pData, uncompSize);
            mz_free(pData);
            success = true;
        }
    }

    mz_zip_reader_end(&zip);
    UnmapViewOfFile(pMem);
    CloseHandle(hMap);
    CloseHandle(hFile);

    return success;
}

bool ArchivePreview::InspectPresentation(const std::wstring& zipPath, int& outSlideCount, bool& outHasThumbnail) {
    outSlideCount = 0;
    outHasThumbnail = false;

    HANDLE hFile = CreateFileW(zipPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size;
    if (!GetFileSizeEx(hFile, &size) || size.QuadPart == 0) {
        CloseHandle(hFile);
        return false;
    }

    HANDLE hMap = CreateFileMappingW(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!hMap) {
        CloseHandle(hFile);
        return false;
    }

    const void* pMem = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
    if (!pMem) {
        CloseHandle(hMap);
        CloseHandle(hFile);
        return false;
    }

    mz_zip_archive zip = {};
    if (!mz_zip_reader_init_mem(&zip, pMem, static_cast<size_t>(size.QuadPart), 0)) {
        UnmapViewOfFile(pMem);
        CloseHandle(hMap);
        CloseHandle(hFile);
        return false;
    }

    mz_uint numFiles = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < numFiles; ++i) {
        mz_zip_archive_file_stat stat = {};
        if (!mz_zip_reader_file_stat(&zip, i, &stat)) continue;

        std::string fn = stat.m_filename;
        std::replace(fn.begin(), fn.end(), '\\', '/');
        std::transform(fn.begin(), fn.end(), fn.begin(), [](unsigned char c) { return (char)std::tolower(c); });

        if (fn == "docprops/thumbnail.jpeg" || fn == "docprops/thumbnail.jpg" || fn == "docprops/thumbnail.png") {
            outHasThumbnail = true;
        }

        if (fn.rfind("ppt/slides/slide", 0) == 0 && fn.size() >= 4 && fn.substr(fn.size() - 4) == ".xml") {
            outSlideCount++;
        }
    }

    mz_zip_reader_end(&zip);
    UnmapViewOfFile(pMem);
    CloseHandle(hMap);
    CloseHandle(hFile);

    return true;
}

void ArchivePreview::CreateResources(ID2D1RenderTarget* rt) {
    if (m_textBrush) return;
    rt->CreateSolidColorBrush(D2D1::ColorF(0.92f, 0.94f, 0.96f, 1.0f), &m_textBrush);
    rt->CreateSolidColorBrush(D2D1::ColorF(0.55f, 0.60f, 0.68f, 1.0f), &m_dimBrush);
    rt->CreateSolidColorBrush(D2D1::ColorF(0.23f, 0.51f, 0.96f, 1.0f), &m_accentBrush);
    rt->CreateSolidColorBrush(D2D1::ColorF(0.12f, 0.15f, 0.20f, 1.0f), &m_headerBrush);
    rt->CreateSolidColorBrush(D2D1::ColorF(0.08f, 0.10f, 0.14f, 0.5f), &m_rowAltBrush);
    rt->CreateSolidColorBrush(D2D1::ColorF(0.20f, 0.25f, 0.35f, 0.4f), &m_hoverBrush);
    rt->CreateSolidColorBrush(D2D1::ColorF(0.20f, 0.24f, 0.30f, 1.0f), &m_borderBrush);
}

void ArchivePreview::DiscardResources() {
    m_textBrush.Reset();
    m_dimBrush.Reset();
    m_accentBrush.Reset();
    m_headerBrush.Reset();
    m_rowAltBrush.Reset();
    m_hoverBrush.Reset();
    m_borderBrush.Reset();
}

void ArchivePreview::OnMouseWheel(int delta) {
    float scrollStep = 80.0f;
    if (delta > 0) {
        m_scrollY = (std::max)(0.0f, m_scrollY - scrollStep);
    } else {
        m_scrollY += scrollStep;
    }
}

void ArchivePreview::OnMouseMove(float x, float y) {
    m_mouseX = x;
    m_mouseY = y;
}

void ArchivePreview::Render(ID2D1RenderTarget* rt, IDWriteFactory* dwriteFactory, const D2D1_RECT_F& area) {
    if (!m_isLoaded || !rt || !dwriteFactory) return;
    CreateResources(rt);

    float width = area.right - area.left;
    float height = area.bottom - area.top;
    if (width <= 0 || height <= 0) return;

    // 1. Top Summary Banner
    float bannerHeight = 70.0f;
    D2D1_RECT_F bannerRect = D2D1::RectF(area.left + 16, area.top + 12, area.right - 16, area.top + 12 + bannerHeight);

    // Rounded card background for banner
    D2D1_ROUNDED_RECT roundedBanner = D2D1::RoundedRect(bannerRect, 8.0f, 8.0f);
    rt->FillRoundedRectangle(roundedBanner, m_headerBrush.Get());
    rt->DrawRoundedRectangle(roundedBanner, m_borderBrush.Get(), 1.0f);

    // Title format
    Microsoft::WRL::ComPtr<IDWriteTextFormat> titleFormat;
    dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                   DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                   16.0f, L"en-us", &titleFormat);

    Microsoft::WRL::ComPtr<IDWriteTextFormat> subtitleFormat;
    dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                   DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                   12.0f, L"en-us", &subtitleFormat);

    if (titleFormat) {
        std::wstring titleText = L"\U0001F4E6 " + m_fileName;
        D2D1_RECT_F titleRect = D2D1::RectF(bannerRect.left + 14, bannerRect.top + 10, bannerRect.right - 14, bannerRect.top + 34);
        rt->DrawTextW(titleText.c_str(), static_cast<UINT32>(titleText.length()), titleFormat.Get(), titleRect, m_textBrush.Get(),
                      D2D1_DRAW_TEXT_OPTIONS_CLIP | D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
    }

    if (subtitleFormat) {
        double totalRatio = (m_totalUncompressed > 0)
            ? (100.0 - (static_cast<double>(m_totalCompressed) * 100.0 / m_totalUncompressed))
            : 0.0;
        if (totalRatio < 0.0) totalRatio = 0.0;

        std::wstringstream ss;
        ss << m_fileCount << L" files";
        if (m_folderCount > 0) ss << L", " << m_folderCount << L" folders";
        ss << L"  \u2022  Uncompressed: " << Utils::FormatFileSize(m_totalUncompressed);
        ss << L"  \u2022  Compressed: " << Utils::FormatFileSize(m_totalCompressed);
        ss << L"  \u2022  Saved: " << std::fixed << std::setprecision(1) << totalRatio << L"%";

        std::wstring statsText = ss.str();
        D2D1_RECT_F statsRect = D2D1::RectF(bannerRect.left + 14, bannerRect.top + 36, bannerRect.right - 14, bannerRect.top + 58);
        rt->DrawTextW(statsText.c_str(), static_cast<UINT32>(statsText.length()), subtitleFormat.Get(), statsRect, m_dimBrush.Get(),
                      D2D1_DRAW_TEXT_OPTIONS_CLIP | D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
    }

    // 2. Table Column Headers
    float tableTop = bannerRect.bottom + 12.0f;
    float headerRowHeight = 28.0f;
    D2D1_RECT_F colHeaderRect = D2D1::RectF(area.left + 16, tableTop, area.right - 16, tableTop + headerRowHeight);

    rt->FillRectangle(colHeaderRect, m_headerBrush.Get());
    rt->DrawLine(D2D1::Point2F(colHeaderRect.left, colHeaderRect.bottom),
                 D2D1::Point2F(colHeaderRect.right, colHeaderRect.bottom), m_borderBrush.Get(), 1.0f);

    float tableWidth = colHeaderRect.right - colHeaderRect.left;
    float colSizeW = 85.0f;
    float colCompW = 95.0f;
    float colRatioW = 60.0f;
    float colDateW = 135.0f;
    float fixedW = colSizeW + colCompW + colRatioW + colDateW;
    float colNameW = (tableWidth > fixedW + 150.0f) ? (tableWidth - fixedW) : (tableWidth * 0.48f);

    Microsoft::WRL::ComPtr<IDWriteTextFormat> colFormat;
    dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                   DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                   11.0f, L"en-us", &colFormat);

    Microsoft::WRL::ComPtr<IDWriteTextFormat> colRightFormat;
    dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                   DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                   11.0f, L"en-us", &colRightFormat);
    if (colRightFormat) colRightFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);

    if (colFormat && colRightFormat) {
        float x = colHeaderRect.left + 10.0f;
        D2D1_RECT_F rName = D2D1::RectF(x, tableTop + 5, x + colNameW - 10, tableTop + headerRowHeight);
        rt->DrawTextW(L"NAME", 4, colFormat.Get(), rName, m_dimBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);

        x += colNameW;
        D2D1_RECT_F rSize = D2D1::RectF(x, tableTop + 5, x + colSizeW - 10, tableTop + headerRowHeight);
        rt->DrawTextW(L"SIZE", 4, colRightFormat.Get(), rSize, m_dimBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);

        x += colSizeW;
        D2D1_RECT_F rComp = D2D1::RectF(x, tableTop + 5, x + colCompW - 10, tableTop + headerRowHeight);
        rt->DrawTextW(L"COMPRESSED", 10, colRightFormat.Get(), rComp, m_dimBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);

        x += colCompW;
        D2D1_RECT_F rRatio = D2D1::RectF(x, tableTop + 5, x + colRatioW - 10, tableTop + headerRowHeight);
        rt->DrawTextW(L"RATIO", 5, colRightFormat.Get(), rRatio, m_dimBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);

        x += colRatioW;
        D2D1_RECT_F rDate = D2D1::RectF(x + 10, tableTop + 5, colHeaderRect.right - 10, tableTop + headerRowHeight);
        rt->DrawTextW(L"MODIFIED", 8, colFormat.Get(), rDate, m_dimBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    // 3. Rows Viewport
    float rowsTop = colHeaderRect.bottom;
    float rowsBottom = area.bottom - 12.0f;
    float viewHeight = rowsBottom - rowsTop;
    if (viewHeight <= 0) return;

    D2D1_RECT_F clipRect = D2D1::RectF(area.left + 16, rowsTop, area.right - 16, rowsBottom);
    rt->PushAxisAlignedClip(clipRect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    float rowH = 26.0f;
    m_contentHeight = m_entries.size() * rowH;
    float maxScroll = (std::max)(0.0f, m_contentHeight - viewHeight);
    if (m_scrollY > maxScroll) m_scrollY = maxScroll;

    Microsoft::WRL::ComPtr<IDWriteTextFormat> cellFormat;
    dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                   DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                   12.0f, L"en-us", &cellFormat);
    if (cellFormat) cellFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    Microsoft::WRL::ComPtr<IDWriteTextFormat> cellRightFormat;
    dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                   DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                   12.0f, L"en-us", &cellRightFormat);
    if (cellRightFormat) {
        cellRightFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        cellRightFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    }

    // Determine visible row range
    int firstVisible = static_cast<int>(m_scrollY / rowH);
    if (firstVisible < 0) firstVisible = 0;
    int lastVisible = static_cast<int>((m_scrollY + viewHeight) / rowH) + 1;
    if (lastVisible > static_cast<int>(m_entries.size())) lastVisible = static_cast<int>(m_entries.size());

    for (int i = firstVisible; i < lastVisible; ++i) {
        const auto& entry = m_entries[i];
        float curY = rowsTop + (i * rowH) - m_scrollY;
        D2D1_RECT_F rowRect = D2D1::RectF(clipRect.left, curY, clipRect.right, curY + rowH);

        // Hover or alternating background
        bool isHovered = (m_mouseY >= rowRect.top && m_mouseY < rowRect.bottom &&
                          m_mouseX >= rowRect.left && m_mouseX < rowRect.right);
        if (isHovered) {
            rt->FillRectangle(rowRect, m_hoverBrush.Get());
        } else if (i % 2 == 1) {
            rt->FillRectangle(rowRect, m_rowAltBrush.Get());
        }

        // Row border
        rt->DrawLine(D2D1::Point2F(rowRect.left, rowRect.bottom),
                     D2D1::Point2F(rowRect.right, rowRect.bottom), m_borderBrush.Get(), 0.5f);

        float x = rowRect.left + 10.0f;

        // Icon + Middle-truncated Name
        std::wstring icon = entry.isDirectory ? L"\U0001F4C1 " : L"\U0001F4C4 ";
        float availableTextW = (std::max)(50.0f, colNameW - 30.0f);
        std::wstring compactName = TruncateMiddle(entry.name, availableTextW, cellFormat.Get(), dwriteFactory);
        std::wstring nameDisplay = icon + compactName;

        D2D1_RECT_F rName = D2D1::RectF(x, curY + 4, x + colNameW - 8.0f, curY + rowH);
        rt->DrawTextW(nameDisplay.c_str(), static_cast<UINT32>(nameDisplay.length()), cellFormat.Get(), rName,
                      entry.isDirectory ? m_accentBrush.Get() : m_textBrush.Get(),
                      D2D1_DRAW_TEXT_OPTIONS_CLIP | D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);

        // Size
        x += colNameW;
        std::wstring sizeStr = entry.isDirectory ? L"-" : Utils::FormatFileSize(entry.uncompressedSize);
        D2D1_RECT_F rSize = D2D1::RectF(x, curY + 4, x + colSizeW - 10, curY + rowH);
        rt->DrawTextW(sizeStr.c_str(), static_cast<UINT32>(sizeStr.length()), cellRightFormat.Get(), rSize, m_dimBrush.Get(),
                      D2D1_DRAW_TEXT_OPTIONS_CLIP);

        // Compressed Size
        x += colSizeW;
        std::wstring compStr = entry.isDirectory ? L"-" : Utils::FormatFileSize(entry.compressedSize);
        D2D1_RECT_F rComp = D2D1::RectF(x, curY + 4, x + colCompW - 10, curY + rowH);
        rt->DrawTextW(compStr.c_str(), static_cast<UINT32>(compStr.length()), cellRightFormat.Get(), rComp, m_dimBrush.Get(),
                      D2D1_DRAW_TEXT_OPTIONS_CLIP);

        // Ratio
        x += colCompW;
        std::wstringstream rss;
        if (entry.isDirectory || entry.uncompressedSize == 0) {
            rss << L"-";
        } else {
            rss << std::fixed << std::setprecision(0) << entry.ratio << L"%";
        }
        std::wstring ratioStr = rss.str();
        D2D1_RECT_F rRatio = D2D1::RectF(x, curY + 4, x + colRatioW - 10, curY + rowH);
        rt->DrawTextW(ratioStr.c_str(), static_cast<UINT32>(ratioStr.length()), cellRightFormat.Get(), rRatio, m_dimBrush.Get(),
                      D2D1_DRAW_TEXT_OPTIONS_CLIP);

        // Date
        x += colRatioW;
        D2D1_RECT_F rDate = D2D1::RectF(x + 10, curY + 4, rowRect.right - 10, curY + rowH);
        rt->DrawTextW(entry.dateModified.c_str(), static_cast<UINT32>(entry.dateModified.length()), cellFormat.Get(), rDate, m_dimBrush.Get(),
                      D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    rt->PopAxisAlignedClip();

    // 4. Scrollbar (if content is scrollable)
    if (m_contentHeight > viewHeight) {
        float scrollbarWidth = 6.0f;
        float scrollbarX = clipRect.right - scrollbarWidth - 2.0f;
        float trackHeight = viewHeight;
        float thumbHeight = (std::max)(24.0f, (viewHeight / m_contentHeight) * trackHeight);
        float thumbY = rowsTop + (m_scrollY / (m_contentHeight - viewHeight)) * (trackHeight - thumbHeight);

        D2D1_ROUNDED_RECT thumbRect = D2D1::RoundedRect(
            D2D1::RectF(scrollbarX, thumbY, scrollbarX + scrollbarWidth, thumbY + thumbHeight),
            3.0f, 3.0f);
        rt->FillRoundedRectangle(thumbRect, m_dimBrush.Get());
    }
}
