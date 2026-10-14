#include "RawThumbnailExtractor.h"
#include <windows.h>
#include <algorithm>
#include <unordered_set>
#include <vector>
#include <cstring>

namespace {

// Common TIFF tags
constexpr uint16_t TAG_NEW_SUBFILE_TYPE    = 0x00FE;
constexpr uint16_t TAG_IMAGE_WIDTH         = 0x0100;
constexpr uint16_t TAG_IMAGE_LENGTH        = 0x0101;
constexpr uint16_t TAG_COMPRESSION         = 0x0103;
constexpr uint16_t TAG_STRIP_OFFSETS       = 0x0111;
constexpr uint16_t TAG_STRIP_BYTE_COUNTS   = 0x0117;
constexpr uint16_t TAG_SUB_IFDS            = 0x014A;
constexpr uint16_t TAG_JPEG_INTERCHANGE_FMT= 0x0201; // Tag mentioned by user
constexpr uint16_t TAG_JPEG_INTERCHANGE_LEN= 0x0202; // Tag mentioned by user
constexpr uint16_t TAG_EXIF_IFD            = 0x8769;
constexpr uint16_t TAG_MAKER_NOTE          = 0x927C;
constexpr uint16_t TAG_DNG_VERSION         = 0xC612;

struct JpegCandidate {
    uint32_t offset = 0;
    uint32_t length = 0;
};

inline uint16_t ReadU16(const uint8_t* p, bool le) {
    return le ? static_cast<uint16_t>(p[0] | (p[1] << 8))
              : static_cast<uint16_t>((p[0] << 8) | p[1]);
}

inline uint32_t ReadU32(const uint8_t* p, bool le) {
    return le ? static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24))
              : static_cast<uint32_t>((p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]);
}

inline uint32_t ReadBigEndianU32(const uint8_t* p) {
    return static_cast<uint32_t>((p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]);
}

bool IsValidJpeg(const uint8_t* data, size_t fileSize, uint32_t offset, uint32_t length) {
    if (offset >= fileSize || length < 128) return false;
    if (static_cast<uint64_t>(offset) + length > fileSize) return false;
    return (data[offset] == 0xFF && data[offset + 1] == 0xD8);
}

// Find JPEG EOI (\xFF\xD9) starting from offset
size_t FindJpegEOI(const uint8_t* data, size_t start, size_t maxScan) {
    if (start + 2 >= maxScan) return 0;
    for (size_t i = start + 2; i + 1 < maxScan; ++i) {
        if (data[i] == 0xFF && data[i + 1] == 0xD9) {
            return i + 2;
        }
    }
    return 0;
}

