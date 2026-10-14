#pragma once

#include <string>
#include <vector>
#include <cstdint>

/// Ultra-lightweight Camera RAW Exif/TIFF thumbnail and preview JPEG extractor.
/// Parses TIFF IFD0, IFD1, SubIFD, and ExifIFD for Tag 0x0201 (JPEGInterchangeFormat)
/// and Tag 0x0202 (JPEGInterchangeFormatLength), as well as Fujifilm RAF and Canon CR3.
/// Memory-mapped file access reads only the header and embedded JPEG without full file decompression.
class RawThumbnailExtractor {
public:
    /// Extract embedded preview/thumbnail JPEG from Camera RAW file on disk.
    /// Returns true and populates outJpegBytes on success.
    static bool Extract(const std::wstring& filePath, std::vector<uint8_t>& outJpegBytes);

    /// Extract embedded preview/thumbnail JPEG from an in-memory buffer.
    static bool ExtractFromMemory(const uint8_t* data, size_t size, std::vector<uint8_t>& outJpegBytes);

    /// Check if the file extension is a known Camera RAW format.
    static bool IsRawExtension(const std::wstring& ext);
};
