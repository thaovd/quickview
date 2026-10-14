#pragma once

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <string>

namespace MpvImageLoader {
    /// Tải các định dạng ảnh như HEIC, HEIF, AVIF thông qua libmpv (FFmpeg HW/HEVC decoder)
    /// Trả về IWICFormatConverter định dạng 32bppPBGRA tương thích hoàn toàn với Direct2D.
    Microsoft::WRL::ComPtr<IWICFormatConverter> Load(
        const std::wstring& path,
        IWICImagingFactory* wicFactory,
        UINT& outWidth,
        UINT& outHeight);

    /// Kiểm tra phần mở rộng tệp có nên ưu tiên tải qua MpvImageLoader không (heic, heif, avif)
    bool IsMpvImageExtension(const std::wstring& ext);
}