// Parse a single IFD and collect tags
void ParseIFD(
    const uint8_t* data, size_t fileSize, uint32_t ifdOffset, bool isLE,
    std::vector<JpegCandidate>& candidates,
    std::vector<uint32_t>& subIfdOffsets,
    uint32_t& nextIfdOffset,
    std::unordered_set<uint32_t>& visited)
{
    nextIfdOffset = 0;
    if (ifdOffset == 0 || ifdOffset + 2 > fileSize) return;
    if (visited.count(ifdOffset)) return;
    visited.insert(ifdOffset);

    uint16_t numEntries = ReadU16(data + ifdOffset, isLE);
    size_t entryTableEnd = ifdOffset + 2 + static_cast<size_t>(numEntries) * 12;
    if (entryTableEnd + 4 > fileSize) return;

    uint32_t jpegOffset = 0;
    uint32_t jpegLength = 0;
    uint16_t compression = 0;
    uint32_t stripOffset = 0;
    uint32_t stripLength = 0;

    for (uint16_t i = 0; i < numEntries; ++i) {
        const uint8_t* entry = data + ifdOffset + 2 + (i * 12);
        uint16_t tag = ReadU16(entry, isLE);
        uint16_t type = ReadU16(entry + 2, isLE);
        uint32_t count = ReadU32(entry + 4, isLE);

        uint32_t valOrOffset = 0;
        if (type == 3 && count == 1) { // SHORT
            valOrOffset = ReadU16(entry + 8, isLE);
        } else { // LONG or offset
            valOrOffset = ReadU32(entry + 8, isLE);
        }

        switch (tag) {
        case TAG_JPEG_INTERCHANGE_FMT: // 0x0201
            jpegOffset = valOrOffset;
            break;

        case TAG_JPEG_INTERCHANGE_LEN: // 0x0202
            jpegLength = valOrOffset;
            break;

        case TAG_COMPRESSION: // 0x0103
            compression = static_cast<uint16_t>(valOrOffset);
            break;

        case TAG_STRIP_OFFSETS: // 0x0111
            stripOffset = valOrOffset;
            break;

        case TAG_STRIP_BYTE_COUNTS: // 0x0117
            stripLength = valOrOffset;
            break;

        case TAG_SUB_IFDS: // 0x014A
            if (count == 1) {
                if (valOrOffset > 0 && valOrOffset < fileSize) subIfdOffsets.push_back(valOrOffset);
            } else if (count > 1 && count <= 32) {
                if (valOrOffset + count * 4 <= fileSize) {
                    for (uint32_t c = 0; c < count; ++c) {
                        uint32_t subOff = ReadU32(data + valOrOffset + c * 4, isLE);
                        if (subOff > 0 && subOff < fileSize) subIfdOffsets.push_back(subOff);
                    }
                }
            }
            break;

        case TAG_EXIF_IFD: // 0x8769
            if (valOrOffset > 0 && valOrOffset < fileSize) {
                subIfdOffsets.push_back(valOrOffset);
            }
            break;

        default:
            break;
        }
    }

    // Check Tag 0x0201 / 0x0202
    if (jpegOffset > 0 && jpegLength > 0) {
        if (IsValidJpeg(data, fileSize, jpegOffset, jpegLength)) {
            candidates.push_back({ jpegOffset, jpegLength });
        }
    } else if (jpegOffset > 0 && jpegLength == 0) {
        // Find length by scanning EOI
        size_t eoi = FindJpegEOI(data, jpegOffset, (std::min)(fileSize, static_cast<size_t>(jpegOffset + 16 * 1024 * 1024)));
        if (eoi > jpegOffset) {
            candidates.push_back({ jpegOffset, static_cast<uint32_t>(eoi - jpegOffset) });
        }
    }

    // Check StripOffsets with JPEG compression (6 or 7)
    if ((compression == 6 || compression == 7) && stripOffset > 0 && stripLength > 0) {
        if (IsValidJpeg(data, fileSize, stripOffset, stripLength)) {
            candidates.push_back({ stripOffset, stripLength });
        }
    }

    // Read next IFD offset
    nextIfdOffset = ReadU32(data + entryTableEnd, isLE);
}

// Fujifilm RAF extractor
bool ExtractFromFujifilm(const uint8_t* data, size_t fileSize, std::vector<uint8_t>& outJpegBytes) {
    if (fileSize < 120) return false;
    if (memcmp(data, "FUJIFILM", 8) != 0) return false;

    // In Fujifilm RAF, embedded JPEG offset is at 84 (0x54) and length is at 88 (0x58)
    uint32_t jpgOffset = ReadBigEndianU32(data + 84);
    uint32_t jpgLength = ReadBigEndianU32(data + 88);

    if (IsValidJpeg(data, fileSize, jpgOffset, jpgLength)) {
        outJpegBytes.assign(data + jpgOffset, data + jpgOffset + jpgLength);
        return true;
    }
    return false;
}

