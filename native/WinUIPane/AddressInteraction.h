#pragma once
#include "ShellCore.h"
#include <winrt/Microsoft.UI.Input.h>

// Platform observations only. Rust owns gesture lifetime and completion rights.
struct NativeAddressInteraction {
    NativeShellCore& core;
    winrt::Windows::Foundation::Point origin{};
    uint32_t pointer{};
    uint64_t token{};
    explicit NativeAddressInteraction(NativeShellCore& value) : core(value) {}
    ShellAddressInteractionState State() const { return core.AddressInteraction(); }
    bool FocusWithin() const { return State().focused != 0; }
    bool Holding() const { return State().holding != 0; }
    bool Current(ShellAddressInteractionState const& effect) const { return State().token == effect.token; }
    ShellAddressInteractionState Observe(bool inside) {
        return core.AddressInteraction(SHELL_ADDRESS_INTERACTION_KIND_OBSERVE, 0, 0, inside);
    }
    void PointerFocus() { core.AddressInteraction(SHELL_ADDRESS_INTERACTION_KIND_POINTER_FOCUS); }
    void Begin(winrt::Microsoft::UI::Input::PointerPoint const& point, bool entry) {
        origin = point.Position();
        pointer = point.PointerId();
        token = core.AddressInteraction(SHELL_ADDRESS_INTERACTION_KIND_BEGIN, pointer, 0, entry).token;
    }
    void Move(winrt::Microsoft::UI::Input::PointerPoint const& point) {
        const auto position = point.Position();
        // WinUI positions are DIPs, so request the system tolerance at 96 DPI.
        const bool moved = std::abs(position.X-origin.X) > GetSystemMetricsForDpi(SM_CXDRAG,96) ||
                           std::abs(position.Y-origin.Y) > GetSystemMetricsForDpi(SM_CYDRAG,96);
        core.AddressInteraction(SHELL_ADDRESS_INTERACTION_KIND_MOVE, point.PointerId(), token, moved);
    }
    ShellAddressInteractionState Release(winrt::Microsoft::UI::Input::PointerPoint const& point, bool editorOwned) {
        Move(point);
        return core.AddressInteraction(SHELL_ADDRESS_INTERACTION_KIND_RELEASE, point.PointerId(), token, editorOwned);
    }
    void Cancel(uint32_t id, uint64_t ticket) {
        core.AddressInteraction(SHELL_ADDRESS_INTERACTION_KIND_CANCEL, id, ticket);
    }
    void Interrupt(bool editorOwned) {
        core.AddressInteraction(SHELL_ADDRESS_INTERACTION_KIND_INTERRUPT, 0, 0, editorOwned);
    }
    ShellAddressInteractionState PrepareSubmit(bool editorOwned) {
        return core.AddressInteraction(SHELL_ADDRESS_INTERACTION_KIND_PREPARE_SUBMIT, 0, 0, editorOwned);
    }
};
