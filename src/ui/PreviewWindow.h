#pragma once

#include "Utils.h"
#include "FileTypeDetector.h"
#include "ImagePreview.h"
#include "FileInfoView.h"
#include "VideoPreview.h"
#include "MarkdownPreview.h"
#include "PdfPreview.h"
#include "ArchivePreview.h"
#include "SvgPreview.h"
#include "FontPreview.h"
#include "ExcelPreview.h"
#include "Resource.h"
#include <d2d1_3.h>
#include <shobjidl.h>
#include <chrono>

#define TIMER_VIDEO_PROGRESS 3

/// Main preview popup window.
/// Manages the lifecycle of all preview modules and renders the chrome (title bar, footer).
class PreviewWindow {
public:
    PreviewWindow();
    ~PreviewWindow();

    /// Create the hidden popup window and initialize D2D/DWrite/WIC factories
    bool Create(HINSTANCE hInstance);

    /// Show preview for the given file (creates/resizes window, loads content)
    void ShowPreview(const std::wstring& filePath);

    /// Hide and cleanup the preview
    void HidePreview();

    /// Switch to another file smoothly while preview is already visible
    void SwitchFile(const std::wstring& filePath);

    bool IsVisible() const { return m_visible; }
    HWND GetHwnd()   const { return m_hwnd; }

private:
    // ── Window Procedure ──
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    // ── Initialization ──
    bool InitFactories();
    bool CreateRenderTarget();
    void DiscardRenderTarget();
    bool CreateTextFormats();

    // ── Rendering ──
    void Render();
    void DrawTitleBar();
    void DrawFooter();
    void DrawContentImage();
    void DrawContentText();
    void DrawContentMarkdown();
    void DrawContentPdf();
    void DrawContentAudio();
    void DrawContentArchive();
    void DrawContentSvg();
    void DrawContentFont();
    void DrawContentExcel();
    void DrawContentPowerPoint();
    void DrawContentFileInfo();
    void DrawScrollbar();
    void DrawVideoControls();
    void DrawPdfControls();

    // ── File Loading ──
    void LoadFile(const std::wstring& path);
    void CleanupPreview();

    // ── Preview Handler (COM) ──
    CLSID FindPreviewHandlerCLSID(const std::wstring& extension);
    bool  LoadPreviewHandler(const std::wstring& path);
    void  DestroyPreviewHandler();

    // ── Layout ──
    void UpdateLayout();
    SIZE CalculateWindowSize();
    D2D1_RECT_F GetContentRect() const;

    // ── System Tray ──
    void AddTrayIcon();
    void RemoveTrayIcon();
    void ShowTrayMenu();

    // ── Context Menu & Full Screen Actions ──
    void ShowContextMenu();
    void ShowVideoContextMenu() { ShowContextMenu(); }
    void SaveVideoSnapshot();
    void CopyTimecode();
    void CopyFileName();
    void CopyCurrentFilePath();
    void CopyFolderAddress();
    bool SupportsFullScreen() const;
    void ToggleFullScreen();
    void ExitFullScreen();

    // ── Hardware Fade Animation ──
    void StartFadeIn();
    void StartFadeOut();

    // ── Async Loading & macOS Spinner ──
    void DrawLoadingSpinner(const D2D1_RECT_F& contentRect);
    void StartAsyncLoad(const std::wstring& path, uint64_t gen);
    void OnFileLoadedAsync(void* pResult);

    // ── Window State ──
    HWND       m_hwnd       = nullptr;
    HWND       m_handlerWnd = nullptr;  // child HWND for IPreviewHandler
    HWND       m_lastActiveShellWnd = nullptr; // Cửa sổ Explorer/Desktop trước khi mở preview
    HINSTANCE  m_hInst      = nullptr;
    bool       m_visible    = false;
    bool       m_trayAdded  = false;
    bool       m_isFullscreen = false;
    RECT       m_normalWindowRect = {};

    // ── D2D / DWrite / WIC Factories ──
    ComPtr<ID2D1Factory>        m_d2dFactory;
    ComPtr<IDWriteFactory>      m_dwriteFactory;
    ComPtr<IWICImagingFactory>  m_wicFactory;

    // ── Render Resources ──
    ComPtr<ID2D1HwndRenderTarget> m_rt;
    ComPtr<ID2D1DeviceContext5>    m_dc5;
    ComPtr<ID2D1SolidColorBrush>  m_brush;
    ComPtr<ID2D1StrokeStyle>      m_spinnerStrokeStyle;
    ComPtr<IDWriteTextFormat>     m_titleFont;
    ComPtr<IDWriteTextFormat>     m_bodyFont;
    ComPtr<IDWriteTextFormat>     m_smallFont;
    ComPtr<IDWriteTextFormat>     m_monoFont;
    ComPtr<IDWriteTextFormat>     m_lineNumFont;
    ComPtr<ID2D1Bitmap>           m_appLogoBitmap;
    void EnsureAppLogoLoaded();

    // ── Preview Modules ──
    ImagePreview     m_imagePreview;
    FileInfoView     m_fileInfoView;
    VideoPreview     m_videoPreview;
    MarkdownPreview  m_mdPreview;
    PdfPreview       m_pdfPreview;
    ArchivePreview   m_archivePreview;
    SvgPreview       m_svgPreview;
    FontPreview      m_fontPreview;
    ExcelPreview     m_excelPreview;

    // ── PowerPoint Specific State ──
    bool m_pptHasThumbnail = false;
    int  m_pptSlideCount   = 0;

    // ── Preview Handler (COM host) ──
    ComPtr<IPreviewHandler> m_previewHandler;

    // ── Current File State ──
    std::wstring       m_currentFile;
    std::wstring       m_lastPreviewedFile;
    FileType           m_currentType   = FileType::Unknown;
    Utils::FileInfo    m_fileInfo;

    // ── Text Preview State ──
    std::wstring              m_textContent;
    std::vector<std::wstring> m_textLines;
    float                     m_scrollY      = 0.0f;
    float                     m_totalTextH   = 0.0f;

    // ── UI State ──
    bool m_closeHover = false;
    int  m_winW = 0;
    int  m_winH = 0;

    // ── Video Controls State ──
    bool        m_videoPlayHover = false;
    bool        m_videoMuteHover = false;
    bool        m_videoSeekHover = false;
    bool        m_videoSeeking   = false;
    D2D1_RECT_F m_videoPlayRect  = {};
    D2D1_RECT_F m_videoMuteRect  = {};
    D2D1_RECT_F m_videoSeekRect  = {};

    // ── PDF Controls State ──
    bool        m_pdfPrevHover   = false;
    bool        m_pdfNextHover   = false;
    D2D1_RECT_F m_pdfPrevRect    = {};
    D2D1_RECT_F m_pdfNextRect    = {};


    // ── Hardware Fade Animation State ──
    enum class FadeState { None, In, Out };
    FadeState                             m_fadeState      = FadeState::None;
    std::chrono::steady_clock::time_point m_fadeStartTime;
    BYTE                                  m_currentAlpha   = 255;

    // ── Async Loading & Spinner State ──
    bool                                  m_isLoading      = false;
    float                                 m_loadingAngle   = 0.0f;
    uint64_t                              m_loadGeneration = 0;
};
