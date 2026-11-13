#pragma once

struct NativeBookmarksToolbar {
    using Canvas = Microsoft::UI::Xaml::Controls::Canvas;
    using Button = Microsoft::UI::Xaml::Controls::Button;
    using JObject = winrt::Windows::Data::Json::JsonObject;

    using FrameworkElement = Microsoft::UI::Xaml::FrameworkElement;
    using Rect = winrt::Windows::Foundation::Rect;
    using Point = winrt::Windows::Foundation::Point;
    using Pointer = winrt::Microsoft::UI::Xaml::Input::Pointer;
    using Operation = winrt::Windows::ApplicationModel::DataTransfer::DataPackageOperation;
    using Image = Microsoft::UI::Xaml::Controls::Image;
    using Viewbox = Microsoft::UI::Xaml::Controls::Viewbox;
    static constexpr wchar_t kDragFormat[] = L"Fenn.BookmarksToolbar";
    struct Artwork {
        Image image;
        Viewbox glyph;
        winrt::hstring key;
    };

    struct Gesture {
        winrt::hstring id, url;
        Pointer pointer;
        FrameworkElement source;
        Point start;
        bool dragging{};
        HWND target{};
    };

    // Both shell hosts create their browser windows on one XAML UI thread.
    // Only that thread can resolve a destination and touch its presentation.
    inline static thread_local std::map<HWND, NativeBookmarksToolbar*> windows;
    inline static thread_local NativeBookmarksToolbar* systemSource{};
    HWND window{};
    winrt::hstring windowId;
    Canvas root;
    std::optional<Gesture> gesture;
    std::function<void(wchar_t const*, uint32_t, FrameworkElement const&, Rect, winrt::hstring const&)> activate;
    std::map<winrt::hstring, Artwork> artwork;
    winrt::hstring activeDrag;
    winrt::hstring publication;
    std::shared_ptr<int> lifetime = std::make_shared<int>(0);

