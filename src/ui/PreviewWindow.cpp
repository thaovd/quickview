#include "PreviewWindow.h"
#include "KeyboardHook.h"
#include "MpvImageLoader.h"
#include "RawThumbnailExtractor.h"
#include "Resource.h"
#include "ShellIntegration.h"
#include <algorithm>
#include <dwmapi.h>
#include <memory>
#include <shellapi.h>
#include <thread>
#include <vector>
#include <windowsx.h>

struct AsyncLoadResult {
  uint64_t generation = 0;
  std::wstring filePath;
  FileType fileType = FileType::Unknown;
  bool success = false;

  // Image / PPT thumbnail bytes
  std::vector<uint8_t> memoryBytes;
  bool isMemory = false;

  // Text
  std::wstring textContent;

  // PowerPoint
  int pptSlideCount = 0;
  bool pptHasThumbnail = false;

  // Markdown pre-parsed blocks
  std::vector<MdBlock> mdBlocks;

  // Excel pre-parsed sheets
  std::vector<ExcelSheetData> excelSheets;

  // Archive pre-parsed data
  ArchiveData archiveData;

  // PDF background loaded doc & rendered page 0
  FPDF_DOCUMENT pdfDoc = nullptr;
  int pdfPageCount = 0;
  double pdfPagePtW = 0.0;
  double pdfPagePtH = 0.0;
  std::vector<uint8_t> pdfPixels;
  int pdfRenderW = 0;
  int pdfRenderH = 0;
  int pdfStride = 0;

  ~AsyncLoadResult() {
    if (pdfDoc) {
      FPDF_CloseDocument(pdfDoc);
      pdfDoc = nullptr;
    }
  }
};

// ─────────────────────────────────────────────────────────
// Preview Handler GUID: {8895b1c6-b41f-4c1c-a562-0d564250836f}
// ─────────────────────────────────────────────────────────
static const GUID IID_IPreviewHandler_GUID = {
    0x8895b1c6,
    0xb41f,
    0x4c1c,
    {0xa5, 0x62, 0x0d, 0x56, 0x42, 0x50, 0x83, 0x6f}};

static const wchar_t *WINDOW_CLASS = L"QuickViewPreviewWindow";
static const wchar_t *HANDLER_CLASS = L"QuickViewHandlerHost";

// ─────────────────────────────────────────────────────────
// Constructor / Destructor
// ─────────────────────────────────────────────────────────

PreviewWindow::PreviewWindow() = default;

PreviewWindow::~PreviewWindow() {
  CleanupPreview();
  RemoveTrayIcon();
  if (m_hwnd)
    DestroyWindow(m_hwnd);
}

// ─────────────────────────────────────────────────────────
// Initialization
// ─────────────────────────────────────────────────────────

bool PreviewWindow::Create(HINSTANCE hInstance) {
  m_hInst = hInstance;

  if (!InitFactories())
    return false;
  if (!CreateTextFormats())
    return false;

  // Register main window class
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.style = CS_HREDRAW | CS_VREDRAW | CS_DROPSHADOW | CS_DBLCLKS;
  wc.lpfnWndProc = WndProc;
  wc.hInstance = hInstance;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hbrBackground = nullptr;
  wc.lpszClassName = WINDOW_CLASS;
  HICON hIconBig = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_QUICKVIEW),
                                     IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR);
  HICON hIconSm = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_QUICKVIEW),
                                    IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);
  if (!hIconBig)
    hIconBig = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_QUICKVIEW));
  if (!hIconSm)
    hIconSm = hIconBig;
  wc.hIcon = hIconBig;
  wc.hIconSm = hIconSm;
  if (!RegisterClassExW(&wc))
    return false;

  // Register handler host class (for IPreviewHandler child window)
  WNDCLASSEXW hc = {};
  hc.cbSize = sizeof(hc);
  hc.style = CS_HREDRAW | CS_VREDRAW;
  hc.lpfnWndProc = DefWindowProcW;
  hc.hInstance = hInstance;
  hc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  hc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
  hc.lpszClassName = HANDLER_CLASS;
  RegisterClassExW(&hc);

  // Create hidden popup window with WS_EX_NOACTIVATE (never steals focus from
  // Explorer)
  m_hwnd = CreateWindowExW(
      WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_NOACTIVATE,
      WINDOW_CLASS, L"QuickView", WS_POPUP | WS_CLIPCHILDREN, 0, 0,
      PREVIEW_MIN_WIDTH, PREVIEW_MIN_HEIGHT, nullptr, nullptr, hInstance, this);

  if (!m_hwnd)
    return false;

  if (hIconBig)
    SendMessageW(m_hwnd, WM_SETICON, ICON_BIG,
                 reinterpret_cast<LPARAM>(hIconBig));
  if (hIconSm)
    SendMessageW(m_hwnd, WM_SETICON, ICON_SMALL,
                 reinterpret_cast<LPARAM>(hIconSm));

  // Set initial 0 opacity for layered window (fades in on show)
  SetLayeredWindowAttributes(m_hwnd, 0, 0, LWA_ALPHA);

  // Enable rounded corners on Windows 11
  Utils::EnableRoundedCorners(m_hwnd);

  // Initialize video preview module
  m_videoPreview.Init(m_hwnd, hInstance);

  // Add system tray icon
  AddTrayIcon();

  return true;
}

bool PreviewWindow::InitFactories() {
  HRESULT hr;

  hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                         m_d2dFactory.GetAddressOf());
  if (FAILED(hr))
    return false;

  hr = DWriteCreateFactory(
      DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
      reinterpret_cast<IUnknown **>(m_dwriteFactory.GetAddressOf()));
  if (FAILED(hr))
    return false;

  hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                        IID_PPV_ARGS(&m_wicFactory));
  if (FAILED(hr))
    return false;

  return true;
}

bool PreviewWindow::CreateRenderTarget() {
  if (m_rt)
    return true;

  RECT rc;
  GetClientRect(m_hwnd, &rc);
  D2D1_SIZE_U size = D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top);

  HRESULT hr = m_d2dFactory->CreateHwndRenderTarget(
      D2D1::RenderTargetProperties(
          D2D1_RENDER_TARGET_TYPE_DEFAULT,
          D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                            D2D1_ALPHA_MODE_PREMULTIPLIED)),
      D2D1::HwndRenderTargetProperties(m_hwnd, size), &m_rt);
  if (FAILED(hr))
    return false;

  m_rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
  m_rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);

  // Query ID2D1DeviceContext5 for native SVG support
  m_rt.As(&m_dc5);

  // Create reusable brush
  m_rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1), &m_brush);

  // Create smooth round-cap stroke style for loading spinner
  if (!m_spinnerStrokeStyle && m_d2dFactory) {
    D2D1_STROKE_STYLE_PROPERTIES strokeProps = D2D1::StrokeStyleProperties(
        D2D1_CAP_STYLE_ROUND,
        D2D1_CAP_STYLE_ROUND,
        D2D1_CAP_STYLE_ROUND,
        D2D1_LINE_JOIN_ROUND);
    m_d2dFactory->CreateStrokeStyle(strokeProps, nullptr, 0, &m_spinnerStrokeStyle);
  }

  return true;
}

void PreviewWindow::EnsureAppLogoLoaded() {
  if (m_appLogoBitmap || !m_rt || !m_wicFactory)
    return;

  auto createFromConverter = [&](IWICFormatConverter *conv) {
    if (conv) {
      m_rt->CreateBitmapFromWicBitmap(conv, &m_appLogoBitmap);
    }
  };

  // 1. Try loading from embedded resource IDB_APP_LOGO
  HRSRC hRes =
      FindResourceW(m_hInst, MAKEINTRESOURCEW(IDB_APP_LOGO), RT_RCDATA);
  if (hRes) {
    HGLOBAL hMem = LoadResource(m_hInst, hRes);
    DWORD size = SizeofResource(m_hInst, hRes);
    if (hMem && size > 0) {
      void *pData = LockResource(hMem);
      Microsoft::WRL::ComPtr<IWICStream> stream;
      if (SUCCEEDED(m_wicFactory->CreateStream(&stream))) {
        if (SUCCEEDED(stream->InitializeFromMemory(
                reinterpret_cast<WICInProcPointer>(pData), size))) {
          Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
          if (SUCCEEDED(m_wicFactory->CreateDecoderFromStream(
                  stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad,
                  &decoder))) {
            Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
            if (SUCCEEDED(decoder->GetFrame(0, &frame))) {
              Microsoft::WRL::ComPtr<IWICFormatConverter> conv;
              if (SUCCEEDED(m_wicFactory->CreateFormatConverter(&conv))) {
                if (SUCCEEDED(conv->Initialize(
                        frame.Get(), GUID_WICPixelFormat32bppPBGRA,
                        WICBitmapDitherTypeNone, nullptr, 0.0,
                        WICBitmapPaletteTypeMedianCut))) {
                  createFromConverter(conv.Get());
                  if (m_appLogoBitmap)
                    return;
                }
              }
            }
          }
        }
      }
    }
  }

  // 2. Fallback to file path
  const wchar_t *paths[] = {L"assets/icon.png", L"src/resources/assets/icon.png",
                            L"src/assets/icon.png",
                            L"C:\\Users\\Vuthao\\Desktop\\icon.png"};

  for (const auto *p : paths) {
    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    if (SUCCEEDED(m_wicFactory->CreateDecoderFromFilename(
            p, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad,
            &decoder))) {
      Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
      if (SUCCEEDED(decoder->GetFrame(0, &frame))) {
        Microsoft::WRL::ComPtr<IWICFormatConverter> conv;
        if (SUCCEEDED(m_wicFactory->CreateFormatConverter(&conv))) {
          if (SUCCEEDED(conv->Initialize(frame.Get(),
                                         GUID_WICPixelFormat32bppPBGRA,
                                         WICBitmapDitherTypeNone, nullptr, 0.0,
                                         WICBitmapPaletteTypeMedianCut))) {
            createFromConverter(conv.Get());
            if (m_appLogoBitmap)
              return;
          }
        }
      }
    }
  }
}

void PreviewWindow::DiscardRenderTarget() {
  m_appLogoBitmap.Reset();
  m_brush.Reset();
  m_dc5.Reset();
  m_rt.Reset();
}

