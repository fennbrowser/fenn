#pragma once
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Windows.Storage.Streams.h>

// A cursor-following visual for the tear-off/detach region of a drag -- the
// one place the on-strip RenderTransform-follows-cursor visual
// (TabStrip.h's UpdateDragVisual) has nothing to drive it, since that's
// only ever fed a point while the drag is still over some strip. This is a
// plain window this codebase fully owns (created/positioned/destroyed by
// Pane.cpp's TabDragSession) -- no OLE, no shell drag-image API, matching
// the tab drag redesign's move away from anything with its
// own hidden WinUI-owned behavior.
namespace TabDragImage {

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
