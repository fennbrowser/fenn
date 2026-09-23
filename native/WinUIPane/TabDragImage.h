#pragma once
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Windows.Storage.Streams.h>

// The sole visual for a dragged tab, for the whole gesture -- not just the
// tear-off/detach region. TabStrip.h's UpdateDragVisual only ever recomputes
// reorder/detaching state now; it never moves or shows the dragged row's
// own RenderTransform. This is a plain window this codebase fully owns
// (created/positioned/destroyed by Pane.cpp's TabDragSession) -- no OLE, no
// shell drag-image API, in keeping with the drag redesign's move away from
// anything with its own hidden WinUI-owned behavior.
namespace TabDragImage {

// Use the installed TabViewItem template's own floating surface, including its
// theme/high-contrast resources and all four rounded corners. Stock dragging is
// disabled in this host, so snapshot capture must enter this visual state itself.
// This is a localized template-state dependency, not a second tab template.
inline void SetFloatingAppearance(winrt::Microsoft::UI::Xaml::Controls::TabViewItem const& row, bool floating) {
    winrt::Microsoft::UI::Xaml::VisualStateManager::GoToState(
        row, floating ? L"DragDropVisualVisible" : L"DragDropVisualNotVisible", false);
}

// The hover/pressed TabContainer sits above the floating background and the
// stock template binds its radius through TopCornerRadiusFilterConverter.
// Use the full native radius only while capturing; retain the original binding
// (or local value) so normal styling remains live after capture or failure.
struct CaptureCorners {
    winrt::Microsoft::UI::Xaml::Controls::Grid container{nullptr};
    winrt::Microsoft::UI::Xaml::Data::BindingBase binding{nullptr};
    winrt::Windows::Foundation::IInspectable local{nullptr};

    explicit CaptureCorners(winrt::Microsoft::UI::Xaml::Controls::TabViewItem const& row) {
        using namespace winrt::Microsoft::UI::Xaml;
        auto root = Media::VisualTreeHelper::GetChild(row, 0).as<FrameworkElement>();
        container = root.FindName(L"TabContainer").try_as<Controls::Grid>();
        if (!container) return;
        auto property = Controls::Grid::CornerRadiusProperty();
        if (auto expression = container.GetBindingExpression(property)) binding = expression.ParentBinding();
        local = container.ReadLocalValue(property);
        container.CornerRadius(row.CornerRadius());
    }
    ~CaptureCorners() {
        if (!container) return;
        try {
            auto property = winrt::Microsoft::UI::Xaml::Controls::Grid::CornerRadiusProperty();
            if (binding) container.SetBinding(property, binding);
            else if (local == winrt::Microsoft::UI::Xaml::DependencyProperty::UnsetValue()) container.ClearValue(property);
            else container.SetValue(property, local);
        } catch (...) { /* The island may have been torn down during capture. */ }
    }
    CaptureCorners(CaptureCorners const&) = delete;
    CaptureCorners& operator=(CaptureCorners const&) = delete;
};

// A coroutine, not a blocking call: RenderTargetBitmap::RenderAsync/
// GetPixelsAsync are real async WinRT operations, and blocking on them
// (a plain .get()) from the UI thread would deadlock -- their own
// completion needs that same thread's dispatcher to pump, which a
// blocking wait prevents. Encodes the resulting HBITMAP as a uint64_t
// since raw handles aren't directly usable as an IAsyncOperation's result
// type; the caller casts it back. Pixels come out top-down BGRA8 with
// premultiplied alpha (RenderTargetBitmap's own convention, same as this
// codebase's existing favicon decoding elsewhere), which is exactly what
// PaintDragVisual's UpdateLayeredWindow call below expects.
inline winrt::Windows::Foundation::IAsyncOperation<uint64_t> RenderElementToBitmapAsync(
    winrt::Microsoft::UI::Xaml::UIElement element, int* outWidth, int* outHeight) {
    using namespace winrt::Microsoft::UI::Xaml::Media::Imaging;
    RenderTargetBitmap rtb;
    co_await rtb.RenderAsync(element);
    auto pixels = co_await rtb.GetPixelsAsync();
    *outWidth = rtb.PixelWidth();
    *outHeight = rtb.PixelHeight();
    std::vector<uint8_t> raw(pixels.Length());
    winrt::Windows::Storage::Streams::DataReader::FromBuffer(pixels).ReadBytes(raw);
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = *outWidth;
    bmi.bmiHeader.biHeight = -*outHeight;  // top-down, matching RenderTargetBitmap's own row order
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits{};
    HBITMAP bitmap = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bitmap && bits && raw.size() >= size_t(*outWidth) * *outHeight * 4) memcpy(bits, raw.data(), raw.size());
    co_return uint64_t(bitmap);
}

// Paints `bitmap` (top-down 32bpp premultiplied BGRA, as RenderElementToBitmapAsync
// produces) into `hwnd` via UpdateLayeredWindow -- real per-pixel alpha, the
// same mechanism Explorer's own file-drag thumbnails use, and simpler than a
// WM_PAINT handler: it pushes pixels directly, no repaint message needed.
// `hwnd` must already have WS_EX_LAYERED (set once at creation; confirmed in
// this session's own earlier investigation to work for a plain WS_POPUP --
// creation only failed for WS_EX_LAYERED combined with WS_CHILD). `topLeft`
// is in screen coordinates.
inline void PaintDragVisual(HWND hwnd, HBITMAP bitmap, int width, int height, POINT topLeft) {
    if (!bitmap) return;
    HDC screenDc = GetDC(nullptr);
    HDC memDc = CreateCompatibleDC(screenDc);
    HBITMAP oldBitmap = static_cast<HBITMAP>(SelectObject(memDc, bitmap));
    SIZE size{width, height};
    POINT srcPoint{0, 0};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(hwnd, screenDc, &topLeft, &size, memDc, &srcPoint, 0, &blend, ULW_ALPHA);
    SelectObject(memDc, oldBitmap);
    DeleteDC(memDc);
    ReleaseDC(nullptr, screenDc);
}

}  // namespace TabDragImage
