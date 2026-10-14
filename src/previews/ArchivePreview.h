#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <string>
#include <vector>
#include <cstdint>

struct ArchiveEntry {
    std::wstring name;
    bool isDirectory = false;
    uint64_t uncompressedSize = 0;
    uint64_t compressedSize = 0;
    double ratio = 0.0;
    std::wstring dateModified;
};

struct ArchiveData {
    std::vector<ArchiveEntry> entries;
    uint64_t totalUncompressed = 0;
    uint64_t totalCompressed = 0;
    int fileCount = 0;
    int folderCount = 0;
};

class ArchivePreview {
public:
    ArchivePreview();
    ~ArchivePreview();

    bool Load(const std::wstring& path);
    void Unload();

    /// Parse archive asynchronously (thread-safe, can run in background)
    static bool ParseArchive(const std::wstring& path, ArchiveData& outData);

    /// Attach pre-parsed archive data
    void SetData(const std::wstring& path, ArchiveData&& data);

    void Render(ID2D1RenderTarget* rt, IDWriteFactory* dwriteFactory, const D2D1_RECT_F& area);
    void OnMouseWheel(int delta);
    void OnMouseMove(float x, float y);

    bool IsLoaded() const { return m_isLoaded; }
    size_t GetEntryCount() const { return m_entries.size(); }

    // Static helper to extract an entry from any ZIP-based file (e.g. PPTX thumbnail)
    static bool ExtractFile(const std::wstring& zipPath, const std::string& entryName, std::vector<uint8_t>& outBytes);

    // Static helper to check PPTX slides
    static bool InspectPresentation(const std::wstring& zipPath, int& outSlideCount, bool& outHasThumbnail);

private:
    bool m_isLoaded = false;
    std::wstring m_filePath;
    std::wstring m_fileName;
    std::vector<ArchiveEntry> m_entries;

    uint64_t m_totalUncompressed = 0;
    uint64_t m_totalCompressed = 0;
    int m_fileCount = 0;
    int m_folderCount = 0;

    float m_scrollY = 0.0f;
    float m_contentHeight = 0.0f;
    float m_mouseY = -1.0f;
    float m_mouseX = -1.0f;

    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_textBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_dimBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_accentBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_headerBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_rowAltBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_hoverBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_borderBrush;

    void CreateResources(ID2D1RenderTarget* rt);
    void DiscardResources();
};