bool PreviewWindow::CreateTextFormats() {
  HRESULT hr;

  hr = m_dwriteFactory->CreateTextFormat(
      L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
      DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"en-US",
      &m_titleFont);
  if (FAILED(hr))
    return false;
  m_titleFont->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
  m_titleFont->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
  {
    DWRITE_TRIMMING trim = { DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
    ComPtr<IDWriteInlineObject> ellipsis;
    if (SUCCEEDED(m_dwriteFactory->CreateEllipsisTrimmingSign(m_titleFont.Get(), &ellipsis))) {
      m_titleFont->SetTrimming(&trim, ellipsis.Get());
    }
  }

  hr = m_dwriteFactory->CreateTextFormat(
      L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
      DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"en-US", &m_bodyFont);
  if (FAILED(hr))
    return false;

  hr = m_dwriteFactory->CreateTextFormat(
      L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
      DWRITE_FONT_STRETCH_NORMAL, 11.0f, L"en-US", &m_smallFont);
  if (FAILED(hr))
    return false;
  m_smallFont->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

  hr = m_dwriteFactory->CreateTextFormat(
      L"Cascadia Mono, Consolas, Courier New", nullptr,
      DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
      DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"en-US", &m_monoFont);
  if (FAILED(hr))
    return false;
  m_monoFont->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

  hr = m_dwriteFactory->CreateTextFormat(
      L"Cascadia Mono, Consolas", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
      DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 11.0f, L"en-US",
      &m_lineNumFont);
  if (SUCCEEDED(hr) && m_lineNumFont) {
    m_lineNumFont->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    m_lineNumFont->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
  }

  return true;
}

// ─────────────────────────────────────────────────────────
// Window Procedure
// ─────────────────────────────────────────────────────────

LRESULT CALLBACK PreviewWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp,
                                        LPARAM lp) {
  PreviewWindow *self = nullptr;

  if (msg == WM_NCCREATE) {
    auto cs = reinterpret_cast<CREATESTRUCT *>(lp);
    self = static_cast<PreviewWindow *>(cs->lpCreateParams);
    if (self) {
      self->m_hwnd = hwnd;
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
  } else {
    self = reinterpret_cast<PreviewWindow *>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  }

  if (self) {
    return self->HandleMessage(hwnd, msg, wp, lp);
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT PreviewWindow::HandleMessage(HWND hwnd, UINT msg, WPARAM wp,
                                     LPARAM lp) {
  switch (msg) {

  case WM_CREATE:
    return 0;

  case WM_PAINT: {
    PAINTSTRUCT ps;
    BeginPaint(m_hwnd, &ps);
    Render();
    EndPaint(m_hwnd, &ps);
    return 0;
  }

  case WM_SIZE: {
    m_winW = LOWORD(lp);
    m_winH = HIWORD(lp);
    if (m_rt) {
      m_rt->Resize(D2D1::SizeU(m_winW, m_winH));
    }
    UpdateLayout();
    if (m_currentType == FileType::Image ||
        (m_currentType == FileType::PowerPoint && m_pptHasThumbnail)) {
      m_imagePreview.ResetView(GetContentRect());
    } else if (m_currentType == FileType::PDF ||
               m_currentType == FileType::Illustrator) {
      m_pdfPreview.ResetView(GetContentRect());
    }
    InvalidateRect(m_hwnd, nullptr, FALSE);
    return 0;
  }

  case WM_KEYDOWN:
    if (wp == VK_ESCAPE) {
      if (m_isFullscreen) {
        ExitFullScreen();
        return 0;
      }
      HidePreview();
      return 0;
    }
    if (wp == VK_SPACE) {
      HidePreview();
      return 0;
    }

    if (wp == VK_F11) {
      if (SupportsFullScreen()) {
        ToggleFullScreen();
        return 0;
      }
    }

    if (m_currentType == FileType::PDF ||
        m_currentType == FileType::Illustrator) {
      if (wp == VK_LEFT || wp == VK_UP || wp == VK_PRIOR) {
        if (m_pdfPreview.PrevPage(m_rt.Get())) {
          m_pdfPreview.ResetView(GetContentRect());
          InvalidateRect(m_hwnd, nullptr, FALSE);
        }
        return 0;
      }
      if (wp == VK_RIGHT || wp == VK_DOWN || wp == VK_NEXT) {
        if (m_pdfPreview.NextPage(m_rt.Get())) {
          m_pdfPreview.ResetView(GetContentRect());
          InvalidateRect(m_hwnd, nullptr, FALSE);
        }
        return 0;
      }
    }

    if (wp == '0') {
      if (m_currentType == FileType::Image ||
          (m_currentType == FileType::PowerPoint && m_pptHasThumbnail)) {
        m_imagePreview.ResetView(GetContentRect());
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return 0;
      }
      if (m_currentType == FileType::PDF ||
          m_currentType == FileType::Illustrator) {
        m_pdfPreview.ResetView(GetContentRect());
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return 0;
      }
      if (m_currentType == FileType::Svg) {
        m_svgPreview.ResetView();
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return 0;
      }
    }

    if (m_currentType == FileType::Video || m_currentType == FileType::Audio) {
      if (wp == VK_LEFT) {
        m_videoPreview.Seek(m_videoPreview.GetPosition() - 5.0);
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return 0;
      }
      if (wp == VK_RIGHT) {
        m_videoPreview.Seek(m_videoPreview.GetPosition() + 5.0);
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return 0;
      }
    }
    break;

  case WM_MOUSEWHEEL: {
    int delta = GET_WHEEL_DELTA_WPARAM(wp);
    POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    ScreenToClient(m_hwnd, &pt);

    if (m_currentType == FileType::Image ||
        (m_currentType == FileType::PowerPoint && m_pptHasThumbnail)) {
      m_imagePreview.OnMouseWheel(delta, pt.x, pt.y, GetContentRect());
      InvalidateRect(m_hwnd, nullptr, FALSE);
    } else if (m_currentType == FileType::PDF ||
               m_currentType == FileType::Illustrator) {
      m_pdfPreview.OnMouseWheel(delta, pt.x, pt.y, GetContentRect());
      InvalidateRect(m_hwnd, nullptr, FALSE);
    } else if (m_currentType == FileType::Svg) {
      m_svgPreview.OnMouseWheel(delta, static_cast<float>(pt.x),
                                static_cast<float>(pt.y));
      InvalidateRect(m_hwnd, nullptr, FALSE);
    } else if (m_currentType == FileType::Archive) {
      m_archivePreview.OnMouseWheel(delta);
      InvalidateRect(m_hwnd, nullptr, FALSE);
    } else if (m_currentType == FileType::Font) {
      m_fontPreview.OnMouseWheel(delta);
      InvalidateRect(m_hwnd, nullptr, FALSE);
    } else if (m_currentType == FileType::Excel) {
      m_excelPreview.OnMouseWheel(delta, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
      InvalidateRect(m_hwnd, nullptr, FALSE);
    } else if (m_currentType == FileType::Markdown) {
      D2D1_RECT_F cr = GetContentRect();
      m_mdPreview.OnMouseWheel(delta, cr.bottom - cr.top);
      InvalidateRect(m_hwnd, nullptr, FALSE);
    } else if (m_currentType == FileType::Text) {
      m_scrollY -= delta * 0.5f;
      m_scrollY = (std::max)(0.0f, (std::min)(m_scrollY, m_totalTextH));
      InvalidateRect(m_hwnd, nullptr, FALSE);
    }
    return 0;
  }

  case WM_MOUSEACTIVATE:
    SetForegroundWindow(m_hwnd);
    return MA_ACTIVATE;

  case WM_LBUTTONDOWN: {
    if (GetForegroundWindow() != m_hwnd) {
      SetForegroundWindow(m_hwnd);
    }
    int mx = GET_X_LPARAM(lp);
    int my = GET_Y_LPARAM(lp);

    // Close button check
    if (my < TITLEBAR_HEIGHT && mx > m_winW - CLOSE_BTN_SIZE - 8) {
      HidePreview();
      return 0;
    }

    // Title bar drag
    if (my < TITLEBAR_HEIGHT) {
      ReleaseCapture();
      SendMessage(m_hwnd, WM_NCLBUTTONDOWN, HTCAPTION, lp);
      return 0;
    }

    // Video / Audio controls interaction
    if ((m_currentType == FileType::Video ||
         m_currentType == FileType::Audio) &&
        m_videoPreview.IsLoaded() && my >= m_winH - TOOLBAR_HEIGHT) {
      // Play / Pause button
      if (mx >= m_videoPlayRect.left && mx <= m_videoPlayRect.right &&
          my >= m_videoPlayRect.top && my <= m_videoPlayRect.bottom) {
        m_videoPreview.TogglePause();
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return 0;
      }
      // Mute / Unmute button
      if (mx >= m_videoMuteRect.left && mx <= m_videoMuteRect.right &&
          my >= m_videoMuteRect.top && my <= m_videoMuteRect.bottom) {
        m_videoPreview.ToggleMute();
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return 0;
      }
      // Seek bar
      if (mx >= m_videoSeekRect.left && mx <= m_videoSeekRect.right &&
          my >= m_videoSeekRect.top && my <= m_videoSeekRect.bottom) {
        m_videoSeeking = true;
        SetCapture(m_hwnd);
        float pct = (mx - m_videoSeekRect.left) /
                    (m_videoSeekRect.right - m_videoSeekRect.left);
        m_videoPreview.SeekPercent(pct);
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return 0;
      }
    }

    // Audio preview: click anywhere on vinyl to play/pause
    if (m_currentType == FileType::Audio && my >= TITLEBAR_HEIGHT &&
        my < m_winH - TOOLBAR_HEIGHT) {
      m_videoPreview.TogglePause();
      InvalidateRect(m_hwnd, nullptr, FALSE);
      return 0;
    }

    // Svg pan
    if (m_currentType == FileType::Svg) {
      m_svgPreview.OnLButtonDown(static_cast<float>(mx),
                                 static_cast<float>(my));
      SetCapture(m_hwnd);
      return 0;
    }

    // Excel tab click
    if (m_currentType == FileType::Excel) {
      m_excelPreview.OnLButtonDown(static_cast<float>(mx),
                                   static_cast<float>(my));
      InvalidateRect(m_hwnd, nullptr, FALSE);
      return 0;
    }

    // PDF / Illustrator interactions (page navigation in toolbar or page pan in
    // content)
    if (m_currentType == FileType::PDF ||
        m_currentType == FileType::Illustrator) {
      if (my >= m_winH - TOOLBAR_HEIGHT) {
        if (mx >= m_pdfPrevRect.left && mx <= m_pdfPrevRect.right &&
            my >= m_pdfPrevRect.top && my <= m_pdfPrevRect.bottom) {
          if (m_pdfPreview.PrevPage(m_rt.Get())) {
            m_pdfPreview.ResetView(GetContentRect());
            InvalidateRect(m_hwnd, nullptr, FALSE);
          }
          return 0;
        }
        if (mx >= m_pdfNextRect.left && mx <= m_pdfNextRect.right &&
            my >= m_pdfNextRect.top && my <= m_pdfNextRect.bottom) {
          if (m_pdfPreview.NextPage(m_rt.Get())) {
            m_pdfPreview.ResetView(GetContentRect());
            InvalidateRect(m_hwnd, nullptr, FALSE);
          }
          return 0;
        }
      } else {
        m_pdfPreview.OnMouseDown(mx, my);
        SetCapture(m_hwnd);
        return 0;
      }
    }

    // Image / PowerPoint thumbnail pan
    if (m_currentType == FileType::Image ||
        (m_currentType == FileType::PowerPoint && m_pptHasThumbnail)) {
      m_imagePreview.OnMouseDown(mx, my);
      SetCapture(m_hwnd);
    }
    return 0;
  }

  case WM_MOUSEMOVE: {
    int mx = GET_X_LPARAM(lp);
    int my = GET_Y_LPARAM(lp);

    // Track close button hover
    bool wasHover = m_closeHover;
    m_closeHover = (my < TITLEBAR_HEIGHT && mx > m_winW - CLOSE_BTN_SIZE - 8);
    if (wasHover != m_closeHover) {
      InvalidateRect(m_hwnd, nullptr, FALSE);
    }

    // Track mouse for WM_MOUSELEAVE
    TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, m_hwnd, 0};
    TrackMouseEvent(&tme);

    // Video / Audio controls hover & seek scrubbing
    if ((m_currentType == FileType::Video ||
         m_currentType == FileType::Audio) &&
        m_videoPreview.IsLoaded()) {
      bool prevPlayHover = m_videoPlayHover;
      bool prevMuteHover = m_videoMuteHover;
      bool prevSeekHover = m_videoSeekHover;

      if (m_videoSeeking) {
        float pct = (mx - m_videoSeekRect.left) /
                    (m_videoSeekRect.right - m_videoSeekRect.left);
        m_videoPreview.SeekPercent(pct);
        InvalidateRect(m_hwnd, nullptr, FALSE);
      } else if (my >= m_winH - TOOLBAR_HEIGHT) {
        m_videoPlayHover =
            (mx >= m_videoPlayRect.left && mx <= m_videoPlayRect.right &&
             my >= m_videoPlayRect.top && my <= m_videoPlayRect.bottom);
        m_videoMuteHover =
            (mx >= m_videoMuteRect.left && mx <= m_videoMuteRect.right &&
             my >= m_videoMuteRect.top && my <= m_videoMuteRect.bottom);
        m_videoSeekHover =
            (mx >= m_videoSeekRect.left && mx <= m_videoSeekRect.right &&
             my >= m_videoSeekRect.top && my <= m_videoSeekRect.bottom);
      } else {
        m_videoPlayHover = false;
        m_videoMuteHover = false;
        m_videoSeekHover = false;
      }

      if (prevPlayHover != m_videoPlayHover ||
          prevMuteHover != m_videoMuteHover ||
          prevSeekHover != m_videoSeekHover) {
        InvalidateRect(m_hwnd, nullptr, FALSE);
      }
    }

    // Svg pan
    if (m_currentType == FileType::Svg) {
      m_svgPreview.OnMouseMove(static_cast<float>(mx), static_cast<float>(my));
      InvalidateRect(m_hwnd, nullptr, FALSE);
    }

    // Archive hover
    if (m_currentType == FileType::Archive) {
      m_archivePreview.OnMouseMove(static_cast<float>(mx),
                                   static_cast<float>(my));
      InvalidateRect(m_hwnd, nullptr, FALSE);
    }

    // Excel hover
    if (m_currentType == FileType::Excel) {
      m_excelPreview.OnMouseMove(static_cast<float>(mx),
                                 static_cast<float>(my));
      InvalidateRect(m_hwnd, nullptr, FALSE);
    }

    // PDF & Illustrator controls hover & pan
    if (m_currentType == FileType::PDF ||
        m_currentType == FileType::Illustrator) {
      m_pdfPreview.OnMouseMove(mx, my, m_hwnd);
      bool prevPrevHover = m_pdfPrevHover;
      bool prevNextHover = m_pdfNextHover;
      if (my >= m_winH - TOOLBAR_HEIGHT) {
        m_pdfPrevHover =
            (mx >= m_pdfPrevRect.left && mx <= m_pdfPrevRect.right &&
             my >= m_pdfPrevRect.top && my <= m_pdfPrevRect.bottom);
        m_pdfNextHover =
            (mx >= m_pdfNextRect.left && mx <= m_pdfNextRect.right &&
             my >= m_pdfNextRect.top && my <= m_pdfNextRect.bottom);
      } else {
        m_pdfPrevHover = false;
        m_pdfNextHover = false;
      }
      if (prevPrevHover != m_pdfPrevHover || prevNextHover != m_pdfNextHover) {
        InvalidateRect(m_hwnd, nullptr, FALSE);
      }
    }

    if (m_currentType == FileType::Image ||
        (m_currentType == FileType::PowerPoint && m_pptHasThumbnail)) {
      m_imagePreview.OnMouseMove(mx, my, m_hwnd);
    }
    return 0;
  }

  case WM_LBUTTONUP:
    if (m_videoSeeking) {
      m_videoSeeking = false;
      InvalidateRect(m_hwnd, nullptr, FALSE);
    }
    if (m_currentType == FileType::Svg) {
      m_svgPreview.OnLButtonUp();
    }
    if (m_currentType == FileType::PDF ||
        m_currentType == FileType::Illustrator) {
      m_pdfPreview.OnMouseUp();
    }
    if (m_currentType == FileType::Image ||
        (m_currentType == FileType::PowerPoint && m_pptHasThumbnail)) {
      m_imagePreview.OnMouseUp();
    }
    if (GetCapture() == m_hwnd) {
      ReleaseCapture();
    }
    return 0;

  case WM_CAPTURECHANGED:
    if (reinterpret_cast<HWND>(lp) != m_hwnd) {
      if (m_videoSeeking) {
        m_videoSeeking = false;
        InvalidateRect(m_hwnd, nullptr, FALSE);
      }
      if (m_currentType == FileType::Svg) {
        m_svgPreview.OnLButtonUp();
      }
      if (m_currentType == FileType::PDF ||
          m_currentType == FileType::Illustrator) {
        m_pdfPreview.OnMouseUp();
      }
      if (m_currentType == FileType::Image ||
          (m_currentType == FileType::PowerPoint && m_pptHasThumbnail)) {
        m_imagePreview.OnMouseUp();
      }
    }
    return 0;

  case WM_MOUSELEAVE:
    if (m_closeHover || m_videoPlayHover || m_videoMuteHover ||
        m_videoSeekHover || m_pdfPrevHover || m_pdfNextHover) {
      m_closeHover = false;
      m_videoPlayHover = false;
      m_videoMuteHover = false;
      m_videoSeekHover = false;
      m_pdfPrevHover = false;
      m_pdfNextHover = false;
      InvalidateRect(m_hwnd, nullptr, FALSE);
    }
    return 0;

  case WM_RBUTTONUP:
  case WM_CONTEXTMENU:
    if (m_visible && !m_currentFile.empty()) {
      // Nếu là Video hoặc Audio và đang phát, tự động tạm dừng lại trước khi hiện context menu
      if ((m_currentType == FileType::Video || m_currentType == FileType::Audio) && m_videoPreview.IsLoaded()) {
        if (m_videoPreview.IsPlaying()) {
          m_videoPreview.Pause();
          InvalidateRect(m_hwnd, nullptr, FALSE);
        }
      }
      ShowContextMenu();
      return 0;
    }
    break;

  case WM_LBUTTONDBLCLK: {
    int mx = GET_X_LPARAM(lp);
    int my = GET_Y_LPARAM(lp);
    // Bỏ qua nếu nhấp đúp vào nút đóng
    if (my < TITLEBAR_HEIGHT && mx > m_winW - CLOSE_BTN_SIZE - 8) {
      return 0;
    }
    // Bỏ qua nếu nhấp đúp vào thanh toolbar phía dưới
    if (my >= m_winH - TOOLBAR_HEIGHT) {
      return 0;
    }

    if (m_currentType == FileType::Video && m_videoPreview.IsLoaded()) {
      // Hồi phục trạng thái play/pause (vì cú nhấp đầu tiên của double-click đã vô tình toggle)
      m_videoPreview.TogglePause();
    }

    if (SupportsFullScreen()) {
      ToggleFullScreen();
    }
    return 0;
  }

  case WM_NCLBUTTONDBLCLK:
    if (SupportsFullScreen()) {
      ToggleFullScreen();
    }
    return 0;

  case WM_APP_QUICKVIEW_TOGGLE:
    Utils::Log(L"[PreviewWindow] WM_APP_QUICKVIEW_TOGGLE received. "
               L"m_visible=%d, wp=0x%p",
               m_visible, (void *)wp);
    {
      HWND targetShell = nullptr;
      if (wp != 0 && IsWindow((HWND)wp)) {
        targetShell = (HWND)wp;
        m_lastActiveShellWnd = targetShell;
      } else {
        HWND fg = GetForegroundWindow();
        ShellWindowType fgType =
            ShellIntegration::GetForegroundWindowType(m_hwnd);
        if (fgType == ShellWindowType::Explorer ||
            fgType == ShellWindowType::Desktop) {
          targetShell = fg;
          m_lastActiveShellWnd = fg;
        } else if (m_lastActiveShellWnd && IsWindow(m_lastActiveShellWnd)) {
          targetShell = m_lastActiveShellWnd;
        } else {
          HWND exp = FindWindowW(L"CabinetWClass", nullptr);
          if (exp) {
            targetShell = exp;
            m_lastActiveShellWnd = exp;
          }
        }
      }

      if (targetShell) {
        HWND hRenameEdit = nullptr;
        if (ShellIntegration::IsExplorerRenamingFile(targetShell, &hRenameEdit) &&
            ShellIntegration::ShouldProtectRenameOnSpace(
                hRenameEdit, KeyboardHook::IsUserEditingRename())) {
          ShellIntegration::CancelExplorerRename(targetShell, hRenameEdit);
        }
      }

      std::wstring file = ShellIntegration::GetSelectedFilePath(targetShell);
      Utils::Log(L"[PreviewWindow] Selected file: '%s'", file.c_str());

      if (m_visible) {
        // Nếu đang mở xem trước:
        // Nếu người dùng đang chọn 1 file KHÁC -> chuyển ngay sang file mới!
        if (!file.empty() && file != m_currentFile) {
          Utils::Log(L"[PreviewWindow] Switching directly to newly selected "
                     L"file: '%s'",
                     file.c_str());
          SwitchFile(file);
        } else {
          // Ấn phím Space từ Explorer (cùng file hiện tại hoặc bỏ chọn): Đóng preview!
          Utils::Log(L"[PreviewWindow] Space in Explorer while visible -> closing preview");
          HidePreview();
        }
      } else {
        // Cửa sổ đang đóng -> mở file được chọn
        if (!file.empty()) {
          ShowPreview(file);
        } else {
          Utils::Log(
              L"[PreviewWindow] No file selected or failed to get file!");
        }
      }
    }
    return 0;

  case WM_APP_QUICKVIEW_CLOSE:
    if (m_visible) {
      HidePreview();
    }
    return 0;

  case WM_APP_QUICKVIEW_NAVIGATE:
    if (m_visible) {
      HWND targetShell =
          (wp != 0 && IsWindow((HWND)wp)) ? (HWND)wp
          : (m_lastActiveShellWnd && IsWindow(m_lastActiveShellWnd))
              ? m_lastActiveShellWnd
              : nullptr;
      std::wstring file = ShellIntegration::GetSelectedFilePath(targetShell);
      if (!file.empty() && file != m_currentFile) {
        SwitchFile(file);
      }
    }
    return 0;

  case WM_APP_VIDEO_CLICK:
    SetForegroundWindow(m_hwnd);
    if (m_currentType == FileType::Video && m_videoPreview.IsLoaded()) {
      m_videoPreview.TogglePause();
      InvalidateRect(m_hwnd, nullptr, FALSE);
    }
    return 0;

  case WM_APP_VIDEO_RCLICK:
    if ((m_currentType == FileType::Video || m_currentType == FileType::Audio) && m_videoPreview.IsLoaded()) {
      if (m_videoPreview.IsPlaying()) {
        m_videoPreview.Pause();
        InvalidateRect(m_hwnd, nullptr, FALSE);
      }
      ShowContextMenu();
    }
    return 0;

  case WM_COPYDATA: {
    auto *pcds = reinterpret_cast<COPYDATASTRUCT *>(lp);
    if (pcds && pcds->lpData) {
      std::wstring path = reinterpret_cast<const wchar_t *>(pcds->lpData);
      if (!path.empty()) {
        ShowPreview(path);
        return TRUE;
      }
    }
    return FALSE;
  }

  case WM_APP_FILE_LOADED: {
    std::unique_ptr<AsyncLoadResult> res(
        reinterpret_cast<AsyncLoadResult *>(lp));
    if (res && res->generation == m_loadGeneration) {
      OnFileLoadedAsync(res.get());
    }
    return 0;
  }

  case WM_ACTIVATE:
    // Tuyệt đối không đóng preview khi click ra ngoài ứng dụng khác (chat app, trình duyệt, v.v.)
    return 0;

  case WM_NCHITTEST: {
    POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    ScreenToClient(m_hwnd, &pt);
    if (pt.y < TITLEBAR_HEIGHT) {
      if (pt.x > m_winW - CLOSE_BTN_SIZE - 8)
        return HTCLIENT;
      return HTCAPTION;
    }
    return HTCLIENT;
  }

  case WM_TIMER:
    if (wp == TIMER_FADE) {
      auto now = std::chrono::steady_clock::now();
      float elapsedMs = static_cast<float>(
          std::chrono::duration_cast<std::chrono::milliseconds>(now - m_fadeStartTime)
              .count());

      if (m_fadeState == FadeState::In) {
        constexpr float DURATION_IN = 90.0f; // 90ms fade in
        float t = (std::min)(1.0f, elapsedMs / DURATION_IN);
        // Sine ease-out: sin(t * PI/2)
        float p = sinf(t * 1.5707963f);
        BYTE alpha = static_cast<BYTE>(p * 255.0f);
        m_currentAlpha = alpha;
        SetLayeredWindowAttributes(m_hwnd, 0, alpha, LWA_ALPHA);

        if (t >= 1.0f) {
          KillTimer(m_hwnd, TIMER_FADE);
          m_fadeState = FadeState::None;
          SetLayeredWindowAttributes(m_hwnd, 0, 255, LWA_ALPHA);
          m_currentAlpha = 255;
        }
      } else if (m_fadeState == FadeState::Out) {
        constexpr float DURATION_OUT = 75.0f; // 75ms fade out
        float t = (std::min)(1.0f, elapsedMs / DURATION_OUT);
        // Quadratic ease-in: 1 - t^2
        float p = (std::max)(0.0f, 1.0f - t * t);
        BYTE alpha = static_cast<BYTE>(p * 255.0f);
        m_currentAlpha = alpha;
        SetLayeredWindowAttributes(m_hwnd, 0, alpha, LWA_ALPHA);

        if (t >= 1.0f) {
          KillTimer(m_hwnd, TIMER_FADE);
          m_fadeState = FadeState::None;
          ShowWindow(m_hwnd, SW_HIDE);
          m_visible = false;
          CleanupPreview();
          if (m_lastActiveShellWnd && IsWindow(m_lastActiveShellWnd)) {
            SetForegroundWindow(m_lastActiveShellWnd);
          }
        }
      }
      return 0;
    }
    if (wp == TIMER_LOADING_SPINNER) {
      if (m_isLoading) {
        m_loadingAngle += 30.0f;
        if (m_loadingAngle >= 360.0f)
          m_loadingAngle -= 360.0f;
        RECT contentR = {0, TITLEBAR_HEIGHT, m_winW, m_winH - TOOLBAR_HEIGHT};
        InvalidateRect(m_hwnd, &contentR, FALSE);
      } else {
        KillTimer(m_hwnd, TIMER_LOADING_SPINNER);
      }
      return 0;
    }
    if (wp == TIMER_VIDEO_PROGRESS) {
      if (m_currentType == FileType::Video && m_videoPreview.IsLoaded()) {
        RECT footerRect = {0, m_winH - TOOLBAR_HEIGHT, m_winW, m_winH};
        InvalidateRect(m_hwnd, &footerRect, FALSE);
      }
      return 0;
    }
    if (wp == TIMER_WATCH_SELECTION) {
      if (m_visible) {
        HWND fg = GetForegroundWindow();
        ShellWindowType fgType =
            ShellIntegration::GetForegroundWindowType(m_hwnd);
        HWND targetShell = nullptr;
        if (fgType == ShellWindowType::Explorer ||
            fgType == ShellWindowType::Desktop) {
          targetShell = fg;
          m_lastActiveShellWnd = fg;
        } else if (m_lastActiveShellWnd && IsWindow(m_lastActiveShellWnd)) {
          targetShell = m_lastActiveShellWnd;
        }
        if (targetShell) {
          std::wstring sel = ShellIntegration::GetSelectedFilePath(targetShell);
          if (!sel.empty() && sel != m_currentFile) {
            Utils::Log(L"[PreviewWindow] Selection changed to '%s' while "
                       L"visible -> switching file immediately",
                       sel.c_str());
            SwitchFile(sel);
          } else if (sel.empty() &&
                     (fgType == ShellWindowType::Explorer ||
                      fgType == ShellWindowType::Desktop) &&
                     !ShellIntegration::IsExplorerRenamingFile(targetShell, nullptr)) {
            // Khi đang trong Explorer / Desktop mà click ra vùng trống (không target file nào nữa) -> đóng preview
            Utils::Log(L"[PreviewWindow] Clicked empty space in Explorer/Desktop (no file targeted) -> closing preview");
            HidePreview();
          }
        }
      }
      return 0;
    }
    break;

  case WM_TRAYICON: {
    UINT uMsg = LOWORD(lp);
    if (uMsg == WM_RBUTTONUP || uMsg == WM_CONTEXTMENU || uMsg == NIN_SELECT ||
        uMsg == WM_LBUTTONUP) {
      ShowTrayMenu();
      return 0;
    }
    break;
  }

  case WM_COMMAND:
    if (LOWORD(wp) == IDM_EXIT) {
      Utils::Log(L"[PreviewWindow] Exit clicked from tray menu");
      RemoveTrayIcon();
      DestroyWindow(m_hwnd);
      return 0;
    }
    if (LOWORD(wp) == IDM_START_WITH_WINDOWS) {
      bool isAutoStart = Utils::IsStartWithWindowsEnabled();
      Utils::SetStartWithWindows(!isAutoStart);
      Utils::Log(L"[PreviewWindow] Toggled Start with Windows: %d -> %d",
                 isAutoStart, !isAutoStart);
      return 0;
    }
    if (LOWORD(wp) == IDM_VIDEO_SAVE_IMAGE) {
      SaveVideoSnapshot();
      return 0;
    }
    if (LOWORD(wp) == IDM_COPY_TIMECODE) {
      CopyTimecode();
      return 0;
    }
    if (LOWORD(wp) == IDM_COPY_FILE_NAME) {
      CopyFileName();
      return 0;
    }
    if (LOWORD(wp) == IDM_VIDEO_COPY_PATH) {
      CopyCurrentFilePath();
      return 0;
    }
    if (LOWORD(wp) == IDM_VIDEO_COPY_FOLDER) {
      CopyFolderAddress();
      return 0;
    }
    if (LOWORD(wp) == IDM_VIDEO_FULLSCREEN) {
      if (SupportsFullScreen()) {
        ToggleFullScreen();
      }
      return 0;
    }
    break;

  case WM_CLOSE:
    Utils::Log(L"[PreviewWindow] WM_CLOSE received -> calling HidePreview()");
    HidePreview();
    return 0;

  case WM_DESTROY:
    Utils::Log(L"[PreviewWindow] WM_DESTROY received -> PostQuitMessage(0)");
    RemoveTrayIcon();
    PostQuitMessage(0);
    return 0;
  }

  return DefWindowProcW(hwnd, msg, wp, lp);
}

// ─────────────────────────────────────────────────────────
// Show / Hide
// ─────────────────────────────────────────────────────────

void PreviewWindow::ShowPreview(const std::wstring &filePath) {
  if (filePath.empty())
    return;

  std::wstring cleanPath = filePath;
  size_t first = cleanPath.find_first_not_of(L" \t\r\n\"");
  if (first != std::wstring::npos) {
    size_t last = cleanPath.find_last_not_of(L" \t\r\n\"");
    cleanPath = cleanPath.substr(first, last - first + 1);
  }
  if (cleanPath.empty())
    return;

  Utils::Log(L"[PreviewWindow] ShowPreview called for '%s'", cleanPath.c_str());
  m_lastPreviewedFile = cleanPath;

  // Lưu cửa sổ shell active trước khi hiển thị preview
  HWND fg = GetForegroundWindow();
  if (fg && fg != m_hwnd) {
    m_lastActiveShellWnd = fg;
  }

  // If already showing same file, just hide (toggle behavior)
  if (m_visible && m_currentFile == cleanPath) {
    HidePreview();
    return;
  }

  // If showing different file, cleanup previous
  if (m_visible) {
    CleanupPreview();
  }

  // Thiết lập thông tin file cơ bản NGAY LẬP TỨC
  m_currentFile = cleanPath;
  m_currentType = FileTypeDetector::Detect(cleanPath);
  m_fileInfo = Utils::GetFileInfo(cleanPath);
  m_scrollY = 0.0f;
  m_totalTextH = 0.0f;

  m_isFullscreen = false;
  m_isLoading = true;
  m_loadingAngle = 0.0f;
  SetTimer(m_hwnd, TIMER_LOADING_SPINNER, 33, nullptr);

  // Đảm bảo render target sẵn sàng
  if (!CreateRenderTarget()) {
    Utils::Log(L"[ShowPreview] CreateRenderTarget failed!");
    return;
  }

  // 1. Kích thước panel chuẩn lên đợi trước NGAY LẬP TỨC (<1ms)
  SIZE sz = CalculateWindowSize();
  if (sz.cx < PREVIEW_MIN_WIDTH)
    sz.cx = 760;
  if (sz.cy < PREVIEW_MIN_HEIGHT)
    sz.cy = 540;
  m_winW = sz.cx;
  m_winH = sz.cy;

  // 2. Căn giữa cửa sổ và hiển thị panel NGAY LẬP TỨC với hiệu ứng Fade In (không chờ file load!)
  Utils::CenterWindow(m_hwnd, m_winW, m_winH);

  if (m_rt) {
    m_rt->Resize(D2D1::SizeU(m_winW, m_winH));
  }
  UpdateLayout();

  m_visible = true;
  StartFadeIn();
  SetTimer(m_hwnd, TIMER_WATCH_SELECTION, 60, nullptr);
  InvalidateRect(m_hwnd, nullptr, FALSE);

  // 3. Sau khi panel đã hiển thị lên đợi trước, đưa preview vào qua luồng bất đồng bộ
  uint64_t gen = ++m_loadGeneration;
  StartAsyncLoad(cleanPath, gen);
}

void PreviewWindow::HidePreview() {
  Utils::Log(L"[PreviewWindow] HidePreview called. m_visible=%d", m_visible);
  if (!m_visible)
    return;
  if (m_isFullscreen) {
    ExitFullScreen();
  }
  KillTimer(m_hwnd, TIMER_WATCH_SELECTION);
  KillTimer(m_hwnd, TIMER_LOADING_SPINNER);

  // Bảo vệ tên file: Hủy chế độ đổi tên trong Explorer (nếu có)
  ShellIntegration::CancelExplorerRename(m_lastActiveShellWnd);

  StartFadeOut();
}

void PreviewWindow::SwitchFile(const std::wstring &filePath) {
  if (filePath.empty() || filePath == m_currentFile)
    return;

  std::wstring cleanPath = filePath;
  size_t first = cleanPath.find_first_not_of(L" \t\r\n\"");
  if (first != std::wstring::npos) {
    size_t last = cleanPath.find_last_not_of(L" \t\r\n\"");
    cleanPath = cleanPath.substr(first, last - first + 1);
  }
  if (cleanPath.empty())
    return;

  Utils::Log(L"[PreviewWindow] SwitchFile called for '%s'", cleanPath.c_str());

  // Dọn dẹp nội dung cũ
  CleanupPreview();

  // Thiết lập thông tin file mới NGAY LẬP TỨC
  m_currentFile = cleanPath;
  m_currentType = FileTypeDetector::Detect(cleanPath);
  m_fileInfo = Utils::GetFileInfo(cleanPath);
  m_scrollY = 0.0f;
  m_totalTextH = 0.0f;

  m_isLoading = true;
  m_loadingAngle = 0.0f;
  SetTimer(m_hwnd, TIMER_LOADING_SPINNER, 33, nullptr);

  // 1. Tính kích thước panel chuẩn lên đợi trước NGAY LẬP TỨC (<1ms)
  SIZE sz = CalculateWindowSize();
  if (sz.cx != m_winW || sz.cy != m_winH) {
    m_winW = sz.cx;
    m_winH = sz.cy;
    Utils::CenterWindow(m_hwnd, m_winW, m_winH);
    if (m_rt) {
      m_rt->Resize(D2D1::SizeU(m_winW, m_winH));
    }
  }
  UpdateLayout();
  InvalidateRect(m_hwnd, nullptr, FALSE);

  // 2. Đưa preview file mới vào qua luồng bất đồng bộ
  uint64_t gen = ++m_loadGeneration;
  StartAsyncLoad(cleanPath, gen);
}

// ─────────────────────────────────────────────────────────
// File Loading
// ─────────────────────────────────────────────────────────

void PreviewWindow::LoadFile(const std::wstring &path) {
  m_currentFile = path;
  m_currentType = FileTypeDetector::Detect(path);
  m_fileInfo = Utils::GetFileInfo(path);
  m_scrollY = 0.0f;
  m_totalTextH = 0.0f;
  m_textContent.clear();

  switch (m_currentType) {
  case FileType::Image: {
    std::wstring ext = Utils::GetFileExtension(path);
    if (RawThumbnailExtractor::IsRawExtension(ext)) {
      std::vector<uint8_t> rawBytes;
      if (RawThumbnailExtractor::Extract(path, rawBytes) && !rawBytes.empty()) {
        m_imagePreview.LoadMemory(rawBytes.data(), rawBytes.size(), m_wicFactory.Get());
      }
    } else {
      m_imagePreview.LoadFile(path, m_wicFactory.Get());
    }
    if (m_rt) {
      m_imagePreview.CreateBitmap(m_rt.Get());
    }
    break;
  }

  case FileType::Text:
    m_textContent = Utils::ReadTextFile(path);
    break;

  case FileType::Markdown:
    m_mdPreview.LoadFile(path);
    break;

  case FileType::PDF:
  case FileType::Illustrator:
    m_pdfPreview.Load(path);
    break;

  case FileType::Excel:
    m_excelPreview.Load(path);
    break;

  case FileType::PowerPoint: {
    std::vector<uint8_t> thumbBytes;
    m_pptHasThumbnail = ArchivePreview::ExtractFile(
        path, "docProps/thumbnail.jpeg", thumbBytes);
    if (!m_pptHasThumbnail)
      m_pptHasThumbnail = ArchivePreview::ExtractFile(
          path, "docProps/thumbnail.jpg", thumbBytes);
    if (!m_pptHasThumbnail)
      m_pptHasThumbnail = ArchivePreview::ExtractFile(
          path, "docProps/thumbnail.png", thumbBytes);
    bool hasThumbFromInspect = false;
    ArchivePreview::InspectPresentation(path, m_pptSlideCount,
                                        hasThumbFromInspect);
    m_pptHasThumbnail = m_pptHasThumbnail || hasThumbFromInspect;
    if (m_pptHasThumbnail && !thumbBytes.empty()) {
      m_imagePreview.LoadMemory(thumbBytes.data(), thumbBytes.size(),
                                m_wicFactory.Get());
      if (m_rt) {
        m_imagePreview.CreateBitmap(m_rt.Get());
      }
    }
    break;
  }

  case FileType::Svg:
    if (m_dc5) {
      m_svgPreview.Load(path, m_dc5.Get());
    }
    break;

  case FileType::Font:
    m_fontPreview.Load(path, m_dwriteFactory.Get());
    break;

  case FileType::Archive:
    m_archivePreview.Load(path);
    break;

  case FileType::Video:
  case FileType::Audio:
    if (m_videoPreview.Load(path)) {
      SetTimer(m_hwnd, TIMER_VIDEO_PROGRESS, 33, nullptr);
    }
    break;

  case FileType::Document:
    LoadPreviewHandler(path);
    break;

  case FileType::Unknown:
  default:
    // File info view - loaded after render target is created
    break;
  }
}

void PreviewWindow::CleanupPreview() {
  if (m_isFullscreen) {
    ExitFullScreen();
  }
  KillTimer(m_hwnd, TIMER_VIDEO_PROGRESS);
  m_videoPreview.Reset();
  m_videoSeeking = false;
  m_videoPlayHover = false;
  m_videoMuteHover = false;
  m_videoSeekHover = false;

  m_pdfPreview.Reset();
  m_mdPreview.Reset();
  m_archivePreview.Unload();
  m_svgPreview.Unload();
  m_fontPreview.Unload();
  m_excelPreview.Unload();
  m_pptHasThumbnail = false;
  m_pptSlideCount = 0;

  m_pdfPrevHover = false;
  m_pdfNextHover = false;

  DestroyPreviewHandler();
  m_imagePreview.Reset();
  m_fileInfoView.Reset();
  m_textContent.clear();
  m_textLines.clear();
  m_currentFile.clear();
  m_currentType = FileType::Unknown;
  m_scrollY = 0.0f;
  m_totalTextH = 0.0f;
}

// ─────────────────────────────────────────────────────────
// Preview Handler (COM)
// ─────────────────────────────────────────────────────────

CLSID PreviewWindow::FindPreviewHandlerCLSID(const std::wstring &extension) {
  CLSID clsid = CLSID_NULL;

  // Try direct: HKCR\<ext>\shellex\{8895b1c6-...}
  std::wstring keyPath =
      extension + L"\\shellex\\{8895b1c6-b41f-4c1c-a562-0d564250836f}";
  HKEY hKey = nullptr;
  if (RegOpenKeyExW(HKEY_CLASSES_ROOT, keyPath.c_str(), 0, KEY_READ, &hKey) ==
      ERROR_SUCCESS) {
    wchar_t val[128] = {};
    DWORD size = sizeof(val);
    if (RegQueryValueExW(hKey, nullptr, nullptr, nullptr,
                         reinterpret_cast<LPBYTE>(val),
                         &size) == ERROR_SUCCESS) {
      CLSIDFromString(val, &clsid);
    }
    RegCloseKey(hKey);
    if (clsid != CLSID_NULL)
      return clsid;
  }

  // Try via ProgID: HKCR\<ext> → Default = ProgID → HKCR\<ProgID>\shellex\{...}
  if (RegOpenKeyExW(HKEY_CLASSES_ROOT, extension.c_str(), 0, KEY_READ, &hKey) ==
      ERROR_SUCCESS) {
    wchar_t progId[256] = {};
    DWORD size = sizeof(progId);
    if (RegQueryValueExW(hKey, nullptr, nullptr, nullptr,
                         reinterpret_cast<LPBYTE>(progId),
                         &size) == ERROR_SUCCESS &&
        progId[0]) {
      RegCloseKey(hKey);

      std::wstring progKeyPath =
          std::wstring(progId) +
          L"\\shellex\\{8895b1c6-b41f-4c1c-a562-0d564250836f}";
      if (RegOpenKeyExW(HKEY_CLASSES_ROOT, progKeyPath.c_str(), 0, KEY_READ,
                        &hKey) == ERROR_SUCCESS) {
        wchar_t val[128] = {};
        size = sizeof(val);
        if (RegQueryValueExW(hKey, nullptr, nullptr, nullptr,
                             reinterpret_cast<LPBYTE>(val),
                             &size) == ERROR_SUCCESS) {
          CLSIDFromString(val, &clsid);
        }
        RegCloseKey(hKey);
      }
    } else {
      RegCloseKey(hKey);
    }
  }

  return clsid;
}

bool PreviewWindow::LoadPreviewHandler(const std::wstring &path) {
  DestroyPreviewHandler();

  std::wstring ext = Utils::GetFileExtension(path);
  CLSID handlerClsid = FindPreviewHandlerCLSID(ext);
  if (handlerClsid == CLSID_NULL)
    return false;

  // Create preview handler
  HRESULT hr = CoCreateInstance(handlerClsid, nullptr,
                                CLSCTX_INPROC_SERVER | CLSCTX_LOCAL_SERVER,
                                IID_PPV_ARGS(&m_previewHandler));
  if (FAILED(hr))
    return false;

  // Initialize with file path
  ComPtr<IInitializeWithFile> initFile;
  hr = m_previewHandler.As(&initFile);
  if (SUCCEEDED(hr)) {
    hr = initFile->Initialize(path.c_str(), STGM_READ);
  } else {
    // Try IInitializeWithStream
    ComPtr<IInitializeWithStream> initStream;
    hr = m_previewHandler.As(&initStream);
    if (SUCCEEDED(hr)) {
      ComPtr<IStream> stream;
      hr = SHCreateStreamOnFileW(path.c_str(),
                                 STGM_READ | STGM_SHARE_DENY_WRITE, &stream);
      if (SUCCEEDED(hr)) {
        hr = initStream->Initialize(stream.Get(), STGM_READ);
      }
    }
  }

  if (FAILED(hr)) {
    m_previewHandler.Reset();
    return false;
  }

  // Create child window for handler
  D2D1_RECT_F cr = GetContentRect();
  RECT handlerRect = {static_cast<LONG>(cr.left), static_cast<LONG>(cr.top),
                      static_cast<LONG>(cr.right),
                      static_cast<LONG>(cr.bottom)};

  m_handlerWnd = CreateWindowExW(
      0, HANDLER_CLASS, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
      handlerRect.left, handlerRect.top, handlerRect.right - handlerRect.left,
      handlerRect.bottom - handlerRect.top, m_hwnd, nullptr, m_hInst, nullptr);

  if (!m_handlerWnd) {
    m_previewHandler.Reset();
    return false;
  }

  // Set window and trigger preview
  m_previewHandler->SetWindow(m_handlerWnd, &handlerRect);

  RECT localRect = {0, 0, handlerRect.right - handlerRect.left,
                    handlerRect.bottom - handlerRect.top};
  m_previewHandler->SetRect(&localRect);

  hr = m_previewHandler->DoPreview();
  if (FAILED(hr)) {
    DestroyPreviewHandler();
    return false;
  }

  return true;
}

void PreviewWindow::DestroyPreviewHandler() {
  if (m_previewHandler) {
    m_previewHandler->Unload();
    m_previewHandler.Reset();
  }
  if (m_handlerWnd) {
    DestroyWindow(m_handlerWnd);
    m_handlerWnd = nullptr;
  }
}

// ─────────────────────────────────────────────────────────
// Layout
// ─────────────────────────────────────────────────────────

void PreviewWindow::UpdateLayout() {
  D2D1_RECT_F cr = GetContentRect();
  RECT contentR = {static_cast<LONG>(cr.left), static_cast<LONG>(cr.top),
                   static_cast<LONG>(cr.right), static_cast<LONG>(cr.bottom)};

  if (m_currentType == FileType::Video && m_videoPreview.IsLoaded()) {
    m_videoPreview.Resize(contentR);
  }

  // Update handler child window position
  if (m_handlerWnd && m_previewHandler) {
    MoveWindow(m_handlerWnd, contentR.left, contentR.top,
               contentR.right - contentR.left, contentR.bottom - contentR.top,
               TRUE);

    RECT localRect = {0, 0, contentR.right - contentR.left,
                      contentR.bottom - contentR.top};
    m_previewHandler->SetRect(&localRect);
  }
}

SIZE PreviewWindow::CalculateWindowSize() {
  SIZE sz = {PREVIEW_MIN_WIDTH, PREVIEW_MIN_HEIGHT};

  switch (m_currentType) {
  case FileType::Image:
    if (m_imagePreview.IsLoaded()) {
      sz = m_imagePreview.GetPreferredSize();
    } else {
      SIZE imgSz = Utils::GetImageDimensions(m_currentFile, m_wicFactory.Get());
      if (imgSz.cx > 0 && imgSz.cy > 0) {
        float maxW = static_cast<float>(PREVIEW_MAX_WIDTH);
        float maxH = static_cast<float>(PREVIEW_MAX_HEIGHT - TITLEBAR_HEIGHT - TOOLBAR_HEIGHT);
        float scale = (std::min)(1.0f, (std::min)(maxW / imgSz.cx, maxH / imgSz.cy));
        int prefW = (std::max)(PREVIEW_MIN_WIDTH, static_cast<int>(imgSz.cx * scale));
        int prefH = (std::max)(PREVIEW_MIN_HEIGHT - TITLEBAR_HEIGHT - TOOLBAR_HEIGHT, static_cast<int>(imgSz.cy * scale));
        sz.cx = prefW;
        sz.cy = prefH + TITLEBAR_HEIGHT + TOOLBAR_HEIGHT;
      } else {
        sz.cx = 760;
        sz.cy = 540;
      }
    }
    break;

  case FileType::Text:
    sz.cx = 700;
    sz.cy = 560;
    break;

  case FileType::Markdown:
    sz.cx = 760;
    sz.cy = 620;
    break;

  case FileType::Video:
    if (m_videoPreview.IsLoaded()) {
      SIZE pref = m_videoPreview.GetPreferredSize(
          PREVIEW_MAX_WIDTH,
          PREVIEW_MAX_HEIGHT - TITLEBAR_HEIGHT - TOOLBAR_HEIGHT);
      sz.cx = pref.cx;
      sz.cy = pref.cy + TITLEBAR_HEIGHT + TOOLBAR_HEIGHT;
    } else {
      SIZE vidSize = Utils::GetVideoDimensions(m_currentFile);
      if (vidSize.cx > 0 && vidSize.cy > 0) {
        float maxW = static_cast<float>(PREVIEW_MAX_WIDTH);
        float maxH = static_cast<float>(PREVIEW_MAX_HEIGHT - TITLEBAR_HEIGHT - TOOLBAR_HEIGHT);
        float scale = (std::min)(1.0f, (std::min)(maxW / vidSize.cx, maxH / vidSize.cy));
        int prefW = (std::max)(PREVIEW_MIN_WIDTH, static_cast<int>(vidSize.cx * scale));
        int prefH = (std::max)(PREVIEW_MIN_HEIGHT - TITLEBAR_HEIGHT - TOOLBAR_HEIGHT, static_cast<int>(vidSize.cy * scale));
        sz.cx = prefW;
        sz.cy = prefH + TITLEBAR_HEIGHT + TOOLBAR_HEIGHT;
      } else {
        sz.cx = 800;
        sz.cy = 500 + TITLEBAR_HEIGHT + TOOLBAR_HEIGHT;
      }
    }
    break;

  case FileType::Audio:
    sz.cx = 520;
    sz.cy = 380;
    break;

  case FileType::PDF:
  case FileType::Illustrator:
    if (m_pdfPreview.IsLoaded()) {
      sz = m_pdfPreview.GetPreferredSize();
    } else {
      sz.cx = 850;
      sz.cy = 650;
    }
    break;

  case FileType::Excel:
    sz.cx = 980;
    sz.cy = 640;
    break;

  case FileType::PowerPoint:
    if (m_pptHasThumbnail && m_imagePreview.IsLoaded()) {
      sz = m_imagePreview.GetPreferredSize();
    } else {
      sz.cx = 880;
      sz.cy = 550;
    }
    break;

  case FileType::Svg:
    if (m_svgPreview.IsLoaded()) {
      D2D1_SIZE_F nSize = m_svgPreview.GetNativeSize();
      float targetW =
          (std::min)((std::max)(nSize.width + 40.0f,
                                static_cast<float>(PREVIEW_MIN_WIDTH)),
                     static_cast<float>(PREVIEW_MAX_WIDTH));
      float targetH =
          (std::min)((std::max)(nSize.height + TITLEBAR_HEIGHT +
                                    TOOLBAR_HEIGHT + 40.0f,
                                static_cast<float>(PREVIEW_MIN_HEIGHT)),
                     static_cast<float>(PREVIEW_MAX_HEIGHT));
      sz.cx = static_cast<LONG>(targetW);
      sz.cy = static_cast<LONG>(targetH);
    } else {
      sz.cx = 800;
      sz.cy = 600;
    }
    break;

  case FileType::Archive:
    sz.cx = 900;
    sz.cy = 600;
    break;

  case FileType::Font:
    sz.cx = 860;
    sz.cy = 640;
    break;

  case FileType::Document:
    sz.cx = 750;
    sz.cy = 600;
    break;

  case FileType::Unknown:
  default:
    sz.cx = 480;
    sz.cy = 520;
    break;
  }

  // Clamp
  sz.cx = (std::max)(static_cast<LONG>(PREVIEW_MIN_WIDTH),
                     (std::min)(sz.cx, static_cast<LONG>(PREVIEW_MAX_WIDTH)));
  sz.cy = (std::max)(static_cast<LONG>(PREVIEW_MIN_HEIGHT),
                     (std::min)(sz.cy, static_cast<LONG>(PREVIEW_MAX_HEIGHT)));

  return sz;
}

D2D1_RECT_F PreviewWindow::GetContentRect() const {
  return D2D1::RectF(static_cast<float>(CONTENT_PADDING),
                     static_cast<float>(TITLEBAR_HEIGHT),
                     static_cast<float>(m_winW - CONTENT_PADDING),
                     static_cast<float>(m_winH - TOOLBAR_HEIGHT));
}

// ─────────────────────────────────────────────────────────
// Rendering
// ─────────────────────────────────────────────────────────

void PreviewWindow::Render() {
  if (!CreateRenderTarget())
    return;
  if (!m_rt)
    return;

  m_rt->BeginDraw();
  m_rt->SetTransform(D2D1::Matrix3x2F::Identity());

  // Content area background
  D2D1_RECT_F contentRect = GetContentRect();
  m_brush->SetColor(Colors::ContentBg);
  m_rt->FillRectangle(contentRect, m_brush.Get());

  if (m_isLoading) {
    DrawLoadingSpinner(contentRect);
  } else {
    // Draw content based on type
    switch (m_currentType) {
    case FileType::Image:
      DrawContentImage();
      break;
    case FileType::Text:
      DrawContentText();
      break;
    case FileType::Markdown:
      DrawContentMarkdown();
      break;
    case FileType::PDF:
    case FileType::Illustrator:
      if (m_pdfPreview.IsLoaded()) {
        DrawContentPdf();
      } else if (!m_previewHandler) {
        DrawContentFileInfo();
      }
      break;
    case FileType::Excel:
      DrawContentExcel();
      break;
    case FileType::PowerPoint:
      DrawContentPowerPoint();
      break;
    case FileType::Svg:
      DrawContentSvg();
      break;
    case FileType::Font:
      DrawContentFont();
      break;
    case FileType::Archive:
      DrawContentArchive();
      break;
    case FileType::Audio:
      DrawContentAudio();
      break;
    case FileType::Video:
      if (!m_videoPreview.IsLoaded()) {
        float cx = (contentRect.left + contentRect.right) * 0.5f;
        float cy = (contentRect.top + contentRect.bottom) * 0.5f;
        m_brush->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.35f));
        m_rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 32.0f, 32.0f), m_brush.Get());
        m_brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.5f));
        m_rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 32.0f, 32.0f), m_brush.Get(), 1.5f);
        ComPtr<ID2D1PathGeometry> playPath;
        if (SUCCEEDED(m_d2dFactory->CreatePathGeometry(&playPath))) {
          ComPtr<ID2D1GeometrySink> sink;
          if (SUCCEEDED(playPath->Open(&sink))) {
            sink->BeginFigure(D2D1::Point2F(cx - 8.0f, cy - 12.0f), D2D1_FIGURE_BEGIN_FILLED);
            sink->AddLine(D2D1::Point2F(cx + 14.0f, cy));
            sink->AddLine(D2D1::Point2F(cx - 8.0f, cy + 12.0f));
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            sink->Close();
            m_brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.85f));
            m_rt->FillGeometry(playPath.Get(), m_brush.Get());
          }
        }
      }
      break;
    case FileType::Document:
      // Content rendered by IPreviewHandler in child window
      if (!m_previewHandler) {
        DrawContentFileInfo(); // Fallback
      }
      break;
    default:
      DrawContentFileInfo();
      break;
    }
  }

  // Draw chrome (title bar and footer) on top
  DrawTitleBar();
  DrawFooter();

  HRESULT hr = m_rt->EndDraw();
  if (hr == D2DERR_RECREATE_TARGET) {
    DiscardRenderTarget();
  }
}