    NativeBookmarksToolbar() {
        using namespace Microsoft::UI::Xaml;
        Automation::AutomationProperties::SetAutomationId(root, L"FennBookmarksToolbar");
        root.Visibility(Visibility::Collapsed);
        root.VerticalAlignment(VerticalAlignment::Top);
        // The browser publishes physical left offsets, including in RTL.
        root.FlowDirection(FlowDirection::LeftToRight);
        root.Background(Media::SolidColorBrush(winrt::Windows::UI::Colors::Transparent()));
        root.AllowDrop(true);
        auto alive = std::weak_ptr<int>(lifetime);
        using Microsoft::UI::Xaml::Input::PointerEventHandler;
        root.AddHandler(
            UIElement::PointerMovedEvent(),
            winrt::box_value(PointerEventHandler([this, alive](auto const&, auto const& args) {
                if (alive.expired() || !gesture || gesture->pointer.PointerId() != args.Pointer().PointerId()) return;
                auto current = args.GetCurrentPoint(root);
                if (!current.Properties().IsLeftButtonPressed()) {
                    EndGesture();
                    return;
                }
                auto point = current.Position();
                if (!gesture->dragging) {
                    if (std::abs(point.X - gesture->start.X) < 4 && std::abs(point.Y - gesture->start.Y) < 4) return;
                    gesture->source.ReleasePointerCapture(args.Pointer());
                    if (!root.CapturePointer(args.Pointer())) {
                        EndGesture();
                        return;
                    }
                    gesture->dragging = true;
                }
                args.Handled(true);
                POINT cursor{};
                if (!GetCursorPos(&cursor)) return;
                bool external = false;
                auto* target = DragTarget(cursor, external);
                const auto targetWindow = target ? target->window : nullptr;
                if (gesture->target != targetWindow) {
                    CancelHover(gesture->target);
                    gesture->target = targetWindow;
                }
                if (target) {
                    target->SendDrag(L"hover", windowId, gesture->id, target->LocalPoint(cursor),
                                     uint32_t(args.KeyModifiers()) & 15u);
                } else if (external && !gesture->url.empty()) {
                    auto source = gesture->source;
                    EndGesture();
                    try {
                        source.StartDragAsync(current);
                    } catch (winrt::hresult_error const&) {}
                }
            })),
            true);
        root.AddHandler(
            UIElement::PointerReleasedEvent(),
            winrt::box_value(PointerEventHandler([this, alive](auto const&, auto const& args) {
                if (alive.expired() || !gesture || gesture->pointer.PointerId() != args.Pointer().PointerId()) return;
                auto current = args.GetCurrentPoint(root);
                if (current.Properties().PointerUpdateKind() !=
                    Microsoft::UI::Input::PointerUpdateKind::LeftButtonReleased)
                    return;
                if (!gesture->dragging) {
                    EndGesture();
                    return;
                }
                args.Handled(true);
                POINT cursor{};
                bool external = false;
                if (GetCursorPos(&cursor)) {
                    if (auto* target = DragTarget(cursor, external)) {
                        target->SendDrag(L"drop", windowId, gesture->id, target->LocalPoint(cursor),
                                         uint32_t(args.KeyModifiers()) & 15u);
                    }
                }
                EndGesture();
            })),
            true);
        root.PointerCanceled([this, alive](auto const&, auto const&) {
            if (!alive.expired()) EndGesture();
        });
        root.PointerCaptureLost([this, alive](auto const&, auto const& args) {
            if (!alive.expired() && gesture && gesture->dragging && args.OriginalSource() == root) {
                CancelHover(gesture->target);
                gesture.reset();
            }
        });
        root.PreviewKeyDown([this, alive](auto const&, auto const& args) {
            if (!alive.expired() && gesture && gesture->dragging && args.Key() == Windows::System::VirtualKey::Escape) {
                EndGesture();
                args.Handled(true);
            }
        });
        root.DragOver([this, alive](auto const&, auto const& args) {
            if (alive.expired() || !systemSource || !args.DataView().Contains(kDragFormat)) return;
            args.AcceptedOperation(Modifiers() & 1 ? Operation::Copy : Operation::Move);
            args.Handled(true);
        });
        root.Drop([this, alive](auto const&, auto const& args) {
            if (alive.expired() || !systemSource || !args.DataView().Contains(kDragFormat)) return;
            auto point = args.GetPosition(root);
            args.AcceptedOperation(DropLocal(point, true));
            args.Handled(true);
        });
        root.ContextRequested([this, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const& args) {
            winrt::Windows::Foundation::Point point{};
            if (alive.expired() || !activate || !args.TryGetPosition(root, point)) return;
            args.Handled(true);
            activate(L"bookmarks-toolbar:context:0", 0, root, {point.X, point.Y, 1, 1}, {});
        });
    }

    ~NativeBookmarksToolbar() {
        if (systemSource == this) systemSource = nullptr;
        if (window) windows.erase(window);
    }

    void BindWindow(HWND owner) {
        window = owner;
        windows.insert_or_assign(owner, this);
    }

    bool Available() const {
        return activate && root.Visibility() == Microsoft::UI::Xaml::Visibility::Visible && root.XamlRoot() &&
               !windowId.empty();
    }

    Point LocalPoint(POINT cursor) const {
        auto screen = root.XamlRoot().ContentIsland().CoordinateConverter();
        auto point = screen.ConvertScreenToLocal(winrt::Windows::Graphics::PointInt32{cursor.x, cursor.y});
        return root.TransformToVisual(nullptr).Inverse().TransformPoint(point);
    }

    static NativeBookmarksToolbar* FindWindow(HWND owner) {
        auto found = windows.find(owner);
        return found != windows.end() && found->second->Available() ? found->second : nullptr;
    }

    NativeBookmarksToolbar* DragTarget(POINT cursor, bool& external) const {
        const auto hit = GetAncestor(WindowFromPoint(cursor), GA_ROOT);
        if (!hit) return nullptr;
        if (auto* target = FindWindow(hit)) return target;
        if (auto* target = FindWindow(GetAncestor(hit, GA_ROOTOWNER))) return target;

        // Gecko folder popups are separate HWNDs, potentially owned by the
        // hidden browser proxy. Keep their last browser destination and let
        // Gecko hit-test its own popup chain.
        DWORD hitPid{}, geckoPid{};
        GetWindowThreadProcessId(hit, &hitPid);
        auto prior = FindWindow(gesture->target);
        if (prior) {
            auto proxy = static_cast<HWND>(GetPropW(prior->window, L"Fenn.GeckoEndpoint"));
            if (proxy) GetWindowThreadProcessId(proxy, &geckoPid);
            if (hitPid == GetCurrentProcessId() || hitPid == geckoPid) return prior;
        }
        wchar_t name[64]{};
        GetClassNameW(hit, name, ARRAYSIZE(name));
        external = hit != GetDesktopWindow() && wcscmp(name, L"Progman") != 0 && wcscmp(name, L"WorkerW") != 0;
        return nullptr;
    }

