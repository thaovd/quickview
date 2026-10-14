#include "MarkdownPreview.h"
#include "md4c.h"
#include <algorithm>

struct ParseContext {
    std::vector<MdBlock>* blocks = nullptr;
    MdBlock currentBlock;
    bool inBlock = false;
    bool currentBold = false;
    bool currentItalic = false;
    bool currentCode = false;
    bool currentLink = false;
    int listIndex = -1;
    int indentLevel = 0;
};

static int enter_block_callback(MD_BLOCKTYPE type, void* detail, void* userdata) {
    auto* ctx = static_cast<ParseContext*>(userdata);
    ctx->currentBlock = MdBlock();
    ctx->inBlock = true;

    switch (type) {
    case MD_BLOCK_H: {
        auto* h = static_cast<MD_BLOCK_H_DETAIL*>(detail);
        if (h->level == 1)      ctx->currentBlock.kind = MdBlockKind::Heading1;
        else if (h->level == 2) ctx->currentBlock.kind = MdBlockKind::Heading2;
        else if (h->level == 3) ctx->currentBlock.kind = MdBlockKind::Heading3;
        else                    ctx->currentBlock.kind = MdBlockKind::Heading4;
        break;
    }
    case MD_BLOCK_CODE:
        ctx->currentBlock.kind = MdBlockKind::CodeBlock;
        break;
    case MD_BLOCK_QUOTE:
        ctx->currentBlock.kind = MdBlockKind::Quote;
        break;
    case MD_BLOCK_UL:
        ctx->listIndex = -1;
        ctx->indentLevel++;
        break;
    case MD_BLOCK_OL: {
        auto* ol = static_cast<MD_BLOCK_OL_DETAIL*>(detail);
        ctx->listIndex = (ol->start > 0) ? static_cast<int>(ol->start) : 1;
        ctx->indentLevel++;
        break;
    }
    case MD_BLOCK_LI:
        ctx->currentBlock.kind = MdBlockKind::ListItem;
        ctx->currentBlock.listIndex = ctx->listIndex;
        ctx->currentBlock.indentLevel = (std::max)(0, ctx->indentLevel - 1);
        if (ctx->listIndex > 0) ctx->listIndex++;
        break;
    case MD_BLOCK_HR:
        ctx->currentBlock.kind = MdBlockKind::HorizontalRule;
        break;
    case MD_BLOCK_P:
    default:
        ctx->currentBlock.kind = MdBlockKind::Paragraph;
        break;
    }
    return 0;
}

static int leave_block_callback(MD_BLOCKTYPE type, void* detail, void* userdata) {
    auto* ctx = static_cast<ParseContext*>(userdata);
    if (type == MD_BLOCK_UL || type == MD_BLOCK_OL) {
        if (ctx->indentLevel > 0) ctx->indentLevel--;
        if (ctx->indentLevel == 0) ctx->listIndex = -1;
        return 0;
    }

    if (ctx->inBlock) {
        for (const auto& span : ctx->currentBlock.spans) {
            ctx->currentBlock.fullText += span.text;
        }
        if (!ctx->currentBlock.fullText.empty() || ctx->currentBlock.kind == MdBlockKind::HorizontalRule) {
            ctx->blocks->push_back(std::move(ctx->currentBlock));
        }
        ctx->inBlock = false;
    }
    return 0;
}

static int enter_span_callback(MD_SPANTYPE type, void* detail, void* userdata) {
    auto* ctx = static_cast<ParseContext*>(userdata);
    if (type == MD_SPAN_STRONG) ctx->currentBold = true;
    else if (type == MD_SPAN_EM) ctx->currentItalic = true;
    else if (type == MD_SPAN_CODE) ctx->currentCode = true;
    else if (type == MD_SPAN_A) ctx->currentLink = true;
    return 0;
}

static int leave_span_callback(MD_SPANTYPE type, void* detail, void* userdata) {
    auto* ctx = static_cast<ParseContext*>(userdata);
    if (type == MD_SPAN_STRONG) ctx->currentBold = false;
    else if (type == MD_SPAN_EM) ctx->currentItalic = false;
    else if (type == MD_SPAN_CODE) ctx->currentCode = false;
    else if (type == MD_SPAN_A) ctx->currentLink = false;
    return 0;
}

