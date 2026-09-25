#pragma once
#include <d2d1.h>
#include <windows.graphics.interop.h>
#include <winrt/Windows.Graphics.h>
#pragma comment(lib, "d2d1.lib")

// Native geometry adapter for a post-effect clip; no raster surface or Win2D.
struct TabAmbientClip : winrt::implements<TabAmbientClip,
    winrt::Windows::Graphics::IGeometrySource2D, ABI::Windows::Graphics::IGeometrySource2DInterop> {
    winrt::com_ptr<ID2D1PathGeometry> geometry;
    explicit TabAmbientClip(winrt::com_ptr<ID2D1PathGeometry> value) : geometry(std::move(value)) {}
    HRESULT __stdcall GetGeometry(ID2D1Geometry** value) noexcept override {
        if (!value) return E_POINTER;
        *value = geometry.get(); (*value)->AddRef(); return S_OK;
    }
    HRESULT __stdcall TryGetGeometryUsingFactory(ID2D1Factory*, ID2D1Geometry** value) noexcept override {
        if (!value) return E_POINTER;
        *value = nullptr; return S_OK;
    }
};
