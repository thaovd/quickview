#include "StbImageLoader.h"
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

namespace StbImageLoader {

Microsoft::WRL::ComPtr<IWICFormatConverter> Load(
    const std::wstring& path,
    IWICImagingFactory* wicFactory,
    UINT& outWidth,
    UINT& outHeight)
{
    outWidth  = 0;
    outHeight = 0;
    if (!wicFactory) return nullptr;

    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return nullptr;

    LARGE_INTEGER fileSize;
    if (!GetFileSizeEx(hFile, &fileSize) || fileSize.QuadPart == 0) {
        CloseHandle(hFile);
        return nullptr;
    }

    HANDLE hMap = CreateFileMappingW(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!hMap) {
        CloseHandle(hFile);
        return nullptr;
    }

    const unsigned char* pData = static_cast<const unsigned char*>(
        MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0));
    if (!pData) {
        CloseHandle(hMap);
        CloseHandle(hFile);
        return nullptr;
    }

    int w = 0, h = 0, channels = 0;
    // Force 4 channels (RGBA)
    unsigned char* data = stbi_load_from_memory(
        pData, static_cast<int>(fileSize.QuadPart), &w, &h, &channels, 4);

    UnmapViewOfFile(pData);
    CloseHandle(hMap);
    CloseHandle(hFile);

    if (!data || w <= 0 || h <= 0) {
        if (data) stbi_image_free(data);
        return nullptr;
    }

    // Convert RGBA to premultiplied BGRA (GUID_WICPixelFormat32bppPBGRA)
    std::vector<unsigned char> pbgra(static_cast<size_t>(w) * h * 4);
    for (int i = 0; i < w * h; ++i) {
        unsigned char r = data[i * 4 + 0];
        unsigned char g = data[i * 4 + 1];
        unsigned char b = data[i * 4 + 2];
        unsigned char a = data[i * 4 + 3];

        pbgra[i * 4 + 0] = static_cast<unsigned char>((static_cast<int>(b) * a + 127) / 255);
        pbgra[i * 4 + 1] = static_cast<unsigned char>((static_cast<int>(g) * a + 127) / 255);
        pbgra[i * 4 + 2] = static_cast<unsigned char>((static_cast<int>(r) * a + 127) / 255);
        pbgra[i * 4 + 3] = a;
    }
    stbi_image_free(data);

    Microsoft::WRL::ComPtr<IWICBitmap> wicBitmap;
    HRESULT hr = wicFactory->CreateBitmapFromMemory(
        static_cast<UINT>(w),
        static_cast<UINT>(h),
        GUID_WICPixelFormat32bppPBGRA,
        static_cast<UINT>(w * 4),
        static_cast<UINT>(pbgra.size()),
        pbgra.data(),
        &wicBitmap);
    if (FAILED(hr)) return nullptr;

    Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
    hr = wicFactory->CreateFormatConverter(&converter);
    if (FAILED(hr)) return nullptr;

    hr = converter->Initialize(
        wicBitmap.Get(),
        GUID_WICPixelFormat32bppPBGRA,
        WICBitmapDitherTypeNone,
        nullptr, 0.0,
        WICBitmapPaletteTypeMedianCut);
    if (FAILED(hr)) return nullptr;

    outWidth  = static_cast<UINT>(w);
    outHeight = static_cast<UINT>(h);
    return converter;
}

} // namespace StbImageLoader