void PreviewWindow::DrawTitleBar() {
  if (!m_rt || !m_brush)
    return;

  float w = static_cast<float>(m_winW);

  // Title bar background
  D2D1_RECT_F titleRect =
      D2D1::RectF(0, 0, w, static_cast<float>(TITLEBAR_HEIGHT));
  m_brush->SetColor(Colors::TitleBar);
  m_rt->FillRectangle(titleRect, m_brush.Get());

  // Separator line
  m_brush->SetColor(Colors::Separator);
  m_rt->FillRectangle(D2D1::RectF(0, TITLEBAR_HEIGHT - 1.0f, w,
                                  static_cast<float>(TITLEBAR_HEIGHT)),
                      m_brush.Get());

  EnsureAppLogoLoaded();

  float titleStartX = 14.0f;
  if (m_appLogoBitmap) {
    float logoSize = 22.0f;
    float logoY = (TITLEBAR_HEIGHT - logoSize) / 2.0f;
    D2D1_RECT_F logoRect = D2D1::RectF(
        titleStartX, logoY, titleStartX + logoSize, logoY + logoSize);
    m_rt->DrawBitmap(m_appLogoBitmap.Get(), logoRect);
    titleStartX += logoSize + 10.0f;
  }

  // File name
  std::wstring fileName = m_fileInfo.name;
  if (!fileName.empty() && m_titleFont) {
    m_brush->SetColor(Colors::Text);
    D2D1_RECT_F nameRect =
        D2D1::RectF(titleStartX, 0, w - CLOSE_BTN_SIZE - 20.0f,
                    static_cast<float>(TITLEBAR_HEIGHT));
    m_rt->DrawText(fileName.c_str(), static_cast<UINT32>(fileName.length()),
                   m_titleFont.Get(), nameRect, m_brush.Get(),
                   D2D1_DRAW_TEXT_OPTIONS_CLIP);
  }

  // Close button
  float btnX = w - CLOSE_BTN_SIZE - 8.0f;
  float btnY = (TITLEBAR_HEIGHT - CLOSE_BTN_SIZE) / 2.0f;
  D2D1_RECT_F closeRect =
      D2D1::RectF(btnX, btnY, btnX + CLOSE_BTN_SIZE, btnY + CLOSE_BTN_SIZE);

  if (m_closeHover) {
    m_brush->SetColor(Colors::CloseHover);
    D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(closeRect, 6.0f, 6.0f);
    m_rt->FillRoundedRectangle(rr, m_brush.Get());
    m_brush->SetColor(D2D1::ColorF(1, 1, 1));
  } else {
    m_brush->SetColor(Colors::TextDim);
  }

  // Draw X
  float cx = btnX + CLOSE_BTN_SIZE / 2.0f;
  float cy = btnY + CLOSE_BTN_SIZE / 2.0f;
  float s = 5.0f;
  m_rt->DrawLine(D2D1::Point2F(cx - s, cy - s), D2D1::Point2F(cx + s, cy + s),
                 m_brush.Get(), 1.5f);
  m_rt->DrawLine(D2D1::Point2F(cx + s, cy - s), D2D1::Point2F(cx - s, cy + s),
                 m_brush.Get(), 1.5f);
}

