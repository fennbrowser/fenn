#pragma once
#include <windows.graphics.effects.interop.h>
#include <winrt/Windows.Graphics.Effects.h>
#include <d2d1effects.h>

// A small native effect description avoids adding a Win2D runtime dependency.
// Only the decorative light fields enter this effect; never text or controls.
struct TabGlowDiffusion : winrt::implements<TabGlowDiffusion,
    winrt::Windows::Graphics::Effects::IGraphicsEffect,
    winrt::Windows::Graphics::Effects::IGraphicsEffectSource,
    ABI::Windows::Graphics::Effects::IGraphicsEffectD2D1Interop> {
    winrt::hstring name{L"Diffusion"};
    winrt::Windows::Graphics::Effects::IGraphicsEffectSource source{
        winrt::Microsoft::UI::Composition::CompositionEffectSourceParameter(L"Backdrop")};
    winrt::hstring Name() const { return name; }
    void Name(winrt::hstring const& value) { name = value; }
    HRESULT __stdcall GetEffectId(GUID* value) noexcept override {
        if (!value) return E_POINTER;
        // CLSID_D2D1GaussianBlur, from d2d1effects.h.
        *value = winrt::guid{L"1feb6d69-2fe6-4ac9-8c58-1d7f93e7a6a5"};
        return S_OK;
    }
    HRESULT __stdcall GetNamedPropertyMapping(LPCWSTR, UINT*,
        ABI::Windows::Graphics::Effects::GRAPHICS_EFFECT_PROPERTY_MAPPING*) noexcept override { return E_INVALIDARG; }
    HRESULT __stdcall GetPropertyCount(UINT* count) noexcept override {
        if (!count) return E_POINTER;
        *count = 3; return S_OK;
    }
    HRESULT __stdcall GetProperty(UINT index, ABI::Windows::Foundation::IPropertyValue** value) noexcept override {
        if (!value) return E_POINTER;
        *value = nullptr;
        try {
            using winrt::Windows::Foundation::PropertyValue;
            winrt::Windows::Foundation::IInspectable property{nullptr};
            switch (index) {
            case D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION: property = PropertyValue::CreateSingle(3.f); break;
            case D2D1_GAUSSIANBLUR_PROP_OPTIMIZATION: property = PropertyValue::CreateUInt32(D2D1_GAUSSIANBLUR_OPTIMIZATION_BALANCED); break;
            case D2D1_GAUSSIANBLUR_PROP_BORDER_MODE: property = PropertyValue::CreateUInt32(D2D1_BORDER_MODE_SOFT); break;
            default: return E_INVALIDARG;
            }
            *value = reinterpret_cast<ABI::Windows::Foundation::IPropertyValue*>(
                winrt::detach_abi(property.as<winrt::Windows::Foundation::IPropertyValue>()));
            return S_OK;
        } catch (...) { return winrt::to_hresult(); }
    }
    HRESULT __stdcall GetSourceCount(UINT* count) noexcept override {
        if (!count) return E_POINTER;
        *count = 1; return S_OK;
    }
    HRESULT __stdcall GetSource(UINT index, ABI::Windows::Graphics::Effects::IGraphicsEffectSource** value) noexcept override {
        if (!value) return E_POINTER;
        *value = nullptr;
        if (index) return E_INVALIDARG;
        auto copy = source;
        *value = reinterpret_cast<ABI::Windows::Graphics::Effects::IGraphicsEffectSource*>(winrt::detach_abi(copy));
        return S_OK;
    }
};
