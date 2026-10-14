# QuickView

<p align="center">
  <img src="assets/icon.png" width="128" height="128" alt="QuickView Logo" />
  <br>
  <b>Preview File for Windows</b>
  <br>
</p>

---



## Định dạng được hỗ trợ (Supported Formats)

| Nhóm định dạng | Chi tiết hỗ trợ | Công nghệ |
|---|---|---|
| **Ảnh (Images)** | PNG, JPG, JPEG, WebP, GIF, BMP, ICO, TIFF, PSD | WIC, stb_image |
| **Ảnh RAW máy ảnh** | Sony (.ARW), Canon (.CR2/.CR3), Nikon (.NEF), DNG, v.v. | Bóc tách thẻ Exif Thumbnail nhúng |
| **Video & Âm thanh** | MP4, MKV, MOV, AVI, WMV, WebM, MP3, WAV, FLAC, AAC, M4A | libmpv & Direct2D timeline |
| **Tài liệu & Đồ họa** | PDF, Adobe Illustrator (.ai) | PDFium |
| **Markdown** | .md (Tiêu đề, danh sách, trích dẫn, code block, bảng) | md4c & Direct2D layout |
| **Bảng tính Excel** | .xlsx (Hiển thị bảng tính đa trang sheet tabs) | OpenXLSX |
| **Bản trình chiếu** | .pptx (Trích xuất thumbnail slide độ nét cao) | Zip docProps |
| **Vector & Font chữ** | SVG, TTF, OTF | ID2D1DeviceContext5 SVG DOM, IDWrite |
| **Văn bản & Code** | TXT, C/C++, C#, Python, JS, TS, HTML, CSS, JSON, XML, INI... | Win32 Memory-Mapped File |
| **Tệp nén** | ZIP, JAR, v.v. (Danh sách cây thư mục, dung lượng nén) | miniz |
| **Thư mục & Tệp khác** | Xem thông tin chi tiết, kích thước, ngày tạo/sửa đổi | Windows Shell API |

---

## Cài đặt (Installation)

### Dùng bộ cài đặt Installer (Khuyên dùng)
Tải file cài đặt mới nhất tại thư mục `installer_output/QuickView_Setup_v1.0.3.exe`:
1. Chạy `QuickView_Setup_v1.0.3.exe`.
2. Chọn tùy chọn tạo biểu tượng Desktop và **Khởi động cùng Windows** nếu muốn.
3. Bấm **Install** để hoàn tất trong 3 giây.

---



### Yêu cầu môi trường
- Windows 10/11 64-bit
- Visual Studio 2022 (với C++ Desktop Development)
- CMake 3.20 trở lên

### Các bước biên dịch:
```powershell
# 1. Clone mã nguồn
git clone https://github.com/thaovd/quickview.git
cd quickview

# 2. Tạo thư mục build và cấu hình CMake
cmake -B build -A x64

# 3. Biên dịch bản Release
cmake --build build --config Release --target QuickView
```

### Đóng gói Installer:
Sử dụng [Inno Setup 6](https://jrsoftware.org/isdl.php):
```powershell
iscc installer/QuickView.iss
```
File cài đặt sẽ được tạo tại: `installer_output/QuickView_Setup_v1.0.3.exe`.

---

## Tác giả & Giấy phép (License)
- Tác giả: **Vũ Duy Thảo** ([@thaovd](https://github.com/thaovd))
- Giấy phép: MIT License