void PreviewWindow::DrawFooter() {
  if (!m_rt || !m_brush)
    return;

  if ((m_currentType == FileType::Video || m_currentType == FileType::Audio) &&
      m_videoPreview.IsLoaded()) {
    DrawVideoControls();
    return;
  }

  if ((m_currentType == FileType::PDF ||
       m_currentType == FileType::Illustrator) &&
      m_pdfPreview.IsLoaded()) {
    DrawPdfControls();
    return;
  }

  float w = static_cast<float>(m_winW);
  float footerTop = static_cast<float>(m_winH - TOOLBAR_HEIGHT);

  // Footer background
  D2D1_RECT_F footerRect =
      D2D1::RectF(0, footerTop, w, static_cast<float>(m_winH));
  m_brush->SetColor(Colors::Footer);
  m_rt->FillRectangle(footerRect, m_brush.Get());

  // Separator line
  m_brush->SetColor(Colors::Separator);
  m_rt->FillRectangle(D2D1::RectF(0, footerTop, w, footerTop + 1.0f),
                      m_brush.Get());

  // Build info string
  std::wstring info;

  // Dimensions for images / PowerPoint thumbnail
  if (m_currentType == FileType::Image ||
      (m_currentType == FileType::PowerPoint && m_pptHasThumbnail)) {
    std::wstring dims = m_imagePreview.GetDimensionsString();
    if (!dims.empty()) {
      info += dims + L"  \u2022  ";
    }
  }

  // SVG dimensions
  if (m_currentType == FileType::Svg && m_svgPreview.IsLoaded()) {
    D2D1_SIZE_F nSize = m_svgPreview.GetNativeSize();
    info += std::to_wstring(static_cast<int>(nSize.width)) + L" \u00D7 " +
            std::to_wstring(static_cast<int>(nSize.height)) + L"  \u2022  ";
  }

  // Excel sheets
  if (m_currentType == FileType::Excel && m_excelPreview.IsLoaded()) {
    info += std::to_wstring(m_excelPreview.GetSheetCount()) +
            L" sheet(s)  \u2022  ";
  }

  // PowerPoint slides & thumbnail
  if (m_currentType == FileType::PowerPoint) {
    if (m_pptSlideCount > 0) {
      info += std::to_wstring(m_pptSlideCount) + L" slide(s)  \u2022  ";
    }
    if (m_pptHasThumbnail) {
      info += L"Slide 1 Thumbnail  \u2022  ";
    }
  }

  // Archive entries
  if (m_currentType == FileType::Archive && m_archivePreview.IsLoaded()) {
    info +=
        std::to_wstring(m_archivePreview.GetEntryCount()) + L" items  \u2022  ";
  }

  // File size
  info += Utils::FormatFileSize(m_fileInfo.size);

  // File type
  info += L"  \u2022  ";
  if (m_currentType == FileType::Image &&
      RawThumbnailExtractor::IsRawExtension(m_fileInfo.extension)) {
    if (m_fileInfo.extension == L".cr2" || m_fileInfo.extension == L".cr3" ||
        m_fileInfo.extension == L".crw") {
      info += L"Canon RAW Image";
    } else if (m_fileInfo.extension == L".nef" ||
               m_fileInfo.extension == L".nrw") {
      info += L"Nikon RAW Image";
    } else if (m_fileInfo.extension == L".arw" ||
               m_fileInfo.extension == L".srf" ||
               m_fileInfo.extension == L".sr2") {
      info += L"Sony RAW Image";
    } else if (m_fileInfo.extension == L".dng") {
      info += L"Digital Negative (DNG)";
    } else if (m_fileInfo.extension == L".raf") {
      info += L"Fujifilm RAW Image";
    } else if (m_fileInfo.extension == L".rw2") {
      info += L"Panasonic RAW Image";
    } else if (m_fileInfo.extension == L".orf") {
      info += L"Olympus RAW Image";
    } else if (m_fileInfo.extension == L".pef" ||
               m_fileInfo.extension == L".ptx") {
      info += L"Pentax RAW Image";
    } else {
      info += L"Camera RAW Image";
    }
  } else {
    info += FileTypeDetector::GetTypeLabel(m_currentType);
  }

  // Draw info text
  if (m_smallFont) {
    m_brush->SetColor(Colors::TextDim);
    D2D1_RECT_F textRect =
        D2D1::RectF(16.0f, footerTop, w - 16.0f, static_cast<float>(m_winH));
    m_rt->DrawText(info.c_str(), static_cast<UINT32>(info.length()),
                   m_smallFont.Get(), textRect, m_brush.Get());
  }

  // Keyboard hint on right side
  std::wstring hint = L"ESC to close";
  if (m_smallFont) {
    m_brush->SetColor(Colors::TextMuted);
    ComPtr<IDWriteTextLayout> layout;
    m_dwriteFactory->CreateTextLayout(
        hint.c_str(), static_cast<UINT32>(hint.length()), m_smallFont.Get(),
        200, TOOLBAR_HEIGHT, &layout);
    if (layout) {
      DWRITE_TEXT_METRICS metrics;
      layout->GetMetrics(&metrics);
      float hintX = w - metrics.width - 16.0f;
      D2D1_RECT_F hintRect =
          D2D1::RectF(hintX, footerTop, w - 16.0f, static_cast<float>(m_winH));
      m_rt->DrawText(hint.c_str(), static_cast<UINT32>(hint.length()),
                     m_smallFont.Get(), hintRect, m_brush.Get());
    }
  }
}

