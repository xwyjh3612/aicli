// Title-bar group picker built on the shared rounded select popup.

void ShowGroupDropdown(){
    g_groups=g_store.LoadGroups();
    std::vector<aicli::ui::RoundedSelectEntry> entries;
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

    aicli::ui::RoundedSelectPopupOptions options;
    options.owner=g_hwnd;
    options.anchor=g_group;
    options.palette=g_theme;
    options.text_font=g_font_header;
    options.detail_font=g_font_small;
    options.icon_font=g_font_header_icon;
    options.dpi=g_layout_metrics.dpi;
    options.open_state_changed=[](bool open){
        g_group_dropdown_open=open;
        if(g_group)InvalidateRect(g_group,nullptr,FALSE);
    };
    const auto selected=aicli::ui::ShowRoundedSelectPopup(
        options,std::move(entries));
    if(!selected||*selected==g_group_name)return;
    g_group_name=*selected;
    g_store.SetSetting(L"last_group",g_group_name);
    RefreshProjectList();
    RefreshProjectDetails();
    Layout();
}
