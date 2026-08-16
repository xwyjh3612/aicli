// Shared owner-draw buttons and project-list row rendering. Included by
// app/main.cpp inside its private namespace while the Win32 view state is
// progressively converted to explicit controllers.

LRESULT CALLBACK ButtonProc(HWND h,UINT m,WPARAM w,LPARAM l){
    if(m==WM_ERASEBKGND)return 1;
    if(m==WM_MOUSEMOVE){if(g_hover_button!=h){HWND old=g_hover_button;g_hover_button=h;if(old)InvalidateRect(old,nullptr,FALSE);InvalidateRect(h,nullptr,FALSE);TRACKMOUSEEVENT t{sizeof(t),TME_LEAVE,h,0};TrackMouseEvent(&t);}}
    else if(m==WM_MOUSELEAVE){if(g_hover_button==h)g_hover_button=nullptr;InvalidateRect(h,nullptr,FALSE);}
    else if(m==WM_LBUTTONDOWN||m==WM_LBUTTONUP||m==WM_SETFOCUS||m==WM_KILLFOCUS){InvalidateRect(h,nullptr,FALSE);}
    return CallWindowProcW(g_button_proc,h,m,w,l);
}
void InvalidateProjectRow(HWND list,int row){
    if(!list||row<0)return;
    RECT item{};
    if(SendMessageW(list,LB_GETITEMRECT,row,reinterpret_cast<LPARAM>(&item))!=LB_ERR)
        InvalidateRect(list,&item,FALSE);
}
LRESULT CALLBACK ProjectListSubclassProc(HWND h,UINT m,WPARAM w,LPARAM l,
                                         UINT_PTR,DWORD_PTR){
    if(m==WM_ERASEBKGND){
        // Owner-drawn rows paint their complete cards.  Hover invalidations
        // use erase=FALSE, so they never pass through a blank frame.  A real
        // background erase (initial show/resize) still has to fill the empty
        // area below the last project, otherwise the LISTBOX appears
        // transparent and exposes windows behind the application.
        HDC dc=reinterpret_cast<HDC>(w);
        if(dc){
            RECT client{};GetClientRect(h,&client);
            HBRUSH brush=g_list_brush?g_list_brush:CreateSolidBrush(SidebarBackground());
            FillRect(dc,&client,brush);
            if(!g_list_brush)DeleteObject(brush);
        }
        return 1;
    }else if(m==WM_PAINT){
        const LRESULT result=DefSubclassProc(h,m,w,l);
        if(SendMessageW(h,LB_GETCOUNT,0,0)==0){
            HDC dc=GetDC(h);
            if(dc){
                RECT client{};GetClientRect(h,&client);
                Gdiplus::Graphics graphics(dc);
                graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                const float cx=(client.left+client.right)/2.0f;
                const float cy=(client.top+client.bottom)/2.0f-ScaleUiF(16.0f);
                const COLORREF iconColor=g_light_theme?RGB(123,134,149):RGB(112,126,146);
                Gdiplus::Pen folderPen(SmoothColor(iconColor),ScaleUiF(1.5f));
                folderPen.SetStartCap(Gdiplus::LineCapRound);
                folderPen.SetEndCap(Gdiplus::LineCapRound);
                folderPen.SetLineJoin(Gdiplus::LineJoinRound);
                const float folderX=cx-ScaleUiF(19.0f);
                const float folderY=cy-ScaleUiF(14.0f);
                const float folderW=ScaleUiF(38.0f);
                const float folderH=ScaleUiF(28.0f);
                Gdiplus::GraphicsPath folderPath;
                folderPath.StartFigure();
                folderPath.AddLine(folderX+ScaleUiF(2.0f),folderY+ScaleUiF(5.0f),
                                   folderX+ScaleUiF(13.0f),folderY+ScaleUiF(5.0f));
                folderPath.AddLine(folderX+ScaleUiF(13.0f),folderY+ScaleUiF(5.0f),
                                   folderX+ScaleUiF(18.0f),folderY+ScaleUiF(10.0f));
                folderPath.AddLine(folderX+ScaleUiF(18.0f),folderY+ScaleUiF(10.0f),
                                   folderX+folderW-ScaleUiF(2.0f),folderY+ScaleUiF(10.0f));
                folderPath.AddLine(folderX+folderW-ScaleUiF(2.0f),folderY+ScaleUiF(10.0f),
                                   folderX+folderW-ScaleUiF(2.0f),folderY+folderH-ScaleUiF(2.0f));
                folderPath.AddLine(folderX+folderW-ScaleUiF(2.0f),folderY+folderH-ScaleUiF(2.0f),
                                   folderX+ScaleUiF(2.0f),folderY+folderH-ScaleUiF(2.0f));
                folderPath.CloseFigure();
                graphics.DrawPath(&folderPen,&folderPath);
                SetBkMode(dc,TRANSPARENT);
                SetTextColor(dc,muted);
                SelectObject(dc,g_font_project_list?g_font_project_list:g_font_small);
                RECT text{client.left+ScaleUi(8),static_cast<LONG>(cy+ScaleUiF(22.0f)),client.right-ScaleUi(8),
                          static_cast<LONG>(cy+ScaleUiF(48.0f))};
                DrawTextW(dc,L"当前没有项目",-1,&text,
                          DT_CENTER|DT_TOP|DT_SINGLELINE);
                ReleaseDC(h,dc);
            }
        }
        return result;
    }else if(m==WM_LBUTTONDOWN){
        g_project_drag_candidate=-1;
        if(aicli::ui::GetStyledInputText(g_search).empty()){
            POINT point{GET_X_LPARAM(l),GET_Y_LPARAM(l)};
            const LRESULT hit=SendMessageW(h,LB_ITEMFROMPOINT,0,l);
            if(!HIWORD(hit)){
                const int row=static_cast<int>(LOWORD(hit));
                RECT item{};
                if(SendMessageW(h,LB_GETITEMRECT,row,reinterpret_cast<LPARAM>(&item))!=LB_ERR){
                    const RECT handle{item.left+ScaleUi(3),item.top+ScaleUi(3),item.left+ScaleUi(31),item.bottom-ScaleUi(3)};
                    if(PtInRect(&handle,point))g_project_drag_candidate=row;
                }
            }
        }
    }else if(m==WM_LBUTTONUP){
        if(!g_project_dragging)g_project_drag_candidate=-1;
    }else if(m==WM_MOUSEMOVE){
        const LRESULT hit=SendMessageW(h,LB_ITEMFROMPOINT,0,l);
        const int next=HIWORD(hit)?-1:static_cast<int>(LOWORD(hit));
        if(next!=g_project_hover_row){
            const int previous=g_project_hover_row;
            g_project_hover_row=next;
            InvalidateProjectRow(h,previous);
            InvalidateProjectRow(h,next);
        }
        TRACKMOUSEEVENT tracking{sizeof(tracking),TME_LEAVE,h,0};
        TrackMouseEvent(&tracking);
    }else if(m==WM_MOUSELEAVE){
        const int previous=g_project_hover_row;
        g_project_hover_row=-1;
        InvalidateProjectRow(h,previous);
    }else if(m==WM_SETCURSOR&&LOWORD(l)==HTCLIENT&&
             aicli::ui::GetStyledInputText(g_search).empty()){
        POINT point{};
        GetCursorPos(&point);
        ScreenToClient(h,&point);
        const LRESULT hit=SendMessageW(h,LB_ITEMFROMPOINT,0,MAKELPARAM(point.x,point.y));
        if(!HIWORD(hit)){
            RECT item{};
            const int row=static_cast<int>(LOWORD(hit));
            if(SendMessageW(h,LB_GETITEMRECT,row,reinterpret_cast<LPARAM>(&item))!=LB_ERR){
                const RECT handle{item.left+ScaleUi(3),item.top+ScaleUi(3),item.left+ScaleUi(31),item.bottom-ScaleUi(3)};
                if(PtInRect(&handle,point)){
                    SetCursor(LoadCursorW(nullptr,IDC_SIZEALL));
                    return TRUE;
                }
            }
        }
    }else if(m==WM_SETFOCUS||m==WM_KILLFOCUS){
        const LRESULT result=DefSubclassProc(h,m,w,l);
        const LRESULT selected=SendMessageW(h,LB_GETCURSEL,0,0);
        if(selected!=LB_ERR)InvalidateProjectRow(h,static_cast<int>(selected));
        return result;
    }else if(m==WM_NCDESTROY){
        g_project_hover_row=-1;
        RemoveWindowSubclass(h,ProjectListSubclassProc,kProjectListSubclassId);
    }
    return DefSubclassProc(h,m,w,l);
}
void DrawModernButton(const DRAWITEMSTRUCT* item){
    RECT r=item->rcItem;const bool down=(item->itemState&ODS_SELECTED)!=0;const bool disabled=(item->itemState&ODS_DISABLED)!=0;const bool hover=item->hwndItem==g_hover_button;const bool focused=GetFocus()==item->hwndItem;HWND parentWindow=GetParent(item->hwndItem);
    const bool projectDropDown=parentWindow==g_project_dialog&&(item->CtlID==ID_D_TYPE||item->CtlID==ID_D_GROUP);
    const bool projectButton=parentWindow==g_project_dialog;
    COLORREF under=panel;
    bool onMainTitleBar=false;
    if(parentWindow==g_hwnd){POINT pos{};ClientToScreen(item->hwndItem,&pos);ScreenToClient(parentWindow,&pos);onMainTitleBar=pos.y<g_layout_metrics.title_bar_height;under=onMainTitleBar?TitleBarBackground():(pos.x<g_layout_metrics.sidebar_width?SidebarBackground():bg);}
    HBRUSH clear=CreateSolidBrush(under);FillRect(item->hDC,&r,clear);DeleteObject(clear);
    wchar_t text[256]{};GetWindowTextW(item->hwndItem,text,256);SetBkMode(item->hDC,TRANSPARENT);
    const bool title_button=parentWindow==g_hwnd&&
        (item->CtlID==ID_MINIMIZE||item->CtlID==ID_MAXIMIZE||item->CtlID==ID_CLOSE);
    const bool header_action_button=parentWindow==g_hwnd&&onMainTitleBar&&
        (item->CtlID==ID_GROUP_DEFAULT||item->CtlID==ID_GROUP_ADD||
         item->CtlID==ID_BIND_PHONE||item->CtlID==ID_DEVICES||
         item->CtlID==ID_SETTINGS);
    if(title_button){
        COLORREF fill=TitleBarBackground();
        if(hover){
            if(item->CtlID==ID_CLOSE)fill=g_theme.titlebar_close_hover;
            else fill=g_theme.titlebar_hover;
        }
        if(down){
            if(item->CtlID==ID_CLOSE)fill=g_theme.titlebar_close_pressed;
            else fill=g_theme.titlebar_pressed;
        }
        HBRUSH button_brush=CreateSolidBrush(fill);FillRect(item->hDC,&r,button_brush);DeleteObject(button_brush);
        const COLORREF glyph=(item->CtlID==ID_CLOSE&&hover)?RGB(255,255,255):TitleBarText();
        HPEN glyph_pen=CreatePen(PS_SOLID,1,glyph);auto old_pen=SelectObject(item->hDC,glyph_pen);
        const int cx=(r.left+r.right)/2,cy=(r.top+r.bottom)/2;
        if(item->CtlID==ID_MINIMIZE){MoveToEx(item->hDC,cx-7,cy+1,nullptr);LineTo(item->hDC,cx+7,cy+1);}
        else if(item->CtlID==ID_MAXIMIZE){
            if(IsZoomed(g_hwnd)){MoveToEx(item->hDC,cx-4,cy-6,nullptr);LineTo(item->hDC,cx+7,cy-6);LineTo(item->hDC,cx+7,cy+5);MoveToEx(item->hDC,cx-7,cy-3,nullptr);LineTo(item->hDC,cx+4,cy-3);LineTo(item->hDC,cx+4,cy+8);LineTo(item->hDC,cx-7,cy+8);LineTo(item->hDC,cx-7,cy-3);}
            else{Rectangle(item->hDC,cx-7,cy-6,cx+7,cy+7);}
        }else{MoveToEx(item->hDC,cx-6,cy-6,nullptr);LineTo(item->hDC,cx+6,cy+6);MoveToEx(item->hDC,cx+6,cy-6,nullptr);LineTo(item->hDC,cx-6,cy+6);}
        SelectObject(item->hDC,old_pen);DeleteObject(glyph_pen);return;
    }
    if(projectDropDown){
        const COLORREF fill=down?g_theme.pressed:
            (focused?g_theme.field_focus:(hover?g_theme.hover:g_theme.field));
        // Fill the entire rounded child region first. A fill made only through
        // RoundRect leaves its antialiased/exclusive edge showing the modal
        // background, which looked like the dropdown did not reach its frame.
        HBRUSH brush=CreateSolidBrush(fill);FillRect(item->hDC,&r,brush);DeleteObject(brush);
        const COLORREF stroke=focused?accent:border;
        HPEN pen=CreatePen(PS_SOLID,1,stroke);auto oldPen=SelectObject(item->hDC,pen);auto oldBrush=SelectObject(item->hDC,GetStockObject(HOLLOW_BRUSH));RoundRect(item->hDC,r.left,r.top,r.right,r.bottom,7,7);SelectObject(item->hDC,oldBrush);SelectObject(item->hDC,oldPen);DeleteObject(pen);
        SetTextColor(item->hDC,fg);SelectObject(item->hDC,g_font);RECT textRect{r.left+ScaleUi(10),r.top,r.right-ScaleUi(34),r.bottom};DrawTextW(item->hDC,text,-1,&textRect,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
        SetTextColor(item->hDC,focused?accent:muted);SelectObject(item->hDC,g_icon_font?g_icon_font:g_font);RECT arrow{r.right-ScaleUi(30),r.top,r.right-ScaleUi(7),r.bottom};DrawTextW(item->hDC,L"\xE70D",-1,&arrow,DT_CENTER|DT_VCENTER|DT_SINGLELINE);return;
    }
    COLORREF fill=under,stroke=RGB(52,59,70);
    if(projectButton){
        const bool primary=item->CtlID==ID_D_OK;
        if(primary){fill=disabled?g_theme.surface:(down?g_theme.accent_strong:(hover?g_theme.accent:g_theme.accent_strong));stroke=fill;}
        else{fill=disabled?g_theme.surface:(down?g_theme.pressed:(hover?g_theme.hover:g_theme.field));stroke=fill;}
    }else{
        if(item->CtlID==ID_D_CANCEL||item->CtlID==ID_EDIT_PROJECT||item->CtlID==ID_OPEN_PROJECT_FOLDER)fill=elevated;
        if(item->CtlID==ID_D_OK||item->CtlID==ID_START_PROJECT)fill=accent2;
        if(disabled)fill=g_light_theme?RGB(235,238,243):RGB(33,37,45);
        else if(down){
            // Header actions already provide a hover cue.  A bright blue
            // pressed frame is perceived as a flash when a modal opens on
            // the next compositor frame, especially on the fast second open.
            // Keep the pressed surface identical to hover for these actions.
            fill=onMainTitleBar
                ?(g_light_theme?RGB(229,235,243):RGB(34,40,51))
                :(g_light_theme?RGB(205,220,241):RGB(37,91,174));
        }else if(hover)fill=g_light_theme?RGB(229,235,243):RGB(34,40,51);
        if(item->CtlID==ID_CLOSE)fill=down?RGB(157,44,55):(hover?RGB(120,38,47):panel);if(item->CtlID==ID_MINIMIZE)fill=hover?RGB(37,42,51):panel;
        stroke=disabled?(g_light_theme?RGB(218,223,231):RGB(43,48,57)):(hover?(g_light_theme?RGB(164,181,205):RGB(78,104,142)):border);if(item->CtlID==ID_D_OK||item->CtlID==ID_START_PROJECT)stroke=hover?RGB(117,177,255):RGB(64,128,225);
        if(item->CtlID==ID_NEW_PROJECT){
            fill=disabled?SidebarBackground():(down?g_theme.pressed:(hover?g_theme.hover:SidebarBackground()));
            stroke=disabled?g_theme.border:(hover?(g_light_theme?RGB(139,151,168):RGB(91,106,128)):g_theme.border);
        }
        if(header_action_button&&!g_light_theme){
            fill=disabled?RGB(12,15,30):(down?RGB(29,35,61):(hover?RGB(22,28,51):RGB(14,18,39)));
            stroke=disabled?RGB(35,41,62):(hover?RGB(67,79,108):RGB(42,49,75));
        }
    }
    if(item->CtlID==ID_NEW_PROJECT){
        RECT shell{r.left,r.top,r.right-1,r.bottom-1};
        DrawSmoothRoundedRect(item->hDC,shell,6.0f,fill,stroke,1.0f);
        Gdiplus::Graphics graphics(item->hDC);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        const COLORREF glyphColor=disabled?muted:(hover?fg:g_theme.muted);
        Gdiplus::Pen glyph(SmoothColor(glyphColor),ScaleUiF(1.35f));
        glyph.SetStartCap(Gdiplus::LineCapRound);
        glyph.SetEndCap(Gdiplus::LineCapRound);
        const float cx=(r.left+r.right)/2.0f;
        const float cy=(r.top+r.bottom)/2.0f;
        const float half=ScaleUiF(3.75f);
        graphics.DrawLine(&glyph,cx-half,cy,cx+half,cy);
        graphics.DrawLine(&glyph,cx,cy-half,cx,cy+half);
        return;
    }
    HBRUSH brush=CreateSolidBrush(fill);HPEN pen=CreatePen(PS_SOLID,1,stroke);auto oldBrush=SelectObject(item->hDC,brush);auto oldPen=SelectObject(item->hDC,pen);RoundRect(item->hDC,r.left+1,r.top+1,r.right-1,r.bottom-1,9,9);SelectObject(item->hDC,oldBrush);SelectObject(item->hDC,oldPen);DeleteObject(brush);DeleteObject(pen);
    const bool primary_button=item->CtlID==ID_D_OK||item->CtlID==ID_START_PROJECT;COLORREF text_color=disabled?muted:((primary_button||(!g_light_theme&&hover))?RGB(255,255,255):fg);HFONT text_font=(item->CtlID==ID_NEW_PROJECT||item->CtlID==ID_EDIT_PROJECT||item->CtlID==ID_OPEN_PROJECT_FOLDER)?g_icon_font:(onMainTitleBar?g_font_header:g_font);UINT text_format=DT_CENTER|DT_VCENTER|DT_SINGLELINE;if(onMainTitleBar&&!g_window_chrome.active()&&!hover&&!down){text_color=TitleBarText();}SetTextColor(item->hDC,text_color);
    if(header_action_button){
        const wchar_t* icon=L"\xE710";
        if(item->CtlID==ID_GROUP_DEFAULT){
            icon=g_group_name==kAllGroupsName?L"\xE71D":
                 (g_group_name==kStartedProjectsName?L"\xE768":L"\xE8B7");
        }else if(item->CtlID==ID_BIND_PHONE){icon=L"\xE717";}
        else if(item->CtlID==ID_DEVICES){icon=L"\xE772";}
        else if(item->CtlID==ID_SETTINGS){icon=L"\xE713";}

        if(item->CtlID==ID_GROUP_ADD){
            SelectObject(item->hDC,g_font_header_icon);
            DrawTextW(item->hDC,icon,-1,&r,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
            return;
        }

        SIZE iconSize{},textSize{};
        auto previousFont=SelectObject(item->hDC,g_font_header_icon);
        GetTextExtentPoint32W(item->hDC,icon,1,&iconSize);
        SelectObject(item->hDC,g_font_header);
        GetTextExtentPoint32W(item->hDC,text,static_cast<int>(wcslen(text)),&textSize);
        const int gap=ScaleUi(5);
        const int padding=ScaleUi(8);
        const int contentWidth=iconSize.cx+gap+textSize.cx;
        const int availableWidth=std::max(1L,r.right-r.left-padding*2);
        const int startX=r.left+padding+
            std::max(0,(availableWidth-std::min(contentWidth,availableWidth))/2);
        RECT iconRect{startX,r.top,startX+iconSize.cx,r.bottom};
        SelectObject(item->hDC,g_font_header_icon);
        DrawTextW(item->hDC,icon,-1,&iconRect,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
        RECT textRect{iconRect.right+gap,r.top,r.right-padding,r.bottom};
        SelectObject(item->hDC,g_font_header);
        DrawTextW(item->hDC,text,-1,&textRect,
                  DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
        SelectObject(item->hDC,previousFont);
        return;
    }
    SelectObject(item->hDC,text_font);DrawTextW(item->hDC,text,-1,&r,text_format);
}
void DrawProjectItemContent(const DRAWITEMSTRUCT* item){
    if(item->itemID==(UINT)-1)return;
    const int projectIndex=static_cast<int>(
        SendMessageW(item->hwndItem,LB_GETITEMDATA,item->itemID,0));
    if(projectIndex<0||projectIndex>=static_cast<int>(g_projects.size()))return;

    const auto& project=g_projects[projectIndex];
    const bool selected=(item->itemState&ODS_SELECTED)!=0;
    const bool hovered=static_cast<int>(item->itemID)==g_project_hover_row;
    const bool sourceDragging=g_project_dragging&&
        static_cast<int>(item->itemID)==g_project_drag_source;
    RECT row=item->rcItem;
    HBRUSH rowBackground=CreateSolidBrush(SidebarBackground());
    FillRect(item->hDC,&row,rowBackground);
    DeleteObject(rowBackground);

    // Two physical pixels between adjacent project cards, with only a subtle
    // two-pixel corner radius as requested for the compact sidebar.
    RECT card{row.left+1,row.top+1,row.right-1,row.bottom-1};
    COLORREF cardFill=g_light_theme?RGB(250,251,253):SidebarBackground();
    if(sourceDragging){
        cardFill=SidebarBackground();
    }else if(selected){
        cardFill=g_light_theme?RGB(238,245,254):RGB(23,38,65);
    }else if(hovered){
        cardFill=g_light_theme?RGB(243,247,252):RGB(17,28,48);
    }
    DrawSmoothRoundedRect(item->hDC,card,2.0f,cardFill,cardFill,0.0f);

    COLORREF stateColor=g_light_theme?RGB(100,113,131):RGB(139,157,183);
    enum class StateIcon{Idle,Running,Paused,Warning};
    StateIcon stateIcon=StateIcon::Idle;
    if(g_island.HasSession(project.id)){
        stateIcon=StateIcon::Running;
        stateColor=RGB(73,203,145);
    }else if(project.status==L"paused"){
        stateIcon=StateIcon::Paused;
        stateColor=g_light_theme?RGB(177,113,16):RGB(245,178,72);
    }else if(project.status==L"cancelled"||project.status==L"error"){
        // A stopped project is a recoverable state, so use a warm warning
        // triangle instead of a red close/cancel glyph.
        stateIcon=StateIcon::Warning;
        stateColor=g_light_theme?RGB(190,132,26):RGB(244,187,72);
    }
    if(!g_light_theme&&stateIcon==StateIcon::Idle){
        stateColor=selected?RGB(168,199,240):
            (hovered?RGB(145,164,189):RGB(113,128,150));
    }
    if(sourceDragging)stateColor=muted;

    {
        Gdiplus::Graphics graphics(item->hDC);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

        Gdiplus::Pen grip(SmoothColor(
            hovered&&!sourceDragging
                ?(g_light_theme?RGB(111,128,150):RGB(131,145,165))
                :(g_light_theme?RGB(157,166,178):RGB(91,101,116))),ScaleUiF(1.25f));
        grip.SetStartCap(Gdiplus::LineCapRound);
        grip.SetEndCap(Gdiplus::LineCapRound);
        const float gripCenter=(card.top+card.bottom)/2.0f;
        for(float offset=-ScaleUiF(4.0f);offset<=ScaleUiF(4.0f);offset+=ScaleUiF(4.0f))
            graphics.DrawLine(&grip,static_cast<float>(card.left)+ScaleUiF(11.0f),gripCenter+offset,
                              static_cast<float>(card.left)+ScaleUiF(20.0f),gripCenter+offset);

        const float iconSize=ScaleUiF(20.0f);
        const float iconX=static_cast<float>(card.right)-iconSize-ScaleUiF(11.0f);
        const float iconY=(card.top+card.bottom-iconSize)/2.0f;
        Gdiplus::RectF iconBounds{iconX,iconY,iconSize,iconSize};
        const bool workingSpinner=stateIcon==StateIcon::Running&&IsProjectWorking(project);
        Gdiplus::SolidBrush halo(SmoothColor(stateColor,30));
        if(!workingSpinner&&stateIcon!=StateIcon::Running&&
           stateIcon!=StateIcon::Idle)
            graphics.FillEllipse(&halo,iconBounds);
        Gdiplus::Pen iconPen(SmoothColor(stateColor),ScaleUiF(1.65f));
        iconPen.SetStartCap(Gdiplus::LineCapRound);
        iconPen.SetEndCap(Gdiplus::LineCapRound);
        Gdiplus::SolidBrush iconBrush(SmoothColor(stateColor));
        const float cx=iconX+iconSize/2.0f,cy=iconY+iconSize/2.0f;
        if(workingSpinner){
            Gdiplus::Pen trackPen(SmoothColor(stateColor,65),ScaleUiF(1.65f));
            Gdiplus::Pen spinnerPen(SmoothColor(stateColor),ScaleUiF(2.35f));
            trackPen.SetStartCap(Gdiplus::LineCapRound);
            trackPen.SetEndCap(Gdiplus::LineCapRound);
            spinnerPen.SetStartCap(Gdiplus::LineCapRound);
            spinnerPen.SetEndCap(Gdiplus::LineCapRound);
            graphics.DrawEllipse(&trackPen,iconX+ScaleUiF(2.0f),iconY+ScaleUiF(2.0f),
                                 iconSize-ScaleUiF(4.0f),iconSize-ScaleUiF(4.0f));
            graphics.DrawArc(&spinnerPen,iconX+ScaleUiF(2.0f),iconY+ScaleUiF(2.0f),
                             iconSize-ScaleUiF(4.0f),iconSize-ScaleUiF(4.0f),
                             g_project_spinner_angle,112.0f);
        }else if(stateIcon==StateIcon::Running){
            const float pulse=(std::sin(
                g_project_idle_pulse_angle*3.14159265f/180.0f)+1.0f)*0.5f;
            const float diameter=ScaleUiF(8.0f+2.0f*pulse);
            const auto channel=[pulse](BYTE low,BYTE high){
                return static_cast<BYTE>(low+(high-low)*pulse+0.5f);
            };
            const COLORREF pulseColor=RGB(
                channel(0x1f,0x49),channel(0x79,0xcb),channel(0x55,0x91));
            Gdiplus::SolidBrush pulseBrush(SmoothColor(pulseColor));
            graphics.FillEllipse(&pulseBrush,cx-diameter/2.0f,cy-diameter/2.0f,
                                 diameter,diameter);
        }else if(stateIcon==StateIcon::Paused){
            graphics.DrawLine(&iconPen,cx-ScaleUiF(2.5f),cy-ScaleUiF(4.0f),cx-ScaleUiF(2.5f),cy+ScaleUiF(4.0f));
            graphics.DrawLine(&iconPen,cx+ScaleUiF(2.5f),cy-ScaleUiF(4.0f),cx+ScaleUiF(2.5f),cy+ScaleUiF(4.0f));
        }else if(stateIcon==StateIcon::Warning){
            Gdiplus::PointF triangle[3]{
                {cx,iconY+ScaleUiF(3.0f)},
                {iconX+ScaleUiF(3.0f),iconY+iconSize-ScaleUiF(3.0f)},
                {iconX+iconSize-ScaleUiF(3.0f),iconY+iconSize-ScaleUiF(3.0f)}};
            graphics.DrawPolygon(&iconPen,triangle,3);
            graphics.DrawLine(&iconPen,cx,cy-ScaleUiF(2.5f),cx,cy+ScaleUiF(1.5f));
            graphics.FillEllipse(&iconBrush,cx-ScaleUiF(0.9f),cy+ScaleUiF(3.0f),ScaleUiF(1.8f),ScaleUiF(1.8f));
        }else{
            graphics.DrawEllipse(&iconPen,cx-ScaleUiF(4.0f),cy-ScaleUiF(4.0f),ScaleUiF(8.0f),ScaleUiF(8.0f));
        }
    }

    SetBkMode(item->hDC,TRANSPARENT);
    auto oldFont=SelectObject(item->hDC,g_font_project_list);
    wchar_t name[256]{};
    SendMessageW(item->hwndItem,LB_GETTEXT,item->itemID,(LPARAM)name);
    SetTextColor(item->hDC,sourceDragging?muted:fg);
    RECT title{card.left+ScaleUi(29),card.top,card.right-ScaleUi(39),card.bottom};
    DrawTextW(item->hDC,name,-1,&title,
              DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
    SelectObject(item->hDC,oldFont);

    if(g_project_dragging){
        const int count=static_cast<int>(SendMessageW(item->hwndItem,LB_GETCOUNT,0,0));
        int markerY=-1;
        if(g_project_drag_insert==static_cast<int>(item->itemID))markerY=row.top+2;
        else if(g_project_drag_insert==count&&static_cast<int>(item->itemID)==count-1)
            markerY=row.bottom-3;
        if(markerY>=0){
            Gdiplus::Graphics graphics(item->hDC);
            graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            Gdiplus::Pen glow(SmoothColor(accent,70),4.0f);
            Gdiplus::Pen marker(SmoothColor(accent),2.0f);
            glow.SetStartCap(Gdiplus::LineCapRound);glow.SetEndCap(Gdiplus::LineCapRound);
            marker.SetStartCap(Gdiplus::LineCapRound);marker.SetEndCap(Gdiplus::LineCapRound);
            graphics.DrawLine(&glow,static_cast<float>(row.left+10),static_cast<float>(markerY),
                              static_cast<float>(row.right-8),static_cast<float>(markerY));
            graphics.DrawLine(&marker,static_cast<float>(row.left+10),static_cast<float>(markerY),
                              static_cast<float>(row.right-8),static_cast<float>(markerY));
            Gdiplus::SolidBrush markerDot(SmoothColor(accent));
            graphics.FillEllipse(&markerDot,static_cast<float>(row.left+6),
                                  static_cast<float>(markerY-3),6.0f,6.0f);
        }
    }
}
void DrawProjectItem(const DRAWITEMSTRUCT* item){
    if(!item||!item->hDC||item->itemID==(UINT)-1)return;
    const RECT row=item->rcItem;
    const int width=std::max(1L,row.right-row.left);
    const int height=std::max(1L,row.bottom-row.top);
    HDC buffer=CreateCompatibleDC(item->hDC);
    HBITMAP bitmap=buffer?CreateCompatibleBitmap(item->hDC,width,height):nullptr;
    if(!buffer||!bitmap){
        if(bitmap)DeleteObject(bitmap);
        if(buffer)DeleteDC(buffer);
        DrawProjectItemContent(item);
        return;
    }
    const HGDIOBJ previous=SelectObject(buffer,bitmap);
    // Keep the original list coordinates so all card/icon geometry remains
    // identical, but render into an off-screen row-sized surface first.
    SetWindowOrgEx(buffer,row.left,row.top,nullptr);
    DRAWITEMSTRUCT buffered=*item;
    buffered.hDC=buffer;
    DrawProjectItemContent(&buffered);
    SetWindowOrgEx(buffer,0,0,nullptr);
    BitBlt(item->hDC,row.left,row.top,width,height,buffer,0,0,SRCCOPY);
    SelectObject(buffer,previous);
    DeleteObject(bitmap);
    DeleteDC(buffer);
}
