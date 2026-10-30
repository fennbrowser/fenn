#pragma once

// Local-draft presenter with explicit live-browser attachment. Not wired into
// ordinary startup until restore/editor adoption can settle these drafts. Uses
// the same Pane/NativeAddress/NativeTabStrip objects as connected presentation.
namespace LocalStartup {
inline void Require(bool value){if(!value)throw_hresult(E_FAIL);}
struct Presenter {
    struct Reset { bool& value; ~Reset(){value=false;} };
    Pane& pane;
    bool applying{};
    bool browserEditor{};
    std::function<void()> browserAdd;
    ShellStartupView view{};
    // Presentation IDs never double as Firefox IDs. Rust projects the explicit
    // draft/browser identity mappings used by these callbacks.
    std::map<uint32_t,uint64_t> drafts;
    std::map<uint32_t,uint64_t> browsers;
    std::function<hstring(uint64_t)> browserTitle;
    std::function<void(ShellStatus)> outcome;
    std::function<void()> restore;
#ifdef WINUI_PANE_TESTING
    std::function<bool()> sameControls;
    std::function<bool()> sameEditor;
#endif
    // Supplied by Firefox once the early localization path is available.
    // These are pre-publication fallbacks, not browser policy or search labels.
    struct Labels {
        hstring newTab{L"New tab"}, pending{L"Waiting for browser"},
            full{L"Cannot queue this action yet. Your text has been kept."},
            composition{L"Finish composing before changing tabs."};
    } labels;
    hstring blocked;
    Microsoft::UI::Xaml::Controls::Button recoveryButton{nullptr}, recoveryAction{nullptr};
    Microsoft::UI::Xaml::Controls::Flyout recoveryFlyout{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock recoveryMessage{nullptr};
    Microsoft::UI::Xaml::Controls::ColumnDefinition recoveryColumn{nullptr};
    Microsoft::UI::Xaml::Controls::Grid recoveryHost{nullptr};
    winrt::event_token recoveryClick{};
    hstring recoverySession, recoveryRevision;
    void Recovery(hstring const& title,hstring const& action,bool allowed,
                  hstring const& session,hstring const& revision) {
        using namespace Microsoft::UI::Xaml;
        using namespace Microsoft::UI::Xaml::Controls;
        if(!recoveryButton && !blocked.empty()) {
            recoveryButton=Button();recoveryButton.Height(32);recoveryButton.Margin({4,0,0,0});
            Automation::AutomationProperties::SetAutomationId(recoveryButton,L"FennStartupRecovery");
            recoveryFlyout=Flyout();recoveryFlyout.ShouldConstrainToRootBounds(false);
            recoveryFlyout.Placement(Primitives::FlyoutPlacementMode::BottomEdgeAlignedRight);
            recoveryMessage=TextBlock();
            recoveryMessage.TextWrapping(TextWrapping::Wrap);recoveryMessage.MaxWidth(320);
            recoveryAction=Button();
            Automation::AutomationProperties::SetAutomationId(recoveryAction,L"FennStartupRecoveryAction");
            StackPanel content;content.Spacing(12);content.Children().Append(recoveryMessage);
            content.Children().Append(recoveryAction);recoveryFlyout.Content(content);
            recoveryButton.Flyout(recoveryFlyout);
            recoveryClick=recoveryAction.Click([this](auto const&,auto const&) {
                NativeTabMenu::Object request;
                request.Insert(L"kind",NativeTabMenu::Value::CreateStringValue(L"startup-recovery"));
                request.Insert(L"session",NativeTabMenu::Value::CreateStringValue(recoverySession));
                request.Insert(L"revision",NativeTabMenu::Value::CreateStringValue(recoveryRevision));
                if(pane.Enqueue(NativeShellCore::StripAction,0,0,request.Stringify()))recoveryFlyout.Hide();
            });
            recoveryHost=pane.address->field.Parent().as<Grid>();
            recoveryColumn=ColumnDefinition();recoveryColumn.Width(GridLengthHelper::Auto());
            Grid::SetColumn(recoveryButton,recoveryHost.ColumnDefinitions().Size());
            recoveryHost.ColumnDefinitions().Append(recoveryColumn);
            recoveryHost.Children().Append(recoveryButton);
        }
        if(!recoveryButton)return;
        if(recoveryRevision!=revision)recoveryFlyout.Hide();
        recoverySession=session;recoveryRevision=revision;
        recoveryButton.Content(box_value(title));
        recoveryMessage.Text(blocked);recoveryAction.Content(box_value(action));
        recoveryAction.Visibility(allowed?Visibility::Visible:Visibility::Collapsed);
        recoveryButton.Visibility(blocked.empty()?Visibility::Collapsed:Visibility::Visible);
    }
    explicit Presenter(Pane& owner,bool deferred=false) : pane(owner) {if(!deferred)Initialize();}
    void Initialize() {
        auto& address=*pane.address;
        browserAdd=pane.strip->add;
        restore=[this,add=pane.strip->add,select=pane.strip->select,close=pane.strip->close,
                 dispatch=pane.strip->dispatch,preview=pane.strip->preview,
                 drag=pane.strip->view.CanDragTabs(),reorder=pane.strip->view.CanReorderTabs(),
                 presentation=pane.strip->presentation,layout=pane.strip->groupLayout,hasLayout=pane.strip->hasGroupLayout]() mutable {
            pane.strip->add=add;pane.strip->select=select;pane.strip->close=close;
            pane.strip->dispatch=dispatch;pane.strip->preview=preview;
            pane.strip->view.CanDragTabs(drag);pane.strip->view.CanReorderTabs(reorder);
            pane.strip->presentation=std::move(presentation);
            pane.strip->groupLayout=std::move(layout);pane.strip->hasGroupLayout=hasLayout;
        };
        BindLocalCallbacks();
        pane.strip->add=[this] {
            if(Switching()){Feedback(SHELL_STATUS_REJECTED);return;}
            if(browserEditor){if(browserAdd)browserAdd();return;}
            if(Observe() && Run(1)){Refresh(true);FocusEditor();}
        };
        pane.strip->select=[this](uint32_t id) {
            if(applying)return;
            if(Switching()){Feedback(SHELL_STATUS_REJECTED);Refresh(false);return;}
            if(!drafts.count(id)){
                if(browserEditor && browsers.count(id) && browsers.at(id)<=UINT32_MAX)
                    pane.QueueStripSelection(uint32_t(browsers.at(id)));
                else if(browsers.count(id))SwitchEditor(view.selected,browsers.at(id),false);
                else {Refresh(false);Feedback(SHELL_STATUS_REJECTED);}
                return;
            }
            if(browserEditor) {
                ShellStartupTransfer mapping{},state{};
                if(shell_core_startup_transfer(pane.shell.core,0,0,0,0,0,0,0,false,&state)==SHELL_STATUS_OK)
                    for(size_t i=0;i<state.mappings;++i)
                        if(shell_core_startup_mapping(pane.shell.core,state.session,state.generation,i,&mapping)==SHELL_STATUS_OK && mapping.draft==drafts.at(id)) {
                            SwitchEditor(mapping.draft,mapping.browser,true);return;
                        }
                Feedback(SHELL_STATUS_REJECTED);Refresh(false);return;
            }
            if(Observe() && Run(3,drafts.at(id)))Refresh(true);
            else Refresh(false);
        };
        pane.strip->close=[this](uint32_t id) {
            if(Switching()){Feedback(SHELL_STATUS_REJECTED);return;}
            if(!drafts.count(id)) {
                if(pane.tabTitles.size()<=1 && view.tabs){Feedback(SHELL_STATUS_REJECTED);return;}
                if(browsers.count(id) && browsers.at(id)<=UINT32_MAX)
                    pane.QueueClose(static_cast<uint32_t>(browsers.at(id)));
                return;
            }
            const bool selected=drafts.at(id)==view.selected;
            if(selected && pane.address->HasActiveComposition()){Feedback(SHELL_STATUS_REJECTED);return;}
            // Closing another tab must not rewrite this editor, including an
            // edit too large to admit. Closing this draft explicitly cancels it.
            if(Run(4,drafts.at(id))) {
                if(!view.tabs && browsers.empty())PostMessageW(pane.parent,WM_CLOSE,0,0);
                else if(!view.tabs){if(!browserEditor)pane.address->field.IsReadOnly(true);Refresh(false);}
                else Refresh(selected);
            }
        };
        // No browser menus, gestures or previews are valid before binding.
        pane.strip->dispatch={}; pane.strip->preview={};
        pane.strip->view.CanDragTabs(false);
        pane.strip->view.CanReorderTabs(false);
        // Browser IDs cannot index startup presentation metadata. Rich/grouped
        // layouts are rejected before attachment; never reuse their badges on
        // a different draft whose presentation ID happens to match.
        pane.strip->presentation.clear();
        pane.strip->groupLayout.clear();pane.strip->hasGroupLayout=true;
        pane.startupKey=[this](MSG const& message){return Key(message);};
        address.root.Visibility(Microsoft::UI::Xaml::Visibility::Visible);
        address.root.Margin({0,0,0,0});
        address.navigation->Apply({},true);
        address.navigation->Layout({});
        browserTitle=[this](uint64_t id){
            auto found=id<=UINT32_MAX?pane.tabTitles.find(static_cast<uint32_t>(id)):pane.tabTitles.end();
            return found==pane.tabTitles.end()?hstring{}:found->second;
        };
        Require(Run(0));
        if(!view.tabs)Require(Run(1));
        Refresh(true);
        pane.startupRender=[this]{Refresh(false);};
        pane.startupObserve=[this]{return browserEditor || !view.selected || Observe();};
        pane.startupAddressApply=[this](auto const& snapshot){return ApplyBrowserAddress(snapshot);};
#ifdef WINUI_PANE_TESTING
        std::map<uint64_t,Microsoft::UI::Xaml::Controls::TabViewItem> originalBrowsers;
        for(auto const& [row,browser]:browsers)originalBrowsers.emplace(browser,pane.strip->rows.at(row));
        sameEditor=[this,field=pane.address->field,source=pane.addressSource,originalBrowsers] {
            if(field!=pane.address->field || source!=pane.addressSource || !IsWindow(pane.parent))return false;
            for(auto const& [row,browser]:browsers) {
                auto old=originalBrowsers.find(browser);
                if(old!=originalBrowsers.end() && pane.strip->rows.at(row)!=old->second)return false;
            }
            return true;
        };
        pane.startupNormalIdentity=[&owner=pane,field=pane.address->field,source=pane.addressSource] {
            return field==owner.address->field && source==owner.addressSource && IsWindow(owner.parent);
        };
        const auto selectedRow=std::find_if(drafts.begin(),drafts.end(),[&](auto const& entry){return entry.second==view.selected;});
        const auto nativeId=selectedRow->first;
        sameControls=[this,field=pane.address->field,source=pane.addressSource,row=pane.strip->rows.at(nativeId),nativeId] {
            return field==pane.address->field && source==pane.addressSource && IsWindow(pane.parent) &&
                pane.strip->rows.contains(nativeId) && pane.strip->rows.at(nativeId)==row;
        };
#endif
    }
    ~Presenter() {
        if(recoveryButton) {
            recoveryFlyout.Hide();recoveryAction.Click(recoveryClick);
            uint32_t index{};
            if(recoveryHost.Children().IndexOf(recoveryButton,index))recoveryHost.Children().RemoveAt(index);
            if(recoveryHost.ColumnDefinitions().IndexOf(recoveryColumn,index))recoveryHost.ColumnDefinitions().RemoveAt(index);
        }
        if(!restore)return;
        pane.startupRender={};pane.startupObserve={};
        pane.startupAddressApply={};
        pane.address->field.IsReadOnly(false);
        pane.address->field.IsEnabled(true);pane.address->toolbarRoot.IsHitTestVisible(true);
        pane.address->startupEdit={}; pane.address->startupSubmit={};
        pane.startupKey={};
        restore();
    }
    void Feedback(ShellStatus result) {
        if(outcome)outcome(result);
        auto& field=pane.address->field;
        if(result==SHELL_STATUS_OK && blocked.empty()) {
            Microsoft::UI::Xaml::Automation::AutomationProperties::SetItemStatus(field,L"");
            Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(field,nullptr);
            return;
        }
        const auto text=!blocked.empty()?blocked:pane.address->HasActiveComposition() ? labels.composition : labels.full;
        Microsoft::UI::Xaml::Automation::AutomationProperties::SetItemStatus(field,text);
        Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(field,box_value(text));
    }
    bool Observe() {
        if(applying)return true;
        auto& address=*pane.address;const auto text=address.field.Text();
        const auto result=shell_core_startup_editor(pane.shell.core,view.selected,
            reinterpret_cast<const uint16_t*>(text.c_str()),text.size(),
            address.field.SelectionStart(),address.field.SelectionLength(),address.HasActiveComposition());
        Feedback(result);
        if(result==SHELL_STATUS_OK)NotifyStartup(pane);
        return result==SHELL_STATUS_OK;
    }
    void FocusEditor() {
        pane.addressSource.NavigateFocus(XamlSourceFocusNavigationRequest(XamlSourceFocusNavigationReason::First));
        pane.address->field.Focus(FocusState::Keyboard);
    }
    bool Key(MSG const& message) {
        if(message.message!=WM_KEYDOWN || !(GetKeyState(VK_CONTROL)&0x8000) ||
            (GetKeyState(VK_MENU)&0x8000))return false;
        const bool shift=(GetKeyState(VK_SHIFT)&0x8000)!=0;
        if(message.wParam==VK_TAB && view.tabs) {
            std::vector<ShellStartupRow> rows(SHELL_STARTUP_ROW_CAPACITY);uint32_t count{};
            if(shell_core_startup_rows(pane.shell.core,0,0,0,nullptr,0,0,rows.data(),rows.size(),&count)==SHELL_STATUS_OK && count) {
                size_t selected=0;for(size_t i=0;i<count;++i)if(rows[i].selected)selected=i;
                pane.strip->select(rows[(selected+(shift?count-1:1))%count].presentation);
            }
            return true;
        }
        if(browserEditor)return false;
        if(message.wParam=='T' && !shift){pane.strip->add();return true;}
        if(message.wParam=='L' && !shift){
            if(!pane.address->HasActiveComposition()){FocusEditor();pane.address->field.SelectAll();}
            return true;
        }
        if(message.wParam=='W' && !shift){
            for(auto const& [id,draft]:drafts)if(draft==view.selected){pane.strip->close(id);break;}
            return true;
        }
        return false;
    }
    void BindLocalCallbacks() {
        pane.address->startupEdit=[this](hstring const&){if(!applying)Observe();};
        pane.address->startupSubmit=[this](uint32_t modifiers){
            if(Observe() && Run(5,view.selected,modifiers))Refresh(false);
        };
    }
    bool Switching() const {
        ShellStartupTransfer state{};
        return shell_core_startup_transfer(pane.shell.core,0,0,0,0,0,0,0,false,&state)!=SHELL_STATUS_OK || state.kind==4 || state.kind==5;
    }
    void SwitchEditor(uint64_t draft,uint64_t browser,bool local) {
        if(pane.address->HasActiveComposition() || (!(GetKeyState(VK_TAB)&0x8000) && ((GetKeyState(VK_SHIFT)|GetKeyState(VK_CONTROL))&0x8000))) {
            Feedback(SHELL_STATUS_REJECTED);Refresh(false);return;
        }
        if(browserEditor)pane.address->Edit();
        else if(!Observe()){Refresh(false);return;}
        ShellStartupTransfer state{};
        if(shell_core_startup_transfer(pane.shell.core,0,0,0,0,0,0,0,false,&state)!=SHELL_STATUS_OK ||
            shell_core_startup_transfer(pane.shell.core,local?13:12,0,state.session,state.generation,0,draft,browser,false,&state)!=SHELL_STATUS_OK) {
            Feedback(SHELL_STATUS_REJECTED);Refresh(false);return;
        }
        pane.address->field.IsReadOnly(true);pane.address->field.IsEnabled(false);
        pane.address->toolbarRoot.IsHitTestVisible(false);Feedback(SHELL_STATUS_OK);
        NotifyStartup(pane);Refresh(false);
    }
    bool ApplyBrowserAddress(NativeAddress::JObject const& snapshot) {
        ShellStartupTransfer current{};
        if(shell_core_startup_transfer(pane.shell.core,0,0,0,0,0,0,0,false,&current)!=SHELL_STATUS_OK ||
            current.kind==4 || current.kind==5)return false;
        if(!browserEditor && current.adoption_ready) {
            if(!Observe())return false;
            Require(shell_core_startup_transfer(pane.shell.core,0,0,0,0,0,0,0,false,&current)==SHELL_STATUS_OK);
            if(!current.adoption_ready || current.lost || current.kind || pane.address->HasActiveComposition() ||
                !snapshot.GetNamedBoolean(L"enabled",false) ||
                snapshot.GetNamedString(L"startupBrowser",L"0")!=to_hstring(current.browser))return false;
            const hstring text{reinterpret_cast<const wchar_t*>(current.text),uint32_t(current.length)};
            if(snapshot.GetNamedString(L"text",L"")!=text)return false;
            const auto start=current.selection_start,length=current.selection_length;
            const auto browser=current.browser;
            // Keep the draft authoritative until the exact normal publication
            // has applied successfully on this same owner thread.
            const auto session=current.session,generation=current.generation;
            pane.address->ResetSessionState();pane.shell.ResetAddressPublication();pane.address->session=L"";
            applying=true;Reset reset{applying};
            // Gecko has not observed local editor focus yet. Its pre-handoff
            // content-focus bit must not release this live native field before
            // the ordinary focus request below reaches the browser proxy.
            pane.address->Sync(snapshot,true);
            pane.address->field.Select(start,length);
            if(pane.address->field.Text()!=text || pane.address->HasActiveComposition() || pane.address->session.empty())return false;
            if(shell_core_startup_transfer(pane.shell.core,16,0,session,generation,0,0,browser,false,&current)!=SHELL_STATUS_OK)return false;
            pane.address->startupEdit={};pane.address->startupSubmit={};
            pane.address->field.IsReadOnly(false);browserEditor=true;
            // Local focus never started a Firefox query. Admit the ordinary
            // focus request now so Enter works even without another text edit.
            if(pane.address->EditorOwned())pane.address->Action(L"focus");
            Feedback(SHELL_STATUS_OK);
            return true;
        }
        if(browserEditor)pane.address->observeFocus();
        if(!browserEditor) {
            ShellStartupTransfer transfer{};
            if(shell_core_startup_transfer(pane.shell.core,0,0,0,0,0,0,0,false,&transfer)!=SHELL_STATUS_OK ||
                !transfer.editor_browser || transfer.lost || pane.address->HasActiveComposition() ||
                !snapshot.GetNamedBoolean(L"enabled",false) ||
                snapshot.GetNamedString(L"startupBrowser",L"0")!=to_hstring(transfer.editor_browser))return false;
            pane.address->ResetSessionState();
            pane.shell.ResetAddressPublication();
            pane.address->session=L"";
        }
        applying=true;Reset reset{applying};
        pane.address->Sync(snapshot);
        if(!browserEditor) {
            pane.address->startupEdit={};pane.address->startupSubmit={};
            pane.address->field.IsReadOnly(false);browserEditor=true;
            Feedback(SHELL_STATUS_OK);
        }
        return true;
    }
    bool Finish() {
        if(!browserEditor || pane.address->HasActiveComposition() || pane.strip->IsDragging() || !drafts.empty())return false;
        ShellStartupTransfer transfer{};
        if(shell_core_startup_transfer(pane.shell.core,0,0,0,0,0,0,0,false,&transfer)!=SHELL_STATUS_OK ||
            transfer.lost || transfer.kind || transfer.mappings || !transfer.editor_browser)return false;
        auto& strip=*pane.strip;
        if(browsers.size()!=strip.rows.size())return false;
        std::map<uint32_t,Microsoft::UI::Xaml::Controls::TabViewItem> normal;
        for(auto const& [id,row]:strip.rows) {
            if(!browsers.contains(id) || !browsers.at(id) || browsers.at(id)>UINT32_MAX)return false;
            if(!normal.emplace(uint32_t(browsers.at(id)),row).second)return false;
        }
        strip.ClearSettling();strip.groupSlide.Cancel();
        for(auto const& [id,row]:strip.rows)strip.StopRowAnimation(row,id);
        // These maps index existing visual objects; rekey them without clearing
        // ItemsSource or recreating a TabViewItem (including its header controls).
        auto rekey=[&](auto& entries){
            auto next=entries;next.clear();
            for(auto const& [id,value]:entries)if(browsers.contains(id))next.emplace(uint32_t(browsers.at(id)),value);
            entries=std::move(next);
        };
        rekey(strip.outlines);rekey(strip.fadedTabs);rekey(strip.separatorTints);
        std::set<uint32_t> outlined;
        for(auto id:strip.outlinedTabs)if(browsers.contains(id))outlined.insert(uint32_t(browsers.at(id)));
        strip.outlinedTabs=std::move(outlined);
        for(auto& id:strip.publishedOrder)id=uint32_t(browsers.at(id));
        for(auto& slot:strip.groupLayout)slot.tab=uint32_t(browsers.at(slot.tab));
        if(browsers.contains(strip.activeTabId))strip.activeTabId=uint32_t(browsers.at(strip.activeTabId));
        if(browsers.contains(strip.queuedActive))strip.queuedActive=uint32_t(browsers.at(strip.queuedActive));
        strip.enteringTab=0;
        for(auto const& [id,row]:normal) {
            row.Tag(box_value(id));
            Microsoft::UI::Xaml::Automation::AutomationProperties::SetAutomationId(row,L"FennTab-"+to_hstring(id));
            strip.AttachMenu(row,id);
        }
#ifdef WINUI_PANE_TESTING
        pane.startupNormalIdentity=[&owner=pane,editor=pane.startupNormalIdentity,normal] {
            if(!editor())return false;
            for(auto const& [id,row]:normal) {
                if(!owner.strip->rows.contains(id) || owner.strip->rows.at(id)!=row || unbox_value<uint32_t>(row.Tag())!=id)return false;
            }
            return true;
        };
#endif
        strip.rows=std::move(normal);
        auto layout=strip.groupLayout;
        restore();restore={};
        strip.groupLayout=std::move(layout);strip.hasGroupLayout=true;
        pane.startupRender={};pane.startupObserve={};pane.startupAddressApply={};pane.startupKey={};
        pane.address->startupEdit={};pane.address->startupSubmit={};
        // Gecko attachment exposes a new accessible document beneath the host.
        // Reaffirm the retained native focus owner without moving focus, changing
        // selection, or recreating the editor (which would interrupt IME).
        if(pane.address->EditorOwned()) {
            using namespace Microsoft::UI::Xaml::Automation::Peers;
            if(auto peer=FrameworkElementAutomationPeer::FromElement(pane.address->field))
                peer.RaiseAutomationEvent(AutomationEvents::AutomationFocusChanged);
        }
        return true;
    }
    bool Run(uint32_t operation,uint64_t tab=0,uint32_t argument=0,hstring const& text={}) {
        ShellStartupView next{};
        const auto result=shell_core_startup(pane.shell.core,operation,tab,argument,
            reinterpret_cast<const uint16_t*>(text.c_str()),text.size(),&next);
        if(result==SHELL_STATUS_OK) {
            view=next;
            if(operation)NotifyStartup(pane);
        }
        if(operation)Feedback(result);
        return result==SHELL_STATUS_OK;
    }
    void Refresh(bool editor) {
        Require(Run(0));
        applying=true;
        Reset reset{applying};
        ShellStartupTransfer state{};
        Require(shell_core_startup_transfer(pane.shell.core,0,0,0,0,0,0,0,false,&state)==SHELL_STATUS_OK);
        const bool switching=state.kind==4 || state.kind==5;
        pane.address->toolbarRoot.IsHitTestVisible(!switching);
        pane.address->field.IsEnabled(!switching);
        if(browserEditor && view.selected) {
            NativeAddress::JObject disabled;
            disabled.Insert(L"version",NativeAddress::JValue::CreateNumberValue(1));
            disabled.Insert(L"enabled",NativeAddress::JValue::CreateBooleanValue(false));
            // Retire browser session/actions without hiding or reparenting the
            // live editor; those layout changes can reset its restored caret.
            pane.address->Sync(disabled,true);browserEditor=false;BindLocalCallbacks();
            pane.address->root.Visibility(Microsoft::UI::Xaml::Visibility::Visible);
            pane.address->navigation->Apply({},true);
            editor=true;
        }
        if(!switching)pane.address->field.IsReadOnly(!view.selected && !browserEditor);
        std::vector<WinUITab> tabs;
        std::vector<ShellStartupRow> rows(SHELL_STARTUP_ROW_CAPACITY);
        uint32_t count{};
        Require(shell_core_startup_rows(pane.shell.core,0,0,0,nullptr,0,0,
            rows.data(),rows.size(),&count)==SHELL_STATUS_OK);
        std::vector<hstring> titles; titles.reserve(count);
        std::map<uint32_t,uint64_t> nextDrafts;
        std::map<uint32_t,uint64_t> nextBrowsers;
        uint32_t selected{};
        std::set<uint64_t> pending;
        for(size_t i=0;i<view.pending;++i){ShellStartupView item{};
            if(shell_core_startup_read(pane.shell.core,true,i,&item)==SHELL_STATUS_OK)pending.insert(item.tab);}
        for(size_t i=0;i<count;++i) {
            const auto& row=rows[i];
            if(row.draft) {
                nextDrafts.emplace(row.presentation,row.draft);
                titles.push_back(pending.contains(row.draft)?labels.pending:labels.newTab);
            } else {
                // Browser-owned rows require producer labels, never invented
                // titles. Commands resolve to browser IDs; mixed editor
                // selection is acknowledged before replacing local text.
                Require(bool(browserTitle));
                titles.push_back(browserTitle(row.browser));
                nextBrowsers.emplace(row.presentation,row.browser);
            }
            tabs.push_back({row.presentation,titles.back().c_str(),1});
            if(row.selected)selected=row.presentation;
        }
        // Keep the retained TabView collection source. Clearing its layout flag
        // does not detach ItemsSource; direct TabItems removal then throws.
        // Publish the startup identities as one ungrouped run instead.
        pane.strip->groupLayout.clear();
        for(auto const& tab:tabs){ShellStripSlot slot{};slot.tab=tab.id;pane.strip->groupLayout.push_back(slot);}
        pane.strip->hasGroupLayout=true;
        try { pane.strip->Sync(tabs.data(),static_cast<uint32_t>(tabs.size()),selected); }
        catch(winrt::hresult_error const& error) {
#ifdef WINUI_PANE_TESTING
            fwprintf(stderr,L"Startup row render failed: %08x %s rows=%u selected=%u drafts=%zu\n",
                unsigned(error.code()),error.message().c_str(),count,selected,view.tabs);
#endif
            throw;
        }
        drafts=std::move(nextDrafts);
        browsers=std::move(nextBrowsers);
        for(size_t i=0;i<count;++i) {
            auto found=pane.strip->rows.find(rows[i].presentation);
            if(found!=pane.strip->rows.end())found->second.IsEnabled(true);
        }
        if(editor) {
            // Copy borrowed Rust data before any callback can mutate the core.
            Require(Run(0));
            const hstring text{reinterpret_cast<const wchar_t*>(view.text),static_cast<uint32_t>(view.length)};
            pane.address->syncing=true;
            Reset unsync{pane.address->syncing};
            if(pane.address->field.Text()!=text)pane.address->field.Text(text);
            pane.address->field.Select(view.selection_start,view.selection_length);
        }
    }
};
}
