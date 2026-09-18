#pragma once
#include <ole2.h>
#include <optional>

// Minimal OLE drag-drop plumbing for the tab drag redesign -- see
// the tab drag redesign. Carries just enough to identify a
// drag's source window and tab; reorder/adopt/detach decisions live in
// NativeTabStrip (TabStrip.h) and Pane (Pane.cpp), not here.
namespace TabDragDrop {

struct Payload {
    HWND sourceHwnd;
    uint32_t tabId;
};

inline uint32_t Format() {
    static uint32_t format = RegisterClipboardFormatW(L"FirefoxWinUITabDrag");
    return format;
}

inline FORMATETC Etc() {
    return {CLIPFORMAT(Format()), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
}

// One private format, handed over unconditionally at construction: nothing
// ever needs to negotiate a different one from this object.
struct DataObject : winrt::implements<DataObject, IDataObject> {
    Payload payload;
    explicit DataObject(Payload p) : payload(p) {}

    HRESULT GetData(FORMATETC* format, STGMEDIUM* medium) {
        if (!format || !medium || format->cfFormat != CLIPFORMAT(Format()) || !(format->tymed & TYMED_HGLOBAL))
            return DV_E_FORMATETC;
        HGLOBAL global = GlobalAlloc(GMEM_MOVEABLE, sizeof(Payload));
        if (!global) return E_OUTOFMEMORY;
        *static_cast<Payload*>(GlobalLock(global)) = payload;
        GlobalUnlock(global);
        *medium = {};
        medium->tymed = TYMED_HGLOBAL;
        medium->hGlobal = global;
        return S_OK;
    }
    HRESULT GetDataHere(FORMATETC*, STGMEDIUM*) { return E_NOTIMPL; }
    HRESULT QueryGetData(FORMATETC* format) {
        return format && format->cfFormat == CLIPFORMAT(Format()) ? S_OK : DV_E_FORMATETC;
    }
    HRESULT GetCanonicalFormatEtc(FORMATETC*, FORMATETC* out) {
        if (out) *out = {};
        return DATA_S_SAMEFORMATETC;
    }
    HRESULT SetData(FORMATETC*, STGMEDIUM*, BOOL) { return E_NOTIMPL; }
    // Never called by this codebase's own drop targets -- they know the one
    // format to ask for directly -- so stubbed rather than implementing a
    // real IEnumFORMATETC for a format this object never varies.
    HRESULT EnumFormatEtc(DWORD, IEnumFORMATETC**) { return E_NOTIMPL; }
    HRESULT DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT DUnadvise(DWORD) { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT EnumDAdvise(IEnumSTATDATA**) { return OLE_E_ADVISENOTSUPPORTED; }
};

// Reads a Payload back out of an IDataObject via a plain
// QueryGetData/GetData round trip. Returns nullopt for a foreign drag (a
// file, a URL, plain text) -- callers must reject that, not misread it as a
// tab id.
inline std::optional<Payload> Extract(IDataObject* data) {
    if (!data) return std::nullopt;
    FORMATETC etc = Etc();
    if (FAILED(data->QueryGetData(&etc))) return std::nullopt;
    STGMEDIUM medium{};
    if (FAILED(data->GetData(&etc, &medium))) return std::nullopt;
    Payload result{};
    if (auto* locked = static_cast<Payload*>(GlobalLock(medium.hGlobal))) {
        result = *locked;
        GlobalUnlock(medium.hGlobal);
    }
    ReleaseStgMedium(&medium);
    return result;
}

// The gesture's entire lifetime is decided here: Escape cancels, releasing
// the left button ends it as a drop (whether anything actually accepted it
// is read from DoDragDrop's own output effect afterward, not tracked here).
// GiveFeedback defers to the shell's default cursors; a shell-drawn drag
// image (IDragSourceHelper) is later work -- see the design doc's
// sequencing step 4.
struct DropSource : winrt::implements<DropSource, IDropSource> {
    HRESULT QueryContinueDrag(BOOL escapePressed, DWORD keyState) {
        if (escapePressed) return DRAGDROP_S_CANCEL;
        if (!(keyState & MK_LBUTTON)) return DRAGDROP_S_DROP;
        return S_OK;
    }
    HRESULT GiveFeedback(DWORD) { return DRAGDROP_S_USEDEFAULTCURSORS; }
};

}  // namespace TabDragDrop
