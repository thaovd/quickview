#pragma once

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <string>

namespace StbImageLoader {
    /// Loads image formats supported by stb_image (PSD, TGA, HDR, PIC, PNM, etc.)
    /// into an IWICFormatConverter compatible with Direct2D.
    /// Handles Unicode file paths.
    Microsoft::WRL::ComPtr<IWICFormatConverter> Load(
        const std::wstring& path,
        IWICImagingFactory* wicFactory,
        UINT& outWidth,
        UINT& outHeight);
}
