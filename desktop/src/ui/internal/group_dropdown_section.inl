// Custom title-bar group picker.  The native HMENU used previously could not
// match the application's compact rounded surfaces and forced the group name,
// count and arrow into one unalignable string.  This popup remains
// non-activating so opening it does not make the custom title bar flash.

struct GroupDropdownPopupEntry {
    std::wstring value;
    std::wstring label;
    std::wstring count;
    const wchar_t* icon{};
    bool selected{};
    bool separator_before{};
};

struct GroupDropdownPopupState {
    HWND window{};
    std::vector<GroupDropdownPopupEntry> entries;
    int row_height{};
    int padding{};
    int separator_height{};
    int viewport_height{};
    int content_height{};
    int scroll_offset{};
    int hover_index{-1};
    int keyboard_index{-1};
    bool done{};
    bool accepted{};
    bool closing{};
    std::wstring result;

    int EntryTop(int index) const {
        int top=0;
        for(int current=0;current<index;++current){
            if(entries[current].separator_before)top+=separator_height;
            top+=row_height;
        }
        if(index>=0&&index<static_cast<int>(entries.size())&&
           entries[index].separator_before)top+=separator_height;
        return top;
    }

    int EntryBottom(int index) const {
        return EntryTop(index)+row_height;
    }

    int MaxScroll() const {
        return std::max(0,content_height-viewport_height);
    }

    void ClampScroll() {
        scroll_offset=std::clamp(scroll_offset,0,MaxScroll());
    }

    void EnsureVisible(int index) {
        if(index<0||index>=static_cast<int>(entries.size()))return;
        const int top=EntryTop(index);
        const int bottom=EntryBottom(index);
        if(top<scroll_offset)scroll_offset=top;
        else if(bottom>scroll_offset+viewport_height)
            scroll_offset=bottom-viewport_height;
        ClampScroll();
    }

    int HitTest(POINT point,const RECT& client) const {
        if(point.x<0||point.x>=client.right||
           point.y<padding||point.y>=client.bottom-padding)return -1;
        const int content_y=point.y-padding+scroll_offset;
        for(int index=0;index<static_cast<int>(entries.size());++index){
            const int top=EntryTop(index);
            if(content_y>=top&&content_y<EntryBottom(index))return index;
        }
        return -1;
    }
};

constexpr wchar_t kGroupDropdownClassName[]=L"AicliGroupDropdownPopup";

GroupDropdownPopupState* GroupDropdownState(HWND window){
    return reinterpret_cast<GroupDropdownPopupState*>(
        GetWindowLongPtrW(window,GWLP_USERDATA));
}

void InvalidateGroupDropdown(GroupDropdownPopupState* state){
    if(state&&state->window)InvalidateRect(state->window,nullptr,FALSE);
}

void ScrollGroupDropdown(GroupDropdownPopupState* state,int delta){
    if(!state||state->MaxScroll()<=0)return;
    const int previous=state->scroll_offset;
    state->scroll_offset+=delta;
    state->ClampScroll();
    if(previous!=state->scroll_offset)InvalidateGroupDropdown(state);
}

void FinishGroupDropdown(GroupDropdownPopupState* state,bool accepted,int index=-1){
    if(!state||state->closing)return;
    state->closing=true;
    if(accepted&&index>=0&&index<static_cast<int>(state->entries.size())){
        state->accepted=true;
        state->result=state->entries[index].value;
    }
    if(GetCapture()==state->window)ReleaseCapture();
    if(state->window&&IsWindow(state->window))DestroyWindow(state->window);
}

