#pragma once
#include <cmath>
#include <cstdint>
#include <algorithm>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Windows.Storage.Streams.h>

// Shared validation/decoding for the JSON snapshots crossing the Firefox/WinUI
// boundary (address, tab menu, tab strip). Every value here originates from an
// untrusted-shaped payload: validate fully before it reaches XAML or is trusted
// as an identity. Kept independent of any single surface's struct so it can be
// included before them and reused without one surface depending on another's
// internals.
namespace NativeJson {
    using JsonObject = winrt::Windows::Data::Json::JsonObject;

    // Matches Number.MAX_SAFE_INTEGER, the same bound Firefox's *.sys.mjs
    // session code enforces on revision/generation counters before they cross
    // the boundary. Kept as a double end to end; a uint32 is not wide enough
    // and these values are never used as array sizes or loop bounds.
    constexpr double kMaxSafeInteger = 9007199254740991.0;

    // Limits Firefox's *.sys.mjs session code enforces on the same fields
    // before sending them, so a mismatch here would reject valid input:
    // - kMaxTabs: WinUITabStripSession rows/groups, WinUITabMenuSession targets/event.tabs
    // - kMaxListEntries: WinUIUrlbarSession mode entries and result ids
    // - kMaxFieldText: WinUIUrlbarSession address text
    // - kMaxMenuModels: WinUITabMenuSession distinct menu shapes
    constexpr size_t kMaxTabs = 10000;
    constexpr size_t kMaxListEntries = 100;
    constexpr size_t kMaxFieldText = 32768;
    constexpr size_t kMaxMenuModels = 128;

    // Native-only defense-in-depth caps: Firefox's JS does not independently
    // bound these individual field lengths, only aggregate array sizes and
    // total serialized bytes. Grouped by observed role, not by JSON key name
    // -- some fields share a key name ("label", "title") across surfaces but
    // carry different limits, and are intentionally not merged here.
    constexpr size_t kMaxShortLabel = 1024;  // menu/mode/group captions, hints
    constexpr size_t kMaxDetailText = 2048;  // per-tab detail/status, group description
    constexpr size_t kMaxOpaqueId = 256;     // address row/action/mode-entry ids

    // A nonzero window-local identity or count (tab id, model index, version).
    inline uint32_t RequireId(JsonObject const& object, wchar_t const* key) {
        auto n = object.GetNamedNumber(key);
        if (!std::isfinite(n) || n < 1 || n > UINT32_MAX || std::floor(n) != n)
            throw winrt::hresult_invalid_argument();
        return uint32_t(n);
    }

    // A monotonic revision/generation counter. Zero is valid (it marks "no
    // state yet"); unlike RequireId this never becomes a uint32.
    inline double RequireRevision(JsonObject const& object, wchar_t const* key) {
        auto n = object.GetNamedNumber(key);
        if (!std::isfinite(n) || n < 0 || n > kMaxSafeInteger || std::floor(n) != n)
            throw winrt::hresult_invalid_argument();
        return n;
    }

    inline void RequireLength(winrt::hstring const& value, size_t max) {
        if (value.size() > max) throw winrt::hresult_invalid_argument();
    }

    // Bounds on one JSON-encoded hex-pixel image, folded into a running total
    // so a snapshot cannot spend an unbounded byte budget across many images.
    struct ImageLimits { double maxDimension; size_t maxTotalPixels; };

    inline void ValidateImage(JsonObject const& item, size_t& totalPixels, ImageLimits const& limits) {
        if (!item.HasKey(L"image") ||
            item.GetNamedValue(L"image").ValueType() == winrt::Windows::Data::Json::JsonValueType::Null) return;
        const auto data = item.GetNamedObject(L"image");
        const double w = data.GetNamedNumber(L"width"), h = data.GetNamedNumber(L"height");
        if (w < 1 || h < 1 || w > limits.maxDimension || h > limits.maxDimension ||
            w != int(w) || h != int(h)) throw winrt::hresult_invalid_argument();
        const auto pixels = data.GetNamedString(L"pixels");
        totalPixels += size_t(w) * size_t(h);
        if (totalPixels > limits.maxTotalPixels || pixels.size() != size_t(w) * size_t(h) * 8 ||
            std::any_of(pixels.begin(), pixels.end(), [](wchar_t c) { return !((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f')); }))
            throw winrt::hresult_invalid_argument();
    }

    // Decodes a "width"/"height"/"pixels" object already checked by ValidateImage.
    inline Microsoft::UI::Xaml::Media::Imaging::WriteableBitmap DecodeImage(JsonObject const& data) {
        Microsoft::UI::Xaml::Media::Imaging::WriteableBitmap bitmap(
            int(data.GetNamedNumber(L"width")), int(data.GetNamedNumber(L"height")));
        auto bytes = bitmap.PixelBuffer().data();
        auto pixels = data.GetNamedString(L"pixels");
        auto nibble = [](wchar_t c) { return c <= L'9' ? c - L'0' : c - L'a' + 10; };
        for (uint32_t j = 0; j < pixels.size() / 2; ++j)
            bytes[j] = uint8_t(nibble(pixels[j * 2]) * 16 + nibble(pixels[j * 2 + 1]));
        bitmap.Invalidate();
        return bitmap;
    }
}
