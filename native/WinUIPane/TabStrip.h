#pragma once
#include <functional>
#include <winrt/Microsoft.UI.Input.h>
#include "JsonValidation.h"
#include "TabGroupsButton.h"

// Strip-only experiment: TabView owns presentation, Firefox owns all mutations.
// No content pages, template internals, drag reordering or tear-out are used.
struct NativeTabStrip {
    Controls::Grid root{nullptr};
    Controls::TabView view{nullptr};
    NativeTabMenu& menu;
    std::function<void(uint32_t)> select, close;
    std::function<void()> add;
    std::map<uint32_t, Controls::TabViewItem> rows;
    // Favicons are the small end of what NativeAddress's row images (104,
    // 65536) allow: one glyph-sized icon per tab, but potentially many more
    // tabs per snapshot than address rows, so the per-snapshot total is its
    // own budget rather than reusing kIconImageLimits or NativeAddress's.
    static constexpr NativeJson::ImageLimits kTabImageLimits{32, 262144};
    struct Presentation {
        hstring detail;
        uint32_t flags{};
        hstring status;
        hstring imagePixels;
        uint32_t imageWidth{}, imageHeight{};
    };
    std::map<uint32_t, Presentation> presentation;
    TabGroupsButton groupsButton;
    uint32_t presentationRevision{};
    hstring presentationFingerprint;
    bool syncing{};
    std::shared_ptr<int> lifetime = std::make_shared<int>(0);

    NativeTabStrip(NativeTabMenu& model) : menu(model) {
        root =
            Markup::XamlReader::Load(
                LR"(<Grid xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" Background="{ThemeResource SolidBackgroundFillColorBaseBrush}"><TabView TabWidthMode="Equal" CanDragTabs="False" CanReorderTabs="False" AllowDrop="False" VerticalAlignment="Top" AutomationProperties.AutomationId="FirefoxWinUITabStrip" AutomationProperties.Name="Tabs in this window" /></Grid>)")
                .as<Controls::Grid>();
        view = root.Children().GetAt(0).as<Controls::TabView>();
        view.TabStripHeader(groupsButton.button);
        groupsButton.currentRevision = [this] { return presentationRevision; };
        auto alive = std::weak_ptr<int>(lifetime);
        view.SelectionChanged([this, alive](auto const&, auto const&) {
            if (alive.expired() || syncing || !view.SelectedItem()) return;
            // Modified gestures are observed on the exact tab below. Never let
            // TabView's single selection replace Firefox's multiselected set.
            if (GetKeyState(VK_CONTROL) & 0x8000 || GetKeyState(VK_SHIFT) & 0x8000) return;
            auto row = view.SelectedItem().as<Controls::TabViewItem>();
            select(unbox_value<uint32_t>(row.Tag()));
        });
        view.TabCloseRequested([this, alive](auto const&, Controls::TabViewTabCloseRequestedEventArgs const& args) {
            if (!alive.expired() && !syncing) close(unbox_value<uint32_t>(args.Tab().Tag()));
        });
        view.AddTabButtonClick([this, alive](auto const&, auto const&) {
            if (!alive.expired() && !syncing) add();
        });
    }
    ~NativeTabStrip() { lifetime.reset(); }