void PreviewWindow::DrawVideoControls() {
  if (!m_rt || !m_brush)
    return;

  float w = static_cast<float>(m_winW);
  float footerTop = static_cast<float>(m_winH - TOOLBAR_HEIGHT);
  float footerH = static_cast<float>(TOOLBAR_HEIGHT);
  float cy = footerTop + footerH / 2.0f;

  // 1. Footer background & subtle separator
  D2D1_RECT_F footerRect =
      D2D1::RectF(0, footerTop, w, static_cast<float>(m_winH));
  m_brush->SetColor(Colors::Footer);
  m_rt->FillRectangle(footerRect, m_brush.Get());

  m_brush->SetColor(Colors::Separator);
  m_rt->FillRectangle(D2D1::RectF(0, footerTop, w, footerTop + 1.0f),
                      m_brush.Get());

  // 2. Play / Pause Button (x: 12 to 40)
  float btnSize = 28.0f;
  float playX = 12.0f;
  float playY = cy - btnSize / 2.0f;
  m_videoPlayRect = D2D1::RectF(playX, playY, playX + btnSize, playY + btnSize);

  if (m_videoPlayHover) {
    m_brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.12f));
    m_rt->FillRoundedRectangle(D2D1::RoundedRect(m_videoPlayRect, 6.0f, 6.0f),
                               m_brush.Get());
  }

  // Draw Play or Pause icon
  bool isPlaying = m_videoPreview.IsPlaying();
  m_brush->SetColor(Colors::Text);
  float playCx = playX + btnSize / 2.0f;
  float playCy = cy;

  if (isPlaying) {
    // Pause icon: two vertical bars
    D2D1_ROUNDED_RECT bar1 = D2D1::RoundedRect(
        D2D1::RectF(playCx - 4.5f, playCy - 5.5f, playCx - 1.5f, playCy + 5.5f),
        1.0f, 1.0f);
    D2D1_ROUNDED_RECT bar2 = D2D1::RoundedRect(
        D2D1::RectF(playCx + 1.5f, playCy - 5.5f, playCx + 4.5f, playCy + 5.5f),
        1.0f, 1.0f);
    m_rt->FillRoundedRectangle(bar1, m_brush.Get());
    m_rt->FillRoundedRectangle(bar2, m_brush.Get());
  } else {
    // Play icon: crisp filled triangle
    ComPtr<ID2D1PathGeometry> triangle;
    m_d2dFactory->CreatePathGeometry(&triangle);
    if (triangle) {
      ComPtr<ID2D1GeometrySink> sink;
      triangle->Open(&sink);
      sink->BeginFigure(D2D1::Point2F(playCx - 3.5f, playCy - 6.0f),
                        D2D1_FIGURE_BEGIN_FILLED);
      sink->AddLine(D2D1::Point2F(playCx + 5.5f, playCy));
      sink->AddLine(D2D1::Point2F(playCx - 3.5f, playCy + 6.0f));
      sink->EndFigure(D2D1_FIGURE_END_CLOSED);
      sink->Close();
      m_rt->FillGeometry(triangle.Get(), m_brush.Get());
    }
  }

  // 3. Time string
  double curPos = m_videoPreview.GetPosition();
  double dur = m_videoPreview.GetDuration();
  std::wstring timeStr = Utils::FormatDuration(curPos);
  if (dur > 0.0) {
    timeStr += L" / " + Utils::FormatDuration(dur);
  }

  float timeX = playX + btnSize + 10.0f;
  float timeW = 100.0f;
  D2D1_RECT_F timeRect =
      D2D1::RectF(timeX, footerTop, timeX + timeW, footerTop + footerH);
  if (m_smallFont) {
    m_brush->SetColor(Colors::TextDim);
    m_rt->DrawText(timeStr.c_str(), static_cast<UINT32>(timeStr.length()),
                   m_smallFont.Get(), timeRect, m_brush.Get());
  }

  // 4. Mute / Unmute Button (right side)
  float muteX = w - 40.0f;
  float muteY = cy - btnSize / 2.0f;
  m_videoMuteRect = D2D1::RectF(muteX, muteY, muteX + btnSize, muteY + btnSize);

  if (m_videoMuteHover) {
    m_brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.12f));
    m_rt->FillRoundedRectangle(D2D1::RoundedRect(m_videoMuteRect, 6.0f, 6.0f),
                               m_brush.Get());
  }

  float muteCx = muteX + btnSize / 2.0f;
  float muteCy = cy;
  bool isMuted = m_videoPreview.IsMuted();

  // Speaker body
  ComPtr<ID2D1PathGeometry> speaker;
  m_d2dFactory->CreatePathGeometry(&speaker);
  if (speaker) {
    ComPtr<ID2D1GeometrySink> sink;
    speaker->Open(&sink);
    sink->BeginFigure(D2D1::Point2F(muteCx - 6.0f, muteCy - 3.0f),
                      D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLine(D2D1::Point2F(muteCx - 2.5f, muteCy - 3.0f));
    sink->AddLine(D2D1::Point2F(muteCx + 2.0f, muteCy - 6.5f));
    sink->AddLine(D2D1::Point2F(muteCx + 2.0f, muteCy + 6.5f));
    sink->AddLine(D2D1::Point2F(muteCx - 2.5f, muteCy + 3.0f));
    sink->AddLine(D2D1::Point2F(muteCx - 6.0f, muteCy + 3.0f));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();
    m_brush->SetColor(Colors::Text);
    m_rt->FillGeometry(speaker.Get(), m_brush.Get());
  }

  if (isMuted) {
    // Red slash for mute
    m_brush->SetColor(D2D1::ColorF(0.95f, 0.35f, 0.35f));
    m_rt->DrawLine(D2D1::Point2F(muteCx + 4.0f, muteCy - 5.0f),
                   D2D1::Point2F(muteCx + 8.0f, muteCy + 5.0f), m_brush.Get(),
                   1.8f);
  } else {
    // Sound waves
    m_brush->SetColor(Colors::TextDim);
    m_rt->DrawLine(D2D1::Point2F(muteCx + 5.0f, muteCy - 3.5f),
                   D2D1::Point2F(muteCx + 5.0f, muteCy + 3.5f), m_brush.Get(),
                   1.5f);
    m_rt->DrawLine(D2D1::Point2F(muteCx + 8.0f, muteCy - 6.0f),
                   D2D1::Point2F(muteCx + 8.0f, muteCy + 6.0f), m_brush.Get(),
                   1.5f);
  }

  // 5. Seek Bar
  float seekLeft = timeX + timeW + 8.0f;
  float seekRight = muteX - 16.0f;
  if (seekRight > seekLeft + 20.0f) {
    m_videoSeekRect = D2D1::RectF(seekLeft, cy - 8.0f, seekRight, cy + 8.0f);

    float barH = (m_videoSeekHover || m_videoSeeking) ? 6.0f : 4.0f;
    float barY = cy - barH / 2.0f;
    float barW = seekRight - seekLeft;

    // Background track
    m_brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.18f));
    D2D1_ROUNDED_RECT track =
        D2D1::RoundedRect(D2D1::RectF(seekLeft, barY, seekRight, barY + barH),
                          barH / 2.0f, barH / 2.0f);
    m_rt->FillRoundedRectangle(track, m_brush.Get());

    // Played progress fill
    float progress = 0.0f;
    if (dur > 0.0) {
      progress = static_cast<float>(curPos / dur);
      progress = (std::max)(0.0f, (std::min)(progress, 1.0f));
    }
    float fillW = barW * progress;
    if (fillW > 0.0f) {
      m_brush->SetColor(Colors::Accent);
      D2D1_ROUNDED_RECT fill = D2D1::RoundedRect(
          D2D1::RectF(seekLeft, barY, seekLeft + fillW, barY + barH),
          barH / 2.0f, barH / 2.0f);
      m_rt->FillRoundedRectangle(fill, m_brush.Get());
    }

    // Scrubber thumb circle
    float thumbRadius = (m_videoSeekHover || m_videoSeeking) ? 6.5f : 4.5f;
    float thumbX = seekLeft + fillW;
    m_brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f));
    m_rt->FillEllipse(
        D2D1::Ellipse(D2D1::Point2F(thumbX, cy), thumbRadius, thumbRadius),
        m_brush.Get());

    // Subtle accent border on thumb when active
    if (m_videoSeekHover || m_videoSeeking) {
      m_brush->SetColor(Colors::Accent);
      m_rt->DrawEllipse(
          D2D1::Ellipse(D2D1::Point2F(thumbX, cy), thumbRadius, thumbRadius),
          m_brush.Get(), 1.5f);
    }
  }
}

