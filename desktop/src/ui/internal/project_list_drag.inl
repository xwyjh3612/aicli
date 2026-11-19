// Private to project_list_view.cpp: mouse capture, animation and edge scrolling
// share the same buffered surface as the rows. No layered preview window.
constexpr UINT_PTR kReorderTimer = 0xA1C1;
constexpr float kReorderAnimationMs = 140.0f;

bool ReorderActive(const ProjectListState& state) {
    return state.reorder.candidate >= 0 || state.reorder.source >= 0;
}

void ResetReorder(HWND window, ProjectListState& state) {
    const bool active = ReorderActive(state);
    state.reorder = {};
    KillTimer(window, kReorderTimer);
    if (active && GetCapture() == window) ReleaseCapture();
    if (active) InvalidateRect(window, nullptr, FALSE);
}

void SampleReorderAnimation(ProjectListState& state, ULONGLONG now) {
    auto& drag = state.reorder;
    const float t = std::clamp(static_cast<float>(now - drag.animation_start) / kReorderAnimationMs, 0.0f, 1.0f);
    const float ease = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
    for (int row = 0; row < static_cast<int>(drag.offsets.size()); ++row) {
        float target = 0;
        if (row > drag.source && row <= drag.destination) target = -static_cast<float>(state.item_height);
        if (row < drag.source && row >= drag.destination) target = static_cast<float>(state.item_height);
        drag.offsets[row] = drag.from_offsets[row] + (target - drag.from_offsets[row]) * ease;
    }
}

void UpdateReorderDestination(HWND window, ProjectListState& state) {
    auto& drag = state.reorder;
    RECT client{};
    GetClientRect(window, &client);
    drag.valid = PtInRect(&client, drag.pointer) != FALSE;
    drag.floating_y = std::clamp(drag.pointer.y - drag.grab_y,
        0.0f, static_cast<float>(std::max(0, ClientHeight(window) - state.item_height)));
    // Use content coordinates (including pixel scroll), never the list ends
    // merely because the pointer has reached the edge of the viewport.
    const float center = drag.floating_y + state.item_height / 2.0f + state.scroll_offset;
    drag.valid = drag.valid && center >= drag.first * state.item_height &&
        (drag.last == static_cast<int>(state.items.size()) - 1 || center < (drag.last + 1) * state.item_height);
    const int proposed = std::clamp(static_cast<int>(center / state.item_height), drag.first, drag.last);
    const float hysteresis = static_cast<float>(Scale(window, 4));
    const bool crossed = proposed > drag.destination
        ? center >= proposed * state.item_height + hysteresis
        : center <= (proposed + 1) * state.item_height - hysteresis;
    if (drag.valid && proposed != drag.destination && crossed) {
        SampleReorderAnimation(state, GetTickCount64());
        drag.from_offsets = drag.offsets;
        drag.animation_start = GetTickCount64();
        drag.destination = proposed;
    }
    SetCursor(LoadCursorW(nullptr, drag.valid ? IDC_SIZENS : IDC_NO));
    InvalidateRect(window, nullptr, FALSE);
}

void DrawOwnerRow(HWND window, ProjectListState& state, HDC dc, int row, int y, bool floating = false) {
    RECT client{};
    GetClientRect(window, &client);
    DRAWITEMSTRUCT item{};
    item.CtlType = ODT_LISTBOX;
    item.CtlID = static_cast<UINT>(GetDlgCtrlID(window));
    item.itemID = static_cast<UINT>(row);
    item.itemAction = ODA_DRAWENTIRE;
    item.itemState = row == state.selected ? ODS_SELECTED : 0;
    if (floating) item.itemState |= kProjectItemDragPreview;
    if (GetFocus() == window && row == state.selected) item.itemState |= ODS_FOCUS;
    item.hwndItem = window;
    item.hDC = dc;
    item.rcItem = {client.left, y, client.right, y + state.item_height};
    item.itemData = static_cast<ULONG_PTR>(state.items[row].data);
    if (HWND parent = GetParent(window))
        SendMessageW(parent, WM_DRAWITEM, item.CtlID, reinterpret_cast<LPARAM>(&item));
}

void DrawReorderPlaceholder(HWND window, const ProjectListState& state, HDC dc) {
    RECT client{};
    GetClientRect(window, &client);
    const int y = state.reorder.destination * state.item_height - state.scroll_offset;
    RECT bounds{client.left + Scale(window, 2), y + 2, client.right - Scale(window, 2), y + state.item_height - 2};
    const COLORREF fill = BlendBackdropColor(state.theme.background, state.theme.reorder_hint, 24);
    DrawSmoothRoundedRect(dc, bounds, 2.0f, fill,
        BlendBackdropColor(state.theme.background, state.theme.reorder_hint, 100), 1.0f, true);
}

void DrawFloatingProject(HWND window, ProjectListState& state, HDC dc) {
    RECT client{};
    GetClientRect(window, &client);
    const int y = static_cast<int>(std::lround(state.reorder.floating_y));
    // A restrained shadow, with the actual owner-drawn project row on top.
    {
        Gdiplus::Graphics graphics(dc);
        for (int inset = 4; inset >= 1; --inset) {
            Gdiplus::Pen shadow(Gdiplus::Color(12, 0, 0, 0), 1.0f);
            graphics.DrawRectangle(&shadow, client.left + inset, y + state.item_height + inset - 1,
                                   std::max(1L, client.right - client.left - inset * 2), 1L);
        }
    }
    DrawOwnerRow(window, state, dc, state.reorder.source, y, true);
}

