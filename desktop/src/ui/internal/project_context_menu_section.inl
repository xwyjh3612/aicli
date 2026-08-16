// Project/group context-menu rendering and commands. Included by app/main.cpp
// inside its private namespace until the main-window controller is fully
// object-owned.

void OpenProjectFolder(){const int index=SelectedProjectIndex();if(index<0||index>=(int)g_projects.size())return;const auto& folder=g_projects[index].folder;if(folder.empty()){MessageBoxW(g_hwnd,L"当前项目还没有设置项目文件夹。",L"打开项目文件夹",MB_OK|MB_ICONINFORMATION);return;}if((INT_PTR)ShellExecuteW(g_hwnd,L"open",folder.c_str(),nullptr,nullptr,SW_SHOWNORMAL)<=32)MessageBoxW(g_hwnd,L"无法打开项目文件夹。",L"打开项目文件夹",MB_OK|MB_ICONWARNING);}
void OpenProjectCommandPrompt(){const int index=SelectedProjectIndex();if(index<0||index>=(int)g_projects.size())return;std::wstring error;if(!aicli::app::OpenCommandPromptAt(g_projects[index].folder,&error))MessageBoxW(g_hwnd,error.c_str(),L"用 CMD 打开项目目录",MB_OK|MB_ICONWARNING);}
void MoveSelectedProjectToGroup(const std::wstring& group){const int index=SelectedProjectIndex();if(index<0||index>=(int)g_projects.size())return;g_projects[index].group=group;g_store.SaveProject(g_projects[index]);g_group_name=group;g_store.SetSetting(L"last_group",group);RefreshProjectList();SetText(g_status,L"项目已移动到 "+group);Layout();}
void MoveSelectedProjectToDefaultGroup(){MoveSelectedProjectToGroup(L"默认分组");}
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
    SetForegroundWindow(owner);
    std::vector<aicli::ui::RoundedContextMenuEntry> entries{
        {ID_CTX_OPEN_FOLDER,L"打开项目文件夹",L"\xE8B7"},
        {ID_CTX_OPEN_CMD,L"用 CMD 打开",L"\xE756"},
        {ID_CTX_EDIT,L"编辑项目",L"\xE70F"},
        {ID_CTX_MOVE_DEFAULT,L"移动到分组",L"\xE8B7",true,false,true,true},
        {ID_CTX_RESTART,running||paused?L"重新启动":L"立即开始",running||paused?L"\xE72C":L"\xE768",true,false,true},
        {paused?ID_CTX_CONTINUE:ID_CTX_PAUSE,paused?L"继续会话":L"暂停会话",paused?L"\xE768":L"\xE769",running||paused},
        {ID_CTX_DELETE,L"删除项目",L"\xE74D",true,true,true},
    };
    aicli::ui::RoundedContextMenuOptions menu_options;
    menu_options.owner=owner;
    menu_options.screen_position=screen;
    menu_options.palette=g_theme;
    menu_options.text_font=g_font;
    menu_options.icon_font=g_icon_font?g_icon_font:g_font;
    menu_options.dpi=g_layout_metrics.dpi;
    auto result=aicli::ui::ShowRoundedContextMenu(menu_options,std::move(entries));
    if(result&&result->command==ID_CTX_MOVE_DEFAULT){
        std::vector<aicli::ui::RoundedSelectEntry> group_entries;
        group_entries.reserve(g_groups.size());
        for(const auto& group:g_groups){
            group_entries.push_back({group.name,group.name,L"",L"\xE8B7",group.name==g_projects[index].group});
        }
        aicli::ui::RoundedSelectPopupOptions group_options;
        group_options.owner=owner;
        group_options.palette=g_theme;
        group_options.text_font=g_font;
        group_options.detail_font=g_font_small;
        group_options.icon_font=g_icon_font?g_icon_font:g_font;
        group_options.dpi=g_layout_metrics.dpi;
        group_options.use_screen_anchor=true;
        group_options.screen_anchor=result->item_screen_rect;
        group_options.placement=aicli::ui::RoundedPopupPlacement::Right;
        group_options.minimum_width=190;
        if(auto group=aicli::ui::ShowRoundedSelectPopup(group_options,std::move(group_entries)))
            MoveSelectedProjectToGroup(*group);
    }else if(result){
        SendMessageW(owner,WM_COMMAND,MAKEWPARAM(result->command,0),0);
    }
    PostMessageW(owner,WM_NULL,0,0);
}
