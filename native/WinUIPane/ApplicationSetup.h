#pragma once
#include <windows.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>

// Called once by the application owner, before creating any browser windows.
// The desktop shell and rollback DLL share resources, not dispatcher ownership.
namespace ApplicationSetup {
inline void Configure(const winrt::Microsoft::UI::Xaml::Application& app) {
    using namespace winrt;
    using namespace Microsoft::UI::Xaml;
    // Closing the last browser window must not implicitly end the host loop.
    app.DispatcherShutdownMode(DispatcherShutdownMode::OnExplicitShutdown);
    app.Resources().MergedDictionaries().Append(Controls::XamlControlsResources());
    // TabView otherwise defaults to 240; Firefox caps tabs at 225 CSS pixels.
    app.Resources().Insert(box_value(L"TabViewItemMaxWidth"), box_value(225.0));
    // Keep the documented COM/static-destructor exit safeguard in both hosts.
    // Application ownership alone does not prove deferred controls releases safe.
    HMODULE controls{};
    if (GetModuleHandleW(L"Microsoft.UI.Xaml.Controls.dll"))
        check_bool(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN,
                                     L"Microsoft.UI.Xaml.Controls.dll", &controls));
}
}
