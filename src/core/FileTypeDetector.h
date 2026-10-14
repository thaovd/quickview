#pragma once

#include <string>
#include <unordered_set>
#include <algorithm>

enum class FileType {
    Image,
    Video,
    Audio,
    PDF,
    Document,
    Text,
    Markdown,
    Excel,
    PowerPoint,
    Svg,
    Illustrator,
    Archive,
    Font,
    Unknown
};

class FileTypeDetector {
public:
    static FileType Detect(const std::wstring& filePath);
    static const wchar_t* GetTypeLabel(FileType type);

private:
    static std::wstring GetLowerExtension(const std::wstring& path);
};