    static void CancelHover(HWND owner) {
        if (auto* target = FindWindow(owner))
            target->activate(L"bookmarks-toolbar:drag-cancel", 0, target->root, {0, 0, 1, 1}, {});
    }

    Operation RouteDrop(auto const& info, POINT cursor, bool drop, winrt::hstring const& externalUrl) {
        if (!Available()) return Operation::None;
        auto point = LocalPoint(cursor);
        if (!Contains(point)) return Operation::None;
        const auto data = info.Data();
        if (systemSource && data.Contains(kDragFormat)) return DropLocal(point, drop);
        using winrt::Windows::ApplicationModel::DataTransfer::StandardDataFormats;
        if (!data.Contains(StandardDataFormats::WebLink())) return Operation::None;
        if (!drop) return Operation::Copy;
        if (externalUrl.empty() || externalUrl.size() > 8192) return Operation::None;
        activate(L"bookmarks-toolbar:drop:0", 0, root, {point.X, point.Y, 1, 1}, externalUrl);
        return Operation::Copy;
    }

    bool Contains(Point point) const {
        return point.X >= 0 && point.Y >= 0 && point.X < root.ActualWidth() && point.Y < root.ActualHeight();
    }

    void EndGesture() {
        auto previous = std::exchange(gesture, std::nullopt);
        if (previous && previous->dragging) {
            CancelHover(previous->target);
            root.ReleasePointerCapture(previous->pointer);
        }
    }

    void SendDrag(std::wstring_view verb, winrt::hstring const& sourceWindow, winrt::hstring const& id, Point point,
                  uint32_t modifiers) {
        if (!Available()) return;
        std::wstring command = L"bookmarks-toolbar:";
        command.append(verb).append(L":").append(sourceWindow.c_str()).append(L":").append(id.c_str());
        activate(command.c_str(), modifiers, root, {point.X, point.Y, 1, 1}, {});
    }

    Operation DropLocal(Point point, bool drop) {
        const auto modifiers = Modifiers();
        if (drop) {
            auto* source = std::exchange(systemSource, nullptr);
            SendDrag(L"drop", source->windowId, std::exchange(source->activeDrag, {}), point, modifiers);
        }
        return modifiers & 1 ? Operation::Copy : Operation::Move;
    }

    void BindGesture(FrameworkElement const& source, winrt::hstring const& id, winrt::hstring const& url) {
        using Microsoft::UI::Xaml::UIElement;
        auto alive = std::weak_ptr<int>(lifetime);
        source.AddHandler(
            UIElement::PointerPressedEvent(),
            winrt::box_value(Microsoft::UI::Xaml::Input::PointerEventHandler(
                [this, alive, id, url](auto const& sender, auto const& args) {
                    if (alive.expired() || systemSource) return;
                    auto point = args.GetCurrentPoint(root);
                    if (point.Properties().PointerUpdateKind() ==
                        Microsoft::UI::Input::PointerUpdateKind::LeftButtonPressed)
                        gesture.emplace(
                            Gesture{id, url, args.Pointer(), sender.template as<FrameworkElement>(), point.Position()});
                })),
            true);
        source.DragStarting([this, alive, id, url](auto const&, auto const& args) {
            if (alive.expired() || systemSource) {
                args.Cancel(true);
                return;
            }
            auto data = args.Data();
            data.SetData(kDragFormat, winrt::box_value(L"1"));
            if (!url.empty()) {
                try {
                    winrt::Windows::Foundation::Uri uri(url);
                    if (uri.SchemeName() == L"http" || uri.SchemeName() == L"https") {
                        data.SetWebLink(uri);
                        data.SetText(url);
                    }
                } catch (winrt::hresult_error const&) {}
            }
            args.AllowedOperations(Operation::Move | Operation::Copy);
            activeDrag = id;
            systemSource = this;
        });
        source.DropCompleted([this, alive, id](auto const&, auto const&) {
            if (!alive.expired() && activeDrag == id) {
                activeDrag.clear();
                if (systemSource == this) systemSource = nullptr;
            }
        });
    }