// Canon CR3 (ISO Base Media File Format) extractor
bool ExtractFromCanonCr3(const uint8_t* data, size_t fileSize, std::vector<uint8_t>& outJpegBytes) {
    if (fileSize < 64) return false;
    // Check for ftypcrx or similar ISOBMFF signature
    bool isCr3 = false;
    if (memcmp(data + 4, "ftyp", 4) == 0) {
        if (memcmp(data + 8, "crx ", 4) == 0 || memcmp(data + 8, "isom", 4) == 0) {
            isCr3 = true;
        }
    }
    if (!isCr3) return false;

    // Scan for 'PRVW' box within first 8MB
    size_t limit = (std::min)(fileSize, static_cast<size_t>(8 * 1024 * 1024));
    for (size_t i = 0; i + 32 < limit; ++i) {
        if (memcmp(data + i, "PRVW", 4) == 0 || memcmp(data + i, "prvw", 4) == 0) {
            // Found PRVW box! Inside PRVW, look for JPEG SOI (\xFF\xD8\xFF)
            for (size_t j = i + 4; j < i + 64 && j + 4 < limit; ++j) {
                if (data[j] == 0xFF && data[j + 1] == 0xD8 && data[j + 2] == 0xFF) {
                    size_t eoi = FindJpegEOI(data, j, limit);
                    if (eoi > j && (eoi - j) >= 1024) {
                        outJpegBytes.assign(data + j, data + eoi);
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

// Fallback scanner for largest embedded JPEG
bool ScanForLargestJpeg(const uint8_t* data, size_t fileSize, std::vector<uint8_t>& outJpegBytes) {
    size_t limit = (std::min)(fileSize, static_cast<size_t>(16 * 1024 * 1024));
    JpegCandidate best = {};

    for (size_t i = 0; i + 4 < limit; i += 2) {
        if (data[i] == 0xFF && data[i + 1] == 0xD8 && data[i + 2] == 0xFF) {
            uint8_t marker = data[i + 3];
            // Valid JPEG markers after SOI: APP0..APP15, DQT, SOF0..SOF2
            if (marker == 0xE0 || marker == 0xE1 || marker == 0xDB ||
                marker == 0xC0 || marker == 0xC2 || marker == 0xEE) {
                size_t eoi = FindJpegEOI(data, i, limit);
                if (eoi > i) {
                    uint32_t len = static_cast<uint32_t>(eoi - i);
                    if (len > best.length && len >= 16384) { // At least 16KB
                        best.offset = static_cast<uint32_t>(i);
                        best.length = len;
                    }
                }
            }
        }
    }

    if (best.length >= 16384) {
        outJpegBytes.assign(data + best.offset, data + best.offset + best.length);
        return true;
    }
    return false;
}

} // anonymous namespace

bool RawThumbnailExtractor::IsRawExtension(const std::wstring& ext) {
    static const std::unordered_set<std::wstring> s_rawExts = {
        L".cr2", L".cr3", L".crw",
        L".nef", L".nrw",
        L".arw", L".srf", L".sr2",
        L".dng",
        L".raf",
        L".rw2", L".raw", L".rwl",
        L".orf",
        L".pef", L".ptx",
        L".srw",
        L".mrw",
        L".3fr", L".fff",
        L".mef", L".mos"
    };
    return s_rawExts.count(ext) > 0;
}

bool RawThumbnailExtractor::ExtractFromMemory(const uint8_t* data, size_t size, std::vector<uint8_t>& outJpegBytes) {
    if (!data || size < 64) return false;

    // 1. Fujifilm RAF
    if (ExtractFromFujifilm(data, size, outJpegBytes)) {
        return true;
    }

    // 2. Canon CR3
    if (ExtractFromCanonCr3(data, size, outJpegBytes)) {
        return true;
    }

    // 3. TIFF / Exif based RAW (Canon CR2, Nikon NEF, Sony ARW, Adobe DNG, Olympus ORF, Pentax PEF, Panasonic RW2)
    bool isLE = false;
    if (data[0] == 'I' && data[1] == 'I') {
        isLE = true;
    } else if (data[0] == 'M' && data[1] == 'M') {
        isLE = false;
    } else {
        // Try fallback JPEG scan if not standard TIFF header
        return ScanForLargestJpeg(data, size, outJpegBytes);
    }

    uint16_t magic = ReadU16(data + 2, isLE);
    // Standard TIFF magic is 42 (0x002A). Panasonic RW2 is 85 (0x0055). Olympus ORF is 0x4F52 / 0x5352.
    if (magic != 42 && magic != 85 && magic != 0x4F52 && magic != 0x5352) {
        // Non-standard magic, still try reading first IFD or fallback
    }

    uint32_t firstIfdOffset = ReadU32(data + 4, isLE);
    if (firstIfdOffset == 0 || firstIfdOffset >= size) {
        return ScanForLargestJpeg(data, size, outJpegBytes);
    }

    std::vector<JpegCandidate> candidates;
    std::vector<uint32_t> subIfdsToVisit;
    std::unordered_set<uint32_t> visited;

    uint32_t currentIfd = firstIfdOffset;
    int ifdCount = 0;

    // Traverse main IFD chain (IFD0 -> IFD1 -> IFD2)
    while (currentIfd > 0 && currentIfd < size && ifdCount < 32) {
        uint32_t nextIfd = 0;
        ParseIFD(data, size, currentIfd, isLE, candidates, subIfdsToVisit, nextIfd, visited);
        currentIfd = nextIfd;
        ifdCount++;
    }

    // Traverse all SubIFDs (crucial for DNG, NEF, ARW preview images)
    for (size_t s = 0; s < subIfdsToVisit.size() && s < 32; ++s) {
        uint32_t subOffset = subIfdsToVisit[s];
        uint32_t dummyNext = 0;
        ParseIFD(data, size, subOffset, isLE, candidates, subIfdsToVisit, dummyNext, visited);
    }

    // Pick candidate with largest length (gives best quality preview)
    const JpegCandidate* bestCandidate = nullptr;
    for (const auto& cand : candidates) {
        if (!bestCandidate || cand.length > bestCandidate->length) {
            bestCandidate = &cand;
        }
    }

    if (bestCandidate && bestCandidate->length >= 1024) {
        outJpegBytes.assign(data + bestCandidate->offset, data + bestCandidate->offset + bestCandidate->length);
        return true;
    }

    // 4. Last resort: scan buffer for embedded JPEG
    return ScanForLargestJpeg(data, size, outJpegBytes);
}

#include "Utils.h"

bool RawThumbnailExtractor::Extract(const std::wstring& filePath, std::vector<uint8_t>& outJpegBytes) {
    HANDLE hFile = CreateFileW(
        filePath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        Utils::Log(L"[RawExtractor] Failed to open file '%s': error %lu", filePath.c_str(), GetLastError());
        return false;
    }

    LARGE_INTEGER liSize = {};
    if (!GetFileSizeEx(hFile, &liSize) || liSize.QuadPart < 64) {
        CloseHandle(hFile);
        return false;
    }

    // Try memory mapping first (fastest for local and supported network files)
    HANDLE hMap = CreateFileMappingW(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
    const void* pMem = nullptr;
    if (hMap) {
        pMem = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
    }

    bool success = false;
    if (pMem) {
        success = ExtractFromMemory(
            reinterpret_cast<const uint8_t*>(pMem),
            static_cast<size_t>(liSize.QuadPart),
            outJpegBytes);
        UnmapViewOfFile(pMem);
    } else {
        // Fallback: Read first 16MB for network shares or when memory mapping fails
        DWORD toRead = static_cast<DWORD>((std::min)(liSize.QuadPart, static_cast<LONGLONG>(16 * 1024 * 1024)));
        std::vector<uint8_t> buffer(toRead);
        DWORD bytesRead = 0;
        SetFilePointer(hFile, 0, nullptr, FILE_BEGIN);
        if (ReadFile(hFile, buffer.data(), toRead, &bytesRead, nullptr) && bytesRead > 64) {
            success = ExtractFromMemory(buffer.data(), bytesRead, outJpegBytes);
        }
    }

    if (hMap) CloseHandle(hMap);
    CloseHandle(hFile);

    if (success) {
        Utils::Log(L"[RawExtractor] Successfully extracted %zu bytes embedded JPEG from '%s'", outJpegBytes.size(), filePath.c_str());
    } else {
        Utils::Log(L"[RawExtractor] Could not extract embedded JPEG from '%s' (file size: %llu bytes)", filePath.c_str(), liSize.QuadPart);
    }

    return success;
}