void PreviewWindow::DrawContentImage() {
  if (!m_rt)
    return;
  D2D1_RECT_F cr = GetContentRect();
  m_imagePreview.Render(m_rt.Get(), cr);
}

void PreviewWindow::DrawContentText() {
  if (!m_rt || !m_brush || !m_monoFont)
    return;

  D2D1_RECT_F cr = GetContentRect();
  float areaW = cr.right - cr.left;
  float areaH = cr.bottom - cr.top;

  // Code background
  m_brush->SetColor(Colors::CodeBg);
  m_rt->FillRectangle(cr, m_brush.Get());

  // Set clip region
  m_rt->PushAxisAlignedClip(cr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

  float lineH = 18.0f;
  float padX = 60.0f; // space for line numbers
  float padY = 12.0f;

  m_totalTextH = (std::max)(0.0f, static_cast<float>(m_textLines.size()) * lineH +
                                      padY * 2 - areaH);

  // Draw visible lines
  float y = cr.top + padY - m_scrollY;
  int maxLines = static_cast<int>(m_textLines.size());
  int startLine = (std::max)(0, static_cast<int>((m_scrollY - padY) / lineH));
  int endLine =
      (std::min)(maxLines, startLine + static_cast<int>(areaH / lineH) + 2);

  y += startLine * lineH;

  for (int i = startLine; i < endLine; i++) {
    if (y > cr.bottom)
      break;
    if (y + lineH < cr.top) {
      y += lineH;
      continue;
    }

    // Line number
    if (m_lineNumFont) {
      wchar_t numBuf[16];
      swprintf_s(numBuf, L"%d", i + 1);
      m_brush->SetColor(Colors::LineNumber);
      D2D1_RECT_F numRect =
          D2D1::RectF(cr.left + 8, y, cr.left + padX - 12, y + lineH);
      m_rt->DrawText(numBuf, static_cast<UINT32>(wcslen(numBuf)),
                     m_lineNumFont.Get(), numRect, m_brush.Get());
    }

    // Line content
    if (!m_textLines[i].empty()) {
      m_brush->SetColor(Colors::Text);
      D2D1_RECT_F textRect =
          D2D1::RectF(cr.left + padX, y, cr.right - 8, y + lineH);
      m_rt->DrawText(m_textLines[i].c_str(), static_cast<UINT32>(m_textLines[i].length()),
                     m_monoFont.Get(), textRect, m_brush.Get());
    }

    y += lineH;
  }

  // Scrollbar
  if (m_totalTextH > 0) {
    DrawScrollbar();
  }

  m_rt->PopAxisAlignedClip();
}

void PreviewWindow::DrawScrollbar() {
  if (!m_rt || !m_brush)
    return;

  D2D1_RECT_F cr = GetContentRect();
  float areaH = cr.bottom - cr.top;
  float scrollbarW = 6.0f;
  float scrollbarX = cr.right - scrollbarW - 4.0f;

  // Track
  m_brush->SetColor(Colors::ScrollTrack);
  D2D1_ROUNDED_RECT track =
      D2D1::RoundedRect(D2D1::RectF(scrollbarX, cr.top + 4,
                                    scrollbarX + scrollbarW, cr.bottom - 4),
                        3.0f, 3.0f);
  m_rt->FillRoundedRectangle(track, m_brush.Get());

  // Thumb
  float totalH = areaH + m_totalTextH;
  float thumbH = (std::max)(30.0f, areaH * areaH / totalH);
  float thumbY = cr.top + 4 + (m_scrollY / m_totalTextH) * (areaH - 8 - thumbH);

  m_brush->SetColor(Colors::ScrollThumb);
  D2D1_ROUNDED_RECT thumb = D2D1::RoundedRect(
      D2D1::RectF(scrollbarX, thumbY, scrollbarX + scrollbarW, thumbY + thumbH),
      3.0f, 3.0f);
  m_rt->FillRoundedRectangle(thumb, m_brush.Get());
}

void PreviewWindow::DrawContentFileInfo() {
  if (!m_rt || !m_brush)
    return;
  D2D1_RECT_F cr = GetContentRect();
  m_fileInfoView.Render(m_rt.Get(), m_dwriteFactory.Get(), cr, m_brush.Get());
}

void PreviewWindow::DrawContentMarkdown() {
  if (!m_rt || !m_brush)
    return;
  D2D1_RECT_F cr = GetContentRect();
  m_rt->PushAxisAlignedClip(cr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

  m_mdPreview.Render(m_rt.Get(), m_dwriteFactory.Get(), m_bodyFont.Get(),
                     m_titleFont.Get(), m_monoFont.Get(), m_smallFont.Get(),
                     cr);

  float areaH = cr.bottom - cr.top;
  float totalH = m_mdPreview.GetTotalHeight();
  if (totalH > areaH) {
    float scrollbarW = 6.0f;
    float scrollbarX = cr.right - scrollbarW - 4.0f;

    m_brush->SetColor(Colors::ScrollTrack);
    D2D1_ROUNDED_RECT track =
        D2D1::RoundedRect(D2D1::RectF(scrollbarX, cr.top + 4,
                                      scrollbarX + scrollbarW, cr.bottom - 4),
                          3.0f, 3.0f);
    m_rt->FillRoundedRectangle(track, m_brush.Get());

    float thumbH = (std::max)(30.0f, areaH * areaH / totalH);
    float maxScroll = totalH - areaH;
    float scrollFraction =
        (maxScroll > 0.0f) ? (m_mdPreview.GetScrollY() / maxScroll) : 0.0f;
    float thumbY = cr.top + 4 + scrollFraction * (areaH - 8 - thumbH);

    m_brush->SetColor(Colors::ScrollThumb);
    D2D1_ROUNDED_RECT thumb =
        D2D1::RoundedRect(D2D1::RectF(scrollbarX, thumbY,
                                      scrollbarX + scrollbarW, thumbY + thumbH),
                          3.0f, 3.0f);
    m_rt->FillRoundedRectangle(thumb, m_brush.Get());
  }

  m_rt->PopAxisAlignedClip();
}

void PreviewWindow::DrawContentPdf() {
  if (!m_rt)
    return;
  D2D1_RECT_F cr = GetContentRect();
  m_rt->PushAxisAlignedClip(cr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
  m_pdfPreview.Render(m_rt.Get(), cr);
  m_rt->PopAxisAlignedClip();
}

void PreviewWindow::DrawContentAudio() {
  if (!m_rt || !m_brush)
    return;
  D2D1_RECT_F cr = GetContentRect();
  m_rt->PushAxisAlignedClip(cr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

  float cx = (cr.left + cr.right) / 2.0f;
  float cy = (cr.top + cr.bottom) / 2.0f - 24.0f;

  // 1. Vinyl Record Disc (dark outer circle)
  float discRadius = 72.0f;
  m_brush->SetColor(D2D1::ColorF(0.12f, 0.12f, 0.14f, 1.0f));
  m_rt->FillEllipse(
      D2D1::Ellipse(D2D1::Point2F(cx, cy), discRadius, discRadius),
      m_brush.Get());

  // Disc rim border
  m_brush->SetColor(D2D1::ColorF(0.24f, 0.24f, 0.28f, 1.0f));
  m_rt->DrawEllipse(
      D2D1::Ellipse(D2D1::Point2F(cx, cy), discRadius, discRadius),
      m_brush.Get(), 1.5f);

  // Grooves
  m_brush->SetColor(D2D1::ColorF(0.16f, 0.16f, 0.19f, 1.0f));
  m_rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), discRadius - 12.0f,
                                  discRadius - 12.0f),
                    m_brush.Get(), 1.0f);
  m_rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), discRadius - 22.0f,
                                  discRadius - 22.0f),
                    m_brush.Get(), 1.0f);
  m_rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), discRadius - 32.0f,
                                  discRadius - 32.0f),
                    m_brush.Get(), 1.0f);

  // Center Label (Accent color)
  float labelRadius = 26.0f;
  m_brush->SetColor(Colors::Accent);
  m_rt->FillEllipse(
      D2D1::Ellipse(D2D1::Point2F(cx, cy), labelRadius, labelRadius),
      m_brush.Get());

  // Center Spindle Hole
  float holeRadius = 6.0f;
  m_brush->SetColor(Colors::ContentBg);
  m_rt->FillEllipse(
      D2D1::Ellipse(D2D1::Point2F(cx, cy), holeRadius, holeRadius),
      m_brush.Get());

  // Center music note icon
  std::wstring note = L"\u266B";
  if (m_titleFont) {
    m_brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.9f));
    D2D1_RECT_F noteRect =
        D2D1::RectF(cx - 15.0f, cy - 17.0f, cx + 15.0f, cy + 15.0f);
    m_rt->DrawText(note.c_str(), static_cast<UINT32>(note.length()),
                   m_titleFont.Get(), noteRect, m_brush.Get());
  }

  // 2. Track Title
  float titleY = cy + discRadius + 18.0f;
  if (m_titleFont) {
    m_brush->SetColor(Colors::Text);
    std::wstring title = m_fileInfo.name;
    float maxW = (cr.right - cr.left) - 40.0f;
    if (maxW < 100.0f) maxW = 100.0f;

    ComPtr<IDWriteTextLayout> titleLayout;
    if (SUCCEEDED(m_dwriteFactory->CreateTextLayout(
            title.c_str(), static_cast<UINT32>(title.length()),
            m_titleFont.Get(), maxW, 56.0f, &titleLayout))) {
      titleLayout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
      titleLayout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
      DWRITE_TRIMMING trim = { DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
      ComPtr<IDWriteInlineObject> ellipsis;
      if (SUCCEEDED(m_dwriteFactory->CreateEllipsisTrimmingSign(m_titleFont.Get(), &ellipsis))) {
        titleLayout->SetTrimming(&trim, ellipsis.Get());
      }
      DWRITE_TEXT_METRICS tm = {};
      titleLayout->GetMetrics(&tm);
      m_rt->DrawTextLayout(D2D1::Point2F(cr.left + 20.0f, titleY), titleLayout.Get(), m_brush.Get());
      titleY += tm.height + 6.0f;
    } else {
      D2D1_RECT_F titleRect =
          D2D1::RectF(cr.left + 20.0f, titleY, cr.right - 20.0f, titleY + 28.0f);
      m_rt->DrawText(title.c_str(), static_cast<UINT32>(title.length()),
                     m_titleFont.Get(), titleRect, m_brush.Get());
      titleY += 30.0f;
    }
  }

  // 3. Audio Format & Size Badge
  float badgeY = titleY + 4.0f;
  if (m_smallFont) {
    std::wstring meta = m_fileInfo.typeName + L"  \u2022  " +
                        Utils::FormatFileSize(m_fileInfo.size);
    m_brush->SetColor(Colors::TextDim);
    D2D1_RECT_F metaRect =
        D2D1::RectF(cr.left + 20.0f, badgeY, cr.right - 20.0f, badgeY + 20.0f);
    m_rt->DrawText(meta.c_str(), static_cast<UINT32>(meta.length()),
                   m_smallFont.Get(), metaRect, m_brush.Get());
  }

  m_rt->PopAxisAlignedClip();
}

void PreviewWindow::DrawPdfControls() {
  if (!m_rt || !m_brush)
    return;

  float w = static_cast<float>(m_winW);
  float footerTop = static_cast<float>(m_winH - TOOLBAR_HEIGHT);
  float footerH = static_cast<float>(TOOLBAR_HEIGHT);
  float cy = footerTop + footerH / 2.0f;

  // Footer background & separator
  D2D1_RECT_F footerRect =
      D2D1::RectF(0, footerTop, w, static_cast<float>(m_winH));
  m_brush->SetColor(Colors::Footer);
  m_rt->FillRectangle(footerRect, m_brush.Get());

  m_brush->SetColor(Colors::Separator);
  m_rt->FillRectangle(D2D1::RectF(0, footerTop, w, footerTop + 1.0f),
                      m_brush.Get());

  // Navigation buttons: Prev and Next
  float btnSize = 28.0f;
  float prevX = 12.0f;
  float prevY = cy - btnSize / 2.0f;
  m_pdfPrevRect = D2D1::RectF(prevX, prevY, prevX + btnSize, prevY + btnSize);

  float nextX = prevX + btnSize + 6.0f;
  float nextY = prevY;
  m_pdfNextRect = D2D1::RectF(nextX, nextY, nextX + btnSize, nextY + btnSize);

  int curPage = m_pdfPreview.GetCurrentPage();
  int totalPages = m_pdfPreview.GetPageCount();

  // Prev button
  bool canPrev = (curPage > 0);
  if (m_pdfPrevHover && canPrev) {
    m_brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.12f));
    m_rt->FillRoundedRectangle(D2D1::RoundedRect(m_pdfPrevRect, 6.0f, 6.0f),
                               m_brush.Get());
  }
  m_brush->SetColor(canPrev ? Colors::Text : Colors::TextMuted);
  std::wstring prevIcon = L"\u25C0";
  if (m_smallFont) {
    m_rt->DrawText(
        prevIcon.c_str(), 1, m_smallFont.Get(),
        D2D1::RectF(prevX, prevY + 4.0f, prevX + btnSize, prevY + btnSize),
        m_brush.Get());
  }

  // Next button
  bool canNext = (curPage + 1 < totalPages);
  if (m_pdfNextHover && canNext) {
    m_brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.12f));
    m_rt->FillRoundedRectangle(D2D1::RoundedRect(m_pdfNextRect, 6.0f, 6.0f),
                               m_brush.Get());
  }
  m_brush->SetColor(canNext ? Colors::Text : Colors::TextMuted);
  std::wstring nextIcon = L"\u25B6";
  if (m_smallFont) {
    m_rt->DrawText(
        nextIcon.c_str(), 1, m_smallFont.Get(),
        D2D1::RectF(nextX, nextY + 4.0f, nextX + btnSize, nextY + btnSize),
        m_brush.Get());
  }

  // Page indicator: "Page X of Y" or "Artboard X of Y"
  wchar_t pageStr[64];
  if (m_currentType == FileType::Illustrator) {
    swprintf_s(pageStr, L"Artboard %d of %d", curPage + 1, totalPages);
  } else {
    swprintf_s(pageStr, L"Page %d of %d", curPage + 1, totalPages);
  }
  float textX = nextX + btnSize + 12.0f;
  if (m_smallFont) {
    m_brush->SetColor(Colors::TextDim);
    D2D1_RECT_F tr =
        D2D1::RectF(textX, footerTop, textX + 160.0f, footerTop + footerH);
    m_rt->DrawText(pageStr, static_cast<UINT32>(wcslen(pageStr)),
                   m_smallFont.Get(), tr, m_brush.Get());
  }

  // Right side hint: "← / → to turn page" or "switch artboard"
  std::wstring hint = (m_currentType == FileType::Illustrator)
                          ? L"\u2190 / \u2192 to switch artboard"
                          : L"\u2190 / \u2192 to turn page";
  if (m_smallFont) {
    m_brush->SetColor(Colors::TextMuted);
    ComPtr<IDWriteTextLayout> layout;
    m_dwriteFactory->CreateTextLayout(
        hint.c_str(), static_cast<UINT32>(hint.length()), m_smallFont.Get(),
        240, TOOLBAR_HEIGHT, &layout);
    if (layout) {
      DWRITE_TEXT_METRICS metrics;
      layout->GetMetrics(&metrics);
      float hintX = w - metrics.width - 16.0f;
      D2D1_RECT_F hintRect =
          D2D1::RectF(hintX, footerTop, w - 16.0f, static_cast<float>(m_winH));
      m_rt->DrawText(hint.c_str(), static_cast<UINT32>(hint.length()),
                     m_smallFont.Get(), hintRect, m_brush.Get());
    }
  }
}