    // Parse a complete bounded copy before changing XAML or accepting any IDs.
    void Publish(NativeTabMenu::Object const& data) {
        if (data.GetNamedNumber(L"version") != 1) throw hresult_invalid_argument();
        auto revision = NativeJson::RequireId(data, L"revision");
        auto fingerprint = data.Stringify();
        if (revision < presentationRevision ||
            (revision == presentationRevision && fingerprint != presentationFingerprint))
            throw hresult_invalid_argument();
        if (revision == presentationRevision) return;
        auto label = data.GetNamedString(L"groupsLabel");
        const bool rtl = data.GetNamedBoolean(L"rtl");
        auto entries = data.GetNamedArray(L"rows"), groups = data.GetNamedArray(L"groups");
        if (label.empty() || label.size() > NativeJson::kMaxShortLabel || entries.Size() > NativeJson::kMaxTabs ||
            groups.Size() > NativeJson::kMaxTabs)
            throw hresult_invalid_argument();
        std::map<uint32_t, Presentation> next;
        size_t totalImagePixels = 0;
        for (auto value : entries) {
            auto entry = value.GetObject();
            auto id = NativeJson::RequireId(entry, L"id");
            auto flags = entry.GetNamedNumber(L"flags");
            auto detail = entry.GetNamedString(L"detail");
            auto status = entry.GetNamedString(L"status", L"");
            if (!std::isfinite(flags) || flags < 0 || flags > 15 || std::floor(flags) != flags ||
                detail.size() > NativeJson::kMaxDetailText || status.size() > NativeJson::kMaxDetailText)
                throw hresult_invalid_argument();
            NativeJson::ValidateImage(entry, totalImagePixels, kTabImageLimits);
            hstring imagePixels;
            uint32_t imageWidth{}, imageHeight{};
            if (entry.HasKey(L"image") &&
                entry.GetNamedValue(L"image").ValueType() != winrt::Windows::Data::Json::JsonValueType::Null) {
                auto image = entry.GetNamedObject(L"image");
                imagePixels = image.GetNamedString(L"pixels");
                imageWidth = uint32_t(image.GetNamedNumber(L"width"));
                imageHeight = uint32_t(image.GetNamedNumber(L"height"));
            }
            if (!next.emplace(id, Presentation{detail, uint32_t(flags), status, imagePixels, imageWidth, imageHeight})
                     .second)
                throw hresult_invalid_argument();
        }
        std::set<uint32_t> ids;
        for (auto value : groups) {
            auto group = value.GetObject();
            auto id = NativeJson::RequireId(group, L"id");
            auto name = group.GetNamedString(L"name"), text = group.GetNamedString(L"label");
            group.GetNamedBoolean(L"collapsed");
            if (!ids.insert(id).second || name.empty() || name.size() > NativeJson::kMaxShortLabel || text.empty() ||
                text.size() > NativeJson::kMaxDetailText)
                throw hresult_invalid_argument();
        }
        groupsButton.Apply(label, groups);
        root.FlowDirection(rtl ? FlowDirection::RightToLeft : FlowDirection::LeftToRight);
        presentation.swap(next);
        presentationRevision = revision;
        presentationFingerprint = fingerprint;
    }

