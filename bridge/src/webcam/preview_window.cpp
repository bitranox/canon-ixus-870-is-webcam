#include "preview_window.h"

#ifdef _WIN32

#include <windows.h>
#include <cstring>
#include <vector>

namespace webcam {

static const char* const WND_CLASS_NAME = "CHDKWebcamPreview";
static bool s_class_registered = false;

// Zoom delta accumulator — set by wnd_proc, read by get_zoom_delta
static int s_zoom_delta = 0;

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_ERASEBKGND:
        return 1; // suppress flicker
    case WM_KEYDOWN:
        if (wp == VK_OEM_PLUS || wp == VK_ADD)
            s_zoom_delta += 1;
        else if (wp == VK_OEM_MINUS || wp == VK_SUBTRACT)
            s_zoom_delta -= 1;
        return 0;
    case WM_MOUSEWHEEL:
        {
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
            if (delta > 0) s_zoom_delta += 1;
            else if (delta < 0) s_zoom_delta -= 1;
        }
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

struct PreviewWindow::Impl {
    HWND hwnd = nullptr;
    bool open = false;
    std::string base_title;

    // DIB info for StretchDIBits (RGB24 top-down)
    BITMAPINFO bmi{};
    int frame_w = 0;
    int frame_h = 0;
};

PreviewWindow::PreviewWindow() : impl_(std::make_unique<Impl>()) {}
PreviewWindow::~PreviewWindow() { shutdown(); }

bool PreviewWindow::init(const PreviewConfig& config) {
    HINSTANCE hinst = GetModuleHandleA(nullptr);

    if (!s_class_registered) {
        WNDCLASSA wc{};
        wc.lpfnWndProc = wnd_proc;
        wc.hInstance = hinst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        wc.lpszClassName = WND_CLASS_NAME;
        if (!RegisterClassA(&wc)) return false;
        s_class_registered = true;
    }

    // Calculate window rect from desired client size
    RECT rc = { 0, 0, config.width, config.height };
    DWORD style = WS_OVERLAPPEDWINDOW;
    AdjustWindowRect(&rc, style, FALSE);

    impl_->hwnd = CreateWindowExA(
        0, WND_CLASS_NAME, config.title.c_str(), style,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, hinst, nullptr);

    if (!impl_->hwnd) return false;

    ShowWindow(impl_->hwnd, SW_SHOW);
    UpdateWindow(impl_->hwnd);
    impl_->open = true;
    impl_->base_title = config.title;

    // Prepare BITMAPINFO header (filled in fully on first frame)
    memset(&impl_->bmi, 0, sizeof(impl_->bmi));
    impl_->bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    impl_->bmi.bmiHeader.biPlanes = 1;
    impl_->bmi.bmiHeader.biBitCount = 24;
    impl_->bmi.bmiHeader.biCompression = BI_RGB;

    return true;
}

void PreviewWindow::show_frame(const uint8_t* rgb_data, int width, int height, int stride) {
    if (!impl_->open || !impl_->hwnd) return;

    // Update BITMAPINFO and window title if frame dimensions changed
    if (width != impl_->frame_w || height != impl_->frame_h) {
        impl_->bmi.bmiHeader.biWidth = width;
        // Negative height = top-down DIB (our RGB data is top-down)
        impl_->bmi.bmiHeader.biHeight = -height;
        impl_->frame_w = width;
        impl_->frame_h = height;

        std::string title = impl_->base_title + " (" +
            std::to_string(width) + "x" + std::to_string(height) + ")";
        SetWindowTextA(impl_->hwnd, title.c_str());
    }

    // Convert RGB to BGR in-place-style via a temp buffer
    // GDI expects BGR byte order
    int bgr_stride = (width * 3 + 3) & ~3; // DWORD-aligned
    std::vector<uint8_t> bgr(bgr_stride * height);

    for (int y = 0; y < height; y++) {
        const uint8_t* src = rgb_data + y * stride;
        uint8_t* dst = bgr.data() + y * bgr_stride;
        for (int x = 0; x < width; x++) {
            dst[x * 3 + 0] = src[x * 3 + 2]; // B
            dst[x * 3 + 1] = src[x * 3 + 1]; // G
            dst[x * 3 + 2] = src[x * 3 + 0]; // R
        }
    }

    RECT rc;
    GetClientRect(impl_->hwnd, &rc);
    int dst_w = rc.right - rc.left;
    int dst_h = rc.bottom - rc.top;

    HDC hdc = GetDC(impl_->hwnd);
    SetStretchBltMode(hdc, HALFTONE);
    StretchDIBits(hdc,
        0, 0, dst_w, dst_h,          // destination rect
        0, 0, width, height,          // source rect
        bgr.data(), &impl_->bmi,
        DIB_RGB_COLORS, SRCCOPY);
    ReleaseDC(impl_->hwnd, hdc);
}

bool PreviewWindow::pump_messages() {
    MSG msg;
    while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            impl_->open = false;
            return false;
        }
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return impl_->open;
}

int PreviewWindow::get_zoom_delta() {
    int d = s_zoom_delta;
    s_zoom_delta = 0;
    return d;
}

void PreviewWindow::shutdown() {
    if (impl_->hwnd) {
        DestroyWindow(impl_->hwnd);
        impl_->hwnd = nullptr;
    }
    impl_->open = false;
}

bool PreviewWindow::is_open() const {
    return impl_->open;
}

} // namespace webcam

#else // !_WIN32

namespace webcam {

struct PreviewWindow::Impl { bool open = false; };
PreviewWindow::PreviewWindow() : impl_(std::make_unique<Impl>()) {}
PreviewWindow::~PreviewWindow() = default;
bool PreviewWindow::init(const PreviewConfig&) { return false; }
void PreviewWindow::show_frame(const uint8_t*, int, int, int) {}
bool PreviewWindow::pump_messages() { return false; }
int PreviewWindow::get_zoom_delta() { return 0; }
void PreviewWindow::shutdown() {}
bool PreviewWindow::is_open() const { return false; }

} // namespace webcam

#endif
