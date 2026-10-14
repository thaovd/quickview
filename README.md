# QuickView

<p align="center">
  <img src="assets/icon.png" width="128" height="128" alt="QuickView Logo" />
  <br>
  <b>macOS-style Fast File Preview for Windows</b>
  <br>
  <i>Xem trước tệp tức thì với một phím Cách (Space) — Nhanh, Đẹp, Hiện đại</i>
</p>

---

## Giới thiệu (Overview)

**QuickView** mang trải nghiệm **Quick Look** mượt mà của macOS lên Windows. Chỉ cần nhấn phím **Space** trên bất kỳ tệp hoặc thư mục nào trong Windows Explorer hay Desktop, nội dung tệp sẽ hiển thị tức thì với giao diện kính mờ Acrylic Direct2D sắc nét.

- **Tốc độ tức thì**: Mở tệp ngay lập tức mà không cần khởi động phần mềm chuyên dụng nặng nề.
- **Đóng êm ái**: Hiệu ứng thu nhỏ và mờ dần (macOS Popup Zoom Out) 90ms mượt mà khi đóng bằng `Space` hoặc `Esc`.
- **Thông minh**: Tự động nhận diện khi người dùng đang gõ phím đổi tên tệp hoặc tìm kiếm để không can thiệp phím `Space`.
- **Chuyển tệp liền mạch**: Đang mở QuickView, chỉ cần chọn tệp khác là nội dung lập tức cập nhật không cần tắt mở lại.

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

## Hướng dẫn sử dụng (Usage)

- **Xem nhanh tệp / thư mục**: Chọn tệp trong Windows Explorer hoặc Desktop rồi nhấn phím `Space`.
- **Đóng cửa sổ xem trước**: Nhấn lại phím `Space` hoặc phím `Esc` hoặc bấm nút `✕`.
- **Duyệt tệp kế tiếp**: Khi cửa sổ QuickView đang mở, dùng các phím mũi tên hoặc click chọn tệp khác để xem tiếp ngay lập tức.
- **Khay hệ thống (System Tray)**: Click chuột phải vào biểu tượng QuickView ở góc dưới bên phải màn hình để:
  - Bật / Tắt khởi động cùng Windows.
  - Thoát ứng dụng hoàn toàn.

---

## Hướng dẫn biên dịch (Building from Source)

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
