// Project/group context-menu rendering and commands. Included by app/main.cpp
// inside its private namespace until the main-window controller is fully
// object-owned.

struct ContextMenuEntry{const wchar_t* icon;const wchar_t* label;bool submenu;bool compact_icon{};bool current{};};
void DrawContextMenuItem(const DRAWITEMSTRUCT* item){
    const auto* entry=reinterpret_cast<const ContextMenuEntry*>(item->itemData);
    if(!entry)return;
    RECT r=item->rcItem;
    if(!entry->label[0]){HBRUSH background=CreateSolidBrush(g_light_theme?RGB(250,251,253):RGB(34,36,40));FillRect(item->hDC,&r,background);DeleteObject(background);HPEN line=CreatePen(PS_SOLID,std::max(1,ScaleUi(1)),g_light_theme?RGB(218,223,231):RGB(57,60,67));auto old=(HPEN)SelectObject(item->hDC,line);const int y=(r.top+r.bottom)/2;MoveToEx(item->hDC,r.left+ScaleUi(8),y,nullptr);LineTo(item->hDC,r.right-ScaleUi(8),y);SelectObject(item->hDC,old);DeleteObject(line);return;}
    const bool selected=(item->itemState&ODS_SELECTED)!=0;
    const bool disabled=(item->itemState&ODS_DISABLED)!=0;
    HBRUSH fill=CreateSolidBrush(selected?(g_light_theme?RGB(226,237,251):RGB(55,62,73)):(g_light_theme?RGB(250,251,253):RGB(34,36,40)));FillRect(item->hDC,&r,fill);DeleteObject(fill);
    SetBkMode(item->hDC,TRANSPARENT);SetTextColor(item->hDC,disabled?(g_light_theme?RGB(153,160,170):RGB(111,116,126)):(g_light_theme?RGB(38,44,54):RGB(222,226,233)));
    SelectObject(item->hDC,entry->compact_icon?g_font_header_icon:(g_icon_font?g_icon_font:g_font));RECT icon{r.left+ScaleUi(13),r.top+ScaleUi(1),r.left+ScaleUi(39),r.bottom-ScaleUi(1)};DrawTextW(item->hDC,entry->icon,-1,&icon,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    SelectObject(item->hDC,g_font);RECT text{r.left+ScaleUi(49),r.top+ScaleUi(1),r.right-ScaleUi(34),r.bottom-ScaleUi(1)};DrawTextW(item->hDC,entry->label,-1,&text,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
    if(entry->current){SetTextColor(item->hDC,accent);SelectObject(item->hDC,g_font_header_icon);RECT current{r.right-ScaleUi(29),r.top+ScaleUi(1),r.right-ScaleUi(9),r.bottom-ScaleUi(1)};DrawTextW(item->hDC,L"\xE73E",-1,&current,DT_CENTER|DT_VCENTER|DT_SINGLELINE);}
    if(entry->submenu){SelectObject(item->hDC,g_font_small);RECT arrow{r.right-ScaleUi(28),r.top+ScaleUi(1),r.right-ScaleUi(10),r.bottom-ScaleUi(1)};DrawTextW(item->hDC,L">",-1,&arrow,DT_CENTER|DT_VCENTER|DT_SINGLELINE);}
}
void ConfigureContextMenu(HMENU menu,HBRUSH background){MENUINFO info{sizeof(info)};info.fMask=MIM_BACKGROUND;info.hbrBack=background;SetMenuInfo(menu,&info);}
void AppendContextEntry(HMENU menu,UINT flags,UINT_PTR id,const ContextMenuEntry& entry){AppendMenuW(menu,flags|MF_OWNERDRAW,id,reinterpret_cast<LPCWSTR>(&entry));}
void OpenProjectFolder(){const int index=SelectedProjectIndex();if(index<0||index>=(int)g_projects.size())return;const auto& folder=g_projects[index].folder;if(folder.empty()){MessageBoxW(g_hwnd,L"当前项目还没有设置项目文件夹。",L"打开项目文件夹",MB_OK|MB_ICONINFORMATION);return;}if((INT_PTR)ShellExecuteW(g_hwnd,L"open",folder.c_str(),nullptr,nullptr,SW_SHOWNORMAL)<=32)MessageBoxW(g_hwnd,L"无法打开项目文件夹。",L"打开项目文件夹",MB_OK|MB_ICONWARNING);}
void OpenProjectCommandPrompt(){const int index=SelectedProjectIndex();if(index<0||index>=(int)g_projects.size())return;std::wstring error;if(!aicli::app::OpenCommandPromptAt(g_projects[index].folder,&error))MessageBoxW(g_hwnd,error.c_str(),L"用 CMD 打开项目目录",MB_OK|MB_ICONWARNING);}
void MoveSelectedProjectToGroup(const std::wstring& group){const int index=SelectedProjectIndex();if(index<0||index>=(int)g_projects.size())return;g_projects[index].group=group;g_store.SaveProject(g_projects[index]);g_group_name=group;g_store.SetSetting(L"last_group",group);RefreshProjectList();SetText(g_status,L"项目已移动到 "+group);Layout();}
void MoveSelectedProjectToDefaultGroup(){MoveSelectedProjectToGroup(L"默认分组");}
void ShowGroupDropdown(){
    g_groups=g_store.LoadGroups();g_projects=g_store.LoadProjects();if(g_groups.empty())return;
    HMENU menu=CreatePopupMenu();if(!menu)return;HBRUSH brush=CreateSolidBrush(g_light_theme?RGB(250,251,253):RGB(34,36,40));ConfigureContextMenu(menu,brush);
    std::vector<std::wstring> labels;labels.reserve(g_groups.size()+2);
    labels.push_back(std::wstring(kAllGroupsName)+L" ("+std::to_wstring(g_projects.size())+L")");
    const auto started_count=std::count_if(g_projects.begin(),g_projects.end(),[](const Project& project){return g_island.HasSession(project.id);});
    labels.push_back(std::wstring(kStartedProjectsName)+L" ("+std::to_wstring(started_count)+L")");
    for(const auto& group:g_groups){const auto count=std::count_if(g_projects.begin(),g_projects.end(),[&](const Project& project){const std::wstring value=project.group.empty()?L"默认分组":project.group;return value==group.name;});labels.push_back(group.name+L" ("+std::to_wstring(count)+L")");}
    std::vector<ContextMenuEntry> entries;entries.reserve(labels.size());
    entries.push_back({L"\xE71D",labels[0].c_str(),false,true,g_group_name==kAllGroupsName});
    AppendContextEntry(menu,MF_ENABLED,ID_GROUP_SELECT_BASE,entries.back());
    entries.push_back({L"\xE768",labels[1].c_str(),false,true,g_group_name==kStartedProjectsName});
    AppendContextEntry(menu,MF_ENABLED,ID_GROUP_SELECT_BASE+1,entries.back());
    for(size_t i=0;i<g_groups.size();++i){entries.push_back({L"\xE8B7",labels[i+2].c_str(),false,true,g_groups[i].name==g_group_name});AppendContextEntry(menu,MF_ENABLED,ID_GROUP_SELECT_BASE+2+static_cast<UINT>(i),entries.back());}
    RECT button{};GetWindowRect(g_group,&button);const UINT command=TrackPopupMenuEx(menu,TPM_RETURNCMD|TPM_LEFTALIGN|TPM_TOPALIGN|TPM_NONOTIFY,button.left,button.bottom,g_hwnd,nullptr);DestroyMenu(menu);DeleteObject(brush);
    if(command==ID_GROUP_SELECT_BASE){g_group_name=kAllGroupsName;g_store.SetSetting(L"last_group",g_group_name);RefreshProjectList();RefreshProjectDetails();Layout();}
    else if(command==ID_GROUP_SELECT_BASE+1){g_group_name=kStartedProjectsName;g_store.SetSetting(L"last_group",g_group_name);RefreshProjectList();RefreshProjectDetails();Layout();}
    else if(command>=ID_GROUP_SELECT_BASE+2&&command<ID_GROUP_SELECT_BASE+2+g_groups.size()){g_group_name=g_groups[command-ID_GROUP_SELECT_BASE-2].name;g_store.SetSetting(L"last_group",g_group_name);RefreshProjectList();RefreshProjectDetails();Layout();}
}
void CreateGroupFromHeader(){
    std::wstring name;if(!aicli::ui::ShowGroupNameDialog(g_hwnd,L"添加分组",L"创建后会立即切换到这个分组。",L"",g_light_theme,name))return;std::wstring error;if(!g_store.AddGroup(name,&error)){MessageBoxW(g_hwnd,error.c_str(),L"添加分组",MB_OK|MB_ICONWARNING);return;}g_groups=g_store.LoadGroups();g_group_name=name;g_store.SetSetting(L"last_group",name);RefreshProjectList();RefreshProjectDetails();Layout();SetText(g_status,L"已创建分组 "+name);
}
void ShowProjectContextMenu(HWND owner,LPARAM position){
    int index=SelectedProjectIndex();
    POINT screen{};
    if(position==static_cast<LPARAM>(-1)){
        if(index<0||index>=(int)g_projects.size())return;
        RECT item{};
        if(SendMessageW(g_list,LB_GETITEMRECT,index,(LPARAM)&item)==LB_ERR)return;
        screen={item.left+28,item.bottom};
        ClientToScreen(g_list,&screen);
    }else{
        screen={GET_X_LPARAM(position),GET_Y_LPARAM(position)};
        POINT client=screen;
        ScreenToClient(g_list,&client);
        const LRESULT hit=SendMessageW(g_list,LB_ITEMFROMPOINT,0,MAKELPARAM(client.x,client.y));
        if(HIWORD(static_cast<DWORD_PTR>(hit))!=0)return;
        index=LOWORD(static_cast<DWORD_PTR>(hit));
        if(index<0||index>=(int)g_visible_project_indices.size())return;
        SendMessageW(g_list,LB_SETCURSEL,index,0);
        index=SelectedProjectIndex();
        RefreshProjectDetails();
        Layout();
    }
    const bool running=g_island.HasSession(g_projects[index].id);
    const bool paused=g_projects[index].status==L"paused";
    HMENU menu=CreatePopupMenu(),groups=CreatePopupMenu();
    if(!menu||!groups){if(menu)DestroyMenu(menu);if(groups)DestroyMenu(groups);return;}
    HBRUSH menuBrush=CreateSolidBrush(g_light_theme?RGB(250,251,253):RGB(34,36,40));ConfigureContextMenu(menu,menuBrush);ConfigureContextMenu(groups,menuBrush);
    ContextMenuEntry open{L"\xE8B7",L"打开项目文件夹",false};
    ContextMenuEntry command_prompt{L"\xE756",L"用 CMD 打开项目目录",false};
    ContextMenuEntry details{L"\xE946",L"项目详情",false};
    ContextMenuEntry move{L"\xE8AB",L"移动到分组",true};
    ContextMenuEntry restart{L"\xE72C",L"重新启动项目",false};
    ContextMenuEntry session{L"\xE769",paused?L"继续会话":L"暂停会话",false};
    ContextMenuEntry close{L"\xE74D",L"关闭项目",false};
    ContextMenuEntry separator{L"",L"",false};
    AppendContextEntry(menu,MF_ENABLED,ID_CTX_OPEN_FOLDER,open);
    AppendContextEntry(menu,MF_ENABLED,ID_CTX_OPEN_CMD,command_prompt);
    AppendContextEntry(menu,MF_ENABLED,ID_CTX_EDIT,details);
    std::vector<ContextMenuEntry> group_entries;group_entries.reserve(g_groups.size());
    for(size_t group_index=0;group_index<g_groups.size();++group_index){
        group_entries.push_back({L"\xE8AB",g_groups[group_index].name.c_str(),false});
        AppendMenuW(groups,MF_OWNERDRAW|MF_ENABLED,ID_CTX_MOVE_GROUP_BASE+static_cast<UINT>(group_index),reinterpret_cast<LPCWSTR>(&group_entries.back()));
    }
    AppendMenuW(menu,MF_OWNERDRAW|MF_POPUP,(UINT_PTR)groups,reinterpret_cast<LPCWSTR>(&move));
    AppendContextEntry(menu,MF_ENABLED,ID_CTX_RESTART,restart);
    AppendContextEntry(menu,(running||paused)?MF_ENABLED:MF_GRAYED,paused?ID_CTX_CONTINUE:ID_CTX_PAUSE,session);
    AppendContextEntry(menu,MF_DISABLED,0,separator);
    AppendContextEntry(menu,MF_ENABLED,ID_CTX_DELETE,close);
    SetForegroundWindow(owner);
    TrackPopupMenuEx(menu,TPM_RIGHTBUTTON|TPM_LEFTALIGN,screen.x,screen.y,owner,nullptr);
    DestroyMenu(menu);
    DeleteObject(menuBrush);
    PostMessageW(owner,WM_NULL,0,0);
}
