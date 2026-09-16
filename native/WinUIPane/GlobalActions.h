#pragma once
#include <functional>
#include <winrt/Windows.Data.Json.h>
#include "JsonValidation.h"

// The global search-actions row (e.g. "Search with X") below the suggestion
// list. Presentation only: Firefox owns action eligibility, labels and
// activation semantics; this only renders rows and forwards clicks.
struct GlobalActionsPanel {
    using JObject = winrt::Windows::Data::Json::JsonObject;
    using JValue = winrt::Windows::Data::Json::JsonValue;

    Microsoft::UI::Xaml::Controls::StackPanel panel;
    struct Visual {
        Microsoft::UI::Xaml::Controls::Button button;
        Microsoft::UI::Xaml::Controls::Image image;
        Microsoft::UI::Xaml::Controls::SymbolIcon symbol;
        Microsoft::UI::Xaml::Controls::TextBlock text, hint;
        winrt::hstring id, resultId, pixels;
    };
    std::vector<Visual> visuals;
    std::shared_ptr<int> lifetime = std::make_shared<int>(0);

    // Provided by the owning NativeAddress. Activating a row requires
    // coordinating with the current edit/session state on the shared
    // pending-action bus, which this struct has no access to, so that
    // coordination stays with the caller; this only forwards the click.
    // dismiss cancels the address surface and returns focus to the field
    // (Escape); focusField only returns focus (Shift+Tab off the first row).
    std::function<void(winrt::hstring const&, winrt::hstring const&)> activate;
    std::function<void()> dismiss, focusField;

    ~GlobalActionsPanel() { lifetime.reset(); }

    GlobalActionsPanel() {
        using namespace Microsoft::UI::Xaml;
        panel.Spacing(4);
        panel.Visibility(Visibility::Collapsed);
        Automation::AutomationProperties::SetName(panel, L"Search actions");
    }

    void Clear() {
        visuals.clear();
        panel.Children().Clear();
    }

    void Sync(JObject const& snapshot) {
        using namespace Microsoft::UI::Xaml;
        using namespace Microsoft::UI::Xaml::Controls;
        auto actions = snapshot.GetNamedArray(L"globalActions", winrt::Windows::Data::Json::JsonArray());
        bool same = actions.Size() == visuals.size();
        for (uint32_t i = 0; same && i < actions.Size(); ++i)
            same = actions.GetAt(i).GetObject().GetNamedString(L"id") == visuals[i].id;
        if (!same) Clear();
        for (uint32_t i = 0; i < actions.Size(); ++i) {
            auto item = actions.GetAt(i).GetObject();
            if (!same) {
                Visual visual;
                visual.id = item.GetNamedString(L"id");
                visual.resultId = item.GetNamedString(L"resultId");
                StackPanel group;
                group.Spacing(2);
                StackPanel content;
                content.Orientation(Orientation::Horizontal);
                content.Spacing(6);
                Grid icon;
                icon.Children().Append(visual.symbol);
                icon.Children().Append(visual.image);
                visual.image.Width(16);
                visual.image.Height(16);
                visual.image.Stretch(Media::Stretch::Uniform);
                visual.symbol.Symbol(Symbol::Find);
                visual.symbol.Width(16);
                visual.symbol.Height(16);
                Automation::AutomationProperties::SetAccessibilityView(visual.image,
                                                                       Automation::Peers::AccessibilityView::Raw);
                Automation::AutomationProperties::SetAccessibilityView(visual.symbol,
                                                                       Automation::Peers::AccessibilityView::Raw);
                visual.text.TextWrapping(TextWrapping::Wrap);
                visual.text.MaxWidth(180);
                content.Children().Append(icon);
                content.Children().Append(visual.text);
                visual.button.Content(content);
                visual.button.HorizontalAlignment(HorizontalAlignment::Left);
                Automation::AutomationProperties::SetAutomationId(visual.button, visual.id);
                auto id = visual.id, resultId = visual.resultId;
                visual.button.Click(
                    [this, alive = std::weak_ptr<int>(lifetime), id, resultId](auto const&, auto const&) {
                        if (!alive.expired()) activate(id, resultId);
                    });
                visual.button.PreviewKeyDown([this, alive = std::weak_ptr<int>(lifetime)](
                                                 auto const&, Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& e) {
                    if (alive.expired()) return;
                    using winrt::Windows::System::VirtualKey;
                    if (e.Key() == VirtualKey::Escape) {
                        dismiss();
                        e.Handled(true);
                    }
                    if (e.Key() == VirtualKey::Tab && (GetKeyState(VK_SHIFT) & 0x8000) &&
                        Microsoft::UI::Xaml::Input::FocusManager::GetFocusedElement(
                            visuals.front().button.XamlRoot()) == visuals.front().button) {
                        focusField();
                        e.Handled(true);
                    }
                });
                group.Children().Append(visual.hint);
                group.Children().Append(visual.button);
                panel.Children().Append(group);
                visuals.push_back(visual);
            }
            auto& visual = visuals[i];
            visual.text.Text(item.GetNamedString(L"title"));
            Automation::AutomationProperties::SetName(visual.button, visual.text.Text());
            auto hint = item.GetNamedString(L"hint", L"");
            visual.hint.Text(hint);
            visual.hint.Visibility(hint.empty() ? Visibility::Collapsed : Visibility::Visible);
            auto pixels = item.GetNamedValue(L"image", JValue::CreateNullValue()).Stringify();
            if (pixels != visual.pixels) {
                visual.image.Source(pixels == L"null" ? nullptr
                                                      : NativeJson::DecodeImage(item.GetNamedObject(L"image")));
                visual.pixels = pixels;
            }
            visual.image.Visibility(pixels == L"null" ? Visibility::Collapsed : Visibility::Visible);
            visual.symbol.Visibility(pixels == L"null" ? Visibility::Visible : Visibility::Collapsed);
        }
        panel.Visibility(actions.Size() ? Visibility::Visible : Visibility::Collapsed);
    }
};
