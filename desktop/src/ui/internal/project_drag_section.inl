// Model adapter for the custom list's native reorder controller.
// Input, pixel scrolling and animation live in ui/internal/project_list_drag.inl.

void ConfigureProjectReordering(){
    auto visible_ids=std::make_shared<std::vector<std::wstring>>();
    aicli::ui::SetProjectListReorderCallbacks(g_list,{
        [visible_ids](int source,int& first,int& last){
            if(aicli::update::IsInstalling()||aicli::ui::HasActiveModal(g_hwnd)||
               !aicli::ui::GetStyledInputText(g_search).empty())return false;
            visible_ids->clear();
            for(const int index:g_visible_project_indices){
                if(index<0||index>=static_cast<int>(g_projects.size()))return false;
                visible_ids->push_back(g_projects[index].id);
            }
            if(source<0||source>=static_cast<int>(g_visible_project_indices.size()))return false;
            const auto group_for=[](int row){
                const auto& project=g_projects[g_visible_project_indices[row]];
                return project.group.empty()?DefaultGroupName():project.group;
            };
            const auto group=group_for(source);
            first=last=source;
            while(first>0&&group_for(first-1)==group)--first;
            while(last+1<static_cast<int>(g_visible_project_indices.size())&&group_for(last+1)==group)++last;
            return first!=last;
        },
        [visible_ids](int source,int destination){
            // Revalidate against the latest saved models before touching order.
            const auto projects=g_store.LoadProjects();
            const auto plan=aicli::project_order::MoveVisible(projects,*visible_ids,
                source,destination,DefaultGroupName());
            if(!plan)return;
            std::wstring error;
            if(!g_store.ReorderProjects(plan->group,plan->ids,&error)){
                SetStatusText(L"无法保存项目顺序："+error);
                return;
            }
            const auto selected_id=g_last_project_id;
            std::unordered_set<std::wstring> shown(visible_ids->begin(),visible_ids->end());
            g_projects=g_store.LoadProjects();
            g_visible_project_indices.clear();
            std::vector<aicli::ui::ProjectListEntry> entries;
            int selected=-1;
            for(int index=0;index<static_cast<int>(g_projects.size());++index){
                const auto& project=g_projects[index];
                if(!shown.contains(project.id))continue;
                if(project.id==selected_id)selected=static_cast<int>(entries.size());
                entries.push_back({project.name,static_cast<LPARAM>(index)});
                g_visible_project_indices.push_back(index);
            }
            // No selection notification, terminal activation, full layout or
            // LB_RESETCONTENT: retain the current project and pixel viewport.
            aicli::ui::ReplaceProjectListEntries(g_list,std::move(entries),selected);
            g_project_hover_row=-1;
            SetStatusText(L"项目顺序已保存");
        }
    });
}
