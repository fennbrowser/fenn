#pragma once

// WinUI-only loading presentation. No browser actions or provisional browser
// state: the normal browser presentation takes over on its first Show request.
namespace NativeStartupSurface {
inline constexpr wchar_t Property[] = L"Fenn.NativeStartupSurface";
struct Surface;
// Match the pane's process-lifetime storage: never release WinRT from DllMain.
inline std::unique_ptr<Surface>& Active() {
    static auto value = new std::unique_ptr<Surface>;
    return *value;
}
inline HRESULT Close(HWND hwnd);

inline bool Eligible(HWND hwnd) {
    if (!hwnd || !NativeWindowHost::Owner(hwnd)) return false;
    if (auto* host = NativeWindowHost::Get(hwnd)) return host->state.phase == 6;
    wchar_t name[64]{};
    return GetPropW(hwnd, L"Fenn.NativeStartupOrigin") == reinterpret_cast<HANDLE>(1) &&
        GetClassNameW(hwnd, name, 64) && !wcscmp(name, L"Fenn.NativeStartupBootstrap.v1");
}

struct Surface {
    HWND hwnd{};
    XamlSlot source{nullptr};
    void Resize() {
        RECT bounds{};
        if (!source || !GetClientRect(hwnd, &bounds)) return;
        source.SiteBridge().MoveAndResize({0, 0, bounds.right, bounds.bottom});
    }
    static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM w, LPARAM l,
                                 UINT_PTR, DWORD_PTR data) {
        auto* self = reinterpret_cast<Surface*>(data);
        if (message == WM_NCDESTROY) {
            Close(hwnd);
            return DefSubclassProc(hwnd, message, w, l);
        }
        try {
            if (message == WM_SIZE) self->Resize();
            // Before Gecko attaches, honor Windows' suggested physical bounds.
            if (message == WM_DPICHANGED && !NativeWindowHost::Get(hwnd)) {
                const auto& r = *reinterpret_cast<RECT*>(l);
                SetWindowPos(hwnd, nullptr, r.left, r.top, r.right-r.left, r.bottom-r.top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
        } catch (...) { /* No exceptions across a Windows callback. */ }
        return DefSubclassProc(hwnd, message, w, l);
    }
    ~Surface() {
        RemoveWindowSubclass(hwnd, Proc, reinterpret_cast<UINT_PTR>(this));
        RemovePropW(hwnd, Property);
        if (source) { try { source.Close(); } catch (...) {} }
    }
};
inline HRESULT Close(HWND hwnd) {
    if (!Active() || (hwnd && Active()->hwnd != hwnd)) return S_FALSE;
    // Clear ownership before Close can dispatch nested window messages.
    auto retiring = std::move(Active());
    return S_OK;
}
inline HRESULT Show(HWND hwnd) {
    if (Active()) return Active()->hwnd == hwnd ? S_FALSE : E_NOT_VALID_STATE;
    auto surface = std::make_unique<Surface>();
    surface->hwnd = hwnd;
    surface->source = XamlSlot::New();
    surface->source.Initialize(GetWindowIdFromWindow(hwnd));
    // English is a pre-publication fallback only. No browser-dependent commands
    // are exposed; localized normal controls replace this entire island.
    surface->source.Content(Markup::XamlReader::Load(LR"(
        <Grid xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
              Background="{ThemeResource ApplicationPageBackgroundThemeBrush}"
              AutomationProperties.AutomationId="FennStartupSurface">
          <StackPanel HorizontalAlignment="Center" VerticalAlignment="Center" Spacing="16">
            <ProgressRing IsActive="True" Width="32" Height="32"
                AutomationProperties.AutomationId="FennStartupProgress"
                AutomationProperties.Name="Loading browser"/>
            <TextBlock Text="Loading browser…" HorizontalAlignment="Center"
                Foreground="{ThemeResource TextFillColorPrimaryBrush}"/>
          </StackPanel>
        </Grid>)").as<UIElement>());
    check_bool(SetWindowSubclass(hwnd, Surface::Proc, reinterpret_cast<UINT_PTR>(surface.get()),
                                 reinterpret_cast<DWORD_PTR>(surface.get())));
    check_bool(SetPropW(hwnd, Property, reinterpret_cast<HANDLE>(1)));
    surface->Resize();
    surface->source.SiteBridge().Show();
    Active() = std::move(surface);
    // Keep normal system caption controls; do not steal foreground focus.
    ShowWindow(hwnd, GetPropW(hwnd,L"Fenn.NativeStartupMaximized") ? SW_SHOWMAXIMIZED : SW_SHOWNOACTIVATE);
    return S_OK;
}
}
