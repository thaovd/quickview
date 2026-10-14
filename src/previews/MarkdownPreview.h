#pragma once

#include "Utils.h"
#include <string>
#include <vector>

enum class MdBlockKind {
    Heading1,
    Heading2,
    Heading3,
    Heading4,
    Paragraph,
    CodeBlock,
    Quote,
    ListItem,
    HorizontalRule
};

struct MdSpan {
    std::wstring text;
    bool isBold   = false;
    bool isItalic = false;
    bool isCode   = false;
    bool isLink   = false;
};

struct MdBlock {
    MdBlockKind kind = MdBlockKind::Paragraph;
    std::vector<MdSpan> spans;
    std::wstring fullText;
    int listIndex = -1; // -1 for bullet, >= 1 for ordered
    int indentLevel = 0;
};

class MarkdownPreview {
public:
    MarkdownPreview() = default;
    ~MarkdownPreview() = default;

    /// Load and parse a markdown file (.md)
    bool LoadFile(const std::wstring& path);

    /// Parse markdown file into blocks (thread-safe, can run in background)
    static bool Parse(const std::wstring& path, std::vector<MdBlock>& outBlocks);

    /// Attach pre-parsed markdown blocks
    void SetBlocks(std::vector<MdBlock>&& blocks);

    /// Render visible markdown content using Direct2D & DirectWrite
    void Render(
        ID2D1RenderTarget* rt,
        IDWriteFactory* dwrite,
        IDWriteTextFormat* bodyFont,
        IDWriteTextFormat* titleFont,
        IDWriteTextFormat* monoFont,
        IDWriteTextFormat* smallFont,
        const D2D1_RECT_F& area);

    /// Mouse wheel scrolling
    void OnMouseWheel(int delta, float viewHeight);

    float GetTotalHeight() const { return m_totalHeight; }
    float GetScrollY() const     { return m_scrollY; }
    void  SetScrollY(float y)    { m_scrollY = y; }
    bool  IsLoaded() const       { return !m_blocks.empty(); }

    void Reset();

private:
    std::vector<MdBlock> m_blocks;
    float m_totalHeight = 0.0f;
    float m_scrollY     = 0.0f;
};
