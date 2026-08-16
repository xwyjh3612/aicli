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
    return type==L"Claude"?L"claude --dangerously-skip-permissions --continue":L"codex --yolo resume --last";
}
void LayoutProjectDialog(HWND h){
    RECT client{}; GetClientRect(h,&client);
    const int dpi=std::max(96,static_cast<int>(GetDpiForWindow(h)));
    const auto S=[dpi](int value){return MulDiv(value,dpi,96);};
    const int availableWidth=static_cast<int>(client.right-client.left);
    const int width=std::min(S(620),std::max(S(540),availableWidth-S(96)));
    const int height=S(550);
    const int left=(client.right-width)/2, top=(client.bottom-height)/2;
    g_project_panel={left,top,left+width,top+height};
    const int padding=S(34);
    g_d_name_rect={left+padding,top+S(107),left+width-padding,top+S(142)};
    g_d_type_rect={left+padding,top+S(181),left+width/2-S(12),top+S(216)};
    g_d_group_rect={left+width/2+S(12),top+S(181),left+width-padding,top+S(216)};
    g_d_folder_rect={left+padding,top+S(255),left+width-padding-S(122),top+S(290)};
    g_d_command_rect={left+padding,top+S(329),left+width-padding,top+S(364)};
    g_d_note_rect={left+padding,top+S(403),left+width-padding,top+S(438)};
    // Text fields keep their parent-painted one-pixel outline. Dropdowns own
    // both their fill and outline, so they occupy the complete field rectangle
    // instead of exposing a strip of modal background around the child HWND.
    auto placeEdit=[&](HWND control,RECT outer){if(!control)return;aicli::ui::LayoutStyledInput(control,outer,S(1),S(5));};
    auto placeDropDown=[&](HWND control,RECT outer){if(!control)return;const int w=outer.right-outer.left,h=outer.bottom-outer.top;MoveWindow(control,outer.left,outer.top,w,h,TRUE);ApplyRoundedRegion(control,w,h,S(6));};
    auto placeButton=[&](HWND control,RECT outer){if(!control)return;MoveWindow(control,outer.left,outer.top,outer.right-outer.left,outer.bottom-outer.top,TRUE);ApplyRoundedRegion(control,outer.right-outer.left,outer.bottom-outer.top,S(7));};
    placeEdit(g_d_name,g_d_name_rect);placeEdit(g_d_note,g_d_note_rect);placeDropDown(g_d_type,g_d_type_rect);placeDropDown(g_d_group,g_d_group_rect);placeEdit(g_d_folder,g_d_folder_rect);placeEdit(g_d_command,g_d_command_rect);
    RECT browse{g_d_folder_rect.right+S(12),g_d_folder_rect.top,g_project_panel.right-padding,g_d_folder_rect.bottom}; placeButton(g_d_folder_browse,browse);
    RECT save{left+padding,top+S(490),left+width/2-S(7),top+S(526)}, cancel{left+width/2+S(7),top+S(490),left+width-padding,top+S(526)};
    HWND child=GetWindow(h,GW_CHILD); while(child){int id=GetDlgCtrlID(child);if(id==ID_D_OK){MoveWindow(child,save.left,save.top,save.right-save.left,save.bottom-save.top,TRUE);ApplyRoundedRegion(child,save.right-save.left,save.bottom-save.top,S(8));}if(id==ID_D_CANCEL){MoveWindow(child,cancel.left,cancel.top,cancel.right-cancel.left,cancel.bottom-cancel.top,TRUE);ApplyRoundedRegion(child,cancel.right-cancel.left,cancel.bottom-cancel.top,S(8));}child=GetWindow(child,GW_HWNDNEXT);}
    aicli::ui::SetModalSurfaceRegion(h,g_project_panel,S(28));
    InvalidateRect(h,nullptr,FALSE);
}
void InvalidateProjectControlFrame(HWND dialog, HWND control) {
    if (!dialog || !control) return;
    RECT frame{};
    if (control == g_d_name) frame = g_d_name_rect;
    else if (control == g_d_note) frame = g_d_note_rect;
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
    const auto S=[](int value){return ScaleUi(value);};
    DrawSmoothRoundedRect(dc,g_project_panel,ScaleUiF(14.0f),panel,border,ScaleUiF(1.0f));
    SetBkMode(dc,TRANSPARENT);
    SelectObject(dc,g_font_modal_title?g_font_modal_title:g_font_medium);
    SetTextColor(dc,fg);
    RECT title{g_project_panel.left+S(34),g_project_panel.top+S(24),g_project_panel.right-S(34),g_project_panel.top+S(50)};
    DrawTextW(dc,g_project_dialog_edit?L"编辑项目":L"新建项目",-1,&title,DT_LEFT|DT_SINGLELINE|DT_VCENTER);
    SelectObject(dc,g_font_modal_subtitle?g_font_modal_subtitle:g_font_small);SetTextColor(dc,muted);
    RECT subtitle{g_project_panel.left+S(34),g_project_panel.top+S(53),g_project_panel.right-S(34),g_project_panel.top+S(75)};
    DrawTextW(dc,L"配置项目信息与 AI CLI 启动环境",-1,&subtitle,DT_LEFT|DT_SINGLELINE|DT_VCENTER);
    SetTextColor(dc,muted);
    auto label=[&](const wchar_t* value,int y,int x=-1){
        if(x<0)x=g_project_panel.left+S(34);
        RECT rect{x,y,x+S(250),y+S(22)};DrawTextW(dc,value,-1,&rect,DT_LEFT|DT_SINGLELINE|DT_VCENTER);
    };
    label(L"项目名称 / Tab 标题",g_project_panel.top+S(81));
    label(L"项目类型",g_project_panel.top+S(155));
    label(L"分组",g_project_panel.top+S(155),g_project_panel.left+(g_project_panel.right-g_project_panel.left)/2+S(12));
    label(L"项目路径",g_project_panel.top+S(229));
    label(L"启动命令",g_project_panel.top+S(303));
    label(L"备注",g_project_panel.top+S(377));
    auto outline=[&](RECT rect,HWND control){
        const bool active=GetFocus()==control;
        DrawRoundedOutline(dc,rect,active?accent:border,ScaleUi(6));
    };
    outline(g_d_name_rect,g_d_name);outline(g_d_note_rect,g_d_note);
    // Type/group dropdowns paint a single combined fill + outline in their
    // owner-draw pass. Painting another outline underneath them caused the
    // visible unfilled rim shown around these two controls.
    outline(g_d_folder_rect,g_d_folder);outline(g_d_command_rect,g_d_command);
}
void ShowProjectDropdown(HWND dialog,HWND anchor,bool projectType){
    std::vector<std::wstring> options;
    if(projectType){for(const auto& type:g_project_types)options.push_back(type.name);}
    else{
        for(const auto& group:g_groups)options.push_back(group.name);
        if(!g_project_draft.group.empty()&&std::find(options.begin(),options.end(),g_project_draft.group)==options.end())options.push_back(g_project_draft.group);
    }
    wchar_t currentText[256]{};GetWindowTextW(anchor,currentText,256);const std::wstring current=currentText;
    std::vector<aicli::ui::RoundedSelectEntry> entries;
    entries.reserve(options.size());
    const wchar_t* icon=projectType?L"\xE756":L"\xE8B7";
    for(const auto& option:options)
        entries.push_back({option,option,L"",icon,option==current,false});

    aicli::ui::RoundedSelectPopupOptions popup;
    popup.owner=dialog;
    popup.anchor=anchor;
    popup.palette=g_theme;
    popup.text_font=g_font;
    popup.detail_font=g_font_small;
    popup.icon_font=g_font_header_icon;
    popup.dpi=std::max(96,static_cast<int>(GetDpiForWindow(dialog)));
    popup.maximum_visible_rows=8;
    popup.match_anchor_width=true;
    popup.open_state_changed=[anchor](bool open){
        g_project_dropdown_open_control=open?anchor:nullptr;
        if(anchor)InvalidateRect(anchor,nullptr,FALSE);
    };
    SetFocus(anchor);
    const auto selected=aicli::ui::ShowRoundedSelectPopup(
        popup,std::move(entries));
    if(selected){
        const std::wstring& value=*selected;
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
    InvalidateRect(anchor,nullptr,FALSE);
}
LRESULT CALLBACK ProjectDialogProc(HWND h,UINT m,WPARAM w,LPARAM l){
    switch(m){
    case WM_CREATE:{
        const std::wstring initialType=g_project_dialog_edit?g_project_draft.type:(g_project_types.empty()?L"Codex":g_project_types.front().name);
        g_d_name=aicli::ui::CreateStyledInput(h,ID_D_NAME,g_font,L"输入项目名称");
        g_d_note=aicli::ui::CreateStyledInput(h,ID_D_NOTE,g_font,L"输入项目备注（可选）");
        g_d_type=MakeDialogControl(L"BUTTON",initialType.c_str(),BS_NOTIFY|WS_TABSTOP,ID_D_TYPE,h,0,0,1,1);
        const std::wstring initialGroup=g_project_draft.group.empty()?((g_group_name==kAllGroupsName||g_group_name==kStartedProjectsName)?L"默认分组":g_group_name):g_project_draft.group;
        g_d_group=MakeDialogControl(L"BUTTON",initialGroup.c_str(),BS_NOTIFY|WS_TABSTOP,ID_D_GROUP,h,0,0,1,1);
        g_d_folder=aicli::ui::CreateStyledInput(h,ID_D_FOLDER,g_font,L"输入或选择项目文件夹");
        g_d_folder_browse=MakeDialogControl(L"BUTTON",L"选择文件夹",BS_PUSHBUTTON|WS_TABSTOP,ID_D_BROWSE,h,0,0,1,1);
        g_d_command=aicli::ui::CreateStyledInput(h,ID_D_COMMAND,g_font,L"输入启动命令");
        for(HWND field:{g_d_name,g_d_note,g_d_folder,g_d_command})
            aicli::ui::SetStyledInputTheme(field,g_theme);
        MakeDialogControl(L"BUTTON",L"保存",BS_DEFPUSHBUTTON|WS_TABSTOP,ID_D_OK,h,0,0,1,1);
        MakeDialogControl(L"BUTTON",L"取消",BS_PUSHBUTTON|WS_TABSTOP,ID_D_CANCEL,h,0,0,1,1);
        if(g_project_dialog_edit){SetWindowTextW(g_d_type,g_project_draft.type.c_str());aicli::ui::SetStyledInputText(g_d_name,g_project_draft.name);aicli::ui::SetStyledInputText(g_d_note,g_project_draft.note);aicli::ui::SetStyledInputText(g_d_folder,g_project_draft.folder);aicli::ui::SetStyledInputText(g_d_command,g_project_draft.command);}else{aicli::ui::SetStyledInputText(g_d_command,DefaultProjectCommand(initialType));}
        LayoutProjectDialog(h);SetFocus(g_d_name);return 0; }
    case WM_SIZE: LayoutProjectDialog(h);return 0;
    case WM_DPICHANGED:ApplyMainFontsToControls();LayoutProjectDialog(h);InvalidateRect(h,nullptr,FALSE);return 0;
    case WM_ERASEBKGND:return 1;
    case WM_PAINT:{PAINTSTRUCT ps;HDC dc=BeginPaint(h,&ps);RECT client{};GetClientRect(h,&client);PaintProjectDialogSurface(dc,client);EndPaint(h,&ps);return 0;}
    case WM_NCHITTEST:return HTCLIENT;
    case WM_CTLCOLORSTATIC:{
        HDC dc=(HDC)w;HWND control=(HWND)l;
        if(control==g_d_folder){
            const bool active=GetFocus()==control;const COLORREF color=active?g_theme.field_focus:g_theme.field;
            SetTextColor(dc,active?fg:g_theme.list_text);SetBkColor(dc,color);SetBkMode(dc,OPAQUE);
            return (LRESULT)(active?g_focus_brush:g_edit_brush);
        }
        SetTextColor(dc,fg);SetBkMode(dc,TRANSPARENT);return (LRESULT)GetStockObject(NULL_BRUSH);
    }
    case WM_CTLCOLOREDIT:{HDC dc=(HDC)w;HWND control=(HWND)l;const bool active=GetFocus()==control;const COLORREF color=active?g_theme.field_focus:g_theme.field;SetTextColor(dc,fg);SetBkColor(dc,color);return (LRESULT)(active?g_focus_brush:g_edit_brush);}
    case WM_CTLCOLORLISTBOX:{HDC dc=(HDC)w;SetTextColor(dc,fg);SetBkColor(dc,g_theme.list_surface);return (LRESULT)g_list_brush;}
    case WM_DRAWITEM: {auto* item=(DRAWITEMSTRUCT*)l;if(item->CtlType==ODT_BUTTON){DrawModernButton(item);return TRUE;}} break;
    case WM_COMMAND:
        if((HIWORD(w)==EN_SETFOCUS||HIWORD(w)==EN_KILLFOCUS||HIWORD(w)==BN_SETFOCUS||HIWORD(w)==BN_KILLFOCUS)&&
           (LOWORD(w)==ID_D_NAME||LOWORD(w)==ID_D_NOTE||LOWORD(w)==ID_D_TYPE||LOWORD(w)==ID_D_GROUP||LOWORD(w)==ID_D_FOLDER||LOWORD(w)==ID_D_COMMAND)){
            InvalidateProjectControlFrame(h,(HWND)l);
        }
        if(LOWORD(w)==ID_D_TYPE&&HIWORD(w)==BN_CLICKED){ShowProjectDropdown(h,g_d_type,true);return 0;}
        if(LOWORD(w)==ID_D_GROUP&&HIWORD(w)==BN_CLICKED){ShowProjectDropdown(h,g_d_group,false);return 0;}
        if(LOWORD(w)==ID_D_BROWSE){std::wstring selected;if(PickFolder(h,selected)){aicli::ui::SetStyledInputText(g_d_folder,selected);if(aicli::ui::GetStyledInputText(g_d_name).empty()){std::filesystem::path folder(selected);while(folder.filename().empty()&&folder.has_parent_path())folder=folder.parent_path();const std::wstring folderName=folder.filename().wstring();if(!folderName.empty())aicli::ui::SetStyledInputText(g_d_name,folderName);}InvalidateProjectControlFrame(h,g_d_folder);InvalidateProjectControlFrame(h,g_d_name);}return 0;}
        if(LOWORD(w)==ID_D_CANCEL){DestroyWindow(h);return 0;}
        if(LOWORD(w)==ID_D_OK){
            g_project_draft.name=aicli::ui::GetStyledInputText(g_d_name);
            g_project_draft.note=aicli::ui::GetStyledInputText(g_d_note);
            g_project_draft.folder=aicli::ui::GetStyledInputText(g_d_folder);
            g_project_draft.command=aicli::ui::GetStyledInputText(g_d_command);
            wchar_t type[256]{}; GetWindowTextW(g_d_type,type,256); g_project_draft.type=type;
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
    case WM_DESTROY:g_project_dropdown_open_control=nullptr;g_project_dialog=nullptr;g_d_name=g_d_note=g_d_type=g_d_group=g_d_folder=g_d_folder_browse=g_d_command=nullptr;return 0;
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
