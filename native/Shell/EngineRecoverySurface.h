#pragma once

namespace Product {
struct EngineTerminalPanel {
    Controls::Grid root{nullptr};
    Controls::Button retry{nullptr};
    Controls::TextBlock waiting{nullptr};
    FrameworkElement::SizeChanged_revoker resized;
};
inline std::map<HWND, EngineTerminalPanel> terminalPanels;
inline Windows::Data::Json::JsonObject terminalPresentation{nullptr};
inline void RememberEnginePresentation() {
    bool rtl{};
    terminalPresentation =
        NativeStartupLabels::Resolve(presentation.empty() ? nullptr : presentation.c_str(), profile.c_str(), rtl);
}
inline hstring EngineLabel(const wchar_t* key, const wchar_t* fallback) {
    if (terminalPresentation) {
        auto labels = terminalPresentation.GetNamedObject(L"labels");
        if (labels.HasKey(key)) return labels.GetNamedString(key);
    }
    return hstring(fallback);
}
inline void ClearEngineTerminal() {
    for (const auto& [window, panel] : terminalPanels) {
        if (auto* host = NativeWindowHost::Get(window)) {
            host->root.Children().Clear();
            NativeWindowHost::GateChrome(host);
            RemovePropW(window, L"Fenn.EngineRecoveryTerminal");
        }
    }
    terminalPanels.clear();
}
inline void RefreshEngineTerminal() {
    if (!engineTerminal) return;
    const auto now = GetTickCount64();
    const auto status = shell_recovery_retry_status(engineBudget, now);
    for (auto it = terminalPanels.begin(); it != terminalPanels.end();) {
        if (!IsWindow(it->first)) {
            it = terminalPanels.erase(it);
            continue;
        }
        it->second.retry.IsEnabled(status.eligible != 0);
        it->second.waiting.Visibility(status.eligible ? Visibility::Collapsed : Visibility::Visible);
        ++it;
    }
    if (!SurvivingWindows()) {
        app.Exit();
        return;
    }
    if (status.episode && status.available_at > now)
        SetTimer(dispatcher, 5, UINT((std::min)(status.available_at - now, uint64_t(USER_TIMER_MAXIMUM))), nullptr);
}
inline void ShowEngineTerminal() {
    const auto status = shell_recovery_retry_status(engineBudget, GetTickCount64());
    for (const auto& saved : survivors) {
        if (saved.identity.kind != 1 || !IsWindow(saved.window) || terminalPanels.contains(saved.window)) continue;
        const HWND window = saved.window;
        auto* host = NativeWindowHost::Get(window);
        if (!host) continue;
        EngineTerminalPanel panel;
        panel.root =
            Markup::XamlReader::Load(
                LR"(<Grid xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" Background="{ThemeResource LayerFillColorDefaultBrush}" AutomationProperties.AutomationId="FennEngineRecovery">
          <StackPanel Margin="32,48,32,32" MaxWidth="520" VerticalAlignment="Center" Spacing="16">
            <TextBlock x:Name="Title" xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml" Style="{ThemeResource TitleTextBlockStyle}" TextWrapping="Wrap"/>
            <TextBlock x:Name="Message" xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml" TextWrapping="Wrap"/>
            <TextBox x:Name="Draft" xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml" IsReadOnly="True" TextWrapping="Wrap" MaxHeight="160" AutomationProperties.AutomationId="FennRecoveryDraft"/>
            <TextBlock x:Name="Waiting" xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml" TextWrapping="Wrap" AutomationProperties.AutomationId="FennRecoveryWaiting"/>
            <StackPanel Orientation="Horizontal" Spacing="8">
              <Button x:Name="Close" xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml" AutomationProperties.AutomationId="FennRecoveryClose"/>
              <Button x:Name="Retry" xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml" AutomationProperties.AutomationId="FennRecoveryRetry"/>
            </StackPanel>
          </StackPanel></Grid>)")
                .as<Controls::Grid>();
        panel.root.FlowDirection(terminalPresentation && terminalPresentation.GetNamedBoolean(L"rtl", false)
                                     ? FlowDirection::RightToLeft
                                     : FlowDirection::LeftToRight);
        panel.root.FindName(L"Title").as<Controls::TextBlock>().Text(
            EngineLabel(L"engineTitle", L"The browser could not recover"));
        panel.root.FindName(L"Message")
            .as<Controls::TextBlock>()
            .Text(EngineLabel(L"engineMessage", L"Retry to recover all open windows, or close this window."));
        auto draft = panel.root.FindName(L"Draft").as<Controls::TextBox>();
        const auto draftLabel = EngineLabel(L"engineDraft", L"Unsent address text");
        draft.Header(box_value(draftLabel));
        Automation::AutomationProperties::SetName(draft, draftLabel);
        draft.FlowDirection(FlowDirection::LeftToRight);
        draft.TextReadingOrder(TextReadingOrder::DetectFromContent);
        draft.Text(saved.hasDraft ? hstring(saved.draft.text) : hstring{});
        draft.Visibility(saved.hasDraft ? Visibility::Visible : Visibility::Collapsed);
        panel.waiting = panel.root.FindName(L"Waiting").as<Controls::TextBlock>();
        panel.waiting.Text(
            EngineLabel(L"engineWaiting", L"Retry will be available shortly. Recovery will not start automatically."));
        panel.retry = panel.root.FindName(L"Retry").as<Controls::Button>();
        panel.retry.Content(box_value(EngineLabel(L"engineRetry", L"Retry")));
        panel.retry.IsEnabled(false);
        const auto episode = status.episode, id = saved.identity.id;
        panel.retry.Click([window, episode, id](auto const&, auto const&) {
            const auto owner = std::find_if(survivors.begin(), survivors.end(), [&](const auto& item) {
                return item.window == window && item.identity.id == id && item.identity.kind == 1;
            });
            if (owner != survivors.end() && IsWindow(window)) RetryEngine(episode);
        });
        auto close = panel.root.FindName(L"Close").as<Controls::Button>();
        close.Content(box_value(EngineLabel(L"engineClose", L"Close window")));
        close.Click([window](auto const&, auto const&) {
            if (IsWindow(window)) PostMessageW(window, WM_CLOSE, 0, 0);
        });
        host->root.Children().Clear();
        host->root.Children().Append(panel.root);
        panel.root.Width(host->root.ActualWidth());
        panel.root.Height(host->root.ActualHeight());
        panel.resized = host->root.SizeChanged(
            auto_revoke, [weak = make_weak(panel.root)](auto const&, SizeChangedEventArgs const& args) {
                if (auto root = weak.get()) {
                    root.Width(args.NewSize().Width);
                    root.Height(args.NewSize().Height);
                }
            });
        NativeWindowHost::GateChrome(host).IsEnabled(true);
        terminalPanels.emplace(window, std::move(panel));
        if (GetForegroundWindow() == window) close.Focus(FocusState::Programmatic);
    }
}
}  // namespace Product