    void UpdateHeader(Controls::TabViewItem const& row, uint32_t id, hstring const& title) {
        auto header = row.Header().as<Controls::Grid>();
        // The loading spinner shares the favicon's slot -- Firefox's own tabs
        // work the same way -- rather than reserving its own column: while
        // busy, the spinner replaces the favicon in place; once loading ends,
        // the favicon (if any) reclaims that spot.
        auto favicon = header.Children().GetAt(0).as<Controls::Grid>();
        auto faviconImage = favicon.Children().GetAt(0).as<Controls::Image>();
        auto busy = favicon.Children().GetAt(1).as<Controls::ProgressRing>();
        // Pin/audio are SymbolIcon glyphs at a fixed intrinsic size that
        // Width/Height alone would only clip, not scale; a Viewbox scales
        // the glyph down to fit the 16x16 box instead.
        auto pinBox = header.Children().GetAt(1).as<Controls::Viewbox>();
        auto pin = pinBox.Child().as<Controls::SymbolIcon>();
        auto text = header.Children().GetAt(2).as<Controls::StackPanel>();
        auto name = text.Children().GetAt(0).as<Controls::TextBlock>();
        auto detail = text.Children().GetAt(1).as<Controls::TextBlock>();
        auto audioBox = header.Children().GetAt(3).as<Controls::Viewbox>();
        auto audio = audioBox.Child().as<Controls::SymbolIcon>();
        auto meta = presentation.count(id) ? presentation.at(id) : Presentation{};
        // Compare against the value last applied to this Image, not against
        // any prior Presentation -- the map is replaced wholesale on every
        // Publish(), so it cannot itself remember what is currently shown.
        // No favicon (not yet resolved, or the page genuinely has none) shows
        // no icon at all -- there is no placeholder glyph to fall back to.
        const bool hasImage = !meta.imagePixels.empty();
        const bool isBusy = (meta.flags & 2) != 0;
        auto shown = winrt::unbox_value_or<hstring>(faviconImage.Tag(), L"");
        if (hasImage && shown != meta.imagePixels) {
            faviconImage.Source(NativeJson::DecodeImage(int(meta.imageWidth), int(meta.imageHeight), meta.imagePixels));
            faviconImage.Tag(box_value(meta.imagePixels));
        } else if (!hasImage && !shown.empty()) {
            faviconImage.Source(nullptr);
            faviconImage.Tag(box_value(hstring{}));
        }
        faviconImage.Visibility(!isBusy && hasImage ? Visibility::Visible : Visibility::Collapsed);
        busy.Visibility(isBusy ? Visibility::Visible : Visibility::Collapsed);
        busy.IsActive(isBusy);
        favicon.Visibility(isBusy || hasImage ? Visibility::Visible : Visibility::Collapsed);
        if (name.Text() != title) name.Text(title);
        if (detail.Text() != meta.detail) detail.Text(meta.detail);
        detail.Visibility(meta.detail.empty() ? Visibility::Collapsed : Visibility::Visible);
        pinBox.Visibility(meta.flags & 1 ? Visibility::Visible : Visibility::Collapsed);
        audio.Symbol(meta.flags & 4 ? Controls::Symbol::Mute : Controls::Symbol::Volume);
        audioBox.Visibility(meta.flags & 12 ? Visibility::Visible : Visibility::Collapsed);
        Controls::ToolTipService::SetToolTip(row, box_value(meta.detail.empty() ? title : title + L"\n" + meta.detail));
        Automation::AutomationProperties::SetHelpText(
            row, meta.detail + (meta.detail.empty() || meta.status.empty() ? L"" : L" · ") + meta.status);
    }

    void AttachMenu(Controls::TabViewItem const& row, uint32_t id) {
        Controls::MenuFlyout flyout;
        flyout.Items().Append(NativeTabMenu::Placeholder());
        auto alive = std::weak_ptr<int>(lifetime);
        flyout.Opening([this, alive, id, weak = make_weak(flyout)](auto const&, auto const&) {
            if (!alive.expired() && rows.count(id)) menu.Open(weak.get(), id);
        });
        flyout.Opened([this, alive, weak = make_weak(flyout)](auto const&, auto const&) {
            if (!alive.expired() && weak.get() == menu.active) menu.FocusFirst();
        });
        flyout.Closed([this, alive, id, weak = make_weak(flyout), target = make_weak(row)](auto const&, auto const&) {
            if (alive.expired()) return;
            if (weak.get() == menu.active) menu.Dismiss();
            if (auto row = target.get(); row && rows.count(id) && row.ContextFlyout() == weak.get())
                AttachMenu(row, id);
        });
        row.ContextFlyout(flyout);
    }

