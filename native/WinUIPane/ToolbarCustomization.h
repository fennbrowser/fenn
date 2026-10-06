#pragma once
#include "JsonValidation.h"
#include <map>
#include <set>

struct NativeToolbarCustomization {
    using JObject = winrt::Windows::Data::Json::JsonObject;
    using Map = winrt::Windows::Foundation::Collections::IObservableMap<winrt::hstring,winrt::Windows::Foundation::IInspectable>;
    using Button = Microsoft::UI::Xaml::Controls::Button;
    struct Item { winrt::hstring id, control, label; bool placed{}, movable{}, removable{}; };
    struct State { uint64_t revision{}; bool canReset{}, canUndoReset{}; std::vector<Item> items; std::map<winrt::hstring,winrt::hstring> labels; };
    State state;
    winrt::Windows::Foundation::Collections::IObservableVector<winrt::Windows::Foundation::IInspectable> placedItems = winrt::single_threaded_observable_vector<winrt::Windows::Foundation::IInspectable>();
    winrt::Windows::Foundation::Collections::IObservableVector<winrt::Windows::Foundation::IInspectable> availableItems = winrt::single_threaded_observable_vector<winrt::Windows::Foundation::IInspectable>();
    Microsoft::UI::Xaml::Controls::Flyout flyout{nullptr};
    Microsoft::UI::Xaml::Controls::ListView list{nullptr}, available{nullptr};
    std::map<winrt::hstring,Microsoft::UI::Xaml::Controls::TextBlock> textBlocks;
    Button earlier{nullptr}, later{nullptr}, remove{nullptr}, add{nullptr}, done{nullptr}, reset{nullptr}, undoReset{nullptr};
    Button anchor{nullptr};
    bool updating{}, dragging{}, dragCancelled{};
    uint64_t dragRevision{};
    winrt::hstring dragId;
    std::function<void(wchar_t const*, uint32_t, Button const&)> activate;
    std::shared_ptr<int> lifetime=std::make_shared<int>(0);
    ~NativeToolbarCustomization() { lifetime.reset(); if(flyout) flyout.Hide(); }
    static State Read(JObject const& snapshot) {
        State state;
        if (!snapshot.HasKey(L"navigation")) return state;
        auto nav=snapshot.GetNamedObject(L"navigation");
        if (!nav.HasKey(L"customization")) return state;
        auto obj=nav.GetNamedObject(L"customization");
        state.revision=NativeJson::RequireRevision(obj,L"revision");
        state.canReset=obj.GetNamedBoolean(L"canReset",false);state.canUndoReset=obj.GetNamedBoolean(L"canUndoReset",false);
        auto items=obj.GetNamedArray(L"items");
        if(items.Size()>38) throw winrt::hresult_invalid_argument();
        std::set<winrt::hstring> ids,controls;
        bool address=false;
        for(auto value:items) {
            auto row=value.GetObject();
            Item item{row.GetNamedString(L"id"),row.GetNamedString(L"control"),row.GetNamedString(L"label"),
                row.GetNamedBoolean(L"placed"),row.GetNamedBoolean(L"movable"),row.GetNamedBoolean(L"removable")};
            if(item.id.empty() || item.id.size()>64 || item.control.size()>64 || item.label.size()>4096 || !ids.insert(item.id).second ||
                (item.placed && (item.control.empty() || !controls.insert(item.control).second))) throw winrt::hresult_invalid_argument();
            address |= item.placed && item.control==L"address";
            state.items.push_back(std::move(item));
        }
        if(!address) return {};
        auto labels=obj.GetNamedObject(L"labels");
        for(auto key:{L"title",L"available",L"earlier",L"later",L"add",L"remove",L"done",L"hint",L"reset",L"undo-reset",L"reset-hint"}) {
            auto value=labels.GetNamedString(key);NativeJson::RequireLength(value,4096);state.labels[key]=value;
        }
        return state;
    }
    static winrt::hstring Id(winrt::Windows::Foundation::IInspectable const& item) {
        return item ? winrt::unbox_value<winrt::hstring>(item.as<Map>().Lookup(L"Id")) : winrt::hstring{};
    }
    bool CancelDrag() {
        if(!dragging)return false;
        dragCancelled=true;if(flyout)flyout.Hide();return true;
    }
    winrt::hstring Selected(Microsoft::UI::Xaml::Controls::ListView const& source) {
        auto item=source.SelectedItem();
        return Id(item);
    }
    void Send(winrt::hstring const& id,wchar_t const* verb,winrt::hstring const& target={},uint64_t revision=0) {
        if(!activate || id.empty() || !state.revision) return;
        auto command=std::wstring(L"customize:")+std::to_wstring(revision?revision:state.revision)+L":"+id.c_str()+L":"+verb;
        if(!target.empty()) command+=std::wstring(L":")+target.c_str();
        activate(command.c_str(),0,anchor);
    }
    std::vector<winrt::hstring> Placed() {
        std::vector<winrt::hstring> ids;
        for(auto const& item:state.items) if(item.placed) ids.push_back(item.id);
        return ids;
    }
    void SendGlobal(wchar_t const* verb, bool editor=false) {
        if(!activate || !state.revision)return;
        auto command=std::wstring(L"customize:")+(editor?std::wstring(L"editor"):std::to_wstring(state.revision))+L":"+verb;
        activate(command.c_str(),0,anchor);
    }
    void Buttons() {
        reset.IsEnabled(state.canReset);undoReset.IsEnabled(state.canUndoReset);
        auto id=Selected(list), free=Selected(available);
        auto ids=Placed();auto it=std::find(ids.begin(),ids.end(),id);
        auto row=std::find_if(state.items.begin(),state.items.end(),[&](auto const& item){return item.id==id;});
        bool movable=row!=state.items.end() && row->movable;
        earlier.IsEnabled(movable && it!=ids.end() && it!=ids.begin());
        later.IsEnabled(movable && it!=ids.end() && it+1!=ids.end());
        remove.IsEnabled(row!=state.items.end() && row->removable);
        auto unused=std::find_if(state.items.begin(),state.items.end(),[&](auto const& item){return item.id==free;});
        add.IsEnabled(unused!=state.items.end() && unused->movable);
    }
    void Refresh() {
        if(!list) return;
        auto selected=Selected(list), free=Selected(available);
        updating=true;placedItems.Clear();availableItems.Clear();
        for(auto const& item:state.items) {
            auto value=winrt::single_threaded_observable_map<winrt::hstring,winrt::Windows::Foundation::IInspectable>();
            value.Insert(L"Id",winrt::box_value(item.id));value.Insert(L"Label",winrt::box_value(item.label));
            (item.placed?placedItems:availableItems).Append(value);
            if(item.id==(item.placed?selected:free)) (item.placed?list:available).SelectedItem(value);
        }
        updating=false;Buttons();
    }
    void Apply(State next,bool visible) {
        bool changed=next.revision!=state.revision;
        state=std::move(next);
        if(!visible || !state.revision) {if(flyout) flyout.Hide();return;}
        if(list) {
            for(auto const& [key,block]:textBlocks) block.Text(state.labels.at(key));
            for(auto const& [key,control]:std::initializer_list<std::pair<wchar_t const*,Button>>{{L"earlier",earlier},{L"later",later},{L"remove",remove},{L"add",add},{L"done",done},{L"reset",reset},{L"undo-reset",undoReset}}) control.Content(winrt::box_value(state.labels.at(key)));
        }
        if(changed && !dragging) Refresh();
    }
    void Open(Button const& button) {
        using namespace Microsoft::UI::Xaml;
        using namespace Microsoft::UI::Xaml::Controls;
        if(!state.revision) return;
        anchor=button;
        if(!flyout) {
            flyout=Flyout();flyout.ShouldConstrainToRootBounds(false);flyout.Placement(Microsoft::UI::Xaml::Controls::Primitives::FlyoutPlacementMode::BottomEdgeAlignedRight);StackPanel content;content.Width(420);content.Spacing(8);
            auto text=[&](wchar_t const* key){TextBlock t;textBlocks[key]=t;t.Text(state.labels.at(key));t.TextWrapping(TextWrapping::Wrap);content.Children().Append(t);};
            text(L"title");text(L"hint");
            list=ListView();list.ItemsSource(placedItems);list.MaxHeight(280);list.CanDragItems(true);list.CanReorderItems(true);list.AllowDrop(true);
            Automation::AutomationProperties::SetAutomationId(list,L"FirefoxWinUICustomizeList");
            Automation::AutomationProperties::SetName(list,state.labels.at(L"title"));content.Children().Append(list);
            StackPanel actions;actions.Orientation(Orientation::Horizontal);actions.Spacing(4);content.Children().Append(actions);
            auto make=[&](wchar_t const* key,StackPanel const& parent){Button b;b.Content(winrt::box_value(state.labels.at(key)));Automation::AutomationProperties::SetAutomationId(b,winrt::hstring(L"FirefoxWinUICustomize-")+key);parent.Children().Append(b);return b;};
            earlier=make(L"earlier",actions);later=make(L"later",actions);remove=make(L"remove",actions);
            text(L"available");available=ListView();available.ItemsSource(availableItems);available.MaxHeight(120);
            Automation::AutomationProperties::SetAutomationId(available,L"FirefoxWinUICustomizeAvailable");
            Automation::AutomationProperties::SetName(available,state.labels.at(L"available"));content.Children().Append(available);
            StackPanel bottom;bottom.Orientation(Orientation::Horizontal);bottom.Spacing(8);content.Children().Append(bottom);
            add=make(L"add",bottom);done=make(L"done",bottom);
            text(L"reset-hint");
            StackPanel defaults;defaults.Orientation(Orientation::Horizontal);defaults.Spacing(8);content.Children().Append(defaults);
            reset=make(L"reset",defaults);undoReset=make(L"undo-reset",defaults);
            auto alive=std::weak_ptr<int>(lifetime);
            flyout.Opened([this,alive](auto const&,auto const&){if(!alive.expired())SendGlobal(L"open",true);});
            flyout.Closed([this,alive](auto const&,auto const&){if(!alive.expired())SendGlobal(L"close",true);});
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
            };
            auto itemTemplate=Markup::XamlReader::Load(LR"(<DataTemplate xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"><TextBlock Text="{Binding [Label]}" TextTrimming="CharacterEllipsis"/></DataTemplate>)").as<DataTemplate>();
            list.ItemTemplate(itemTemplate);available.ItemTemplate(itemTemplate);
            list.ContainerContentChanging(prepare);available.ContainerContentChanging(prepare);
            flyout.Closing([this,alive](auto const&,auto const&){if(!alive.expired() && dragging)dragCancelled=true;});
            content.PreviewKeyDown([this,alive](auto const&,auto const& args){
                if(!alive.expired() && args.Key()==winrt::Windows::System::VirtualKey::Escape && CancelDrag())args.Handled(true);
            });
            list.SelectionChanged([this,alive](auto const&,auto const&){if(!alive.expired()&&!updating)Buttons();});
            available.SelectionChanged([this,alive](auto const&,auto const&){if(!alive.expired()&&!updating)Buttons();});
            earlier.Click([this,alive](auto const&,auto const&){if(alive.expired())return;auto ids=Placed();auto id=Selected(list);auto it=std::find(ids.begin(),ids.end(),id);if(it!=ids.end()&&it!=ids.begin())Send(id,L"before",*(it-1));});
            later.Click([this,alive](auto const&,auto const&){if(alive.expired())return;auto ids=Placed();auto id=Selected(list);auto it=std::find(ids.begin(),ids.end(),id);if(it!=ids.end()&&it+1!=ids.end())Send(id,it+2==ids.end()?L"end":L"before",it+2==ids.end()?winrt::hstring{}:*(it+2));});
            remove.Click([this,alive](auto const&,auto const&){if(!alive.expired())Send(Selected(list),L"remove");});
            add.Click([this,alive](auto const&,auto const&){if(!alive.expired())Send(Selected(available),L"add");});
            done.Click([this,alive](auto const&,auto const&){if(!alive.expired())flyout.Hide();});
            list.DragItemsStarting([this,alive](auto const&,auto const& args){
                if(alive.expired())return;
                if(args.Items().Size()!=1){args.Cancel(true);return;}
                args.Data().RequestedOperation(winrt::Windows::ApplicationModel::DataTransfer::DataPackageOperation::Move);
                dragId=Id(args.Items().GetAt(0));
                auto item=std::find_if(state.items.begin(),state.items.end(),[&](auto const& row){return row.id==dragId;});
                if(item==state.items.end() || !item->movable){args.Cancel(true);return;}
                dragRevision=state.revision;dragging=true;dragCancelled=false;
            });
            list.DragItemsCompleted([this,alive](auto const&,auto const& args){
                if(alive.expired())return;dragging=false;
                if(args.DropResult()==winrt::Windows::ApplicationModel::DataTransfer::DataPackageOperation::Move && !dragCancelled && dragRevision==state.revision){
                    for(uint32_t i=0;i<list.Items().Size();i++) {
                        if(Id(list.Items().GetAt(i))!=dragId)continue;
                        auto target=i+1<list.Items().Size()?Id(list.Items().GetAt(i+1)):winrt::hstring{};
                        Send(dragId,target.empty()?L"end":L"before",target,dragRevision);break;
                    }
                }
                Refresh();
            });
            flyout.Content(content);
        }
        flyout.Content().as<FrameworkElement>().FlowDirection(button.FlowDirection());
        flyout.Placement(button.FlowDirection()==FlowDirection::RightToLeft?Primitives::FlyoutPlacementMode::BottomEdgeAlignedLeft:Primitives::FlyoutPlacementMode::BottomEdgeAlignedRight);
        Refresh();flyout.ShowAt(button);
    }
};