void PreviewWindow::DrawContentArchive() {
  if (!m_rt)
    return;
  D2D1_RECT_F cr = GetContentRect();
  m_rt->PushAxisAlignedClip(cr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
  m_archivePreview.Render(m_rt.Get(), m_dwriteFactory.Get(), cr);
  m_rt->PopAxisAlignedClip();
}

void PreviewWindow::DrawContentSvg() {
  if (!m_dc5)
    return;
  D2D1_RECT_F cr = GetContentRect();
  m_rt->PushAxisAlignedClip(cr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
  m_svgPreview.Render(m_dc5.Get(), m_dwriteFactory.Get(), cr);
  m_rt->PopAxisAlignedClip();
}

void PreviewWindow::DrawContentFont() {
  if (!m_rt)
    return;
  D2D1_RECT_F cr = GetContentRect();
  m_rt->PushAxisAlignedClip(cr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
  m_fontPreview.Render(m_rt.Get(), m_dwriteFactory.Get(), cr);
  m_rt->PopAxisAlignedClip();
}

void PreviewWindow::DrawContentExcel() {
  if (!m_rt)
    return;
  D2D1_RECT_F cr = GetContentRect();
  m_rt->PushAxisAlignedClip(cr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
  m_excelPreview.Render(m_rt.Get(), m_dwriteFactory.Get(), cr);
  m_rt->PopAxisAlignedClip();
}

void PreviewWindow::DrawContentPowerPoint() {
  if (!m_rt)
    return;
  D2D1_RECT_F cr = GetContentRect();
  if (m_pptHasThumbnail && m_imagePreview.IsLoaded()) {
    m_rt->PushAxisAlignedClip(cr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    m_imagePreview.Render(m_rt.Get(), cr);
    m_rt->PopAxisAlignedClip();
  } else {
    // Fallback PowerPoint Card
    m_rt->PushAxisAlignedClip(cr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    float cx = (cr.left + cr.right) * 0.5f;
    float cy = (cr.top + cr.bottom) * 0.5f;

    float cardW = 340.0f;
    float cardH = 200.0f;
    D2D1_RECT_F cardRect = D2D1::RectF(cx - cardW * 0.5f, cy - cardH * 0.5f,
                                       cx + cardW * 0.5f, cy + cardH * 0.5f);
    D2D1_ROUNDED_RECT rounded = D2D1::RoundedRect(cardRect, 10.0f, 10.0f);

    m_brush->SetColor(D2D1::ColorF(0.12f, 0.15f, 0.20f, 1.0f));
    m_rt->FillRoundedRectangle(rounded, m_brush.Get());
    m_brush->SetColor(D2D1::ColorF(0.82f, 0.28f, 0.15f, 0.8f));
    m_rt->DrawRoundedRectangle(rounded, m_brush.Get(), 1.5f);

    // Big "P" Icon in PowerPoint red/orange
    D2D1_RECT_F iconRect = D2D1::RectF(cx - 30.0f, cardRect.top + 22.0f,
                                       cx + 30.0f, cardRect.top + 82.0f);
    m_brush->SetColor(D2D1::ColorF(0.82f, 0.28f, 0.15f, 1.0f));
    m_rt->FillRoundedRectangle(D2D1::RoundedRect(iconRect, 8.0f, 8.0f),
                               m_brush.Get());

    if (m_titleFont) {
      m_brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f));
      m_titleFont->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
      m_titleFont->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
      m_rt->DrawTextW(L"P", 1, m_titleFont.Get(), iconRect, m_brush.Get());
      m_titleFont->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }

    if (m_bodyFont) {
      m_brush->SetColor(Colors::Text);
      m_bodyFont->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
      D2D1_RECT_F titleR =
          D2D1::RectF(cardRect.left + 16.0f, cardRect.top + 96.0f,
                      cardRect.right - 16.0f, cardRect.top + 120.0f);
      std::wstring pTitle = L"PowerPoint Presentation";
      m_rt->DrawTextW(pTitle.c_str(), static_cast<UINT32>(pTitle.length()),
                      m_bodyFont.Get(), titleR, m_brush.Get());
      m_bodyFont->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }

    if (m_smallFont) {
      m_brush->SetColor(Colors::TextDim);
      m_smallFont->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
      D2D1_RECT_F descR =
          D2D1::RectF(cardRect.left + 16.0f, cardRect.top + 124.0f,
                      cardRect.right - 16.0f, cardRect.top + 160.0f);
      std::wstring pDesc = (m_pptSlideCount > 0)
                               ? (std::to_wstring(m_pptSlideCount) +
                                  L" slides  \u2022  No embedded thumbnail")
                               : L"No embedded thumbnail available";
      m_rt->DrawTextW(pDesc.c_str(), static_cast<UINT32>(pDesc.length()),
                      m_smallFont.Get(), descR, m_brush.Get());
      m_smallFont->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }
    m_rt->PopAxisAlignedClip();
  }
}

// ─────────────────────────────────────────────────────────
// System Tray
// ─────────────────────────────────────────────────────────

void PreviewWindow::AddTrayIcon() {
  NOTIFYICONDATAW nid = {};
  nid.cbSize = sizeof(nid);
  nid.hWnd = m_hwnd;
  nid.uID = 1;
  nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP;
  nid.uCallbackMessage = WM_TRAYICON;
  nid.uVersion = NOTIFYICON_VERSION_4;

  HICON hTrayIcon = (HICON)LoadImageW(m_hInst, MAKEINTRESOURCEW(IDI_QUICKVIEW),
                                      IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);
  if (!hTrayIcon)
    hTrayIcon = LoadIconW(m_hInst, MAKEINTRESOURCEW(IDI_QUICKVIEW));
  if (!hTrayIcon)
    hTrayIcon = LoadIconW(nullptr, IDI_APPLICATION);
  nid.hIcon = hTrayIcon;

  wcscpy_s(nid.szTip, L"QuickView \u2022 Space to preview");

  Shell_NotifyIconW(NIM_ADD, &nid);
  Shell_NotifyIconW(NIM_SETVERSION, &nid);
  m_trayAdded = true;
}

void PreviewWindow::RemoveTrayIcon() {
  if (!m_trayAdded)
    return;

  NOTIFYICONDATAW nid = {};
  nid.cbSize = sizeof(nid);
  nid.hWnd = m_hwnd;
  nid.uID = 1;
  Shell_NotifyIconW(NIM_DELETE, &nid);
  m_trayAdded = false;
}

void PreviewWindow::ShowTrayMenu() {
  HMENU menu = CreatePopupMenu();

  // Disabled info item
  AppendMenuW(menu, MF_STRING | MF_DISABLED | MF_GRAYED, 0, L"QuickView");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

  bool isAutoStart = Utils::IsStartWithWindowsEnabled();
  UINT autoStartFlags = MF_STRING | (isAutoStart ? MF_CHECKED : MF_UNCHECKED);
  AppendMenuW(menu, autoStartFlags, IDM_START_WITH_WINDOWS,
              L"Start with Windows");

  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Exit QuickView");

  POINT pt;
  GetCursorPos(&pt);
  SetForegroundWindow(m_hwnd);
  TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, m_hwnd,
                 nullptr);
  PostMessageW(m_hwnd, WM_NULL, 0, 0);
  DestroyMenu(menu);
}

bool PreviewWindow::SupportsFullScreen() const {
  return m_currentType == FileType::Video ||
         m_currentType == FileType::Audio ||
         m_currentType == FileType::PDF ||
         m_currentType == FileType::Illustrator ||
         m_currentType == FileType::Excel;
}

void PreviewWindow::ShowContextMenu() {
  HMENU menu = CreatePopupMenu();
  if (!menu)
    return;

  // 1. Chỉ hiển thị "Save image" cho file Video
  if (m_currentType == FileType::Video && m_videoPreview.IsLoaded()) {
    AppendMenuW(menu, MF_STRING, IDM_VIDEO_SAVE_IMAGE, L"Save image");
  }

  // 2. Sao chép timecode cho file Video và Audio
  if ((m_currentType == FileType::Video || m_currentType == FileType::Audio) && m_videoPreview.IsLoaded()) {
    AppendMenuW(menu, MF_STRING, IDM_COPY_TIMECODE, L"Copy timecode");
  }

  // Phân cách giữa các tác vụ media và tác vụ sao chép thông tin file
  if ((m_currentType == FileType::Video || m_currentType == FileType::Audio) && m_videoPreview.IsLoaded()) {
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  }

  // 3. Các hành động sao chép: Copy file name, Copy path, Copy folder address
  AppendMenuW(menu, MF_STRING, IDM_COPY_FILE_NAME, L"Copy file name");
  AppendMenuW(menu, MF_STRING, IDM_VIDEO_COPY_PATH, L"Copy path");
  AppendMenuW(menu, MF_STRING, IDM_VIDEO_COPY_FOLDER, L"Copy folder address");

  // 3. Full screen: Chỉ hiển thị cho file media (Video, Audio), PDF, Excel
  if (SupportsFullScreen()) {
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    if (m_isFullscreen) {
      AppendMenuW(menu, MF_STRING, IDM_VIDEO_FULLSCREEN, L"Exit full screen");
    } else {
      AppendMenuW(menu, MF_STRING, IDM_VIDEO_FULLSCREEN, L"Full screen");
    }
  }

  POINT pt;
  GetCursorPos(&pt);
  SetForegroundWindow(m_hwnd);
  TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_LEFTALIGN, pt.x, pt.y, 0, m_hwnd,
                 nullptr);
  PostMessageW(m_hwnd, WM_NULL, 0, 0);
  DestroyMenu(menu);
}

void PreviewWindow::SaveVideoSnapshot() {
  if (m_currentFile.empty() || !m_videoPreview.IsLoaded())
    return;

  std::wstring baseName = Utils::GetFileName(m_currentFile);
  size_t dotPos = baseName.find_last_of(L'.');
  if (dotPos != std::wstring::npos) {
    baseName = baseName.substr(0, dotPos);
  }

  double pos = m_videoPreview.GetPosition();
  int totalSec = static_cast<int>(pos);
  int m = totalSec / 60;
  int s = totalSec % 60;
  wchar_t timeSuffix[32];
  swprintf_s(timeSuffix, L"_%02dm%02ds", m, s);
  std::wstring defaultFileName = baseName + timeSuffix + L".png";

  wchar_t szFile[MAX_PATH] = {};
  wcsncpy_s(szFile, defaultFileName.c_str(), _TRUNCATE);

  std::wstring dir = Utils::GetDirectory(m_currentFile);

  OPENFILENAMEW ofn = {};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = m_hwnd;
  ofn.lpstrFile = szFile;
  ofn.nMaxFile = MAX_PATH;
  ofn.lpstrFilter =
      L"PNG Image (*.png)\0*.png\0JPEG Image (*.jpg;*.jpeg)\0*.jpg;*.jpeg\0All Files (*.*)\0*.*\0";
  ofn.nFilterIndex = 1;
  ofn.lpstrDefExt = L"png";
  ofn.lpstrInitialDir = dir.c_str();
  ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST;

  if (GetSaveFileNameW(&ofn)) {
    bool ok = m_videoPreview.SaveScreenshot(szFile);
    if (ok) {
      Utils::Log(L"[PreviewWindow] Saved video snapshot to: %s", szFile);
    } else {
      Utils::Log(L"[PreviewWindow] Failed to save video snapshot to: %s", szFile);
    }
  }
}

void PreviewWindow::CopyTimecode() {
  if (!m_videoPreview.IsLoaded())
    return;

  std::wstring tc = m_videoPreview.GetTimecode();
  if (tc.empty())
    return;

  if (!OpenClipboard(m_hwnd))
    return;
  EmptyClipboard();

  size_t bytes = (tc.length() + 1) * sizeof(wchar_t);
  HGLOBAL hGlob = GlobalAlloc(GMEM_MOVEABLE, bytes);
  if (hGlob) {
    void *pData = GlobalLock(hGlob);
    if (pData) {
      memcpy(pData, tc.c_str(), bytes);
      GlobalUnlock(hGlob);
      SetClipboardData(CF_UNICODETEXT, hGlob);
    }
  }
  CloseClipboard();
  Utils::Log(L"[PreviewWindow] Copied timecode to clipboard: %s", tc.c_str());
}

void PreviewWindow::CopyFileName() {
  if (m_currentFile.empty())
    return;

  std::wstring fileName = m_fileInfo.name;
  if (fileName.empty()) {
    fileName = Utils::GetFileName(m_currentFile);
  }
  if (fileName.empty())
    return;

  if (!OpenClipboard(m_hwnd))
    return;
  EmptyClipboard();

  size_t bytes = (fileName.length() + 1) * sizeof(wchar_t);
  HGLOBAL hGlob = GlobalAlloc(GMEM_MOVEABLE, bytes);
  if (hGlob) {
    void *pData = GlobalLock(hGlob);
    if (pData) {
      memcpy(pData, fileName.c_str(), bytes);
      GlobalUnlock(hGlob);
      SetClipboardData(CF_UNICODETEXT, hGlob);
    }
  }
  CloseClipboard();
  Utils::Log(L"[PreviewWindow] Copied file name to clipboard: %s",
             fileName.c_str());
}

void PreviewWindow::CopyCurrentFilePath() {
  if (m_currentFile.empty())
    return;
  if (!OpenClipboard(m_hwnd))
    return;
  EmptyClipboard();

  size_t bytes = (m_currentFile.length() + 1) * sizeof(wchar_t);
  HGLOBAL hGlob = GlobalAlloc(GMEM_MOVEABLE, bytes);
  if (hGlob) {
    void *pData = GlobalLock(hGlob);
    if (pData) {
      memcpy(pData, m_currentFile.c_str(), bytes);
      GlobalUnlock(hGlob);
      SetClipboardData(CF_UNICODETEXT, hGlob);
    }
  }
  CloseClipboard();
  Utils::Log(L"[PreviewWindow] Copied path to clipboard: %s",
             m_currentFile.c_str());
}

void PreviewWindow::CopyFolderAddress() {
  if (m_currentFile.empty())
    return;

  std::wstring folder = m_fileInfo.directory;
  if (folder.empty()) {
    folder = Utils::GetDirectory(m_currentFile);
  }
  if (folder.empty())
    return;

  if (!OpenClipboard(m_hwnd))
    return;
  EmptyClipboard();

  size_t bytes = (folder.length() + 1) * sizeof(wchar_t);
  HGLOBAL hGlob = GlobalAlloc(GMEM_MOVEABLE, bytes);
  if (hGlob) {
    void *pData = GlobalLock(hGlob);
    if (pData) {
      memcpy(pData, folder.c_str(), bytes);
      GlobalUnlock(hGlob);
      SetClipboardData(CF_UNICODETEXT, hGlob);
    }
  }
  CloseClipboard();
  Utils::Log(L"[PreviewWindow] Copied folder address to clipboard: %s",
             folder.c_str());
}

void PreviewWindow::ToggleFullScreen() {
  if (!SupportsFullScreen())
    return;

  if (!m_isFullscreen) {
    GetWindowRect(m_hwnd, &m_normalWindowRect);
    HMONITOR hMon = MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {sizeof(mi)};
    if (GetMonitorInfo(hMon, &mi)) {
      m_isFullscreen = true;
      int cornerPref = 1; // DWMWCP_DONOTROUND
      DwmSetWindowAttribute(m_hwnd, 33, &cornerPref, sizeof(cornerPref));
      SetWindowPos(m_hwnd, HWND_TOPMOST, mi.rcMonitor.left, mi.rcMonitor.top,
                   mi.rcMonitor.right - mi.rcMonitor.left,
                   mi.rcMonitor.bottom - mi.rcMonitor.top,
                   SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }
  } else {
    ExitFullScreen();
  }
}

void PreviewWindow::ExitFullScreen() {
  if (m_isFullscreen) {
    m_isFullscreen = false;
    int cornerPref = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(m_hwnd, 33, &cornerPref, sizeof(cornerPref));
    if (m_normalWindowRect.right > m_normalWindowRect.left &&
        m_normalWindowRect.bottom > m_normalWindowRect.top) {
      SetWindowPos(m_hwnd, HWND_TOPMOST, m_normalWindowRect.left,
                   m_normalWindowRect.top,
                   m_normalWindowRect.right - m_normalWindowRect.left,
                   m_normalWindowRect.bottom - m_normalWindowRect.top,
                   SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    } else {
      Utils::CenterWindow(m_hwnd, m_winW, m_winH);
    }
  }
}

// ─────────────────────────────────────────────────────────
// macOS-Style Loading Spinner & Async Loading
// ─────────────────────────────────────────────────────────

void PreviewWindow::DrawLoadingSpinner(const D2D1_RECT_F &cr) {
  if (!m_rt || !m_brush)
    return;

  float cx = (cr.left + cr.right) * 0.5f;
  float cy = (cr.top + cr.bottom) * 0.5f - 12.0f;

  // 12 spokes xoay phong cách macOS Activity Indicator
  constexpr int numSpokes = 12;
  constexpr float innerR = 12.0f;
  constexpr float outerR = 24.0f;
  constexpr float strokeW = 3.5f;

  int activeIndex = static_cast<int>(m_loadingAngle / 30.0f) % numSpokes;

  for (int i = 0; i < numSpokes; ++i) {
    float angleDeg =
        static_cast<float>(i) * (360.0f / static_cast<float>(numSpokes));
    float angleRad = angleDeg * (3.14159265f / 180.0f);

    int diff = (i - activeIndex + numSpokes) % numSpokes;
    float alpha =
        0.15f + 0.85f * (static_cast<float>(diff) / static_cast<float>(numSpokes));

    m_brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, alpha));

    D2D1_POINT_2F p1 = D2D1::Point2F(cx + cosf(angleRad) * innerR,
                                    cy + sinf(angleRad) * innerR);
    D2D1_POINT_2F p2 = D2D1::Point2F(cx + cosf(angleRad) * outerR,
                                    cy + sinf(angleRad) * outerR);

    m_rt->DrawLine(p1, p2, m_brush.Get(), strokeW, m_spinnerStrokeStyle.Get());
  }

  // Dòng chữ "Loading preview..."
  if (m_smallFont) {
    m_brush->SetColor(Colors::TextDim);
    D2D1_RECT_F textR = D2D1::RectF(cr.left, cy + outerR + 16.0f, cr.right,
                                   cy + outerR + 40.0f);
    m_smallFont->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    m_rt->DrawTextW(L"Loading preview...", 18, m_smallFont.Get(), textR,
                    m_brush.Get());
    m_smallFont->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
  }
}

void PreviewWindow::StartAsyncLoad(const std::wstring &path, uint64_t gen) {
  FileType type = FileTypeDetector::Detect(path);
  HWND hwnd = m_hwnd;

  std::thread([path, gen, type, hwnd]() {
    Utils::Log(L"[PreviewWindow] StartAsyncLoad thread START for '%s', gen=%llu",
               path.c_str(), gen);
    auto res = std::make_unique<AsyncLoadResult>();
    res->generation = gen;
    res->filePath = path;
    res->fileType = type;

    if (type == FileType::Image) {
      std::wstring ext = Utils::GetFileExtension(path);
      if (RawThumbnailExtractor::IsRawExtension(ext)) {
        res->success = RawThumbnailExtractor::Extract(path, res->memoryBytes);
        res->isMemory = res->success;
      } else if (MpvImageLoader::IsMpvImageExtension(ext)) {
        // Đối với định dạng HEIC/HEIF/AVIF, không nạp thô vào memoryBytes để ImagePreview::LoadFile gọi MpvImageLoader
        res->isMemory = false;
        res->success = true;
      } else {
        // Đọc toàn bộ byte ảnh vào bộ nhớ trong luồng nền
        HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                   nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile != INVALID_HANDLE_VALUE) {
          LARGE_INTEGER size;
          if (GetFileSizeEx(hFile, &size) && size.QuadPart > 0 && size.QuadPart < 500 * 1024 * 1024) {
            res->memoryBytes.resize(static_cast<size_t>(size.QuadPart));
            DWORD bytesRead = 0;
            if (ReadFile(hFile, res->memoryBytes.data(), static_cast<DWORD>(size.QuadPart), &bytesRead, nullptr)) {
              res->memoryBytes.resize(bytesRead);
              res->isMemory = true;
              res->success = true;
            }
          }
          CloseHandle(hFile);
        }
        if (!res->success) {
          res->success = true;
        }
      }
    } else if (type == FileType::PowerPoint) {
      res->pptHasThumbnail = ArchivePreview::ExtractFile(
          path, "docProps/thumbnail.jpeg", res->memoryBytes);
      if (!res->pptHasThumbnail)
        res->pptHasThumbnail = ArchivePreview::ExtractFile(
            path, "docProps/thumbnail.jpg", res->memoryBytes);
      if (!res->pptHasThumbnail)
        res->pptHasThumbnail = ArchivePreview::ExtractFile(
            path, "docProps/thumbnail.png", res->memoryBytes);
      bool hasThumbInspect = false;
      ArchivePreview::InspectPresentation(path, res->pptSlideCount,
                                          hasThumbInspect);
      res->pptHasThumbnail = res->pptHasThumbnail || hasThumbInspect;
      res->isMemory = res->pptHasThumbnail;
      res->success = true;
    } else if (type == FileType::Text) {
      res->textContent = Utils::ReadTextFile(path);
      res->success = true;
    } else if (type == FileType::Markdown) {
      res->success = MarkdownPreview::Parse(path, res->mdBlocks);
    } else if (type == FileType::Excel) {
      res->success = ExcelPreview::ParseWorkbook(path, res->excelSheets);
    } else if (type == FileType::Archive) {
      res->success = ArchivePreview::ParseArchive(path, res->archiveData);
    } else if (type == FileType::PDF || type == FileType::Illustrator) {
      int u8Len = WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, nullptr, 0, nullptr, nullptr);
      if (u8Len > 1) {
        std::string u8Path(u8Len - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, &u8Path[0], u8Len, nullptr, nullptr);
        FPDF_DOCUMENT doc = FPDF_LoadDocument(u8Path.c_str(), nullptr);
        if (doc) {
          int pageCount = FPDF_GetPageCount(doc);
          if (pageCount > 0) {
            double ptW = 0.0, ptH = 0.0;
            FPDF_GetPageSizeByIndex(doc, 0, &ptW, &ptH);
            if (ptW <= 0 || ptH <= 0) {
              ptW = 595.0;
              ptH = 842.0;
            }
            float scale = 2.0f;
            int renderW = (std::max)(600, (std::min)(static_cast<int>(ptW * scale), 3200));
            int renderH = (std::max)(800, (std::min)(static_cast<int>(ptH * scale), 4200));
            FPDF_PAGE page = FPDF_LoadPage(doc, 0);
            if (page) {
              FPDF_BITMAP fpdfBmp = FPDFBitmap_CreateEx(renderW, renderH, FPDFBitmap_BGRA, nullptr, 0);
              if (fpdfBmp) {
                FPDFBitmap_FillRect(fpdfBmp, 0, 0, renderW, renderH, 0xFFFFFFFF);
                FPDF_RenderPageBitmap(fpdfBmp, page, 0, 0, renderW, renderH, 0, FPDF_ANNOT | FPDF_LCD_TEXT);
                void *buf = FPDFBitmap_GetBuffer(fpdfBmp);
                int stride = FPDFBitmap_GetStride(fpdfBmp);
                if (buf && stride > 0) {
                  res->pdfPixels.assign(static_cast<uint8_t *>(buf),
                                        static_cast<uint8_t *>(buf) + stride * renderH);
                  res->pdfRenderW = renderW;
                  res->pdfRenderH = renderH;
                  res->pdfStride = stride;
                }
                FPDFBitmap_Destroy(fpdfBmp);
              }
              FPDF_ClosePage(page);
            }
            res->pdfDoc = doc;
            res->pdfPageCount = pageCount;
            res->pdfPagePtW = ptW;
            res->pdfPagePtH = ptH;
            res->success = true;
          } else {
            FPDF_CloseDocument(doc);
          }
        }
      }
    } else {
      res->success = true;
    }

    Utils::Log(L"[PreviewWindow] StartAsyncLoad thread DONE for '%s', posting WM_APP_FILE_LOADED",
               path.c_str());
    PostMessageW(hwnd, WM_APP_FILE_LOADED, 0,
                 reinterpret_cast<LPARAM>(res.release()));
  }).detach();
}

