// Phone pairing modal implementation. Included by app/main.cpp inside its
// private namespace to preserve the existing RemoteAgent and ModalHost wiring.

void RefreshBindingPairing(){
    g_binding_info=g_remote_agent.CreatePairing();g_pending_pairing.reset();g_binding_qr.reset();
    try{const auto value=Utf8Text(g_binding_info.qr_payload);g_binding_qr=qrcodegen::QrCode::encodeText(value.c_str(),qrcodegen::QrCode::Ecc::MEDIUM);}catch(...){g_binding_qr.reset();}
    if(g_binding_dialog)InvalidateRect(g_binding_dialog,nullptr,FALSE);
}
void LayoutBindingDialog(HWND h){RECT client{};GetClientRect(h,&client);const auto S=[](int value){return ScaleUi(value);};const int width=std::min(S(820),std::max(S(700),(int)client.right-S(96)));const int height=S(590);const int left=(client.right-width)/2,top=(client.bottom-height)/2;g_binding_panel={left,top,left+width,top+height};MoveWindow(g_binding_refresh,g_binding_panel.right-S(236),g_binding_panel.bottom-S(58),S(100),S(36),TRUE);MoveWindow(g_binding_close,g_binding_panel.right-S(124),g_binding_panel.bottom-S(58),S(90),S(36),TRUE);MoveWindow(g_binding_approve,g_binding_panel.right-S(236),g_binding_panel.bottom-S(106),S(100),S(36),TRUE);MoveWindow(g_binding_reject,g_binding_panel.right-S(124),g_binding_panel.bottom-S(106),S(90),S(36),TRUE);const bool pending=g_pending_pairing.has_value()&&g_pending_pairing->submitted&&!g_pending_pairing->authorized&&!g_pending_pairing->rejected;ShowWindow(g_binding_approve,pending?SW_SHOW:SW_HIDE);ShowWindow(g_binding_reject,pending?SW_SHOW:SW_HIDE);aicli::ui::SetModalSurfaceRegion(h,g_binding_panel,S(28));}
void PaintBindingDialog(HDC dc,RECT client){
    HBRUSH fallback=CreateSolidBrush(BlendBackdropColor(bg,g_theme.scrim,150));FillRect(dc,&client,fallback);DeleteObject(fallback);
    HBRUSH panelBrush=CreateSolidBrush(RGB(29,31,35));HPEN panelPen=CreatePen(PS_SOLID,1,RGB(59,64,73));auto oldBrush=SelectObject(dc,panelBrush);auto oldPen=SelectObject(dc,panelPen);RoundRect(dc,g_binding_panel.left,g_binding_panel.top,g_binding_panel.right,g_binding_panel.bottom,14,14);SelectObject(dc,oldBrush);SelectObject(dc,oldPen);DeleteObject(panelBrush);DeleteObject(panelPen);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,fg);SelectObject(dc,g_font_modal_title);RECT title{g_binding_panel.left+34,g_binding_panel.top+24,g_binding_panel.right-34,g_binding_panel.top+54};DrawTextW(dc,L"绑定 Android 手机",-1,&title,DT_LEFT|DT_VCENTER|DT_SINGLELINE);SelectObject(dc,g_font_small);SetTextColor(dc,muted);RECT subtitle{g_binding_panel.left+34,g_binding_panel.top+56,g_binding_panel.right-34,g_binding_panel.top+82};DrawTextW(dc,L"必须使用 AI CLI App 内的扫码入口；电脑只需确认这一次",-1,&subtitle,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
    const int qrArea=360,qrLeft=g_binding_panel.left+34,qrTop=g_binding_panel.top+100;HBRUSH white=CreateSolidBrush(RGB(245,247,250));RECT qrBack{qrLeft,qrTop,qrLeft+qrArea,qrTop+qrArea};FillRect(dc,&qrBack,white);DeleteObject(white);if(g_binding_qr){const int modules=g_binding_qr->getSize();const int quiet=4;const int scale=std::max(1,qrArea/(modules+quiet*2));const int actual=scale*(modules+quiet*2);const int originX=qrLeft+(qrArea-actual)/2+quiet*scale,originY=qrTop+(qrArea-actual)/2+quiet*scale;HBRUSH black=CreateSolidBrush(RGB(18,20,24));for(int y=0;y<modules;++y)for(int x=0;x<modules;++x)if(g_binding_qr->getModule(x,y)){RECT module{originX+x*scale,originY+y*scale,originX+(x+1)*scale,originY+(y+1)*scale};FillRect(dc,&module,black);}DeleteObject(black);}else{SetTextColor(dc,RGB(40,43,50));SelectObject(dc,g_font);DrawTextW(dc,L"二维码生成失败",-1,&qrBack,DT_CENTER|DT_VCENTER|DT_SINGLELINE);}
    const int infoLeft=qrLeft+qrArea+34,infoRight=g_binding_panel.right-34;SelectObject(dc,g_font);SetTextColor(dc,RGB(219,224,232));RECT statusTitle{infoLeft,qrTop,infoRight,qrTop+30};DrawTextW(dc,L"等待手机扫码",-1,&statusTitle,DT_LEFT|DT_VCENTER|DT_SINGLELINE);SelectObject(dc,g_font_small);SetTextColor(dc,muted);std::wstring localAddress=g_store.GetSetting(L"local_address");if(localAddress.empty()){try{const auto payload=nlohmann::json::parse(Utf8Text(g_binding_info.qr_payload));localAddress=WideText(payload.value("localAddress",std::string{}));}catch(...){}}std::wstring details=L"本地地址\n"+localAddress+L"\n\n证书指纹\n"+g_binding_info.certificate_fingerprint.substr(0,std::min<size_t>(24,g_binding_info.certificate_fingerprint.size()))+L"…\n\n二维码 5 分钟内有效。已绑定手机断网或电脑重启后不需要重新确认。若首次局域网连接超时，请在设置中执行一次“允许手机访问”。";RECT detailRect{infoLeft,qrTop+40,infoRight,qrTop+258};DrawTextW(dc,details.c_str(),-1,&detailRect,DT_LEFT|DT_TOP|DT_WORDBREAK);
    if(g_pending_pairing&&g_pending_pairing->submitted){SetTextColor(dc,g_pending_pairing->authorized?RGB(83,203,146):g_pending_pairing->rejected?RGB(225,101,112):RGB(255,196,92));SelectObject(dc,g_font_medium);RECT phoneTitle{infoLeft,qrTop+268,infoRight,qrTop+298};const wchar_t* state=g_pending_pairing->authorized?L"绑定完成":g_pending_pairing->rejected?L"已拒绝":L"等待电脑确认";DrawTextW(dc,state,-1,&phoneTitle,DT_LEFT|DT_VCENTER|DT_SINGLELINE);SelectObject(dc,g_font_small);SetTextColor(dc,fg);std::wstring phone=L"设备："+g_pending_pairing->device.name+L"\n型号："+g_pending_pairing->device.model;RECT phoneRect{infoLeft,qrTop+302,infoRight,qrTop+360};DrawTextW(dc,phone.c_str(),-1,&phoneRect,DT_LEFT|DT_TOP|DT_WORDBREAK);}
}
void PaintBindingDialogSurface(HDC dc,RECT client){
    (void)client;
    const auto S=[](int value){return ScaleUi(value);};
    DrawSmoothRoundedRect(dc,g_binding_panel,ScaleUiF(14.0f),panel,border,ScaleUiF(1.0f));
    SetBkMode(dc,TRANSPARENT);SelectObject(dc,g_font_modal_title);SetTextColor(dc,fg);
    RECT title{g_binding_panel.left+S(34),g_binding_panel.top+S(24),g_binding_panel.right-S(34),g_binding_panel.top+S(54)};
    DrawTextW(dc,L"绑定 Android 手机",-1,&title,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
    SelectObject(dc,g_font_small);SetTextColor(dc,muted);
    RECT subtitle{g_binding_panel.left+S(34),g_binding_panel.top+S(56),g_binding_panel.right-S(34),g_binding_panel.top+S(82)};
    DrawTextW(dc,L"必须使用 AI CLI App 内的扫码入口；电脑只需确认这一次",-1,&subtitle,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
    const int qrArea=S(360),qrLeft=g_binding_panel.left+S(34),qrTop=g_binding_panel.top+S(100);
    HBRUSH white=CreateSolidBrush(RGB(245,247,250));RECT qrBack{qrLeft,qrTop,qrLeft+qrArea,qrTop+qrArea};
    FillRect(dc,&qrBack,white);DeleteObject(white);
    if(g_binding_qr){
        const int modules=g_binding_qr->getSize(),quiet=4,scale=std::max(1,qrArea/(modules+quiet*2));
        const int actual=scale*(modules+quiet*2);
        const int originX=qrLeft+(qrArea-actual)/2+quiet*scale,originY=qrTop+(qrArea-actual)/2+quiet*scale;
        HBRUSH black=CreateSolidBrush(RGB(18,20,24));
        for(int y=0;y<modules;++y)for(int x=0;x<modules;++x)if(g_binding_qr->getModule(x,y)){
            RECT module{originX+x*scale,originY+y*scale,originX+(x+1)*scale,originY+(y+1)*scale};FillRect(dc,&module,black);
        }
        DeleteObject(black);
    }else{SetTextColor(dc,fg);SelectObject(dc,g_font);DrawTextW(dc,L"二维码生成失败",-1,&qrBack,DT_CENTER|DT_VCENTER|DT_SINGLELINE);}
    const int infoLeft=qrLeft+qrArea+S(34),infoRight=g_binding_panel.right-S(34);
    SelectObject(dc,g_font);SetTextColor(dc,fg);RECT statusTitle{infoLeft,qrTop,infoRight,qrTop+S(30)};
    DrawTextW(dc,L"等待手机扫码",-1,&statusTitle,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
    SelectObject(dc,g_font_small);SetTextColor(dc,muted);
    std::wstring localAddress=g_store.GetSetting(L"local_address");
    if(localAddress.empty()){try{const auto payload=nlohmann::json::parse(Utf8Text(g_binding_info.qr_payload));localAddress=WideText(payload.value("localAddress",std::string{}));}catch(...){}}
    std::wstring details=L"本地地址\n"+localAddress+L"\n\n证书指纹\n"+g_binding_info.certificate_fingerprint.substr(0,std::min<size_t>(24,g_binding_info.certificate_fingerprint.size()))+L"…\n\n二维码 5 分钟内有效。已绑定手机断网或电脑重启后不需要重新确认。";
    RECT detailRect{infoLeft,qrTop+S(40),infoRight,qrTop+S(258)};DrawTextW(dc,details.c_str(),-1,&detailRect,DT_LEFT|DT_TOP|DT_WORDBREAK);
    if(g_pending_pairing&&g_pending_pairing->submitted){
        SetTextColor(dc,g_pending_pairing->authorized?g_theme.success:g_pending_pairing->rejected?g_theme.danger:g_theme.warning);
        SelectObject(dc,g_font_medium);RECT phoneTitle{infoLeft,qrTop+S(268),infoRight,qrTop+S(298)};
        const wchar_t* state=g_pending_pairing->authorized?L"绑定完成":g_pending_pairing->rejected?L"已拒绝":L"等待电脑确认";
        DrawTextW(dc,state,-1,&phoneTitle,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
        SelectObject(dc,g_font_small);SetTextColor(dc,fg);
        std::wstring phone=L"设备："+g_pending_pairing->device.name+L"\n型号："+g_pending_pairing->device.model;
        RECT phoneRect{infoLeft,qrTop+S(302),infoRight,qrTop+S(360)};DrawTextW(dc,phone.c_str(),-1,&phoneRect,DT_LEFT|DT_TOP|DT_WORDBREAK);
    }
}
LRESULT CALLBACK BindingDialogProc(HWND h,UINT m,WPARAM w,LPARAM l){switch(m){case WM_CREATE:g_binding_refresh=MakeDialogControl(L"BUTTON",L"刷新二维码",BS_PUSHBUTTON|WS_TABSTOP,ID_BIND_REFRESH,h,0,0,1,1);g_binding_close=MakeDialogControl(L"BUTTON",L"关闭",BS_PUSHBUTTON|WS_TABSTOP,ID_BIND_CLOSE,h,0,0,1,1);g_binding_approve=MakeDialogControl(L"BUTTON",L"确认绑定",BS_PUSHBUTTON|WS_TABSTOP,ID_BIND_APPROVE,h,0,0,1,1);g_binding_reject=MakeDialogControl(L"BUTTON",L"拒绝",BS_PUSHBUTTON|WS_TABSTOP,ID_BIND_REJECT,h,0,0,1,1);RefreshBindingPairing();LayoutBindingDialog(h);return 0;case WM_SIZE:LayoutBindingDialog(h);return 0;case WM_DPICHANGED:ApplyMainFontsToControls();LayoutBindingDialog(h);InvalidateRect(h,nullptr,FALSE);return 0;case WM_ERASEBKGND:return 1;case WM_PAINT:{PAINTSTRUCT ps;HDC dc=BeginPaint(h,&ps);RECT client{};GetClientRect(h,&client);PaintBindingDialogSurface(dc,client);EndPaint(h,&ps);return 0;}case WM_DRAWITEM:DrawModernButton((DRAWITEMSTRUCT*)l);return TRUE;case WM_COMMAND:if(LOWORD(w)==ID_BIND_REFRESH){RefreshBindingPairing();return 0;}if(LOWORD(w)==ID_BIND_CLOSE){DestroyWindow(h);return 0;}if((LOWORD(w)==ID_BIND_APPROVE||LOWORD(w)==ID_BIND_REJECT)&&g_pending_pairing){std::wstring error;if(!g_remote_agent.ConfirmPairing(g_pending_pairing->info.pairing_id,LOWORD(w)==ID_BIND_APPROVE,&error)&&!error.empty())MessageBoxW(h,error.c_str(),L"手机绑定",MB_OK|MB_ICONERROR);UpdateDeviceHeader();LayoutBindingDialog(h);InvalidateRect(h,nullptr,FALSE);return 0;}break;case WM_KEYDOWN:if(w==VK_ESCAPE){DestroyWindow(h);return 0;}break;case WM_DESTROY:g_binding_dialog=nullptr;g_binding_refresh=g_binding_close=g_binding_approve=g_binding_reject=nullptr;return 0;}return DefWindowProcW(h,m,w,l);}
void ShowBindingDialog(HWND owner){
    if(!g_remote_agent.Online()){
        const wchar_t* message=aicli::agent::IsMobileAccessEnabled(g_store)
            ?L"手机访问服务启动失败，请检查设置中的监听地址和端口。"
            :L"手机访问服务已关闭，请先在设置的“本机与网络”中开启。";
        MessageBoxW(owner,message,L"绑定手机",MB_OK|MB_ICONERROR);return;
    }
    static bool registered=false;
    if(!registered){
        WNDCLASSW wc{};wc.hInstance=GetModuleHandleW(nullptr);wc.lpfnWndProc=BindingDialogProc;
        wc.lpszClassName=L"AicliBindingDialog";wc.hCursor=LoadCursor(nullptr,IDC_ARROW);
        wc.hbrBackground=nullptr;RegisterClassW(&wc);registered=true;
    }
    aicli::ui::ModalWorkspaceScope workspace(owner,aicli::ui::ModalTopForOwner(owner),&g_theme);
    g_binding_dialog=CreateWindowExW(WS_EX_CONTROLPARENT|WS_EX_TOOLWINDOW,L"AicliBindingDialog",L"",
        WS_POPUP|WS_CLIPCHILDREN|WS_CLIPSIBLINGS,0,0,1,1,
        owner,nullptr,GetModuleHandleW(nullptr),nullptr);
    if(!g_binding_dialog){Layout();return;}
    workspace.AttachSurface(g_binding_dialog);
    aicli::ui::ActivateModalSurface(g_binding_dialog);
    MSG msg{};
    while(IsWindow(g_binding_dialog)&&GetMessageW(&msg,nullptr,0,0)){TranslateMessage(&msg);DispatchMessageW(&msg);}
    g_binding_dialog=nullptr;
    aicli::ui::RestoreModalOwner(owner);
}
