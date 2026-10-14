#include "FileTypeDetector.h"

std::wstring FileTypeDetector::GetLowerExtension(const std::wstring& path) {
    size_t pos = path.rfind(L'.');
    if (pos == std::wstring::npos) return L"";
    std::wstring ext = path.substr(pos);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
    return ext;
}

FileType FileTypeDetector::Detect(const std::wstring& filePath) {
    std::wstring ext = GetLowerExtension(filePath);
    if (ext.empty()) return FileType::Unknown;

    // Image formats (WIC native + stb_image + RAW)
    static const std::unordered_set<std::wstring> imageExts = {
        L".png", L".jpg", L".jpeg", L".bmp", L".gif", L".tiff", L".tif",
        L".webp", L".ico", L".heif", L".heic", L".jfif", L".wdp", L".hdp",
        L".dds", L".jxr",
        // stb_image formats
        L".psd", L".tga", L".hdr", L".pic", L".pnm", L".ppm", L".pgm",
        // Camera RAW formats
        L".cr2", L".cr3", L".crw", L".nef", L".nrw", L".arw", L".srf", L".sr2",
        L".dng", L".raf", L".rw2", L".orf", L".pef", L".ptx", L".raw", L".rwl",
        L".srw", L".mrw", L".3fr"
    };
    if (imageExts.count(ext)) return FileType::Image;

    // Video formats
    static const std::unordered_set<std::wstring> videoExts = {
        L".mp4", L".avi", L".mkv", L".mov", L".wmv", L".flv", L".webm",
        L".m4v", L".3gp", L".mpg", L".mpeg", L".ts", L".m2ts", L".vob",
        L".ogv", L".rm", L".rmvb"
    };
    if (videoExts.count(ext)) return FileType::Video;

    // Audio formats
    static const std::unordered_set<std::wstring> audioExts = {
        L".mp3", L".wav", L".flac", L".aac", L".ogg", L".wma", L".m4a",
        L".opus", L".aiff", L".alac", L".ape", L".mid", L".midi"
    };
    if (audioExts.count(ext)) return FileType::Audio;

    // PDF
    if (ext == L".pdf") return FileType::PDF;

    // Adobe Illustrator (.ai) -> Compatible with PDFium
    if (ext == L".ai") return FileType::Illustrator;

    // SVG Vector format
    if (ext == L".svg" || ext == L".svgz") return FileType::Svg;

    // Font files (TTF, OTF, TTC)
    if (ext == L".ttf" || ext == L".otf" || ext == L".ttc" || ext == L".woff" || ext == L".woff2") {
        return FileType::Font;
    }

    // Markdown
    if (ext == L".md" || ext == L".markdown") return FileType::Markdown;

    // Excel spreadsheets (OpenXLSX)
    static const std::unordered_set<std::wstring> excelExts = {
        L".xlsx", L".xlsm", L".xltx", L".xltm"
    };
    if (excelExts.count(ext)) return FileType::Excel;

    // PowerPoint presentations (docProps/thumbnail.jpeg extraction)
    static const std::unordered_set<std::wstring> pptExts = {
        L".pptx", L".ppsx", L".potx", L".pptm"
    };
    if (pptExts.count(ext)) return FileType::PowerPoint;

    // Archive files (miniz)
    static const std::unordered_set<std::wstring> archiveExts = {
        L".zip", L".jar", L".apk", L".cbz", L".war", L".ear"
    };
    if (archiveExts.count(ext)) return FileType::Archive;

    // Other Office / Document formats (handled by IPreviewHandler or FileInfo)
    static const std::unordered_set<std::wstring> docExts = {
        L".doc", L".docx", L".xls", L".ppt",
        L".odt", L".ods", L".odp", L".rtf", L".epub"
    };
    if (docExts.count(ext)) return FileType::Document;

    // Text / Code formats
    static const std::unordered_set<std::wstring> textExts = {
        L".txt", L".json", L".xml", L".csv", L".log", L".ini",
        L".cfg", L".yaml", L".yml", L".toml", L".html", L".htm", L".css",
        L".js", L".jsx", L".ts", L".tsx", L".py", L".cpp", L".c", L".h",
        L".hpp", L".java", L".cs", L".go", L".rs", L".rb", L".php",
        L".swift", L".kt", L".kts", L".sh", L".bat", L".ps1", L".cmd",
        L".sql", L".r", L".lua", L".dart", L".tex", L".bib", L".cmake",
        L".gradle", L".makefile", L".dockerfile", L".gitignore",
        L".env", L".conf", L".properties", L".sass", L".scss", L".less",
        L".vue", L".svelte", L".astro"
    };
    if (textExts.count(ext)) return FileType::Text;

    return FileType::Unknown;
}

const wchar_t* FileTypeDetector::GetTypeLabel(FileType type) {
    switch (type) {
        case FileType::Image:       return L"Image";
        case FileType::Video:       return L"Video";
        case FileType::Audio:       return L"Audio";
        case FileType::PDF:         return L"PDF Document";
        case FileType::Illustrator: return L"Adobe Illustrator Artwork";
        case FileType::Svg:         return L"Scalable Vector Graphics";
        case FileType::Font:        return L"Font File";
        case FileType::Excel:       return L"Excel Spreadsheet";
        case FileType::PowerPoint:  return L"PowerPoint Presentation";
        case FileType::Archive:     return L"ZIP Archive";
        case FileType::Document:    return L"Document";
        case FileType::Text:        return L"Text File";
        case FileType::Markdown:    return L"Markdown Document";
        case FileType::Unknown:     return L"File";
        default:                    return L"File";
    }
}
