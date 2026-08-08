// Project drag preview, hit testing and reorder behavior. Included by
// app/main.cpp inside its private namespace to retain the current list HWND,
// animation messages and SQLite ordering semantics without behavioral changes.

void EndProjectDragPreview(){
    if(g_project_drag_preview)DestroyWindow(g_project_drag_preview);
    g_project_drag_preview=nullptr;
    g_project_drag_hotspot={};
}
bool BeginProjectDragPreview(HWND list,int row,POINT cursor_screen){
    RECT item{};
    if(SendMessageW(list,LB_GETITEMRECT,row,reinterpret_cast<LPARAM>(&item))==LB_ERR)return false;
    const int project_index=static_cast<int>(SendMessageW(list,LB_GETITEMDATA,row,0));
    if(project_index<0||project_index>=static_cast<int>(g_projects.size()))return false;

    constexpr int shadow_margin=7;
    const int card_width=std::max(1,static_cast<int>(item.right-item.left)-7);
    const int card_height=std::max(1,static_cast<int>(item.bottom-item.top)-6);
    const int width=card_width+shadow_margin*2;
    const int height=card_height+shadow_margin*2+3;

    BITMAPINFO bitmap_info{};
    bitmap_info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    bitmap_info.bmiHeader.biWidth=width;
    bitmap_info.bmiHeader.biHeight=-height;
    bitmap_info.bmiHeader.biPlanes=1;
    bitmap_info.bmiHeader.biBitCount=32;
    bitmap_info.bmiHeader.biCompression=BI_RGB;
    void* pixels{};
    HDC screen=GetDC(nullptr);
    HDC memory=screen?CreateCompatibleDC(screen):nullptr;
    HBITMAP bitmap=memory?CreateDIBSection(memory,&bitmap_info,DIB_RGB_COLORS,&pixels,nullptr,0):nullptr;
    if(!screen||!memory||!bitmap||!pixels){
        if(bitmap)DeleteObject(bitmap);
        if(memory)DeleteDC(memory);
        if(screen)ReleaseDC(nullptr,screen);
        return false;
    }
    std::memset(pixels,0,static_cast<size_t>(width)*height*4);
    auto old_bitmap=SelectObject(memory,bitmap);

    {
        Gdiplus::Bitmap surface(width,height,width*4,PixelFormat32bppPARGB,
                                static_cast<BYTE*>(pixels));
        Gdiplus::Graphics graphics(&surface);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        graphics.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);

        Gdiplus::RectF shadow_bounds{
            static_cast<float>(shadow_margin),static_cast<float>(shadow_margin+4),
            static_cast<float>(card_width),static_cast<float>(card_height)};
        Gdiplus::GraphicsPath shadow_path;
        AddRoundedPath(shadow_path,shadow_bounds,10.0f);
        Gdiplus::SolidBrush shadow_brush(Gdiplus::Color(72,0,0,0));
        graphics.FillPath(&shadow_brush,&shadow_path);

        Gdiplus::RectF card_bounds{
            static_cast<float>(shadow_margin)+0.5f,static_cast<float>(shadow_margin)+0.5f,
            static_cast<float>(card_width)-1.0f,static_cast<float>(card_height)-1.0f};
        Gdiplus::GraphicsPath card_path;
        AddRoundedPath(card_path,card_bounds,9.5f);
        const COLORREF card_fill=g_light_theme?RGB(246,249,253):RGB(28,34,43);
        const COLORREF card_stroke=g_light_theme?RGB(124,166,220):RGB(72,116,177);
        Gdiplus::SolidBrush card_brush(SmoothColor(card_fill,244));
        Gdiplus::Pen card_pen(SmoothColor(card_stroke,245),1.25f);
        graphics.FillPath(&card_brush,&card_path);
        graphics.DrawPath(&card_pen,&card_path);

        Gdiplus::Pen grip(SmoothColor(g_light_theme?RGB(107,123,145):RGB(151,164,184),230),1.4f);
        grip.SetStartCap(Gdiplus::LineCapRound);
        grip.SetEndCap(Gdiplus::LineCapRound);
        const float grip_center=shadow_margin+card_height/2.0f;
        for(float offset=-4.0f;offset<=4.0f;offset+=4.0f)
            graphics.DrawLine(&grip,static_cast<float>(shadow_margin+11),grip_center+offset,
                              static_cast<float>(shadow_margin+20),grip_center+offset);

        const auto& project=g_projects[project_index];
        Gdiplus::FontFamily family(L"Segoe UI");
        Gdiplus::Font preview_font(&family,15.0f,Gdiplus::FontStyleRegular,Gdiplus::UnitPixel);
        Gdiplus::SolidBrush text_brush(SmoothColor(fg,245));
        Gdiplus::StringFormat text_format;
        text_format.SetAlignment(Gdiplus::StringAlignmentNear);
        text_format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        text_format.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
        text_format.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
        Gdiplus::RectF text_bounds{
            static_cast<float>(shadow_margin+29),static_cast<float>(shadow_margin),
            static_cast<float>(std::max(1,card_width-69)),static_cast<float>(card_height)};
        graphics.DrawString(project.name.c_str(),static_cast<INT>(project.name.size()),
                            &preview_font,text_bounds,&text_format,&text_brush);

        COLORREF state_color=g_light_theme?RGB(100,113,131):RGB(139,157,183);
        enum class PreviewState{Idle,Running,Paused,Warning};
        PreviewState state=PreviewState::Idle;
        if(g_island.HasSession(project.id)){
            state=PreviewState::Running;
            state_color=g_light_theme?RGB(28,139,91):RGB(73,203,145);
        }else if(project.status==L"paused"){
            state=PreviewState::Paused;
            state_color=g_light_theme?RGB(177,113,16):RGB(245,178,72);
        }else if(project.status==L"cancelled"||project.status==L"error"){
            state=PreviewState::Warning;
            state_color=g_light_theme?RGB(190,132,26):RGB(244,187,72);
        }
        const float icon_size=20.0f;
        const float icon_x=shadow_margin+card_width-icon_size-11.0f;
        const float icon_y=shadow_margin+(card_height-icon_size)/2.0f;
        const float cx=icon_x+icon_size/2.0f,cy=icon_y+icon_size/2.0f;
        Gdiplus::SolidBrush halo(SmoothColor(state_color,34));
        Gdiplus::SolidBrush icon_brush(SmoothColor(state_color,245));
        Gdiplus::Pen icon_pen(SmoothColor(state_color,245),1.65f);
        icon_pen.SetStartCap(Gdiplus::LineCapRound);
        icon_pen.SetEndCap(Gdiplus::LineCapRound);
        graphics.FillEllipse(&halo,icon_x,icon_y,icon_size,icon_size);
        if(state==PreviewState::Running){
            graphics.DrawEllipse(&icon_pen,icon_x+2.0f,icon_y+2.0f,icon_size-4.0f,icon_size-4.0f);
            graphics.FillEllipse(&icon_brush,cx-3.0f,cy-3.0f,6.0f,6.0f);
        }else if(state==PreviewState::Paused){
            graphics.DrawLine(&icon_pen,cx-2.5f,cy-4.0f,cx-2.5f,cy+4.0f);
            graphics.DrawLine(&icon_pen,cx+2.5f,cy-4.0f,cx+2.5f,cy+4.0f);
        }else if(state==PreviewState::Warning){
            Gdiplus::PointF triangle[3]{{cx,icon_y+3.0f},
                {icon_x+3.0f,icon_y+icon_size-3.0f},
                {icon_x+icon_size-3.0f,icon_y+icon_size-3.0f}};
            graphics.DrawPolygon(&icon_pen,triangle,3);
            graphics.DrawLine(&icon_pen,cx,cy-2.5f,cx,cy+1.5f);
            graphics.FillEllipse(&icon_brush,cx-0.9f,cy+3.0f,1.8f,1.8f);
        }else graphics.DrawEllipse(&icon_pen,cx-4.0f,cy-4.0f,8.0f,8.0f);
    }

    POINT card_origin{item.left+3,item.top+3};
    ClientToScreen(list,&card_origin);
    g_project_drag_hotspot={
        std::clamp<int>(static_cast<int>(cursor_screen.x-card_origin.x)+shadow_margin,0,width-1),
        std::clamp<int>(static_cast<int>(cursor_screen.y-card_origin.y)+shadow_margin,0,height-1)};
    POINT destination{cursor_screen.x-g_project_drag_hotspot.x,
                      cursor_screen.y-g_project_drag_hotspot.y};
    SIZE size{width,height};
    POINT source_origin{};
    BLENDFUNCTION blend{AC_SRC_OVER,0,255,AC_SRC_ALPHA};

    EndProjectDragPreview();
    g_project_drag_preview=CreateWindowExW(
        WS_EX_LAYERED|WS_EX_TOOLWINDOW|WS_EX_TRANSPARENT|WS_EX_NOACTIVATE,
        L"STATIC",L"",WS_POPUP,destination.x,destination.y,width,height,
        g_hwnd,nullptr,GetModuleHandleW(nullptr),nullptr);
    bool updated=g_project_drag_preview&&UpdateLayeredWindow(
        g_project_drag_preview,screen,&destination,&size,memory,&source_origin,0,&blend,ULW_ALPHA);
    if(updated)ShowWindow(g_project_drag_preview,SW_SHOWNOACTIVATE);

    SelectObject(memory,old_bitmap);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr,screen);
    if(!updated){EndProjectDragPreview();return false;}
    return true;
}
void MoveProjectDragPreview(POINT cursor_screen){
    if(!g_project_drag_preview)return;
    SetWindowPos(g_project_drag_preview,HWND_TOP,
                 cursor_screen.x-g_project_drag_hotspot.x,
                 cursor_screen.y-g_project_drag_hotspot.y,
                 0,0,SWP_NOSIZE|SWP_NOACTIVATE|SWP_SHOWWINDOW);
}
void RepaintProjectDragList(HWND list){
    if(!list)return;
    InvalidateRect(list,nullptr,FALSE);
    UpdateWindow(list);
}
void ResetProjectDrag(HWND list){
    const bool repaint=g_project_dragging||g_project_drag_insert>=0;
    EndProjectDragPreview();
    g_project_drag_source=-1;g_project_drag_insert=-1;g_project_dragging=false;
    if(repaint&&list)InvalidateRect(list,nullptr,FALSE);
}
int ProjectDropSlot(HWND list,POINT point){
    const int count=static_cast<int>(SendMessageW(list,LB_GETCOUNT,0,0));
    if(count<=0)return 0;
    RECT client{};GetClientRect(list,&client);
    if(point.y<=client.top)return 0;
    if(point.y>=client.bottom)return count;
    const LRESULT hit=SendMessageW(list,LB_ITEMFROMPOINT,0,MAKELPARAM(point.x,point.y));
    int row=static_cast<int>(LOWORD(hit));
    if(row<0)return 0;if(row>=count)return count;
    RECT item{};if(SendMessageW(list,LB_GETITEMRECT,row,reinterpret_cast<LPARAM>(&item))==LB_ERR)return row;
    return point.y<(item.top+item.bottom)/2?row:row+1;
}
void ReorderVisibleProjects(int source_row,int insert_slot){
    if(g_group_name==kAllGroupsName)return;
    const int count=static_cast<int>(g_visible_project_indices.size());
    if(source_row<0||source_row>=count||insert_slot<0||insert_slot>count)return;
    int destination=insert_slot;if(destination>source_row)--destination;
    if(destination==source_row)return;

    std::vector<std::wstring> ordered_ids;ordered_ids.reserve(g_visible_project_indices.size());
    for(const int index:g_visible_project_indices){if(index>=0&&index<(int)g_projects.size())ordered_ids.push_back(g_projects[index].id);}
    if(source_row>=static_cast<int>(ordered_ids.size()))return;
    const std::wstring moving_id=ordered_ids[source_row];
    ordered_ids.erase(ordered_ids.begin()+source_row);
    destination=std::clamp(destination,0,static_cast<int>(ordered_ids.size()));
    ordered_ids.insert(ordered_ids.begin()+destination,moving_id);

    std::wstring error;
    if(!g_store.ReorderProjects(g_group_name,ordered_ids,&error)){
        MessageBoxW(g_hwnd,(L"无法保存项目顺序："+error).c_str(),L"项目排序",MB_OK|MB_ICONWARNING);
        return;
    }
    g_projects=g_store.LoadProjects();
    g_last_project_id=moving_id;SendMessageW(g_list,LB_SETCURSEL,static_cast<WPARAM>(LB_ERR),0);
    RefreshProjectList();RefreshProjectDetails();Layout();SetText(g_status,L"项目顺序已保存");
}
LRESULT HandleProjectDragListMessage(HWND,LPARAM lparam){
    auto* info=reinterpret_cast<DRAGLISTINFO*>(lparam);
    if(!info||info->hWnd!=g_list)return 0;
    switch(info->uNotification){
    case DL_BEGINDRAG:{
        ResetProjectDrag(g_list);
        if(g_group_name==kAllGroupsName)return FALSE;
        if(!aicli::ui::GetStyledInputText(g_search).empty())return FALSE;
        const int row=LBItemFromPt(g_list,info->ptCursor,FALSE);
        const int count=static_cast<int>(SendMessageW(g_list,LB_GETCOUNT,0,0));
        if(row<0||row>=count)return FALSE;
        POINT client_point=info->ptCursor;
        ScreenToClient(g_list,&client_point);
        RECT item{};
        if(SendMessageW(g_list,LB_GETITEMRECT,row,reinterpret_cast<LPARAM>(&item))==LB_ERR)
            return FALSE;
        const RECT drag_handle{item.left+3,item.top+3,item.left+31,item.bottom-3};
        if(!PtInRect(&drag_handle,client_point))return FALSE;
        BeginProjectDragPreview(g_list,row,info->ptCursor);
        g_project_dragging=true;g_project_drag_source=row;g_project_drag_insert=row;
        RepaintProjectDragList(g_list);return TRUE;
    }
    case DL_DRAGGING:{
        if(!g_project_dragging)return DL_STOPCURSOR;
        LBItemFromPt(g_list,info->ptCursor,TRUE);
        MoveProjectDragPreview(info->ptCursor);
        POINT client_point=info->ptCursor;ScreenToClient(g_list,&client_point);
        const int slot=ProjectDropSlot(g_list,client_point);
        if(slot!=g_project_drag_insert){g_project_drag_insert=slot;RepaintProjectDragList(g_list);}
        return DL_MOVECURSOR;
    }
    case DL_DROPPED:{
        if(!g_project_dragging)return 0;
        POINT client_point=info->ptCursor;ScreenToClient(g_list,&client_point);
        const int source=g_project_drag_source,slot=ProjectDropSlot(g_list,client_point);
        ResetProjectDrag(g_list);ReorderVisibleProjects(source,slot);return 0;
    }
    case DL_CANCELDRAG:
        ResetProjectDrag(g_list);return 0;
    }
    return 0;
}