void PaintGroupDropdown(HWND window,GroupDropdownPopupState* state){
    PAINTSTRUCT paint{};
    HDC dc=BeginPaint(window,&paint);
    if(!dc||!state){
        if(dc)EndPaint(window,&paint);
        return;
    }

    RECT client{};
    GetClientRect(window,&client);
    const int width=std::max(1L,client.right-client.left);
    const int height=std::max(1L,client.bottom-client.top);
    HDC memory=CreateCompatibleDC(dc);
    HBITMAP bitmap=CreateCompatibleBitmap(dc,width,height);
    HGDIOBJ previous_bitmap=SelectObject(memory,bitmap);

    const COLORREF popup_background=g_light_theme?RGB(250,252,255):RGB(16,24,39);
    const COLORREF popup_border=g_light_theme?RGB(205,214,226):RGB(40,52,74);
    const COLORREF hover_background=g_light_theme?RGB(239,245,253):RGB(24,36,59);
    const COLORREF selected_background=g_light_theme?RGB(229,239,252):RGB(27,44,75);
    const COLORREF selected_hover_background=g_light_theme?RGB(220,234,251):RGB(31,51,87);
    const COLORREF popup_text=g_light_theme?RGB(35,43,56):RGB(229,234,242);
    const COLORREF popup_muted=g_light_theme?RGB(111,126,145):RGB(129,144,168);

    HBRUSH background=CreateSolidBrush(popup_background);
    FillRect(memory,&client,background);
    DeleteObject(background);
    RECT surface{0,0,width,height};
    DrawSmoothRoundedRect(memory,surface,ScaleUiF(8.0f),popup_background,
                          popup_border,1.0f);

    const int saved=SaveDC(memory);
    IntersectClipRect(memory,ScaleUi(1),state->padding,
                     width-ScaleUi(1),height-state->padding);
    SetBkMode(memory,TRANSPARENT);

    const bool has_scrollbar=state->MaxScroll()>0;
    const int row_right=width-ScaleUi(has_scrollbar?11:5);
    for(int index=0;index<static_cast<int>(state->entries.size());++index){
        const auto& entry=state->entries[index];
        const int row_top=state->padding+state->EntryTop(index)-state->scroll_offset;
        const int row_bottom=row_top+state->row_height;

        if(entry.separator_before){
            const int separator_y=row_top-state->separator_height/2;
            if(separator_y>=state->padding&&separator_y<height-state->padding){
                HPEN separator=CreatePen(
                    PS_SOLID,std::max(1,ScaleUi(1)),
                    g_light_theme?RGB(222,228,236):RGB(43,56,77));
                HGDIOBJ previous_pen=SelectObject(memory,separator);
                MoveToEx(memory,ScaleUi(12),separator_y,nullptr);
                LineTo(memory,row_right-ScaleUi(7),separator_y);
                SelectObject(memory,previous_pen);
                DeleteObject(separator);
            }
        }
        if(row_bottom<=state->padding||row_top>=height-state->padding)continue;

        const bool hovered=index==state->hover_index;
        if(entry.selected||hovered){
            const COLORREF row_fill=entry.selected
                ?(hovered?selected_hover_background:selected_background)
                :hover_background;
            RECT row{ScaleUi(5),row_top+ScaleUi(1),row_right,
                     row_bottom-ScaleUi(1)};
            DrawSmoothRoundedRect(memory,row,ScaleUiF(6.0f),row_fill,row_fill,0.0f);
        }

        const int left=ScaleUi(12);
        RECT icon_rect{left,row_top,left+ScaleUi(22),row_bottom};
        SetTextColor(memory,entry.selected?accent:popup_muted);
        SelectObject(memory,g_font_header_icon);
        DrawTextW(memory,entry.icon,-1,&icon_rect,
                  DT_CENTER|DT_VCENTER|DT_SINGLELINE);

        RECT check_rect{row_right-ScaleUi(25),row_top,
                        row_right-ScaleUi(5),row_bottom};
        if(entry.selected){
            SetTextColor(memory,accent);
            SelectObject(memory,g_font_header_icon);
            DrawTextW(memory,L"\xE73E",-1,&check_rect,
                      DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        }

        SIZE count_size{};
        SelectObject(memory,g_font_small);
        GetTextExtentPoint32W(memory,entry.count.c_str(),
                              static_cast<int>(entry.count.size()),&count_size);
        const int count_right=check_rect.left-ScaleUi(6);
        RECT count_rect{count_right-count_size.cx,row_top,count_right,row_bottom};
        SetTextColor(memory,popup_muted);
        DrawTextW(memory,entry.count.c_str(),-1,&count_rect,
                  DT_RIGHT|DT_VCENTER|DT_SINGLELINE);

        RECT label_rect{icon_rect.right+ScaleUi(7),row_top,
                        count_rect.left-ScaleUi(9),row_bottom};
        SetTextColor(memory,popup_text);
        SelectObject(memory,g_font_header);
        DrawTextW(memory,entry.label.c_str(),-1,&label_rect,
                  DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
    }
    RestoreDC(memory,saved);

    if(has_scrollbar){
        const int track_top=state->padding+ScaleUi(4);
        const int track_bottom=height-state->padding-ScaleUi(4);
        const int track_height=std::max(1,track_bottom-track_top);
        const int thumb_height=std::max(
            ScaleUi(26),
            static_cast<int>((static_cast<long long>(track_height)*
                              state->viewport_height)/state->content_height));
        const int travel=std::max(0,track_height-thumb_height);
        const int thumb_top=track_top+(state->MaxScroll()>0
            ?static_cast<int>((static_cast<long long>(travel)*state->scroll_offset)/
                             state->MaxScroll())
            :0);
        RECT thumb{width-ScaleUi(6),thumb_top,width-ScaleUi(3),
                   thumb_top+thumb_height};
        const COLORREF thumb_color=g_light_theme?RGB(151,163,180):RGB(94,111,137);
        DrawSmoothRoundedRect(memory,thumb,ScaleUiF(2.0f),thumb_color,
                              thumb_color,0.0f);
    }

    BitBlt(dc,0,0,width,height,memory,0,0,SRCCOPY);
    SelectObject(memory,previous_bitmap);
    DeleteObject(bitmap);
    DeleteDC(memory);
    EndPaint(window,&paint);
}

LRESULT CALLBACK GroupDropdownWindowProc(HWND window,UINT message,
                                         WPARAM wparam,LPARAM lparam){
    auto* state=GroupDropdownState(window);
    switch(message){
    case WM_NCCREATE:{
        const auto* create=reinterpret_cast<const CREATESTRUCTW*>(lparam);
        state=static_cast<GroupDropdownPopupState*>(create->lpCreateParams);
        SetWindowLongPtrW(window,GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(state));
        if(state){state->window=window;}
        return TRUE;
    }
    case WM_MOUSEACTIVATE:return MA_NOACTIVATE;
    case WM_ERASEBKGND:return 1;
    case WM_PAINT:PaintGroupDropdown(window,state);return 0;
    case WM_MOUSEMOVE:{
        if(!state)return 0;
        RECT client{};GetClientRect(window,&client);
        const POINT point{GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)};
        const int next=state->HitTest(point,client);
        if(next!=state->hover_index){
            state->hover_index=next;
            InvalidateGroupDropdown(state);
        }
        return 0;
    }
    case WM_MOUSEWHEEL:{
        if(state){
            const int steps=GET_WHEEL_DELTA_WPARAM(wparam)/WHEEL_DELTA;
            ScrollGroupDropdown(state,-steps*state->row_height*2);
        }
        return 0;
    }
    case WM_LBUTTONUP:{
        if(!state)return 0;
        RECT client{};GetClientRect(window,&client);
        const POINT point{GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)};
        const int hit=state->HitTest(point,client);
        FinishGroupDropdown(state,hit>=0,hit);
        return 0;
    }
    case WM_RBUTTONUP:
    case WM_CANCELMODE:
        FinishGroupDropdown(state,false);
        return 0;
    case WM_CAPTURECHANGED:
        if(state&&!state->closing&&reinterpret_cast<HWND>(lparam)!=window)
            FinishGroupDropdown(state,false);
        return 0;
    case WM_DESTROY:
        if(state){state->window=nullptr;state->done=true;}
        return 0;
    }
    return DefWindowProcW(window,message,wparam,lparam);
}