    void ApplyArtwork(JObject const& images) {
        using Microsoft::UI::Xaml::Visibility;
        for (auto& [id, art] : artwork) {
            auto entry = images.GetNamedObject(id, JObject());
            auto image = entry.GetNamedObject(L"image", JObject());
            auto key = image.Stringify();
            if (key == art.key) continue;
            art.key = key;
            const bool hasImage = image.Size() != 0;
            art.image.Source(hasImage ? NativeJson::DecodeImage(image) : nullptr);
            art.image.Visibility(hasImage ? Visibility::Visible : Visibility::Collapsed);
            art.glyph.Visibility(hasImage ? Visibility::Collapsed : Visibility::Visible);
        }
    }

    static uint32_t Modifiers() {
        return ((GetKeyState(VK_CONTROL) < 0) ? 1u : 0u) | ((GetKeyState(VK_MENU) < 0) ? 2u : 0u) |
               ((GetKeyState(VK_SHIFT) < 0) ? 4u : 0u) |
               ((GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0) ? 8u : 0u);
    }

    void Apply(JObject const& state, bool rtl = false) {
        using namespace Microsoft::UI::Xaml;
        using namespace Microsoft::UI::Xaml::Controls;
        auto images = state.GetNamedObject(L"images", JObject());
        if (images.Size() > 128) winrt::throw_hresult(E_INVALIDARG);
        size_t pixels = 0;
        for (auto const& image : images) NativeJson::ValidateImage(image.Value().GetObject(), pixels, {16, 32768});

        auto rows = state.GetNamedArray(L"entries", winrt::Windows::Data::Json::JsonArray());
        const auto top = state.GetNamedNumber(L"top", 0);
        const auto height = state.GetNamedNumber(L"height", 0);
        if (rows.Size() > 128 || !std::isfinite(top) || !std::isfinite(height) ||
            (rows.Size() && (top < 39 || top > 41 || height < 20 || height > 48)))
            winrt::throw_hresult(E_INVALIDARG);

        auto label = state.GetNamedString(L"label", L"");
        windowId = state.GetNamedString(L"windowId", L"");
        auto key = rows.Stringify() + L":" + std::to_wstring(top) + L":" + std::to_wstring(height) +
                   (rtl ? L":rtl:" : L":ltr:") + label;
        if (publication == key) {
            ApplyArtwork(images);
            return;
        }

        std::vector<UIElement> children;
        std::map<winrt::hstring, Artwork> nextArtwork;
        for (auto const& value : rows) {
            auto row = value.GetObject();
            auto id = row.GetNamedString(L"id");
            auto kind = row.GetNamedString(L"kind");
            auto title = row.GetNamedString(L"label");
            const auto left = row.GetNamedNumber(L"left");
            const auto width = row.GetNamedNumber(L"width");
            if (!std::isfinite(left) || !std::isfinite(width) || left < 0 || width <= 0 || left + width > 32768 ||
                id.empty() || id.size() > 16 || title.size() > 1024)
                winrt::throw_hresult(E_INVALIDARG);

            if (kind == L"separator") {
                auto line =
                    Markup::XamlReader::Load(
                        LR"(<Border xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" Background="{ThemeResource DividerStrokeColorDefaultBrush}" Width="1"/>)")
                        .as<Border>();
                line.Height(height - 10);
                line.VerticalAlignment(VerticalAlignment::Center);
                line.HorizontalAlignment(HorizontalAlignment::Center);
                Grid area;
                area.Background(Media::SolidColorBrush(winrt::Windows::UI::Colors::Transparent()));
                area.Width(width);
                area.Height(height);
                area.Children().Append(line);
                Canvas::SetLeft(area, left);
                if (row.GetNamedBoolean(L"draggable", false)) BindGesture(area, id, {});
                children.push_back(area);
                continue;
            }

            const bool overflow = kind == L"overflow";
            if (!overflow && kind != L"folder" && kind != L"bookmark") winrt::throw_hresult(E_INVALIDARG);
            auto button =
                Markup::XamlReader::Load(
                    LR"(<Button xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" Style="{StaticResource SubtleButtonStyle}" MinWidth="0" MinHeight="0" Padding="4,0"/>)")
                    .as<Button>();
            button.Width(width);
            button.Height(height);
            button.FlowDirection(rtl ? FlowDirection::RightToLeft : FlowDirection::LeftToRight);
            Canvas::SetLeft(button, left);

            StackPanel content;
            content.Orientation(Orientation::Horizontal);
            content.Spacing(4);
            SymbolIcon icon(overflow ? Symbol::More : kind == L"folder" ? Symbol::Folder : Symbol::OutlineStar);
            Viewbox glyph;
            glyph.Width(14);
            glyph.Height(14);
            glyph.Child(icon);
            content.Children().Append(glyph);
            if (kind == L"bookmark") {
                Image image;
                image.Width(14);
                image.Height(14);
                image.Visibility(Visibility::Collapsed);
                content.Children().Append(image);
                nextArtwork.emplace(id, Artwork{image, glyph, {}});
            }
            if (!overflow) {
                TextBlock text;
                text.Text(title);
                text.TextTrimming(TextTrimming::CharacterEllipsis);
                text.MaxWidth((std::max)(0.0, width - 26));
                content.Children().Append(text);
            }
            button.Content(content);
            ToolTipService::SetToolTip(button, winrt::box_value(title));
            Automation::AutomationProperties::SetName(button, title);
            Automation::AutomationProperties::SetAutomationId(
                button, overflow ? L"FennBookmarksOverflow" : L"FennBookmark-" + id);

            auto dispatch = [this, alive = std::weak_ptr<int>(lifetime), id](
                                Button const& sender, std::wstring_view verb, uint32_t modifiers) {
                if (alive.expired() || !activate) return;
                std::wstring command = L"bookmarks-toolbar:";
                command.append(verb).append(L":").append(id.c_str());
                activate(command.c_str(), modifiers, sender,
                         {0, 0, float(sender.ActualWidth()), float(sender.ActualHeight())}, {});
            };
            button.Click([dispatch](auto const& sender, auto const&) {
                dispatch(sender.template as<Button>(), L"open", Modifiers());
            });
            button.ContextRequested([dispatch](auto const& sender, auto const& e) {
                e.Handled(true);
                dispatch(sender.template as<Button>(), L"context", 0);
            });
            if (!overflow && row.GetNamedBoolean(L"draggable", false)) {
                auto url = row.GetNamedString(L"dragUrl", L"");
                NativeJson::RequireLength(url, 8192);
                BindGesture(button, id, url);
            }
            if (!overflow) {
                button.AddHandler(UIElement::PointerReleasedEvent(),
                                  winrt::box_value(Microsoft::UI::Xaml::Input::PointerEventHandler(
                                      [dispatch](auto const& sender, auto const& e) {
                                          auto source = sender.template as<Button>();
                                          if (e.GetCurrentPoint(source).Properties().PointerUpdateKind() !=
                                              Microsoft::UI::Input::PointerUpdateKind::MiddleButtonReleased)
                                              return;
                                          e.Handled(true);
                                          dispatch(source, L"middle", uint32_t(e.KeyModifiers()) & 15u);
                                      })),
                                  true);
            }
            children.push_back(button);
        }

        EndGesture();
        root.Children().Clear();
        for (auto const& child : children) root.Children().Append(child);
        root.Margin(Thickness{0, top, 0, 0});
        root.Height(height);
        root.Visibility(children.empty() ? Visibility::Collapsed : Visibility::Visible);
        Automation::AutomationProperties::SetName(root, label);
        artwork = std::move(nextArtwork);
        ApplyArtwork(images);
        if (children.empty()) {
            activeDrag.clear();
            if (systemSource == this) systemSource = nullptr;
        }
        publication = key;
    }
};
