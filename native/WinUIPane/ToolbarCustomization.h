#pragma once
#include "JsonValidation.h"
#include "shell_core.h"
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <map>
#include <set>

struct NativeToolbarCustomization {
    using JObject = winrt::Windows::Data::Json::JsonObject;
    using Map = winrt::Windows::Foundation::Collections::IObservableMap<winrt::hstring,winrt::Windows::Foundation::IInspectable>;
    using Button = Microsoft::UI::Xaml::Controls::Button;
    struct Item { winrt::hstring id, control, label; bool placed{}, movable{}, removable{}, spring{}, canOverflow{}, canAutoOverflow{}, extension{}; winrt::hstring palette; };
    struct State { uint64_t revision{}; bool canReset{}, canUndoReset{}; std::vector<Item> items; std::map<winrt::hstring,winrt::hstring> labels; };
    State state;
    winrt::Windows::Foundation::Collections::IObservableVector<winrt::Windows::Foundation::IInspectable> availableItems = winrt::single_threaded_observable_vector<winrt::Windows::Foundation::IInspectable>();
    Microsoft::UI::Xaml::Controls::Primitives::Popup popup{nullptr};
    Microsoft::UI::Xaml::Controls::ListViewBase available{nullptr};
    std::map<winrt::hstring,Microsoft::UI::Xaml::Controls::TextBlock> textBlocks;
    Button reset{nullptr}, undoReset{nullptr};
    Button anchor{nullptr};
    Microsoft::UI::Xaml::Controls::MenuFlyout itemMenu{nullptr};
    bool itemMenuOpen{};
    std::map<winrt::hstring,Microsoft::UI::Xaml::FrameworkElement> placedControls;
    winrt::hstring focusAfterChange;
    uint64_t focusRevision{};