bool EnsureGroupDropdownWindowClass(){
    static ATOM window_class{};
    if(window_class)return true;
    WNDCLASSEXW descriptor{sizeof(descriptor)};
    descriptor.style=CS_HREDRAW|CS_VREDRAW|CS_DROPSHADOW;
    descriptor.lpfnWndProc=GroupDropdownWindowProc;
    descriptor.hInstance=GetModuleHandleW(nullptr);
    descriptor.hCursor=LoadCursorW(nullptr,IDC_ARROW);
    descriptor.lpszClassName=kGroupDropdownClassName;
    window_class=RegisterClassExW(&descriptor);
    if(!window_class&&GetLastError()==ERROR_CLASS_ALREADY_EXISTS)
        return true;
    return window_class!=0;
}

int MeasureGroupDropdownWidth(const std::vector<GroupDropdownPopupEntry>& entries){
    HDC dc=GetDC(g_hwnd);
    if(!dc)return ScaleUi(218);
    int widest=0;
    for(const auto& entry:entries){
        SIZE label{},count{};
        HGDIOBJ previous=SelectObject(dc,g_font_header);
        GetTextExtentPoint32W(dc,entry.label.c_str(),
                              static_cast<int>(entry.label.size()),&label);
        SelectObject(dc,g_font_small);
        GetTextExtentPoint32W(dc,entry.count.c_str(),
                              static_cast<int>(entry.count.size()),&count);
        SelectObject(dc,previous);
        widest=std::max(widest,
            ScaleUi(12+22+7+9+6+25+8)+
                static_cast<int>(label.cx+count.cx));
    }
    ReleaseDC(g_hwnd,dc);
    RECT button{};GetWindowRect(g_group,&button);
    const int button_width=button.right-button.left;
    return std::clamp(std::max(widest,button_width),ScaleUi(184),ScaleUi(300));
}