static int text_callback(MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size, void* userdata) {
    auto* ctx = static_cast<ParseContext*>(userdata);
    if (size == 0 || !ctx->inBlock) return 0;

    int wLen = MultiByteToWideChar(CP_UTF8, 0, text, static_cast<int>(size), nullptr, 0);
    if (wLen <= 0) return 0;

    std::wstring wText(wLen, 0);
    MultiByteToWideChar(CP_UTF8, 0, text, static_cast<int>(size), wText.data(), wLen);

    MdSpan span;
    span.text     = std::move(wText);
    span.isBold   = ctx->currentBold;
    span.isItalic = ctx->currentItalic;
    span.isCode   = ctx->currentCode;
    span.isLink   = ctx->currentLink;

    ctx->currentBlock.spans.push_back(std::move(span));
    return 0;
}

bool MarkdownPreview::Parse(const std::wstring& path, std::vector<MdBlock>& outBlocks) {
    outBlocks.clear();

    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER fileSize;
    if (!GetFileSizeEx(hFile, &fileSize) || fileSize.QuadPart == 0) {
        CloseHandle(hFile);
        return false;
    }

    HANDLE hMap = CreateFileMappingW(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!hMap) {
        CloseHandle(hFile);
        return false;
    }

    const char* pData = static_cast<const char*>(MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0));
    if (!pData) {
        CloseHandle(hMap);
        CloseHandle(hFile);
        return false;
    }

    MD_PARSER parser = {
        0,
        MD_FLAG_TABLES | MD_FLAG_TASKLISTS | MD_FLAG_STRIKETHROUGH | MD_FLAG_PERMISSIVEAUTOLINKS,
        enter_block_callback,
        leave_block_callback,
        enter_span_callback,
        leave_span_callback,
        text_callback,
        nullptr,
        nullptr
    };

    ParseContext ctx;
    ctx.blocks = &outBlocks;
    md_parse(pData, static_cast<MD_SIZE>(fileSize.QuadPart), &parser, &ctx);

    UnmapViewOfFile(pData);
    CloseHandle(hMap);
    CloseHandle(hFile);

    return !outBlocks.empty();
}

bool MarkdownPreview::LoadFile(const std::wstring& path) {
    Reset();
    return Parse(path, m_blocks);
}

void MarkdownPreview::SetBlocks(std::vector<MdBlock>&& blocks) {
    Reset();
    m_blocks = std::move(blocks);
}

