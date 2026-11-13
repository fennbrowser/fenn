#pragma once

namespace Product {
struct SurvivorWindow {
    HWND window{};
    ShellRecoveryWindow identity{};
    ShellPresentation::Draft draft;
    bool hasDraft{};
};
struct PendingEngineWindow {
    std::vector<uint8_t> create, publication;
    std::unique_ptr<ShellCore, decltype(&shell_core_destroy)> core{shell_core_create(nullptr), shell_core_destroy};
    ShellRecoveryWindowIdentity identity{};
    uint32_t expected{};
    bool placeholder{};
};
inline std::vector<SurvivorWindow> survivors;
inline std::map<uint32_t, PendingEngineWindow> restoredWindows;
inline std::set<uint32_t> suppressedWindows;
inline std::unique_ptr<EnginePipe::ProcessLaunch> engineLaunch;
inline std::thread connectionCleanup;
inline std::atomic<bool> cleanupReady{};
inline ShellRecovery* engineBudget{};
inline ShellRecoveryHistory engineHistory{};
inline void ImportEngineRecoveryHistory() {
    if (connection && connection->TakeRecoveryHistory(engineHistory)) {
        if (!engineBudget) engineBudget = shell_recovery_create();
        check_bool(shell_recovery_import(engineBudget, engineHistory.engine, GetTickCount64()));
    }
}
inline bool engineTerminal{}, reconciled{}, emptyRequested{};
inline size_t expectedRecoveryWindows{};
inline DWORD engineFailureCode{};
inline ULONGLONG engineDeadline{}, exitDeadline{};

inline bool EngineRecoveryEnabled() {
    wchar_t value[2]{}, recovery[2]{};
    return GetEnvironmentVariableW(L"FENN_NATIVE_ENGINE_RECOVERY", value, 2) == 1 && value[0] == L'1' &&
           GetEnvironmentVariableW(L"FENN_NATIVE_SHELL_RECOVERY", recovery, 2) == 1 && recovery[0] == L'1';
}
inline bool SurvivingWindows() {
    return std::any_of(survivors.begin(), survivors.end(),
                       [](const auto& item) { return item.identity.kind == 1 && IsWindow(item.window); });
}
inline LRESULT CALLBACK RecoverySurvivorClosed(HWND window, UINT message, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    if (message == WM_NCDESTROY) {
        for (auto& item : survivors)
            if (item.window == window) {
                item.identity.kind = 3;
                item.draft.text.clear();
                item.hasDraft = false;
            }
        RemoveWindowSubclass(window, RecoverySurvivorClosed, 1);
        // CreateProcess can fail before a pipe exists, so last-close teardown
        // must not depend on receiving a transport-disconnect wake.
        PostMessageW(dispatcher, EngineCleanupDone, 0, 0);
    }
    return DefSubclassProc(window, message, w, l);
}
inline void SuppressRestored(uint32_t serial) {
    if (suppressedWindows.insert(serial).second)
        PaneLink::ShellSendSerial(serial, SHELL_FRAME_LIFETIME, SHELL_FRAME_LIFETIME_RECOVERY_SUPPRESS, 0, nullptr, 0);
}
void ShowEngineTerminal();
void RefreshEngineTerminal();
void ClearEngineTerminal();
void RetryEngine(uint64_t episode);
inline void EngineTerminal() {
    engineTerminal = true;
    KillTimer(dispatcher, 4);
    if (GetEnvironmentVariableW(L"FENN_PANE_DIAGNOSTICS", nullptr, 0))
        for (const auto& item : survivors)
            if (item.identity.kind == 1 && IsWindow(item.window)) {
                SetPropW(item.window, L"Fenn.EngineRecoveryTerminal", reinterpret_cast<HANDLE>(1));
                SetPropW(item.window, L"Fenn.EngineRecoveryError",
                         reinterpret_cast<HANDLE>(uintptr_t(engineFailureCode ? engineFailureCode
                                                            : connection      ? connection->Error()
                                                                              : 0)));
                SetPropW(item.window, L"Fenn.EngineRecoveryPeer",
                         reinterpret_cast<HANDLE>(uintptr_t(connection ? connection->PeerPid() : 0)));
                SetPropW(item.window, L"Fenn.EngineRecoveryGeneration",
                         reinterpret_cast<HANDLE>(uintptr_t(generation)));
            }
    if (engineBudget) shell_recovery_event(engineBudget, 6, GetTickCount64());
    if (connection && connection->PeerHandle() && !connection->PeerExited())
        TerminateProcess(connection->PeerHandle(), ERROR_PROCESS_ABORTED);
    KillTimer(dispatcher, 3);
    SetTimer(dispatcher, 4, 50, nullptr);
    EnginePipe::Trace("engine-recovery-terminal");
}
inline std::wstring QuoteEngineArgument(const std::wstring& value) {
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (wchar_t c : value) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        result.append(slashes * (c == L'"' ? 2 : 1), L'\\');
        slashes = 0;
        if (c == L'"') result += L'\\';
        result += c;
    }
    result.append(slashes * 2, L'\\');
    result += L'"';
    return result;
}
inline std::vector<wchar_t> EngineEnvironment(const std::wstring& descriptor) {
    LPWCH block = GetEnvironmentStringsW();
    check_bool(block != nullptr);
    std::vector<std::wstring> entries;
    for (const wchar_t* item = block; *item; item += wcslen(item) + 1) {
        if (!_wcsnicmp(item, L"FENN_ENGINE_RECONNECT=", 22) || !_wcsnicmp(item, L"FENN_ENGINE_RECOVERED=", 22))
            continue;
        entries.emplace_back(item);
    }
    FreeEnvironmentStringsW(block);
    entries.push_back(L"FENN_ENGINE_RECONNECT=" + descriptor);
    entries.push_back(L"FENN_ENGINE_RECOVERED=1");
    std::sort(entries.begin(), entries.end(),
              [](const auto& a, const auto& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
    std::vector<wchar_t> result;
    for (const auto& entry : entries) {
        result.insert(result.end(), entry.begin(), entry.end());
        result.push_back(0);
    }
    result.push_back(0);
    return result;
}
inline void RetainEngineWindows() {
    auto windows = ShellPresentation::PaneWindows();
    // Binding can precede pane creation. An interrupted replacement must retire
    // those endpoints too, or terminal Close still targets its dead engine.
    for (const auto& item : survivors)
        if (IsWindow(item.window)) windows.push_back(item.window);
    for (const auto& item : readiness) windows.push_back(item.first);
    std::sort(windows.begin(), windows.end());
    windows.erase(std::unique(windows.begin(), windows.end()), windows.end());
    for (HWND window : windows) {
        auto* host = NativeWindowHost::Get(window);
        if (!host || !host->endpoint) continue;
        auto old = std::find_if(survivors.begin(), survivors.end(),
                                [&](const auto& item) { return item.window == window && item.identity.kind == 1; });
        ShellRecoveryWindowIdentity identity{};
        const auto known = shell_core_recovery_identity(host->core.get(), host->state.generation, 0, 0, nullptr, 0,
                                                        &identity) == SHELL_STATUS_OK;
        if ((known && identity.privacy != 1) || (!known && old == survivors.end())) {
            ShellPresentation::DestroyPane(window);
            NativeWindowHost::Destroy(window);
            continue;
        }
        // The disabled replacement editor does not own a still-pending draft.
        // Keep the surviving window's original identity and text until its
        // readiness commit transfers that text to a live address session.
        SurvivorWindow saved{};
        if (old != survivors.end())
            saved = *old;
        else {
            saved.window = window;
            check_bool(shell_core_endpoint(host->core.get(), &saved.identity.id) == SHELL_STATUS_OK);
            saved.identity.kind = 1;
            memcpy(saved.identity.key, identity.key, 16);
        }
        if (!saved.hasDraft) saved.hasDraft = ShellPresentation::CaptureDraft(window, saved.draft);
        EnginePipe::Trace("engine-draft-captured", uint32_t(saved.identity.id), saved.hasDraft);
        if (GetEnvironmentVariableW(L"FENN_PANE_DIAGNOSTICS", nullptr, 0)) {
            SetPropW(window, L"Fenn.RecoveryDraftCaptured", reinterpret_cast<HANDLE>(uintptr_t(saved.hasDraft + 1)));
            RemovePropW(window, L"Fenn.RecoveryDraftApplied");
        }
        // Detaching XAML clears/disables the old editor, but an external UIA
        // client can still hold its ValuePattern while this shell survives.
        // Obtain WinUI's native wrapper before destroying the control; revoke
        // it only after removal, so reentrant discovery cannot republish it.
        // The wrapper weakly references its peer. Keep the peer alive until
        // UIAutomationCore has finished querying/disconnecting the wrapper.
        using RetiredProvider = std::pair<Microsoft::UI::Xaml::Automation::Peers::AutomationPeer,
                                          winrt::com_ptr<::IRawElementProviderSimple>>;
        std::vector<RetiredProvider> retiredProviders;
        std::set<IUnknown*> seenPeers;
        auto retainPeer = [&](auto&& visit,
                              Microsoft::UI::Xaml::Automation::Peers::AutomationPeer const& peer) -> void {
            using namespace Microsoft::UI::Xaml::Automation::Peers;
            if (peer && seenPeers.insert(winrt::get_unknown(peer)).second) {
                // A peer can exist without an exposed native UIA wrapper (for
                // example in a second window no client inspected). Only that
                // absence is optional; other QI/disconnection failures surface.
                winrt::com_ptr<::IRawElementProviderFragment> fragment;
                const auto queried = winrt::get_unknown(peer)->QueryInterface(__uuidof(::IRawElementProviderFragment),
                                                                              fragment.put_void());
                if (queried != E_NOINTERFACE) check_hresult(queried);
                if (fragment) retiredProviders.emplace_back(peer, fragment.as<::IRawElementProviderSimple>());
                // Items controls expose data-item peers that have no matching
                // visual element. A held SelectionItem refers to that wrapper.
                if (auto children = peer.GetChildren())
                    for (auto const& child : children) visit(visit, child);
            }
        };
        auto retainProviders = [&](auto&& visit, Microsoft::UI::Xaml::DependencyObject const& node) -> void {
            using namespace Microsoft::UI::Xaml::Automation::Peers;
            if (auto element = node.try_as<Microsoft::UI::Xaml::UIElement>())
                retainPeer(retainPeer, FrameworkElementAutomationPeer::FromElement(element));
            for (int index = 0; index < Microsoft::UI::Xaml::Media::VisualTreeHelper::GetChildrenCount(node); ++index)
                visit(visit, Microsoft::UI::Xaml::Media::VisualTreeHelper::GetChild(node, index));
        };
        // Include exposed selection/action peers, not only the address TextBox.
        // Detached popup trees retain their separately approved retirement rule.
        for (auto const& child : host->root.Children()) retainProviders(retainProviders, child);
        check_hresult(NativeWindowHost::RetireDeadEndpoint(window, connection->PeerHandle()));
        check_hresult(ShellPresentation::DestroyPane(window));
        host->root.Children().Clear();
        for (auto const& retiredProvider : retiredProviders)
            check_hresult(UiaDisconnectProvider(retiredProvider.second.get()));
        check_hresult(NativeWindowHost::PrepareReplacementEndpoint(window));
        if (old == survivors.end())
            survivors.push_back(std::move(saved));
        else
            *old = std::move(saved);
        check_bool(SetWindowSubclass(window, RecoverySurvivorClosed, 1, 0));
    }
}
inline void QueueEngineReplacement() {
    ShellPresentation::SetRestartProcess(nullptr);
    NativeWindowHost::RemoteEnginePid = 0;
    PaneLink::SetProcessShellTransport(nullptr, nullptr);
    for (auto*& link : links) {
        shell_link_destroy(link);
        link = nullptr;
    }
    cleanupReady = false;
    connectionCleanup = std::thread([old = std::move(connection)]() mutable {
        old->Stop();
        old.reset();
        cleanupReady = true;
        PostMessageW(dispatcher, EngineCleanupDone, 0, 0);
    });
    engineDeadline = GetTickCount64() + 30000;
    SetTimer(dispatcher, 4, 100, nullptr);
}
inline bool RecoverEngine() {
    ImportEngineRecoveryHistory();
    if (!EngineRecoveryEnabled()) return false;
    if (engineTerminal) {
        if (!SurvivingWindows()) app.Exit();
        return true;
    }
    if (connectionCleanup.joinable() || engineLaunch) return true;
    // A replacement which never confirmed its startup is a failed attempt,
    // not a browser-authorized normal quit (even if its launcher exits zero).
    if (engineRestoring && !confirmed) {
        EngineTerminal();
        return true;
    }
    if (!connection->PeerExited()) {
        if (!exitDeadline) exitDeadline = GetTickCount64() + 2000;
        if (GetTickCount64() >= exitDeadline) return false;
        SetTimer(dispatcher, 4, 100, nullptr);
        return true;
    }
    exitDeadline = 0;
    DWORD exitCode{};
    // Firefox's successful shutdown is terminal, even if its final window
    // retirements were still in flight when the pipe closed.
    if (GetExitCodeProcess(connection->PeerHandle(), &exitCode) && exitCode == 0) return false;
    if (!engineBudget) engineBudget = shell_recovery_create();
    if (!engineBudget) return false;
    RetainEngineWindows();
    engineRestoring = true;
    reconciled = false;
    emptyRequested = false;
    expectedRecoveryWindows = 0;
    restoredWindows.clear();
    suppressedWindows.clear();
    readiness.clear();
    deferredAddressFocus.clear();
    softwareSurfaces.clear();
    KillTimer(dispatcher, 3);
    if (!SurvivingWindows()) {
        shell_recovery_event(engineBudget, 2, 0);
        app.Exit();
        return true;
    }
    if (profile.empty() || !shell_recovery_event(engineBudget, 5, GetTickCount64()) ||
        !shell_recovery_event(engineBudget, 0, GetTickCount64())) {
        EngineTerminal();
        return true;
    }
    QueueEngineReplacement();
    EnginePipe::Trace("engine-recovery-revoke", 0, uint32_t(survivors.size()));
    return true;
}
inline void EngineRecoveryTick() {
    // Last-window closure cancels supervision even while CreateProcess is
    // pending. Exit joins/cancels the owned launch in StopEngineRecovery;
    // waiting for Done first could create a browser after its last owner left.
    if (engineRestoring && !SurvivingWindows()) {
        if (engineBudget) shell_recovery_event(engineBudget, 2, 0);
        app.Exit();
        return;
    }
    ImportEngineRecoveryHistory();
    if (engineTerminal) {
        if (!SurvivingWindows()) {
            app.Exit();
            return;
        }
        if (engineLaunch) {
            if (!engineLaunch->Done()) return;
            engineLaunch.reset();
        }
        if (connectionCleanup.joinable()) {
            if (!cleanupReady) return;
            connectionCleanup.join();
        }
        if (connection && connection->PeerHandle() && !connection->PeerExited()) return;
        RetainEngineWindows();
        readiness.clear();
        deferredAddressFocus.clear();
        softwareSurfaces.clear();
        ShowEngineTerminal();
        KillTimer(dispatcher, 4);
        RefreshEngineTerminal();
        return;
    }
    if (connectionCleanup.joinable()) {
        if (!cleanupReady) return;
        connectionCleanup.join();
        if (!SurvivingWindows()) {
            app.Exit();
            return;
        }
        if (generation == UINT32_MAX) {
            EngineTerminal();
            return;
        }
        const auto nextGeneration = generation + 1;
        std::wstring name;
        winrt::handle server{EnginePipe::Server(name, generation)};
        if (!server || server.get() == INVALID_HANDLE_VALUE) {
            EngineTerminal();
            return;
        }
        generation = nextGeneration;
        confirmed = false;
        recoveryLaunch = true;
        startup = nullptr;
        engineFailureCode = 0;
        links[0] = EnginePipe::Link(generation);
        links[1] = EnginePipe::Link(generation);
        check_bool(links[0] && links[1]);
        PaneLink::SetProcessShellTransport(links[0], PipeSend);
        connection = std::make_unique<EnginePipe::Connection>();
        auto advertisedGeneration = generation;
        auto executable = EnginePipe::Sibling(L"fenn.exe");
#ifdef FENN_SHELL_TESTING
        const bool faultReleased = std::any_of(survivors.begin(), survivors.end(), [](const auto& saved) {
            return IsWindow(saved.window) && GetPropW(saved.window, L"Fenn.TestReleaseEngineFailure");
        });
        if (!faultReleased && ProcessFault(L"engine-launch-failure")) executable += L".missing";
        if (!faultReleased && ProcessFault(L"engine-auth-failure"))
            advertisedGeneration = generation == UINT32_MAX ? 1 : generation + 1;
#endif
        const std::wstring descriptor = L"1:" + std::to_wstring(GetCurrentProcessId()) + L":" +
                                        std::to_wstring(advertisedGeneration) + L":" + name.substr(name.size() - 32);
        const auto command = QuoteEngineArgument(executable) + L" --wait-for-browser --new-instance --profile " +
                             QuoteEngineArgument(profile);
        check_bool(shell_recovery_export(engineBudget, GetTickCount64(), &engineHistory.engine));
        engineLaunch = std::make_unique<EnginePipe::ProcessLaunch>(*connection, std::move(server), executable, command,
                                                                   generation, dispatcher, nullptr, 0,
                                                                   EngineEnvironment(descriptor), true, engineHistory);
        EnginePipe::Trace("engine-recovery-launch", 0, generation);
    }
    if (engineLaunch) {
        if (!engineLaunch->Done()) return;
        const auto result = engineLaunch->Result();
        enginePid = engineLaunch->Pid();
        engineLaunch.reset();
        if (FAILED(result)) {
            engineFailureCode = DWORD(result);
            EngineTerminal();
            return;
        }
        if (connection->Ready()) {
            enginePid = connection->PeerPid();
            NativeWindowHost::RemoteEnginePid = enginePid;
            ShellPresentation::SetRestartProcess(connection->PeerHandle());
        }
    }
    if (!engineRestoring) {
        if (connection && connection->Closed()) Pump();
        return;
    }
    if (!SurvivingWindows()) {
        EngineTerminal();
        app.Exit();
        return;
    }
    if (connection && connection->Closed()) {
        Pump();
        return;
    }
    // The episode deadline bounds launch, inventory and endpoint binding.
    // Once those finish, per-window Rust barriers own active painting time;
    // their approved covered/minimized pause must not expire this engine.
    bool awaitingBind = false;
    if (reconciled)
        for (const auto& [serial, pending] : restoredWindows) {
            uintptr_t handle{};
            if (shell_link_handle(links[0], serial, &handle) == SHELL_STATUS_OK) {
                auto* host = NativeWindowHost::Get(reinterpret_cast<HWND>(handle));
                if (host && !host->endpoint) {
                    awaitingBind = true;
                    break;
                }
            }
        }
    if ((!reconciled || awaitingBind || !suppressedWindows.empty()) && GetTickCount64() > engineDeadline) {
        EngineTerminal();
        return;
    }
    if (reconciled && readiness.empty() && suppressedWindows.empty()) {
        const bool waiting = std::any_of(survivors.begin(), survivors.end(), [](const auto& item) {
            auto* host = NativeWindowHost::Get(item.window);
            return item.identity.kind == 1 && host && !host->endpoint;
        });
        if (!waiting) {
            shell_recovery_event(engineBudget, 1, 0);
            engineRestoring = false;
            survivors.clear();
            restoredWindows.clear();
            KillTimer(dispatcher, 4);
            EnginePipe::Trace("engine-recovery-ready", 0, generation);
        }
    }
}
inline bool EngineDraftEditorReady(HWND window) {
    if (!engineRestoring) return true;
    const auto saved = std::find_if(survivors.begin(), survivors.end(),
                                    [&](const auto& item) { return item.window == window && item.identity.kind == 1; });
    if (saved == survivors.end() || !saved->hasDraft) return true;
    // A disabled startup snapshot succeeds even before the editor exists.
    // A retained edit needs a live address session before readiness can commit.
    return ShellPresentation::AddressSessionReady(window);
}
inline void RestoreEngineDraft(HWND window) {
    if (!engineRestoring) return;
    auto saved =
        std::find_if(survivors.begin(), survivors.end(), [&](const auto& item) { return item.window == window; });
    if (saved == survivors.end() || !saved->hasDraft) return;
    bool sessionReady{};
    if (!ShellPresentation::RestoreDraft(window, saved->draft, sessionReady)) {
        EnginePipe::Trace("engine-draft-no-editor");
        return;
    }
    EnginePipe::Trace("engine-draft-applied", uint32_t(saved->identity.id), sessionReady ? 1 : 0);
    if (GetEnvironmentVariableW(L"FENN_PANE_DIAGNOSTICS", nullptr, 0))
        SetPropW(window, L"Fenn.RecoveryDraftApplied", reinterpret_cast<HANDLE>(uintptr_t(sessionReady ? 2 : 1)));
    if (saved->draft.focused && GetForegroundWindow() == window) ShellPresentation::FocusEditor(window);
    saved->draft.text.clear();
    saved->hasDraft = false;
}
inline bool EngineRecoveryFrame(const ShellFrameView& view) {
    if (engineTerminal) return true;
    if (engineRestoring && view.channel == SHELL_FRAME_LIFETIME && view.kind == SHELL_FRAME_LIFETIME_DESTROY_HOST &&
        suppressedWindows.erase(view.window)) {
        shell_link_retire(links[0], view.window);
        restoredWindows.erase(view.window);
        return true;
    }
    if (!engineRestoring || reconciled) return false;
    if (view.channel == SHELL_FRAME_LIFETIME && view.kind == SHELL_FRAME_LIFETIME_CREATE) {
        check_bool(confirmed && restoredWindows.size() < 128);
        auto [entry, inserted] = restoredWindows.try_emplace(view.window);
        check_bool(inserted && entry->second.core);
        entry->second.create.assign(view.payload, view.payload + view.length);
        return true;
    }
    const auto found = restoredWindows.find(view.window);
    if (found == restoredWindows.end() || view.channel != SHELL_FRAME_TAB_MENU) return false;
    const auto value = Windows::Data::Json::JsonObject::Parse(PaneLink::Text(view.payload, view.length));
    if (!value.HasKey(L"recoveryWindow")) return true;
    check_bool(view.length <= 512 && value.Size() == 3 && value.GetNamedNumber(L"version") == 2 &&
               value.GetNamedBoolean(L"publishOnly"));
    const auto identity = value.GetNamedObject(L"recoveryWindow");
    const auto revision = identity.GetNamedNumber(L"revision"), privacy = identity.GetNamedNumber(L"privacy"),
               count = identity.GetNamedNumber(L"restoreCount");
    const auto key = identity.GetNamedString(L"key");
    const bool placeholder = identity.GetNamedBoolean(L"placeholder", false);
    check_bool(identity.Size() == (placeholder ? 5 : 4) && (!placeholder || key.empty()) && revision >= 1 &&
               revision <= 9007199254740991.0 && floor(revision) == revision && privacy == 1 && count >= 0 &&
               count <= 128 && floor(count) == count);
    auto& pending = found->second;
    uint64_t endpoint{};
    check_bool(shell_core_endpoint(pending.core.get(), &endpoint) == SHELL_STATUS_OK);
    check_bool(shell_core_recovery_identity(pending.core.get(), endpoint, uint64_t(revision), 1,
                                            reinterpret_cast<const uint16_t*>(key.c_str()), key.size(),
                                            &pending.identity) == SHELL_STATUS_OK);
    pending.placeholder = placeholder;
    pending.expected = uint32_t(count);
    pending.publication.assign(view.payload, view.payload + view.length);
    if (!count || restoredWindows.size() != (emptyRequested ? expectedRecoveryWindows : size_t(count))) return true;
    std::vector<ShellRecoveryWindow> old, newWindows;
    for (auto& item : survivors) {
        if (!IsWindow(item.window)) item.identity.kind = 3;
        old.push_back(item.identity);
    }
    for (const auto& [serial, item] : restoredWindows) {
        if ((!emptyRequested && item.expected != count) ||
            (!item.placeholder && !std::any_of(std::begin(item.identity.key), std::end(item.identity.key),
                                               [](uint8_t b) { return b != 0; })))
            return true;
        ShellRecoveryWindow window{};
        window.id = serial;
        window.kind = item.placeholder ? 4 : 1;
        memcpy(window.key, item.identity.key, 16);
        newWindows.push_back(window);
    }
    std::vector<ShellRecoveryWindowEffect> effects(old.size() + newWindows.size());
    size_t size{};
    check_bool(shell_recovery_windows(generation, old.data(), old.size(), newWindows.data(), newWindows.size(),
                                      effects.data(), effects.size(), &size));
    std::vector<uint8_t> emptyKeys;
    for (size_t i = 0; i < size; ++i) {
        if (effects[i].kind == 1 || effects[i].kind == 4 || effects[i].kind == 5) continue;
        if (effects[i].kind != 2 || emptyRequested) {
            EngineTerminal();
            return true;
        }
        const auto saved = std::find_if(survivors.begin(), survivors.end(),
                                        [&](const auto& item) { return item.identity.id == effects[i].survivor; });
        check_bool(saved != survivors.end());
        emptyKeys.insert(emptyKeys.end(), std::begin(saved->identity.key), std::end(saved->identity.key));
    }
    if (!emptyKeys.empty()) {
        expectedRecoveryWindows = restoredWindows.size() + emptyKeys.size() / 16;
        if (expectedRecoveryWindows > 128) {
            EngineTerminal();
            return true;
        }
        emptyRequested = true;
        PaneLink::ShellSendSerial(view.window, SHELL_FRAME_LIFETIME, SHELL_FRAME_LIFETIME_RECOVERY_EMPTY, 0,
                                  emptyKeys.data(), emptyKeys.size());
        EnginePipe::Trace("engine-recovery-empty-request", view.window, uint32_t(emptyKeys.size() / 16));
        return true;
    }
    reconciled = true;
    applyingRecovered = true;
    try {
        for (size_t i = 0; i < size; ++i) {
            const auto& effect = effects[i];
            auto& item = restoredWindows.at(uint32_t(effect.restored));
            if (effect.kind == 4) {
                SuppressRestored(uint32_t(effect.restored));
                continue;
            }
            recoveredTarget = nullptr;
            if (effect.kind == 1) {
                auto saved = std::find_if(survivors.begin(), survivors.end(),
                                          [&](const auto& w) { return w.identity.id == effect.survivor; });
                if (saved == survivors.end() || saved->identity.kind != 1 || !IsWindow(saved->window)) {
                    SuppressRestored(uint32_t(effect.restored));
                    continue;
                }
                recoveredTarget = saved->window;
            }
            ShellFrameView frame{};
            frame.window = uint32_t(effect.restored);
            frame.channel = SHELL_FRAME_LIFETIME;
            frame.kind = SHELL_FRAME_LIFETIME_CREATE;
            frame.payload = item.create.data();
            frame.length = item.create.size();
            Apply(frame);
            frame.channel = SHELL_FRAME_TAB_MENU;
            frame.kind = 0;
            frame.payload = item.publication.data();
            frame.length = item.publication.size();
            Apply(frame);
        }
    } catch (...) {
        applyingRecovered = false;
        recoveredTarget = nullptr;
        throw;
    }
    applyingRecovered = false;
    return true;
}
inline void RetryEngine(uint64_t episode) {
    if (!engineTerminal || !SurvivingWindows() || engineLaunch || connectionCleanup.joinable() || !connection ||
        (connection->PeerHandle() && !connection->PeerExited()) ||
        !shell_recovery_retry(engineBudget, episode, GetTickCount64()))
        return;
    ClearEngineTerminal();
    KillTimer(dispatcher, 5);
    engineTerminal = false;
    engineRestoring = true;
    reconciled = false;
    emptyRequested = false;
    expectedRecoveryWindows = 0;
    restoredWindows.clear();
    suppressedWindows.clear();
    QueueEngineReplacement();
}
inline void StopEngineRecovery() {
    KillTimer(dispatcher, 5);
    ClearEngineTerminal();
    if (engineLaunch) {
        engineLaunch->CancelAndJoin();
        engineLaunch.reset();
    }
    if (connectionCleanup.joinable()) connectionCleanup.join();
    if (engineRestoring && connection && connection->PeerHandle() && !connection->PeerExited())
        TerminateProcess(connection->PeerHandle(), ERROR_PROCESS_ABORTED);
    shell_recovery_destroy(engineBudget);
    engineBudget = nullptr;
}
}  // namespace Product