std::optional<LRESULT> HandleReorderMessage(HWND window, ProjectListState& state,
                                           UINT message, WPARAM wparam, LPARAM lparam) {
    auto& drag = state.reorder;
    if (message == WM_CANCELMODE || message == WM_CAPTURECHANGED) {
        if (ReorderActive(state) && (message != WM_CAPTURECHANGED || reinterpret_cast<HWND>(lparam) != window))
            ResetReorder(window, state);
        return std::nullopt;
    }
    if (message == WM_KILLFOCUS || (message == WM_ENABLE && !wparam) ||
        (message == WM_SHOWWINDOW && !wparam)) {
        ResetReorder(window, state);
        return std::nullopt;
    }
    if (message == WM_KEYDOWN && ReorderActive(state)) {
        if (wparam == VK_ESCAPE) ResetReorder(window, state);
        return 0; // Never change selection or delete projects during a drag.
    }
    if (message == WM_CONTEXTMENU && ReorderActive(state)) return 0;
    if (message == WM_TIMER && wparam == kReorderTimer) {
        if (drag.source < 0) return 0;
        const auto now = GetTickCount64();
        SampleReorderAnimation(state, now);
        if (drag.settling) {
            const float t = std::clamp(static_cast<float>(now - drag.settle_start) / kReorderAnimationMs, 0.0f, 1.0f);
            const float target = static_cast<float>(drag.destination * state.item_height - state.scroll_offset);
            drag.floating_y = drag.settle_from + (target - drag.settle_from) * (1.0f - std::pow(1.0f - t, 3.0f));
            if (t >= 1.0f) {
                const int source = drag.source, destination = drag.destination;
                const auto commit = state.reorder_callbacks.commit;
                ResetReorder(window, state);
                if (source != destination && commit) commit(source, destination);
                return 0;
            }
        } else {
            const float elapsed = static_cast<float>(std::min<ULONGLONG>(50, now - drag.last_tick)) / 1000.0f;
            const int band = std::min(Scale(window, 32), std::max(1, ClientHeight(window) / 3));
            float speed = 0;
            if (drag.valid && drag.pointer.y < band)
                speed = -Scale(window, 720) * std::pow(1.0f - static_cast<float>(drag.pointer.y) / band, 2.0f);
            else if (drag.valid && drag.pointer.y > ClientHeight(window) - band)
                speed = Scale(window, 720) * std::pow(1.0f - static_cast<float>(ClientHeight(window) - drag.pointer.y) / band, 2.0f);
            if (std::abs(speed) > 1.0f) {
                const int pixels = static_cast<int>(std::lround(speed * elapsed));
                if (SetScrollOffset(window, state, state.scroll_offset + pixels)) UpdateReorderDestination(window, state);
            }
        }
        drag.last_tick = now;
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    }
    if (message == WM_SETCURSOR && ReorderActive(state)) {
        SetCursor(LoadCursorW(nullptr, drag.source >= 0 && !drag.valid ? IDC_NO : IDC_SIZENS));
        return TRUE;
    }
    if (message == WM_SETCURSOR && LOWORD(lparam) == HTCLIENT && state.reorder_callbacks.begin) {
        POINT point{};
        GetCursorPos(&point);
        ScreenToClient(window, &point);
        bool outside{};
        HitTestRow(window, state, point, &outside);
        if (!outside && point.x >= Scale(window, 3) && point.x < Scale(window, 31)) {
            SetCursor(LoadCursorW(nullptr, IDC_SIZENS));
            return TRUE;
        }
    }
    if (drag.settling && (message == WM_MOUSEMOVE || message == WM_LBUTTONDOWN ||
                         message == WM_LBUTTONUP || message == WM_LBUTTONDBLCLK || message == WM_MOUSEWHEEL)) return 0;
    if (message == WM_LBUTTONDOWN) {
        POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        bool outside{};
        const int row = HitTestRow(window, state, point, &outside);
        if (!outside && point.x >= Scale(window, 3) && point.x < Scale(window, 31) && state.reorder_callbacks.begin) {
            int first = 0, last = static_cast<int>(state.items.size()) - 1;
            if (state.reorder_callbacks.begin(row, first, last) && first >= 0 && first <= row && last >= row && last < static_cast<int>(state.items.size())) {
                ResetReorder(window, state);
                drag.candidate = row;
                drag.first = first;
                drag.last = last;
                drag.down = drag.pointer = point;
                drag.grab_y = static_cast<float>(point.y - (row * state.item_height - state.scroll_offset));
                SetFocus(window); // Escape must cancel even before the drag threshold.
                SetCapture(window);
                return 0;
            }
        }
    }
    if (message == WM_MOUSEMOVE && ReorderActive(state)) {
        drag.pointer = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        if (drag.source < 0) {
            const int threshold = GetSystemMetricsForDpi(SM_CYDRAG, GetDpiForWindow(window));
            if (std::abs(drag.pointer.y - drag.down.y) < threshold) return 0;
            drag.source = drag.destination = drag.candidate;
            drag.candidate = -1;
            drag.offsets.assign(state.items.size(), 0);
            drag.from_offsets = drag.offsets;
            drag.animation_start = drag.last_tick = GetTickCount64();
            SetFocus(window);
            if (!SetTimer(window, kReorderTimer, 16, nullptr)) {
                ResetReorder(window, state);
                return 0;
            }
        }
        UpdateReorderDestination(window, state);
        return 0;
    }
    if (message == WM_LBUTTONUP && ReorderActive(state)) {
        if (drag.source < 0) { ResetReorder(window, state); return 0; }
        drag.pointer = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        UpdateReorderDestination(window, state);
        if (!drag.valid) { ResetReorder(window, state); return 0; }
        drag.settling = true;
        drag.settle_start = GetTickCount64();
        drag.settle_from = drag.floating_y;
        return 0;
    }
    return std::nullopt;
}