    std::function<void()> editorChanged,dragEnded,clearPreviews;
    std::function<Microsoft::UI::Xaml::FrameworkElement(Item const&)> createPreview;
    std::map<winrt::hstring,Microsoft::UI::Xaml::FrameworkElement> previews;
    winrt::Windows::Graphics::Imaging::SoftwareBitmap dragBitmap{nullptr};
    bool editing{}, externalDrop{};
    winrt::hstring dragToken;
    bool updating{}, dragging{}, dragCancelled{};
    uint64_t dragRevision{};
    winrt::hstring dragId;
    std::function<void(wchar_t const*, uint32_t, Button const&)> activate;
    std::shared_ptr<int> lifetime=std::make_shared<int>(0);
    ~NativeToolbarCustomization() { lifetime.reset(); if(itemMenu)itemMenu.Hide(); if(popup) popup.IsOpen(false); }
    static State Read(JObject const& snapshot) {
        State state;
        if (!snapshot.HasKey(L"navigation")) return state;
        auto nav=snapshot.GetNamedObject(L"navigation");
        if (!nav.HasKey(L"customization")) return state;
        auto obj=nav.GetNamedObject(L"customization");
        state.revision=NativeJson::RequireRevision(obj,L"revision");
        state.canReset=obj.GetNamedBoolean(L"canReset",false);state.canUndoReset=obj.GetNamedBoolean(L"canUndoReset",false);
        auto items=obj.GetNamedArray(L"items");
        if(items.Size()>59) throw winrt::hresult_invalid_argument();
        std::set<winrt::hstring> ids,controls;
        bool address=false;size_t springs=0;
        for(auto value:items) {
            auto row=value.GetObject();
            Item item{row.GetNamedString(L"id"),row.GetNamedString(L"control"),row.GetNamedString(L"displayLabel",row.GetNamedString(L"label")),
                row.GetNamedBoolean(L"placed"),row.GetNamedBoolean(L"movable"),row.GetNamedBoolean(L"removable"),row.GetNamedString(L"kind",L"button")==L"spring",row.GetNamedBoolean(L"canOverflow",false),row.GetNamedBoolean(L"canAutoOverflow",false),row.GetNamedBoolean(L"extension",false)};
            item.palette=row.GetNamedString(L"palette",L"");NativeJson::RequireLength(item.palette,64);
            if(item.id.empty() || item.id.size()>64 || item.control.size()>64 || item.label.size()>4096 || !ids.insert(item.id).second ||
                (item.placed && (item.control.empty() || !controls.insert(item.control).second))) throw winrt::hresult_invalid_argument();
            if(item.spring && item.placed && ++springs>16) throw winrt::hresult_invalid_argument();
            address |= item.placed && item.control==L"address";
            state.items.push_back(std::move(item));
        }
        if(!address) return {};
        auto labels=obj.GetNamedObject(L"labels");
        for(auto key:{L"title",L"available",L"earlier",L"later",L"add",L"remove",L"done",L"hint",L"reset",L"undo-reset",L"reset-hint",L"overflow",L"drop"}) {
            auto value=labels.GetNamedString(key);NativeJson::RequireLength(value,4096);state.labels[key]=value;
        }
        return state;
    }
    static winrt::hstring Id(winrt::Windows::Foundation::IInspectable const& item) {
        return item ? winrt::unbox_value<winrt::hstring>(item.as<Map>().Lookup(L"Id")) : winrt::hstring{};
    }
    void Hide() { focusAfterChange=L"";if(itemMenu)itemMenu.Hide();if(dragEnded)dragEnded();dragCancelled=true;if(popup)popup.IsOpen(false); }
    bool BeginDrag(winrt::hstring const& id, winrt::Windows::ApplicationModel::DataTransfer::DataPackage const& data) {
        auto item=std::find_if(state.items.begin(),state.items.end(),[&](auto const& row){return row.id==id;});
        if(!editing || item==state.items.end() || !item->movable)return false;
        dragBitmap=nullptr;dragId=id;dragRevision=state.revision;dragging=true;dragCancelled=false;externalDrop=false;
        GUID token;winrt::check_hresult(CoCreateGuid(&token));dragToken=winrt::to_hstring(winrt::guid(token));
        data.Properties().Insert(L"FirefoxWinUIToolbarDrag",winrt::box_value(dragToken));
        data.RequestedOperation(winrt::Windows::ApplicationModel::DataTransfer::DataPackageOperation::Move);
        return true;
    }
    bool OwnDrag(Microsoft::UI::Xaml::DragEventArgs const& args) {
        if(!editing || !dragging || dragCancelled || externalDrop || dragRevision!=state.revision)return false;
        auto values=args.DataView().Properties();
        return values.HasKey(L"FirefoxWinUIToolbarDrag") && winrt::unbox_value_or<winrt::hstring>(values.Lookup(L"FirefoxWinUIToolbarDrag"),L"")==dragToken;
    }
    bool Accept(Microsoft::UI::Xaml::DragEventArgs const& args, bool removing=false) {
        if(!OwnDrag(args))return false;
        auto row=std::find_if(state.items.begin(),state.items.end(),[&](auto const& item){return item.id==dragId;});
        if(row==state.items.end() || (removing && (!row->placed || !row->removable)))return false;
        args.AcceptedOperation(winrt::Windows::ApplicationModel::DataTransfer::DataPackageOperation::Move);
        args.DragUIOverride().IsContentVisible(bool(dragBitmap));
        args.DragUIOverride().IsCaptionVisible(false);args.DragUIOverride().IsGlyphVisible(false);
        if(dragBitmap)args.DragUIOverride().SetContentFromSoftwareBitmap(dragBitmap,{float(dragBitmap.PixelWidth())/2,float(dragBitmap.PixelHeight())/2});
        return true;
    }
    void DropAt(Microsoft::UI::Xaml::DragEventArgs const& args,winrt::hstring const& target={},bool removing=false) {
        if(!Accept(args,removing))return;
        args.Handled(true);externalDrop=true;
        if(target==dragId)return;
        auto row=std::find_if(state.items.begin(),state.items.end(),[&](auto const& item){return item.id==dragId;});
        Send(dragId,removing?L"remove":!row->placed?L"insert":target.empty()?L"end":L"before",target,dragRevision);
        // Firefox can remove the source control before WinUI raises DropCompleted.
        // Let the acknowledged layout refresh the palette even in that case.
        dragging=false;
    }
    void EndDrag() {if(dragEnded)dragEnded();dragging=false;dragToken=L"";Refresh();}
    winrt::fire_and_forget CaptureDrag(Microsoft::UI::Xaml::DragStartingEventArgs args,
        Microsoft::UI::Xaml::FrameworkElement element,winrt::hstring id) {
        auto deferral=args.GetDeferral();auto alive=std::weak_ptr<int>(lifetime);auto revision=state.revision;auto token=dragToken;
        try {
            Microsoft::UI::Xaml::Media::Imaging::RenderTargetBitmap bitmap;
            co_await bitmap.RenderAsync(element);
            auto pixels=co_await bitmap.GetPixelsAsync();
            if(alive.expired() || !editing || !dragging || dragCancelled || dragId!=id || dragToken!=token || state.revision!=revision){args.Cancel(true);}
            else if(bitmap.PixelWidth()>0 && bitmap.PixelHeight()>0 && bitmap.PixelWidth()<=1024 && bitmap.PixelHeight()<=256) {
                dragBitmap=winrt::Windows::Graphics::Imaging::SoftwareBitmap::CreateCopyFromBuffer(pixels,
                    winrt::Windows::Graphics::Imaging::BitmapPixelFormat::Bgra8,bitmap.PixelWidth(),bitmap.PixelHeight(),
                    winrt::Windows::Graphics::Imaging::BitmapAlphaMode::Premultiplied);
                args.DragUI().SetContentFromSoftwareBitmap(dragBitmap,{float(bitmap.PixelWidth())/2,float(bitmap.PixelHeight())/2});
            } else args.Cancel(true);
        } catch (...) {args.Cancel(true);}
        deferral.Complete();
    }
    static Microsoft::UI::Xaml::Controls::Control FocusControl(Microsoft::UI::Xaml::FrameworkElement const& element) {
        using namespace Microsoft::UI::Xaml::Controls;
        if(auto control=element.try_as<Control>())return control;
        if(auto panel=element.try_as<Panel>())for(auto child:panel.Children())if(auto control=child.try_as<Control>())return control;
        return nullptr;
    }
    bool FocusFirstPlaced() {
        for(auto const& item:state.items)if(item.placed) {
            auto found=placedControls.find(item.id);if(found==placedControls.end())continue;
            if(auto control=FocusControl(found->second);control && control.IsEnabled())return control.Focus(Microsoft::UI::Xaml::FocusState::Keyboard);
        }
        return false;
    }
    void RestoreKeyboardFocus() {
        if(focusAfterChange.empty() || state.revision==focusRevision)return;
        auto id=focusAfterChange;focusAfterChange=L"";auto revision=state.revision;auto alive=std::weak_ptr<int>(lifetime);
        anchor.DispatcherQueue().TryEnqueue([this,alive,id,revision]{
            if(alive.expired() || !editing || state.revision!=revision)return;
            if(auto found=placedControls.find(id);found!=placedControls.end()) {
                if(auto control=FocusControl(found->second))control.Focus(Microsoft::UI::Xaml::FocusState::Keyboard);
            } else if(available) {
                for(auto value:availableItems)if(Id(value)==id){available.SelectedItem(value);available.ScrollIntoView(value);break;}
                if(!available.SelectedItem())for(auto const& item:state.items)if(item.control==L"new-spring") {
                    for(auto value:availableItems)if(Id(value)==item.id){available.SelectedItem(value);available.ScrollIntoView(value);break;}
                    break;
                }
                available.Focus(Microsoft::UI::Xaml::FocusState::Keyboard);
                if(auto selected=available.SelectedItem())if(auto row=available.ContainerFromItem(selected).try_as<Microsoft::UI::Xaml::Controls::Control>())row.Focus(Microsoft::UI::Xaml::FocusState::Keyboard);
            }
        });
    }
    void OpenItemMenu(Microsoft::UI::Xaml::FrameworkElement const& control) {
        using namespace Microsoft::UI::Xaml;using namespace Microsoft::UI::Xaml::Controls;
        if(!editing || dragging || !control.Tag())return;
        auto id=winrt::unbox_value<winrt::hstring>(control.Tag());
        std::vector<Item> placed;for(auto const& item:state.items)if(item.placed)placed.push_back(item);
        auto found=std::find_if(placed.begin(),placed.end(),[&](auto const& item){return item.id==id;});
        if(found==placed.end() || !found->movable || found->control==L"address")return;
        const auto index=size_t(found-placed.begin());auto revision=state.revision;auto alive=std::weak_ptr<int>(lifetime);
        if(itemMenu)itemMenu.Hide();itemMenu=MenuFlyout();auto menu=itemMenu;
        menu.Opened([this,alive](auto const&,auto const&){if(!alive.expired())itemMenuOpen=true;});
        menu.Closed([this,alive,weak=winrt::make_weak(menu)](auto const&,auto const&){if(!alive.expired() && itemMenu==weak.get())itemMenuOpen=false;});
        auto add=[&](wchar_t const* label,wchar_t const* verb,winrt::hstring const& target,bool enabled) {
            MenuFlyoutItem action;action.Text(state.labels.at(label));action.IsEnabled(enabled);
            Automation::AutomationProperties::SetAutomationId(action,winrt::hstring(L"FirefoxWinUICustomizeAction-")+label);
            action.Click([this,alive,id,revision,verb=std::wstring(verb),target](auto const&,auto const&){
                if(alive.expired() || !editing || revision!=state.revision)return;
                focusAfterChange=id;focusRevision=revision;Send(id,verb.c_str(),target,revision);
            });
            menu.Items().Append(action);
        };
        add(L"earlier",L"before",index?placed[index-1].id:winrt::hstring{},index>0);
        add(L"later",index+2<placed.size()?L"before":L"end",index+2<placed.size()?placed[index+2].id:winrt::hstring{},index+1<placed.size());
        add(L"remove",L"remove",{},found->removable);
        add(L"overflow",L"overflow",{},found->canOverflow);
        menu.ShowAt(control);
    }
    void BindToolbarControl(Microsoft::UI::Xaml::FrameworkElement const& control,Item const& item) {
        if(item.control==L"address")return;
        placedControls.insert_or_assign(item.id,control);
        if(!control.Tag()) {
            auto alive=std::weak_ptr<int>(lifetime);
            control.DragStarting([this,alive](auto const& sender,auto const& args){
                if(alive.expired()){args.Cancel(true);return;}
                auto element=sender.template as<Microsoft::UI::Xaml::FrameworkElement>();auto id=winrt::unbox_value<winrt::hstring>(element.Tag());
                if(!BeginDrag(id,args.Data())){args.Cancel(true);return;}CaptureDrag(args,element,id);
            });
            control.PreviewKeyDown([this,alive](auto const& sender,auto const& args){
                if(alive.expired() || !editing)return;
                if(args.Key()==winrt::Windows::System::VirtualKey::Tab && (GetKeyState(VK_SHIFT)&0x8000)) {
                    auto id=winrt::unbox_value<winrt::hstring>(sender.template as<Microsoft::UI::Xaml::FrameworkElement>().Tag());
                    auto first=std::find_if(state.items.begin(),state.items.end(),[](auto const& item){return item.placed;});
                    if(first!=state.items.end() && first->id==id) {
                        if(undoReset.IsEnabled())undoReset.Focus(Microsoft::UI::Xaml::FocusState::Keyboard);
                        else if(reset.IsEnabled())reset.Focus(Microsoft::UI::Xaml::FocusState::Keyboard);
                        else available.Focus(Microsoft::UI::Xaml::FocusState::Keyboard);
                        args.Handled(true);return;
                    }
                }
                if(args.Key()==winrt::Windows::System::VirtualKey::Application ||
                    (args.Key()==winrt::Windows::System::VirtualKey::F10 && (GetKeyState(VK_SHIFT)&0x8000))) {
                    args.Handled(true);OpenItemMenu(sender.template as<Microsoft::UI::Xaml::FrameworkElement>());
                }
            });
            control.ContextRequested([this,alive](auto const& sender,auto const& args){
                if(alive.expired() || !editing)return;args.Handled(true);OpenItemMenu(sender.template as<Microsoft::UI::Xaml::FrameworkElement>());
            });
            control.DropCompleted([this,alive](auto const&,auto const&){if(!alive.expired())EndDrag();});
        }
        control.Tag(winrt::box_value(item.id));control.CanDrag(editing && item.movable);
        if(item.spring){control.IsHitTestVisible(editing);control.as<Microsoft::UI::Xaml::Controls::Grid>().Background(winrt::Microsoft::UI::Xaml::Media::SolidColorBrush(winrt::Windows::UI::Colors::Transparent()));}
    }
    bool DismissOnEscape() {
        if(itemMenuOpen){itemMenu.Hide();return true;}
        if(!dragging && !editing)return false;
        dragCancelled=true;Hide();return true;
    }
    void Send(winrt::hstring const& id,wchar_t const* verb,winrt::hstring const& target={},uint64_t revision=0) {
        if(!activate || id.empty() || !state.revision) return;
        auto command=std::wstring(L"customize:")+std::to_wstring(revision?revision:state.revision)+L":"+id.c_str()+L":"+verb;
        if(!target.empty()) command+=std::wstring(L":")+target.c_str();
        activate(command.c_str(),0,anchor);
    }
    void SendGlobal(wchar_t const* verb, bool editor=false) {
        if(!activate || !state.revision)return;
        auto command=std::wstring(L"customize:")+(editor?std::wstring(L"editor"):std::to_wstring(state.revision))+L":"+verb;
        activate(command.c_str(),0,anchor);
    }
    void Buttons() {reset.IsEnabled(state.canReset);undoReset.IsEnabled(state.canUndoReset);}
    void PopulatePreview(Microsoft::UI::Xaml::Controls::ContainerContentChangingEventArgs const& args) {
        if(args.InRecycleQueue() || !createPreview)return;
        auto id=Id(args.Item());
        auto found=std::find_if(state.items.begin(),state.items.end(),[&](auto const& item){return item.id==id;});
        if(found==state.items.end())return;
        auto const& item=*found;
        auto presenter=args.ItemContainer().ContentTemplateRoot().try_as<Microsoft::UI::Xaml::Controls::ContentPresenter>();
        if(!presenter)return;
        auto preview=createPreview(item);preview.IsHitTestVisible(false);
        if(auto control=preview.try_as<Microsoft::UI::Xaml::Controls::Control>())control.IsTabStop(false);
        Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(preview,item.label);
        Microsoft::UI::Xaml::Automation::AutomationProperties::SetAutomationId(preview,L"FirefoxWinUICustomizePreview-"+(item.control.empty()?item.id:item.control));
        Microsoft::UI::Xaml::Automation::AutomationProperties::SetAccessibilityView(preview,Microsoft::UI::Xaml::Automation::Peers::AccessibilityView::Content);
        Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(preview,winrt::box_value(item.label));
        previews.insert_or_assign(item.id,preview);presenter.Content(preview);
    }
    void Refresh() {
        if(!available) return;
        updating=true;availableItems.Clear();previews.clear();if(clearPreviews)clearPreviews();
        for(auto const& item:state.items) {
            if(item.placed || item.control==L"address")continue;
            auto value=winrt::single_threaded_observable_map<winrt::hstring,winrt::Windows::Foundation::IInspectable>();
            value.Insert(L"Id",winrt::box_value(item.id));value.Insert(L"Label",winrt::box_value(item.label));
            availableItems.Append(value);
        }
        updating=false;Buttons();
    }
    void Apply(State next,bool visible) {
        bool changed=next.revision!=state.revision;
        state=std::move(next);
        if(!visible || !state.revision) {Hide();return;}
        if(available) {
            for(auto const& [key,block]:textBlocks) block.Text(state.labels.at(key));
            for(auto const& [key,control]:std::initializer_list<std::pair<wchar_t const*,Button>>{{L"reset",reset},{L"undo-reset",undoReset}}) control.Content(winrt::box_value(state.labels.at(key)));
        }
        int position=0,total=0;for(auto const& item:state.items)if(item.placed)total++;
        for(auto const& item:state.items)if(item.placed) {
            position++;auto entry=placedControls.find(item.id);if(entry==placedControls.end())continue;
            if(auto control=FocusControl(entry->second)) {
                Microsoft::UI::Xaml::Automation::AutomationProperties::SetPositionInSet(control,position);
                Microsoft::UI::Xaml::Automation::AutomationProperties::SetSizeOfSet(control,total);
            }
        }
        if(changed && !dragging) {if(itemMenu)itemMenu.Hide();Refresh();}
        RestoreKeyboardFocus();
    }
    void Open(Button const& button) {
        using namespace Microsoft::UI::Xaml;
        using namespace Microsoft::UI::Xaml::Controls;
        if(!state.revision) return;
        anchor=button;
        if(!popup) {
            popup=Primitives::Popup();popup.ShouldConstrainToRootBounds(false);popup.IsLightDismissEnabled(false);StackPanel content;content.Width(280);content.Spacing(8);
            auto text=[&](wchar_t const* key){TextBlock t;textBlocks[key]=t;t.Text(state.labels.at(key));t.TextWrapping(TextWrapping::Wrap);content.Children().Append(t);};
            text(L"title");
            available=GridView();available.ItemsSource(availableItems);available.MaxHeight(240);available.MinHeight(40);available.CanDragItems(true);available.AllowDrop(true);
            Automation::AutomationProperties::SetAutomationId(available,L"FirefoxWinUICustomizeAvailable");
            Automation::AutomationProperties::SetName(available,state.labels.at(L"available"));content.Children().Append(available);
            StackPanel defaults;defaults.Orientation(Orientation::Horizontal);defaults.Spacing(8);content.Children().Append(defaults);
            auto make=[&](wchar_t const* key){Button b;b.Content(winrt::box_value(state.labels.at(key)));Automation::AutomationProperties::SetAutomationId(b,winrt::hstring(L"FirefoxWinUICustomize-")+key);defaults.Children().Append(b);return b;};
            reset=make(L"reset");undoReset=make(L"undo-reset");
            auto alive=std::weak_ptr<int>(lifetime);
            for(auto button:{reset,undoReset})button.PreviewKeyDown([this,alive](auto const& sender,auto const& args){
                if(alive.expired() || args.Key()!=winrt::Windows::System::VirtualKey::Tab || (GetKeyState(VK_SHIFT)&0x8000))return;
                if(sender==undoReset || !undoReset.IsEnabled())args.Handled(FocusFirstPlaced());
            });
            popup.Opened([this,alive](auto const&,auto const&){if(!alive.expired()){editing=true;SendGlobal(L"open",true);if(editorChanged)editorChanged();available.Focus(FocusState::Keyboard);}});
            popup.Closed([this,alive](auto const&,auto const&){if(!alive.expired()){editing=false;dragCancelled=true;SendGlobal(L"close",true);if(editorChanged)editorChanged();}});
            reset.Click([this,alive](auto const&,auto const&){if(!alive.expired())SendGlobal(L"reset");});
            undoReset.Click([this,alive](auto const&,auto const&){if(!alive.expired())SendGlobal(L"undo-reset");});
            auto prepare=[this,alive](auto const&,ContainerContentChangingEventArgs const& args) {
                if(alive.expired() || args.InRecycleQueue())return;
                auto id=Id(args.Item());
                auto found=std::find_if(state.items.begin(),state.items.end(),[&](auto const& item){return item.id==id;});
                if(found==state.items.end())return;
                auto row=args.ItemContainer();
                Automation::AutomationProperties::SetAutomationId(row,L"FirefoxWinUICustomize-"+(found->control.empty()?found->id:found->control));
                Automation::AutomationProperties::SetName(row,found->label);
                ToolTipService::SetToolTip(row,winrt::box_value(found->label));
                row.Padding({0,0,0,0});row.Margin({2,2,2,2});row.MinWidth(32);row.MinHeight(32);
                if(!row.Tag())row.DragStarting([this,alive](auto const& sender,auto const& args){
                    if(alive.expired())return;auto id=winrt::unbox_value<winrt::hstring>(sender.template as<FrameworkElement>().Tag());
                    if(previews.contains(id))CaptureDrag(args,previews.at(id),id);
                });
                row.Tag(winrt::box_value(id));
                // A reordered data item may briefly have two containers. Give each its
                // own WinUI element instead of binding one UIElement into both trees.
                args.RegisterUpdateCallback([this,alive](auto const&,auto const& phase){if(!alive.expired())PopulatePreview(phase);});
            };
            auto itemTemplate=Markup::XamlReader::Load(LR"(<DataTemplate xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"><ContentPresenter/></DataTemplate>)").as<DataTemplate>();
            available.ItemTemplate(itemTemplate);
            available.ContainerContentChanging(prepare);
            content.PreviewKeyDown([this,alive](auto const&,auto const& args){
                if(!alive.expired() && args.Key()==winrt::Windows::System::VirtualKey::Escape){dragCancelled=true;Hide();anchor.Focus(FocusState::Keyboard);args.Handled(true);}
            });
            available.PreviewKeyDown([this,alive](auto const&,auto const& args){
                if(alive.expired())return;
                if(args.Key()==winrt::Windows::System::VirtualKey::Tab) {
                    if(GetKeyState(VK_SHIFT)&0x8000){anchor.Focus(FocusState::Keyboard);args.Handled(true);}
                    else if(!reset.IsEnabled() && !undoReset.IsEnabled())args.Handled(FocusFirstPlaced());
                    return;
                }
                if(args.Key()!=winrt::Windows::System::VirtualKey::Enter)return;
                auto id=Id(available.SelectedItem());if(!id.empty()){Send(id,L"add");args.Handled(true);}
            });
            available.DragItemsStarting([this,alive](auto const&,auto const& args){
                if(alive.expired())return;
                if(args.Items().Size()!=1 || !BeginDrag(Id(args.Items().GetAt(0)),args.Data()))args.Cancel(true);
            });
            available.DragItemsCompleted([this,alive](auto const&,auto const&){if(!alive.expired())EndDrag();});
            available.DragOver([this,alive](auto const&,auto const& args){if(!alive.expired() && Accept(args,true))args.Handled(true);});
            available.Drop([this,alive](auto const&,auto const& args){if(!alive.expired())DropAt(args,{},true);});
            FlyoutPresenter presenter;presenter.Content(content);popup.Child(presenter);
        }
        popup.Child().as<FrameworkElement>().FlowDirection(button.FlowDirection());
        popup.XamlRoot(button.XamlRoot());popup.PlacementTarget(button);
        popup.DesiredPlacement(button.FlowDirection()==FlowDirection::RightToLeft?Primitives::PopupPlacementMode::BottomEdgeAlignedLeft:Primitives::PopupPlacementMode::BottomEdgeAlignedRight);
        Refresh();popup.IsOpen(true);
    }
};