std::optional<std::wstring> RunGroupDropdownPopup(
    std::vector<GroupDropdownPopupEntry> entries){
    if(entries.empty()||!EnsureGroupDropdownWindowClass())return std::nullopt;

    GroupDropdownPopupState state;
    state.entries=std::move(entries);
    state.row_height=ScaleUi(34);
    state.padding=ScaleUi(6);
    state.separator_height=ScaleUi(9);
    for(int index=0;index<static_cast<int>(state.entries.size());++index){
        if(state.entries[index].separator_before)
            state.content_height+=state.separator_height;
        state.content_height+=state.row_height;
        if(state.entries[index].selected)state.keyboard_index=index;
    }
    if(state.keyboard_index<0)state.keyboard_index=0;
    const int maximum_rows=9;
    const int maximum_content=state.row_height*maximum_rows+
        (state.entries.size()>2?state.separator_height:0);
    state.viewport_height=std::min(state.content_height,maximum_content);
    state.EnsureVisible(state.keyboard_index);

    const int width=MeasureGroupDropdownWidth(state.entries);
    const int height=state.viewport_height+state.padding*2;
    RECT button{};GetWindowRect(g_group,&button);
    RECT anchor=button;
    HMONITOR monitor=MonitorFromRect(&anchor,MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitor_info{sizeof(monitor_info)};
    GetMonitorInfoW(monitor,&monitor_info);
    const RECT work=monitor_info.rcWork;
    int x=button.left;
    int y=button.bottom+ScaleUi(5);
    if(x+width>work.right)x=work.right-width;
    x=std::max(static_cast<int>(work.left),x);
    if(y+height>work.bottom)y=button.top-height-ScaleUi(5);
    y=std::clamp(y,static_cast<int>(work.top),
                 static_cast<int>(work.bottom)-height);

    HWND popup=CreateWindowExW(
        WS_EX_TOOLWINDOW|WS_EX_TOPMOST|WS_EX_NOACTIVATE,
        kGroupDropdownClassName,L"",WS_POPUP,x,y,width,height,
        g_hwnd,nullptr,GetModuleHandleW(nullptr),&state);
    if(!popup)return std::nullopt;
    ApplyRoundedRegion(popup,width,height,ScaleUi(9));

    g_group_dropdown_open=true;
    InvalidateRect(g_group,nullptr,FALSE);
    ShowWindow(popup,SW_SHOWNOACTIVATE);
    UpdateWindow(popup);
    SetCapture(popup);

    MSG message{};
    while(!state.done){
        const BOOL result=GetMessageW(&message,nullptr,0,0);
        if(result<=0){
            if(result==0)PostQuitMessage(static_cast<int>(message.wParam));
            break;
        }
        if(message.message==WM_KEYDOWN){
            if(message.wParam==VK_ESCAPE){
                FinishGroupDropdown(&state,false);
                continue;
            }
            if(message.wParam==VK_UP||message.wParam==VK_DOWN){
                const int direction=message.wParam==VK_UP?-1:1;
                state.keyboard_index=std::clamp(
                    state.keyboard_index+direction,0,
                    static_cast<int>(state.entries.size())-1);
                state.hover_index=state.keyboard_index;
                state.EnsureVisible(state.keyboard_index);
                InvalidateGroupDropdown(&state);
                continue;
            }
            if(message.wParam==VK_RETURN||message.wParam==VK_SPACE){
                FinishGroupDropdown(&state,true,state.keyboard_index);
                continue;
            }
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if(state.window&&IsWindow(state.window))FinishGroupDropdown(&state,false);
    g_group_dropdown_open=false;
    InvalidateRect(g_group,nullptr,FALSE);
    if(state.accepted)return state.result;
    return std::nullopt;
}

void ShowGroupDropdown(){
    g_groups=g_store.LoadGroups();
    std::vector<GroupDropdownPopupEntry> entries;
    entries.reserve(g_groups.size()+2);

    const auto add_entry=[&](const std::wstring& value,const wchar_t* icon,
                             std::size_t count,bool separator){
        entries.push_back({value,value,L"("+std::to_wstring(count)+L")",icon,
                           value==g_group_name,separator});
    };
    add_entry(kAllGroupsName,L"\xE71D",g_projects.size(),false);
    const auto started_count=static_cast<std::size_t>(std::count_if(
        g_projects.begin(),g_projects.end(),[](const Project& project){
            return g_island.HasSession(project.id);
        }));
    add_entry(kStartedProjectsName,L"\xE768",started_count,false);
    for(std::size_t index=0;index<g_groups.size();++index){
        const auto& group=g_groups[index];
        const auto count=static_cast<std::size_t>(std::count_if(
            g_projects.begin(),g_projects.end(),[&](const Project& project){
                const std::wstring value=project.group.empty()?L"默认分组":project.group;
                return value==group.name;
            }));
        add_entry(group.name,L"\xE8B7",count,index==0);
    }

    const auto selected=RunGroupDropdownPopup(std::move(entries));
    if(!selected||*selected==g_group_name)return;
    g_group_name=*selected;
    g_store.SetSetting(L"last_group",g_group_name);
    RefreshProjectList();
    RefreshProjectDetails();
    Layout();
}
