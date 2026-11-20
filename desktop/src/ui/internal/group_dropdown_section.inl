// Title-bar group picker built on the shared rounded select popup.

void ShowGroupDropdown(){
    g_groups=g_store.LoadGroups();
    std::vector<aicli::ui::RoundedSelectEntry> entries;
    entries.reserve(g_groups.size()+4);

    const auto add_entry=[&](const std::wstring& value,const wchar_t* icon,
                             std::size_t count,bool separator){
        entries.push_back({value,aicli::project_views::Label(value),L"("+std::to_wstring(count)+L")",icon,
                           value==g_group_name,separator});
    };
    add_entry(kAllGroupsName,L"\xE71D",g_projects.size(),false);
    const auto started_count=static_cast<std::size_t>(std::count_if(
        g_projects.begin(),g_projects.end(),[](const Project& project){
            return g_island.HasSession(project.id);
        }));
    add_entry(kStartedProjectsName,L"\xE768",started_count,false);
    for(const auto* group:{aicli::project_views::kRecent,aicli::project_views::kFrequent})
        add_entry(group,aicli::project_views::Icon(group),ProjectViewRows(g_projects,group).size(),false);
    for(std::size_t index=0;index<g_groups.size();++index){
        const auto& group=g_groups[index];
        const auto count=static_cast<std::size_t>(std::count_if(
            g_projects.begin(),g_projects.end(),[&](const Project& project){
                const std::wstring value=project.group.empty()?DefaultGroupName():project.group;
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
        // Flush only the anchor before the popup is shown/removed. A queued
        // hover/focus repaint must not display the previous expansion state.
        if(g_group)RedrawWindow(g_group,nullptr,nullptr,
                               RDW_INVALIDATE|RDW_NOERASE|RDW_UPDATENOW);
    };
    const auto selected=aicli::ui::ShowRoundedSelectPopup(
        options,std::move(entries));
    if(!selected||(*selected==g_group_name&&!aicli::project_views::IsAutomatic(g_group_name)))return;
    g_group_name=*selected;
    g_store.SetSetting(L"last_group",g_group_name);
    RefreshProjectList();
    Layout();
}
