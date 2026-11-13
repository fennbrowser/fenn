#pragma once
#if defined(FENN_SHELL_TESTING) || defined(FENN_ENGINE_LINK_TESTING)
// One externally armed cut point per isolated guest case. No product build path.
namespace TestWork {
inline winrt::handle Event(const wchar_t* suffix, DWORD access) {
    wchar_t name[200]{};
    const auto length = GetEnvironmentVariableW(L"FENN_TEST_WORK_GATE", name, 200);
    if (!length || length >= 200) return {};
    return winrt::handle{OpenEventW(access, FALSE, (std::wstring(name) + suffix).c_str())};
}
inline bool Armed(const wchar_t* kind) {
    wchar_t selected[32]{};
    if (GetEnvironmentVariableW(L"FENN_TEST_WORK_KIND", selected, 32) >= 32 || wcscmp(selected, kind)) return false;
    auto armed = Event(L".armed", SYNCHRONIZE | EVENT_MODIFY_STATE);
    if (!armed || WaitForSingleObject(armed.get(), 0) != WAIT_OBJECT_0) return false;
    ResetEvent(armed.get());
    return true;
}
inline void Reached() {
    auto event = Event(L".reached", EVENT_MODIFY_STATE);
    if (event) SetEvent(event.get());
}
inline bool WaitRelease() {
    auto event = Event(L".release", SYNCHRONIZE);
    return event && WaitForSingleObject(event.get(), 15000) == WAIT_OBJECT_0;
}
}  // namespace TestWork
#endif
