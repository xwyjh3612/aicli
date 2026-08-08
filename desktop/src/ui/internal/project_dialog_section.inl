// Project create/edit modal implementation.
//
// This file is included inside app/main.cpp's private namespace so the existing
// Win32 callback ordering and shared palette lifetime remain unchanged during
// the staged refactor.

HWND MakeDialogControl(LPCWSTR cls,LPCWSTR text,DWORD style,int id,HWND parent,int x,int y,int w,int h){bool button=wcscmp(cls,L"BUTTON")==0;if(button)style|=BS_OWNERDRAW;HWND c=CreateWindowExW(0,cls,text,WS_CHILD|WS_VISIBLE|style,x,y,w,h,parent,(HMENU)(INT_PTR)id,GetModuleHandleW(nullptr),nullptr);SendMessageW(c,WM_SETFONT,(WPARAM)g_font,TRUE);if(button)EnableButtonHover(c);return c;}
void ReadDialogText(HWND c,std::wstring& value){wchar_t b[1024]{};GetWindowTextW(c,b,1024);value=b;}
bool PickFolder(HWND owner,std::wstring& path);
std::wstring DefaultProjectCommand(const std::wstring& type){
    const auto found=std::find_if(g_project_types.begin(),g_project_types.end(),[&](const ProjectType& value){return _wcsicmp(value.name.c_str(),type.c_str())==0;});
    if(found!=g_project_types.end())return found->default_command;
    return type==L"Claude"?L"claude --dangerously-skip-permissions --continue":L"codex --yolo --no-alt-screen resume --last";
}
void LayoutProjectDialog(HWND h){
    RECT client{}; GetClientRect(h,&client);
    const int availableWidth=static_cast<int>(client.right-client.left);
    const int width=std::min(620,std::max(540,availableWidth-96));
    const int height=476;
    const int left=(client.right-width)/2, top=(client.bottom-height)/2;
    g_project_panel={left,top,left+width,top+height};
    constexpr int padding=34;
    g_d_name_rect={left+padding,top+107,left+width-padding,top+143};
    g_d_type_rect={left+padding,top+181,left+width/2-12,top+217};
    g_d_group_rect={left+width/2+12,top+181,left+width-padding,top+217};
    g_d_folder_rect={left+padding,top+255,left+width-padding-122,top+291};
    g_d_command_rect={left+padding,top+329,left+width-padding,top+365};
    auto placeEdit=[&](HWND control,RECT outer){if(!control)return;aicli::ui::LayoutStyledInput(control,outer,2,5);};
    auto placeDropDown=[&](HWND control,RECT outer){if(!control)return;const int w=outer.right-outer.left-4,h=outer.bottom-outer.top-8;MoveWindow(control,outer.left+2,outer.top+4,w,h,TRUE);ApplyRoundedRegion(control,w,h,5);};
    auto placeButton=[&](HWND control,RECT outer){if(!control)return;MoveWindow(control,outer.left,outer.top,outer.right-outer.left,outer.bottom-outer.top,TRUE);ApplyRoundedRegion(control,outer.right-outer.left,outer.bottom-outer.top,7);};
    placeEdit(g_d_name,g_d_name_rect);if(g_project_dialog_edit)placeEdit(g_d_type,g_d_type_rect);else placeDropDown(g_d_type,g_d_type_rect);placeDropDown(g_d_group,g_d_group_rect);placeEdit(g_d_folder,g_d_folder_rect);placeEdit(g_d_command,g_d_command_rect);
    RECT browse{g_d_folder_rect.right+12,g_d_folder_rect.top,g_project_panel.right-padding,g_d_folder_rect.bottom}; placeButton(g_d_folder_browse,browse);
    RECT save{left+padding,top+416,left+width/2-7,top+452}, cancel{left+width/2+7,top+416,left+width-padding,top+452};
    HWND child=GetWindow(h,GW_CHILD); while(child){int id=GetDlgCtrlID(child);if(id==ID_D_OK){MoveWindow(child,save.left,save.top,save.right-save.left,save.bottom-save.top,TRUE);ApplyRoundedRegion(child,save.right-save.left,save.bottom-save.top,8);}if(id==ID_D_CANCEL){MoveWindow(child,cancel.left,cancel.top,cancel.right-cancel.left,cancel.bottom-cancel.top,TRUE);ApplyRoundedRegion(child,cancel.right-cancel.left,cancel.bottom-cancel.top,8);}child=GetWindow(child,GW_HWNDNEXT);}
    aicli::ui::SetModalSurfaceRegion(h,g_project_panel,28);
    InvalidateRect(h,nullptr,FALSE);
}
void InvalidateProjectControlFrame(HWND dialog, HWND control) {
    if (!dialog || !control) return;
    RECT frame{};
    if (control == g_d_name) frame = g_d_name_rect;
    else if (control == g_d_type) frame = g_d_type_rect;
    else if (control == g_d_group) frame = g_d_group_rect;
    else if (control == g_d_folder) frame = g_d_folder_rect;
    else if (control == g_d_command) frame = g_d_command_rect;
    else {
        GetWindowRect(control, &frame);
        MapWindowPoints(HWND_DESKTOP, dialog,
                        reinterpret_cast<POINT*>(&frame), 2);
    }
    InflateRect(&frame, 2, 2);
    InvalidateRect(dialog, &frame, FALSE);
}