    void Sync(const WinUITab* tabs, uint32_t count, uint32_t active) {
        syncing = true;
        try {
            std::set<uint32_t> current;
            for (uint32_t i = 0; i < count; ++i) {
                const auto id = tabs[i].id;
                current.insert(id);
                if (!rows.count(id)) {
                    Controls::TabViewItem row;
                    row.Tag(box_value(id));
                    row.IsClosable(true);
                    row.HorizontalContentAlignment(HorizontalAlignment::Stretch);
                    row.Header(Markup::XamlReader::Load(
                        LR"(<Grid xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" ColumnSpacing="6"><Grid.ColumnDefinitions><ColumnDefinition Width="Auto"/><ColumnDefinition Width="Auto"/><ColumnDefinition Width="*"/><ColumnDefinition Width="Auto"/></Grid.ColumnDefinitions><Grid Width="16" Height="16"><Image Stretch="Uniform" AutomationProperties.AccessibilityView="Raw"/><ProgressRing Width="16" Height="16" IsActive="False" AutomationProperties.AccessibilityView="Raw"/></Grid><Viewbox Grid.Column="1" Width="16" Height="16"><SymbolIcon Symbol="Pin"/></Viewbox><StackPanel Grid.Column="2" MaxWidth="220" VerticalAlignment="Center"><TextBlock TextTrimming="CharacterEllipsis"/><TextBlock FontSize="10" Foreground="{ThemeResource TextFillColorSecondaryBrush}" TextTrimming="CharacterEllipsis"/></StackPanel><Viewbox Grid.Column="3" Width="16" Height="16"><SymbolIcon Symbol="Volume"/></Viewbox></Grid>)"));
                    Automation::AutomationProperties::SetAutomationId(row, L"FirefoxWinUITab-" + to_hstring(id));
                    auto alive = std::weak_ptr<int>(lifetime);
                    row.AddHandler(
                        UIElement::PointerPressedEvent(),
                        box_value(Microsoft::UI::Xaml::Input::PointerEventHandler(
                            [this, alive, id](auto const&,
                                              Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args) {
                                if (alive.expired() || syncing || !rows.count(id) ||
                                    !args.GetCurrentPoint(nullptr).Properties().IsLeftButtonPressed())
                                    return;
                                if (!(GetKeyState(VK_CONTROL) & 0x8000) && !(GetKeyState(VK_SHIFT) & 0x8000)) return;
                                // Ignore the stock close button; retain its normal input.
                                auto node = args.OriginalSource().try_as<DependencyObject>();
                                while (node && node != rows.at(id)) {
                                    if (node.try_as<Controls::Primitives::ButtonBase>()) return;
                                    node = Media::VisualTreeHelper::GetParent(node);
                                }
                                select(id);
                            })),
                        true);
                    row.PreviewKeyDown(
                        [this, alive, id](auto const&, Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& args) {
                            if (alive.expired() || syncing || !rows.count(id)) return;
                            if (args.Key() == Windows::System::VirtualKey::Space ||
                                args.Key() == Windows::System::VirtualKey::Enter) {
                                if (args.OriginalSource() != rows.at(id)) return;
                                select(id);
                                args.Handled(true);
                            }
                        });
                    rows.emplace(id, row);
                    AttachMenu(row, id);
                }
                auto row = rows.at(id);
                UpdateHeader(row, id, tabs[i].title);
                Automation::AutomationProperties::SetName(row, tabs[i].title);
                // Secondary selection is explicit and theme-aware; SelectedItem
                // remains solely the Firefox active tab.
                bool selected = menu.selectedTabs.count(id) != 0;
                row.FontWeight(Windows::UI::Text::FontWeight{uint16_t(selected ? 600 : 400)});
                Automation::AutomationProperties::SetItemStatus(row, id == active ? L"Active tab"
                                                                     : selected   ? L"Selected tab"
                                                                                  : L"");
                uint32_t old;
                if (!view.TabItems().IndexOf(row, old) || old != i) {
                    if (view.TabItems().IndexOf(row, old)) view.TabItems().RemoveAt(old);
                    view.TabItems().InsertAt(i, row);
                }
            }
            while (view.TabItems().Size() > count) view.TabItems().RemoveAtEnd();
            for (auto it = rows.begin(); it != rows.end();) {
                if (!current.count(it->first))
                    it = rows.erase(it);
                else
                    ++it;
            }
            view.SelectedItem(rows.count(active) ? rows.at(active) : nullptr);
        } catch (...) {
            syncing = false;
            throw;
        }
        syncing = false;
    }
};
