#pragma once
#include "ShellLinkPresentation.h"
#include "../../bridge/WinUIPaneApi.h"
#include <functional>
#include <vector>
#include <winrt/base.h>

// Private owning-thread interface between the desktop shell and the shared pane.
namespace ShellPresentation {
struct Draft {
    std::wstring text;
    int selection{}, length{};
    bool focused{};
};
struct ShareIdentity {
    uint64_t session{}, document{};
};

HRESULT ShowInteractiveStartup(HWND, const wchar_t* profile, const wchar_t* presentation) noexcept;
HRESULT PrepareInteractiveStartup(HWND, const wchar_t* profile, const wchar_t* presentation) noexcept;
void InstallKeyRouting(HWND);
HRESULT ReportedFocusAddress(HWND, BOOL selectAll) noexcept;
HRESULT ReportedSyncTabs(HWND, const WinUITab*, uint32_t count, uint32_t selected, uint32_t revision) noexcept;
HRESULT PostedSyncAddress(HWND, const wchar_t* input) noexcept;
std::vector<HWND> PaneWindows();
HRESULT DestroyPane(HWND) noexcept;

bool HasAddress(HWND);
bool AddressSessionReady(HWND);
bool CaptureDraft(HWND, Draft&);
bool RestoreDraft(HWND, const Draft&, bool& sessionReady);
void FocusEditor(HWND);
ShareIdentity ShareScope(HWND);
bool ShareCurrent(HWND, const uint8_t*, size_t);
HRESULT OpenShare(HWND, const winrt::hstring& title, const winrt::hstring& url, const std::function<bool()>& current);

void SetRestartProcess(HANDLE) noexcept;
void ApplyFrame(const ShellFrameView&, WinUIPaneNotifyFn);
void PrepareAndNotify(HWND) noexcept;
}  // namespace ShellPresentation
