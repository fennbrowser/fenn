#pragma once
#include <shlobj.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Windows.Storage.Streams.h>

// Shell drag image for the region a drag covers that no registered OLE
// drop target ever sees DragOver for -- off any open pane's own window
// entirely, i.e. exactly the tear-off/detach region (see
// the tab drag redesign's "Drag visual" section: the
// existing RenderTransform-follows-cursor visual, driven from DragOver,
// already covers everywhere a target exists; this covers where none does).
// Verified to integrate cleanly (this exact render-then-InitializeFromBitmap
// shape, with S_OK throughout) before being wired into the real drag path.
namespace TabDragImage {

// A coroutine, not a blocking call: RenderTargetBitmap::RenderAsync/
// GetPixelsAsync are real async WinRT operations, and blocking on them
// (a plain .get()) from the UI thread would deadlock -- their own
// completion needs that same thread's dispatcher to pump, which a
// blocking wait prevents. Encodes the resulting HBITMAP as a uint64_t
// since raw handles aren't directly usable as an IAsyncOperation's result
// type; the caller casts it back.
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

// Renders `element` and attaches it to `dataObject` as the OS's own
// shell-drawn drag image via IDragSourceHelper, so DRAGDROP_S_USEDEFAULTCURSORS
// (TabDragDrop::DropSource::GiveFeedback) has an actual image to draw
// instead of a bare cursor. Best-effort only, by design: any failure along
// the way (render error, no drag-drop helper available) just means the
// shell falls back to its own default cursor -- the caller doesn't need to
// treat that as fatal to the drag itself.
inline winrt::Windows::Foundation::IAsyncAction AttachDragImageAsync(
    winrt::Microsoft::UI::Xaml::UIElement element, IDataObject* dataObject) {
    int width{}, height{};
    uint64_t bitmapValue = co_await RenderElementToBitmapAsync(element, &width, &height);
    HBITMAP bitmap = reinterpret_cast<HBITMAP>(bitmapValue);
    if (!bitmap) co_return;
    winrt::com_ptr<IDragSourceHelper> helper;
    if (FAILED(CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(helper.put())))) {
        DeleteObject(bitmap);
        co_return;
    }
    SHDRAGIMAGE image{};
    image.sizeDragImage = {width, height};
    image.ptOffset = {width / 2, height / 2};
    image.hbmpDragImage = bitmap;  // ownership transfers to the helper on success
    image.crColorKey = CLR_NONE;
    if (FAILED(helper->InitializeFromBitmap(&image, dataObject))) DeleteObject(bitmap);
}

}  // namespace TabDragImage