void MarkdownPreview::Render(
    ID2D1RenderTarget* rt,
    IDWriteFactory* dwrite,
    IDWriteTextFormat* bodyFont,
    IDWriteTextFormat* titleFont,
    IDWriteTextFormat* monoFont,
    IDWriteTextFormat* smallFont,
    const D2D1_RECT_F& area)
{
    if (!rt || !dwrite || m_blocks.empty()) return;

    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    rt->CreateSolidColorBrush(Colors::Text, &brush);
    if (!brush) return;

    float viewW = (area.right - area.left);
    float viewH = (area.bottom - area.top);
    float padX = 24.0f;
    float contentW = (std::max)(100.0f, viewW - padX * 2.0f);
    float startX = area.left + padX;
    float curY = area.top + 20.0f - m_scrollY;

    float measuredTotalH = 20.0f;

    for (const auto& block : m_blocks) {
        if (block.kind == MdBlockKind::HorizontalRule) {
            float lineY = curY + 8.0f;
            if (lineY >= area.top && lineY <= area.bottom) {
                brush->SetColor(Colors::Separator);
                rt->FillRectangle(D2D1::RectF(startX, lineY, startX + contentW, lineY + 1.0f), brush.Get());
            }
            curY += 20.0f;
            measuredTotalH += 20.0f;
            continue;
        }

        IDWriteTextFormat* fontToUse = bodyFont;
        D2D1_COLOR_F textColor = Colors::Text;
        float bottomMargin = 12.0f;
        float blockStartX = startX;
        float blockW = contentW;

        if (block.kind == MdBlockKind::Heading1) {
            fontToUse = titleFont;
            textColor = Colors::Accent;
            bottomMargin = 16.0f;
        } else if (block.kind == MdBlockKind::Heading2) {
            fontToUse = titleFont;
            textColor = Colors::Text;
            bottomMargin = 14.0f;
        } else if (block.kind == MdBlockKind::Heading3 || block.kind == MdBlockKind::Heading4) {
            fontToUse = bodyFont;
            textColor = Colors::AccentDim;
            bottomMargin = 10.0f;
        } else if (block.kind == MdBlockKind::CodeBlock) {
            fontToUse = monoFont;
            textColor = Colors::Text;
            bottomMargin = 14.0f;
        } else if (block.kind == MdBlockKind::Quote) {
            fontToUse = bodyFont;
            textColor = Colors::TextDim;
            blockStartX = startX + 16.0f;
            blockW = contentW - 16.0f;
            bottomMargin = 12.0f;
        } else if (block.kind == MdBlockKind::ListItem) {
            fontToUse = bodyFont;
            textColor = Colors::Text;
            float indent = 20.0f + block.indentLevel * 18.0f;
            blockStartX = startX + indent;
            blockW = contentW - indent;
            bottomMargin = 8.0f;
        }

        Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
        HRESULT hr = dwrite->CreateTextLayout(
            block.fullText.c_str(),
            static_cast<UINT32>(block.fullText.length()),
            fontToUse,
            blockW,
            10000.0f,
            &layout);

        if (FAILED(hr) || !layout) {
            curY += 20.0f;
            measuredTotalH += 20.0f;
            continue;
        }

        // Apply spans formatting (Bold, Italic, Code, Link)
        UINT32 charPos = 0;
        for (const auto& span : block.spans) {
            UINT32 spanLen = static_cast<UINT32>(span.text.length());
            if (spanLen > 0) {
                DWRITE_TEXT_RANGE range = { charPos, spanLen };
                if (span.isBold) {
                    layout->SetFontWeight(DWRITE_FONT_WEIGHT_BOLD, range);
                }
                if (span.isItalic) {
                    layout->SetFontStyle(DWRITE_FONT_STYLE_ITALIC, range);
                }
                if (span.isLink) {
                    layout->SetUnderline(TRUE, range);
                }
                charPos += spanLen;
            }
        }

        DWRITE_TEXT_METRICS metrics;
        layout->GetMetrics(&metrics);
        float blockH = metrics.height;

        // Render if visible inside area
        if (curY + blockH >= area.top && curY <= area.bottom) {
            if (block.kind == MdBlockKind::CodeBlock) {
                // Background rounded box
                D2D1_ROUNDED_RECT codeBg = D2D1::RoundedRect(
                    D2D1::RectF(startX, curY - 6.0f, startX + contentW, curY + blockH + 6.0f),
                    6.0f, 6.0f);
                brush->SetColor(Colors::CodeBg);
                rt->FillRoundedRectangle(codeBg, brush.Get());
                brush->SetColor(Colors::Border);
                rt->DrawRoundedRectangle(codeBg, brush.Get(), 1.0f);
            } else if (block.kind == MdBlockKind::Quote) {
                // Left accent vertical bar
                brush->SetColor(Colors::Accent);
                rt->FillRectangle(D2D1::RectF(startX, curY, startX + 3.0f, curY + blockH), brush.Get());
            } else if (block.kind == MdBlockKind::ListItem) {
                // Bullet or number prefix
                float prefixX = blockStartX - 14.0f;
                brush->SetColor(Colors::Accent);
                if (block.listIndex > 0) {
                    wchar_t numBuf[16];
                    swprintf_s(numBuf, L"%d.", block.listIndex);
                    rt->DrawText(numBuf, static_cast<UINT32>(wcslen(numBuf)),
                        smallFont ? smallFont : fontToUse,
                        D2D1::RectF(prefixX - 12.0f, curY, prefixX + 6.0f, curY + 20.0f),
                        brush.Get());
                } else {
                    rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(prefixX, curY + 9.0f), 3.0f, 3.0f), brush.Get());
                }
            }

            brush->SetColor(textColor);
            rt->DrawTextLayout(D2D1::Point2F(blockStartX, curY), layout.Get(), brush.Get());

            if (block.kind == MdBlockKind::Heading1) {
                // Underline separator for H1
                brush->SetColor(Colors::Separator);
                rt->FillRectangle(D2D1::RectF(startX, curY + blockH + 6.0f, startX + contentW, curY + blockH + 7.0f), brush.Get());
            }
        }

        float step = blockH + bottomMargin;
        curY += step;
        measuredTotalH += step;
    }

    m_totalHeight = measuredTotalH + 20.0f;
}

void MarkdownPreview::OnMouseWheel(int delta, float viewHeight) {
    m_scrollY -= delta * 0.5f;
    float maxScroll = (std::max)(0.0f, m_totalHeight - viewHeight);
    m_scrollY = (std::max)(0.0f, (std::min)(m_scrollY, maxScroll));
}

void MarkdownPreview::Reset() {
    m_blocks.clear();
    m_totalHeight = 0.0f;
    m_scrollY     = 0.0f;
}