// The shared ModalHost surface.  Keeping this renderer palette-driven means
// a live theme switch changes the dialog without rebuilding its controls.
void PaintProjectDialogSurface(HDC dc,RECT client){
    // The layered ModalWorkspaceScope scrim is behind this content window.
    // Do not paint an opaque backdrop here; doing so would hide the live
    // terminal/workspace and defeat modal translucency.
    (void)client;
    DrawSmoothRoundedRect(dc,g_project_panel,14,panel,border,1.0f);
    SetBkMode(dc,TRANSPARENT);
    SelectObject(dc,g_font_modal_title?g_font_modal_title:g_font_medium);
    SetTextColor(dc,fg);
    RECT title{g_project_panel.left+34,g_project_panel.top+24,g_project_panel.right-34,g_project_panel.top+50};
    DrawTextW(dc,g_project_dialog_edit?L"编辑项目":L"新建项目",-1,&title,DT_LEFT|DT_SINGLELINE|DT_VCENTER);
    SelectObject(dc,g_font_small);SetTextColor(dc,muted);
    RECT subtitle{g_project_panel.left+34,g_project_panel.top+53,g_project_panel.right-34,g_project_panel.top+75};
    DrawTextW(dc,L"配置项目信息与 AI CLI 启动环境",-1,&subtitle,DT_LEFT|DT_SINGLELINE|DT_VCENTER);
    SetTextColor(dc,muted);
    auto label=[&](const wchar_t* value,int y,int x=g_project_panel.left+34){
        RECT rect{x,y,x+250,y+22};DrawTextW(dc,value,-1,&rect,DT_LEFT|DT_SINGLELINE|DT_VCENTER);
    };
    label(L"项目名称 / Tab 标题",g_project_panel.top+81);
    label(L"项目类型",g_project_panel.top+155);
    label(L"分组",g_project_panel.top+155,g_project_panel.left+(g_project_panel.right-g_project_panel.left)/2+12);
    label(L"项目路径",g_project_panel.top+229);label(L"启动命令",g_project_panel.top+303);
    if(g_project_dialog_edit){
        SetTextColor(dc,g_theme.titlebar_muted);
        RECT locked{g_d_type_rect.right-96,g_project_panel.top+155,g_d_type_rect.right,g_project_panel.top+177};
        DrawTextW(dc,L"不可修改",-1,&locked,DT_RIGHT|DT_SINGLELINE|DT_VCENTER);
    }
    HPEN divider=CreatePen(PS_SOLID,1,border);auto old=SelectObject(dc,divider);
    MoveToEx(dc,g_project_panel.left+34,g_project_panel.top+394,nullptr);
    LineTo(dc,g_project_panel.right-34,g_project_panel.top+394);
    SelectObject(dc,old);DeleteObject(divider);
    auto outline=[&](RECT rect,HWND control){
        const bool active=GetFocus()==control;
        DrawRoundedOutline(dc,rect,active?accent:border,6);
        RECT underline{rect.left+2,rect.bottom-(active?3:2),rect.right-2,rect.bottom-1};
        HBRUSH line=CreateSolidBrush(active?accent:muted);FillRect(dc,&underline,line);DeleteObject(line);
    };
    outline(g_d_name_rect,g_d_name);outline(g_d_type_rect,g_d_type);
    outline(g_d_group_rect,g_d_group);outline(g_d_folder_rect,g_d_folder);outline(g_d_command_rect,g_d_command);
}
void DrawProjectDropdownItem(const DRAWITEMSTRUCT* item){
    const auto* entry=reinterpret_cast<const ProjectDropdownEntry*>(item->itemData);if(!entry)return;
    RECT r=item->rcItem;const bool hover=(item->itemState&ODS_SELECTED)!=0;
    HBRUSH fill=CreateSolidBrush(hover?g_theme.selection:g_theme.list_surface);FillRect(item->hDC,&r,fill);DeleteObject(fill);
    SetBkMode(item->hDC,TRANSPARENT);SelectObject(item->hDC,g_font);
    SetTextColor(item->hDC,entry->selected?accent:fg);
    RECT mark{r.left+10,r.top,r.left+34,r.bottom};DrawTextW(item->hDC,entry->selected?L"✓":L"",-1,&mark,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    RECT textRect{r.left+38,r.top,r.right-12,r.bottom};DrawTextW(item->hDC,entry->label.c_str(),-1,&textRect,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
}
void ShowProjectDropdown(HWND dialog,HWND anchor,bool projectType){
    std::vector<std::wstring> options;
    if(projectType){for(const auto& type:g_project_types)options.push_back(type.name);}
    else{
        for(const auto& group:g_groups)options.push_back(group.name);
        if(!g_project_draft.group.empty()&&std::find(options.begin(),options.end(),g_project_draft.group)==options.end())options.push_back(g_project_draft.group);
    }
    wchar_t currentText[256]{};GetWindowTextW(anchor,currentText,256);const std::wstring current=currentText;
    std::vector<ProjectDropdownEntry> entries;entries.reserve(options.size());for(const auto& option:options)entries.push_back({option,option==current});
    HMENU menu=CreatePopupMenu();if(!menu)return;HBRUSH background=CreateSolidBrush(g_theme.list_surface);MENUINFO info{sizeof(info)};info.fMask=MIM_BACKGROUND|MIM_STYLE;info.hbrBack=background;info.dwStyle=MNS_NOCHECK;SetMenuInfo(menu,&info);
    constexpr UINT base=2400;for(size_t i=0;i<entries.size();++i)AppendMenuW(menu,MF_OWNERDRAW|MF_ENABLED,base+(UINT)i,reinterpret_cast<LPCWSTR>(&entries[i]));
    RECT anchorRect{};GetWindowRect(anchor,&anchorRect);g_project_dropdown_width=anchorRect.right-anchorRect.left;
    SetFocus(anchor);const UINT selected=TrackPopupMenuEx(menu,TPM_RETURNCMD|TPM_NONOTIFY|TPM_LEFTALIGN|TPM_TOPALIGN|TPM_RIGHTBUTTON,anchorRect.left,anchorRect.bottom+2,dialog,nullptr);
    if(selected>=base&&selected<base+entries.size()){
        const std::wstring value=entries[selected-base].label;
        wchar_t previousType[256]{}; GetWindowTextW(anchor, previousType, 256);
        SetWindowTextW(anchor,value.c_str());
        if(projectType){
            wchar_t command[1024]{}; GetWindowTextW(g_d_command,command,1024);
            const std::wstring currentCommand=command;
            const std::wstring previousDefault=DefaultProjectCommand(previousType);
            const bool useDefault=currentCommand.empty()||currentCommand==previousDefault;
            if(useDefault) SetWindowTextW(g_d_command,DefaultProjectCommand(value).c_str());
        }
    }
    DestroyMenu(menu);DeleteObject(background);
    InvalidateRect(anchor,nullptr,FALSE);
}
LRESULT CALLBACK ProjectDialogProc(HWND h,UINT m,WPARAM w,LPARAM l){
    switch(m){
    case WM_CREATE:{
        const std::wstring initialType=g_project_dialog_edit?g_project_draft.type:(g_project_types.empty()?L"Codex":g_project_types.front().name);
        g_d_name=aicli::ui::CreateStyledInput(h,ID_D_NAME,g_font,L"输入项目名称");
        g_d_type=g_project_dialog_edit
            ?aicli::ui::CreateStyledInput(h,ID_D_TYPE,g_font,L"",true)
            :MakeDialogControl(L"BUTTON",initialType.c_str(),BS_NOTIFY|WS_TABSTOP,ID_D_TYPE,h,0,0,1,1);
        const std::wstring initialGroup=g_project_draft.group.empty()?(g_group_name==kAllGroupsName?L"默认分组":g_group_name):g_project_draft.group;
        g_d_group=MakeDialogControl(L"BUTTON",initialGroup.c_str(),BS_NOTIFY|WS_TABSTOP,ID_D_GROUP,h,0,0,1,1);
        g_d_folder=aicli::ui::CreateStyledInput(h,ID_D_FOLDER,g_font,L"请选择项目文件夹",true);
        g_d_folder_browse=MakeDialogControl(L"BUTTON",L"选择文件夹",BS_PUSHBUTTON|WS_TABSTOP,ID_D_BROWSE,h,0,0,1,1);
        g_d_command=aicli::ui::CreateStyledInput(h,ID_D_COMMAND,g_font,L"输入启动命令");
        for(HWND field:{g_d_name,g_d_type,g_d_folder,g_d_command})
            aicli::ui::SetStyledInputTheme(field,g_theme);
        MakeDialogControl(L"BUTTON",L"保存",BS_DEFPUSHBUTTON|WS_TABSTOP,ID_D_OK,h,0,0,1,1);
        MakeDialogControl(L"BUTTON",L"取消",BS_PUSHBUTTON|WS_TABSTOP,ID_D_CANCEL,h,0,0,1,1);
        if(g_project_dialog_edit){aicli::ui::SetStyledInputText(g_d_type,g_project_draft.type);aicli::ui::SetStyledInputText(g_d_name,g_project_draft.name);aicli::ui::SetStyledInputText(g_d_folder,g_project_draft.folder);aicli::ui::SetStyledInputText(g_d_command,g_project_draft.command);}else{aicli::ui::SetStyledInputText(g_d_command,DefaultProjectCommand(initialType));}
        LayoutProjectDialog(h);SetFocus(g_d_name);return 0; }
    case WM_SIZE: LayoutProjectDialog(h);return 0;
    case WM_ERASEBKGND:return 1;
    case WM_PAINT:{PAINTSTRUCT ps;HDC dc=BeginPaint(h,&ps);RECT client{};GetClientRect(h,&client);PaintProjectDialogSurface(dc,client);EndPaint(h,&ps);return 0;}
    case WM_NCHITTEST:return HTCLIENT;
    case WM_CTLCOLORSTATIC:{
        HDC dc=(HDC)w;HWND control=(HWND)l;
        if(control==g_d_folder||(g_project_dialog_edit&&control==g_d_type)){
            const bool active=GetFocus()==control;const COLORREF color=active?g_theme.field_focus:g_theme.field;
            SetTextColor(dc,active?fg:g_theme.list_text);SetBkColor(dc,color);SetBkMode(dc,OPAQUE);
            return (LRESULT)(active?g_focus_brush:g_edit_brush);
        }
        SetTextColor(dc,fg);SetBkMode(dc,TRANSPARENT);return (LRESULT)GetStockObject(NULL_BRUSH);
    }
    case WM_CTLCOLOREDIT:{HDC dc=(HDC)w;HWND control=(HWND)l;const bool active=GetFocus()==control;const COLORREF color=active?g_theme.field_focus:g_theme.field;SetTextColor(dc,fg);SetBkColor(dc,color);return (LRESULT)(active?g_focus_brush:g_edit_brush);}
    case WM_CTLCOLORLISTBOX:{HDC dc=(HDC)w;SetTextColor(dc,fg);SetBkColor(dc,g_theme.list_surface);return (LRESULT)g_list_brush;}
    case WM_MEASUREITEM:{auto* item=(MEASUREITEMSTRUCT*)l;if(item->CtlType==ODT_MENU){item->itemWidth=std::max(160,g_project_dropdown_width-21);item->itemHeight=34;return TRUE;}}break;
    case WM_DRAWITEM: {auto* item=(DRAWITEMSTRUCT*)l;if(item->CtlType==ODT_MENU){DrawProjectDropdownItem(item);return TRUE;}if(item->CtlType==ODT_BUTTON){DrawModernButton(item);return TRUE;}} break;
    case WM_COMMAND:
        if((HIWORD(w)==EN_SETFOCUS||HIWORD(w)==EN_KILLFOCUS||HIWORD(w)==BN_SETFOCUS||HIWORD(w)==BN_KILLFOCUS)&&
           (LOWORD(w)==ID_D_NAME||LOWORD(w)==ID_D_TYPE||LOWORD(w)==ID_D_GROUP||LOWORD(w)==ID_D_FOLDER||LOWORD(w)==ID_D_COMMAND)){
            InvalidateProjectControlFrame(h,(HWND)l);
        }
        if(LOWORD(w)==ID_D_TYPE&&HIWORD(w)==BN_CLICKED&&!g_project_dialog_edit){ShowProjectDropdown(h,g_d_type,true);return 0;}
        if(LOWORD(w)==ID_D_GROUP&&HIWORD(w)==BN_CLICKED){ShowProjectDropdown(h,g_d_group,false);return 0;}
        if(LOWORD(w)==ID_D_BROWSE){std::wstring selected;if(PickFolder(h,selected))aicli::ui::SetStyledInputText(g_d_folder,selected);InvalidateProjectControlFrame(h,g_d_folder);return 0;}
        if(LOWORD(w)==ID_D_CANCEL){DestroyWindow(h);return 0;}
        if(LOWORD(w)==ID_D_OK){
            g_project_draft.name=aicli::ui::GetStyledInputText(g_d_name);
            g_project_draft.folder=aicli::ui::GetStyledInputText(g_d_folder);
            g_project_draft.command=aicli::ui::GetStyledInputText(g_d_command);
            if(g_project_dialog_edit) g_project_draft.type=aicli::ui::GetStyledInputText(g_d_type);
            else { wchar_t type[256]{}; GetWindowTextW(g_d_type,type,256); g_project_draft.type=type; }
            wchar_t group[256]{}; GetWindowTextW(g_d_group,group,256); g_project_draft.group=group;
            if(g_project_draft.name.empty()){MessageBoxW(h,L"项目名称不能为空。",L"项目详情",MB_OK|MB_ICONWARNING);return 0;}
            std::error_code folderError;
            if(g_project_draft.folder.empty()||!std::filesystem::is_directory(g_project_draft.folder,folderError)){MessageBoxW(h,L"请选择一个存在的项目文件夹。",L"项目详情",MB_OK|MB_ICONWARNING);return 0;}
            if(g_project_draft.command.empty())g_project_draft.command=DefaultProjectCommand(g_project_draft.type);
            const bool knownType=std::any_of(g_project_types.begin(),g_project_types.end(),[&](const ProjectType& value){return _wcsicmp(value.name.c_str(),g_project_draft.type.c_str())==0;});
            const bool knownGroup=std::any_of(g_groups.begin(),g_groups.end(),[&](const ProjectGroup& value){return _wcsicmp(value.name.c_str(),g_project_draft.group.c_str())==0;});
            if(g_project_draft.type.empty()||!knownType||g_project_draft.group.empty()||!knownGroup){MessageBoxW(h,L"请选择有效的项目类型和分组。",L"项目详情",MB_OK|MB_ICONWARNING);return 0;}
            g_project_dialog_ok=true;DestroyWindow(h);return 0;
        } break;
    case WM_KEYDOWN:if(w==VK_ESCAPE){DestroyWindow(h);return 0;}break;
    case WM_DESTROY:g_project_dialog=nullptr;g_d_name=g_d_type=g_d_group=g_d_folder=g_d_folder_browse=g_d_command=nullptr;return 0;
    case WM_CLOSE:DestroyWindow(h);return 0; }
    return DefWindowProcW(h,m,w,l);
}
bool ShowProjectDialog(HWND owner,const Project* initial=nullptr){
    static bool registered=false;
    if(!registered){
        WNDCLASSW wc{};wc.hInstance=GetModuleHandleW(nullptr);wc.lpfnWndProc=ProjectDialogProc;
        wc.lpszClassName=L"AicliProjectDialog";wc.hCursor=LoadCursor(nullptr,IDC_ARROW);
        wc.hbrBackground=nullptr;wc.style=CS_DBLCLKS;RegisterClassW(&wc);registered=true;
    }
    g_project_dialog_ok=false;g_project_dialog_edit=initial!=nullptr;
    g_project_draft=initial?*initial:Project{};
    aicli::ui::ModalWorkspaceScope workspace(owner,aicli::ui::ModalTopForOwner(owner),&g_theme);
    g_project_dialog=CreateWindowExW(WS_EX_CONTROLPARENT|WS_EX_TOOLWINDOW,L"AicliProjectDialog",L"",
        WS_POPUP|WS_CLIPCHILDREN|WS_CLIPSIBLINGS,0,0,1,1,
        owner,nullptr,GetModuleHandleW(nullptr),nullptr);
    if(!g_project_dialog){g_project_dialog=nullptr;Layout();return false;}
    workspace.AttachSurface(g_project_dialog);
    aicli::ui::ActivateModalSurface(g_project_dialog, g_d_name);
    MSG msg{};
    while(IsWindow(g_project_dialog)&&GetMessageW(&msg,nullptr,0,0)){TranslateMessage(&msg);DispatchMessageW(&msg);}
    g_project_dialog=nullptr;
    aicli::ui::RestoreModalOwner(owner);
    return g_project_dialog_ok;
}