void PreviewWindow::OnFileLoadedAsync(void *pResult) {
  auto *res = static_cast<AsyncLoadResult *>(pResult);
  if (!res)
    return;

  // Bỏ qua nếu cửa sổ đã bị đóng hoặc người dùng đã chuyển sang file khác
  if (!m_visible || res->generation != m_loadGeneration || res->filePath != m_currentFile) {
    return;
  }

  Utils::Log(L"[PreviewWindow] OnFileLoadedAsync called for '%s', type=%d",
             res->filePath.c_str(), static_cast<int>(res->fileType));

  // Tắt loading spinner
  m_isLoading = false;
  KillTimer(m_hwnd, TIMER_LOADING_SPINNER);

  // Nạp dữ liệu vào module tương ứng
  switch (res->fileType) {
  case FileType::Image:
    if (res->isMemory && !res->memoryBytes.empty()) {
      if (!m_imagePreview.LoadMemory(res->memoryBytes.data(),
                                    res->memoryBytes.size(), m_wicFactory.Get())) {
        m_imagePreview.LoadFile(res->filePath, m_wicFactory.Get());
      }
    } else {
      m_imagePreview.LoadFile(res->filePath, m_wicFactory.Get());
    }
    if (m_rt) {
      m_imagePreview.CreateBitmap(m_rt.Get());
      m_imagePreview.ResetView(GetContentRect());
    }
    break;

  case FileType::PowerPoint:
    m_pptHasThumbnail = res->pptHasThumbnail;
    m_pptSlideCount = res->pptSlideCount;
    if (m_pptHasThumbnail && !res->memoryBytes.empty()) {
      m_imagePreview.LoadMemory(res->memoryBytes.data(),
                                res->memoryBytes.size(), m_wicFactory.Get());
      if (m_rt) {
        m_imagePreview.CreateBitmap(m_rt.Get());
        m_imagePreview.ResetView(GetContentRect());
      }
    }
    break;

  case FileType::Text: {
    m_textContent = std::move(res->textContent);
    m_textLines.clear();
    std::wstring line;
    for (size_t i = 0; i < m_textContent.size(); i++) {
      wchar_t ch = m_textContent[i];
      if (ch == L'\r')
        continue;
      if (ch == L'\n') {
        m_textLines.push_back(std::move(line));
        line.clear();
      } else {
        line += ch;
      }
    }
    if (!line.empty())
      m_textLines.push_back(std::move(line));
    break;
  }

  case FileType::Markdown:
    m_mdPreview.SetBlocks(std::move(res->mdBlocks));
    break;

  case FileType::PDF:
  case FileType::Illustrator:
    if (res->pdfDoc) {
      m_pdfPreview.AttachLoadedDocument(
          res->pdfDoc, res->pdfPageCount, res->pdfPagePtW, res->pdfPagePtH,
          res->pdfPixels.data(), res->pdfRenderW, res->pdfRenderH,
          res->pdfStride, m_rt.Get());
      res->pdfDoc = nullptr; // Chuyển giao quyền sở hữu thành công
    } else {
      m_pdfPreview.Load(res->filePath);
      if (m_rt) {
        m_pdfPreview.CreateBitmap(m_rt.Get());
      }
    }
    m_pdfPreview.ResetView(GetContentRect());
    break;

  case FileType::Excel:
    m_excelPreview.SetSheets(res->filePath, std::move(res->excelSheets));
    break;

  case FileType::Svg:
    if (m_dc5) {
      m_svgPreview.Load(res->filePath, m_dc5.Get());
    }
    break;

  case FileType::Font:
    m_fontPreview.Load(res->filePath, m_dwriteFactory.Get());
    break;

  case FileType::Archive:
    m_archivePreview.SetData(res->filePath, std::move(res->archiveData));
    break;

  case FileType::Video:
  case FileType::Audio:
    if (m_videoPreview.Load(res->filePath)) {
      SetTimer(m_hwnd, TIMER_VIDEO_PROGRESS, 33, nullptr);
      if (!m_videoPreview.IsAudioOnly()) {
        D2D1_RECT_F cr = GetContentRect();
        RECT contentR = {static_cast<LONG>(cr.left), static_cast<LONG>(cr.top),
                         static_cast<LONG>(cr.right), static_cast<LONG>(cr.bottom)};
        m_videoPreview.Resize(contentR);
        m_videoPreview.Show();
      }
    }
    break;

  case FileType::Document:
    LoadPreviewHandler(res->filePath);
    break;

  default:
    break;
  }

  // File info nếu Unknown
  if (m_currentType == FileType::Unknown && m_rt && m_wicFactory) {
    m_fileInfoView.SetFileInfo(m_fileInfo, m_currentFile, m_wicFactory.Get(),
                               m_rt.Get());
  }

  // Tái định cỡ cửa sổ theo tỉ lệ file mới mượt mà
  SIZE sz = CalculateWindowSize();
  if (sz.cx != m_winW || sz.cy != m_winH) {
    m_winW = sz.cx;
    m_winH = sz.cy;
    Utils::CenterWindow(m_hwnd, m_winW, m_winH);
    if (m_rt) {
      m_rt->Resize(D2D1::SizeU(m_winW, m_winH));
    }
    UpdateLayout();
  }

  // Đảm bảo ResetView theo kích thước viewport mới
  if (res->fileType == FileType::Image ||
      (res->fileType == FileType::PowerPoint && m_pptHasThumbnail)) {
    m_imagePreview.ResetView(GetContentRect());
  } else if (res->fileType == FileType::PDF ||
             res->fileType == FileType::Illustrator) {
    m_pdfPreview.ResetView(GetContentRect());
  }

  InvalidateRect(m_hwnd, nullptr, FALSE);
}

void PreviewWindow::StartFadeIn() {
  m_fadeState = FadeState::In;
  m_fadeStartTime = std::chrono::steady_clock::now();
  m_currentAlpha = 0;
  SetLayeredWindowAttributes(m_hwnd, 0, 0, LWA_ALPHA);
  SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
  SetTimer(m_hwnd, TIMER_FADE, 10, nullptr);
}

void PreviewWindow::StartFadeOut() {
  if (!m_visible && m_fadeState != FadeState::In)
    return;
  m_fadeState = FadeState::Out;
  m_fadeStartTime = std::chrono::steady_clock::now();
  SetTimer(m_hwnd, TIMER_FADE, 10, nullptr);
}

