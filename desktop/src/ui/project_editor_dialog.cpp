#include "ui/project_editor_dialog.h"
#include "ui/dialog_metrics.h"
#include "ui/styled_input.h"
#include "ui/drawing_utils.h"
#include "ui/modal_host.h"
#include "ui/rounded_select_popup.h"
#include "data/data_store.h"
#include "features/project_configuration.h"

#include <commctrl.h>
#include <shobjidl.h>
#include <algorithm>
#include <filesystem>
#include <utility>

namespace aicli::ui
{
namespace
{
enum
{
    ID_D_NAME = 2001,
    ID_D_TYPE = 2002,
    ID_D_GROUP = 2003,
    ID_D_FOLDER = 2004,
    ID_D_COMMAND = 2005,
    ID_D_OK = 2006,
    ID_D_CANCEL = 2007,
    ID_D_BROWSE = 2008,
    ID_D_NOTE = 2009,
    ID_D_CLOSE = 2010
};
constexpr wchar_t kAllGroupsName[] = L"全部";
constexpr wchar_t kStartedProjectsName[] = L"已启动";
constexpr UINT_PTR kProjectButtonSubclassId = 0xA1C40;

bool PickFolder(HWND owner, std::wstring &path)
{
    IFileDialog *dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog))))
        return false;
    DWORD flags{};
    dialog->GetOptions(&flags);
    dialog->SetOptions(flags | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    bool selected = false;
    if (SUCCEEDED(dialog->Show(owner)))
    {
        IShellItem *item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item)))
        {
            PWSTR value = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &value)))
            {
                path = value;
                CoTaskMemFree(value);
                selected = true;
            }
            item->Release();
        }
    }
    dialog->Release();
    return selected;
}
} // namespace

struct ProjectEditorDialog::Impl
{
    explicit Impl(ProjectEditorOptions value) : context(std::move(value))
    {
    }
    ~Impl()
    {
        if (IsWindow(window))
            DestroyWindow(window);
        if (field_brush)
            DeleteObject(field_brush);
        if (focus_brush)
            DeleteObject(focus_brush);
        if (list_brush)
            DeleteObject(list_brush);
    }
    ProjectEditorOptions context;
    HWND window{}, name_control{}, note_control{}, type_control{}, group_control{},
        folder_control{}, browse_control{}, command_control{}, close_control{};
    HWND open_dropdown{}, hovered{};
    RECT panel_rect{}, name_rect{}, note_rect{}, type_rect{}, group_rect{}, folder_rect{},
        command_rect{};
    bool accepted{}, editing{};
    Project draft;
    HBRUSH field_brush{}, focus_brush{}, list_brush{};

    const ThemePalette &palette() const
    {
        return context.palette;
    }
    ProjectEditorFonts fonts() const
    {
        return context.fonts();
    }
    int Scale(int value) const
    {
        return DpiScale(window, value);
    }
    float ScaleF(float value) const
    {
        return value * DpiFor(window) / 96.0f;
    }

    void RefreshFonts()
    {
        for (HWND child = GetWindow(window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
            SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(fonts().body), TRUE);
    }
    void RefreshTheme()
    {
        if (field_brush)
            DeleteObject(field_brush);
        if (focus_brush)
            DeleteObject(focus_brush);
        if (list_brush)
            DeleteObject(list_brush);
        field_brush = CreateSolidBrush(palette().field);
        focus_brush = CreateSolidBrush(palette().field_focus);
        list_brush = CreateSolidBrush(palette().list_surface);
        for (HWND field : {name_control, note_control, folder_control, command_control})
            if (field)
                SetStyledInputTheme(field, palette());
        if (window)
            RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_NOERASE | RDW_ALLCHILDREN);
    }
    static LRESULT CALLBACK WindowProc(HWND h, UINT m, WPARAM w, LPARAM l)
    {
        auto *self = reinterpret_cast<Impl *>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (m == WM_NCCREATE)
        {
            self = static_cast<Impl *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
            self->window = h;
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        return self ? self->HandleMessage(h, m, w, l) : DefWindowProcW(h, m, w, l);
    }
    static LRESULT CALLBACK ButtonProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR,
                                       DWORD_PTR reference)
    {
        auto &self = *reinterpret_cast<Impl *>(reference);
        if (m == WM_ERASEBKGND)
            return 1;
        if (m == WM_MOUSEMOVE)
        {
            if (self.hovered != h)
            {
                HWND old = self.hovered;
                self.hovered = h;
                if (old)
                    InvalidateRect(old, nullptr, FALSE);
                InvalidateRect(h, nullptr, FALSE);
                TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, h, 0};
                TrackMouseEvent(&tracking);
            }
        }
        else if (m == WM_MOUSELEAVE)
        {
            if (self.hovered == h)
                self.hovered = nullptr;
            InvalidateRect(h, nullptr, FALSE);
        }
        else if (m == WM_LBUTTONDOWN || m == WM_LBUTTONUP || m == WM_SETFOCUS || m == WM_KILLFOCUS)
        {
            InvalidateRect(h, nullptr, FALSE);
        }
        else if (m == WM_NCDESTROY)
        {
            if (self.hovered == h)
                self.hovered = nullptr;
            RemoveWindowSubclass(h, ButtonProc, kProjectButtonSubclassId);
        }
        return DefSubclassProc(h, m, w, l);
    }
    void EnableButtonHover(HWND control)
    {
        SetWindowSubclass(control, ButtonProc, kProjectButtonSubclassId,
                          reinterpret_cast<DWORD_PTR>(this));
    }
    HWND MakeDialogControl(LPCWSTR cls, LPCWSTR text, DWORD style, int id, HWND parent, int x,
                           int y, int w, int h)
    {
        bool button = wcscmp(cls, L"BUTTON") == 0;
        if (button)
            style |= BS_OWNERDRAW;
        HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, parent,
                                 (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(c, WM_SETFONT, (WPARAM)fonts().body, TRUE);
        if (button)
            EnableButtonHover(c);
        return c;
    }
    void DrawProjectButton(const DRAWITEMSTRUCT *item)
    {
        RECT r = item->rcItem;
        const bool down = (item->itemState & ODS_SELECTED) != 0;
        const bool disabled = (item->itemState & ODS_DISABLED) != 0;
        const bool hover = item->hwndItem == hovered;
        const bool focused = GetFocus() == item->hwndItem;
        HBRUSH clear = CreateSolidBrush(palette().panel);
        FillRect(item->hDC, &r, clear);
        DeleteObject(clear);
        wchar_t text[256]{};
        GetWindowTextW(item->hwndItem, text, 256);
        SetBkMode(item->hDC, TRANSPARENT);
        if (item->CtlID == ID_D_TYPE || item->CtlID == ID_D_GROUP)
        {
            const bool open = open_dropdown == item->hwndItem;
            const COLORREF fill =
                down ? palette().pressed
                     : ((focused || open) ? palette().field_focus
                                          : (hover ? palette().hover : palette().field));
            const COLORREF stroke = (focused || open) ? palette().accent : palette().border;
            // The common button pass has already cleared the control to the
            // dialog surface colour. Painting a rectangular field fill here
            // leaves visible square corners behind the anti-aliased rounded path.
            DrawSmoothRoundedRect(item->hDC, r, ScaleF(6.0f), fill, stroke, 1.0f);

            const wchar_t *fieldIcon = item->CtlID == ID_D_TYPE ? L"\xE756" : L"\xE8B7";
            RECT iconRect{r.left + Scale(9), r.top, r.left + Scale(28), r.bottom};
            SetTextColor(item->hDC, (focused || open) ? palette().accent : palette().muted);
            SelectObject(item->hDC, fonts().icon);
            DrawTextW(item->hDC, fieldIcon, -1, &iconRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            SetTextColor(item->hDC, palette().text);
            SelectObject(item->hDC, fonts().body);
            RECT textRect{iconRect.right + Scale(6), r.top, r.right - Scale(34), r.bottom};
            DrawTextW(item->hDC, text, -1, &textRect,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

            SetTextColor(item->hDC, (focused || open) ? palette().accent : palette().muted);
            SelectObject(item->hDC, fonts().icon);
            RECT arrow{r.right - Scale(28), r.top, r.right - Scale(8), r.bottom};
            DrawTextW(item->hDC, open ? L"\xE70E" : L"\xE70D", -1, &arrow,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            return;
        }
        const bool primary = item->CtlID == ID_D_OK;
        const COLORREF fill =
            primary
                ? (disabled ? palette().surface
                            : (down ? palette().accent_strong
                                    : (hover ? palette().accent : palette().accent_strong)))
                : (disabled
                       ? palette().surface
                       : (down ? palette().pressed : (hover ? palette().hover : palette().field)));
        HBRUSH brush = CreateSolidBrush(fill);
        HPEN pen = CreatePen(PS_SOLID, 1, fill);
        auto old_brush = SelectObject(item->hDC, brush);
        auto old_pen = SelectObject(item->hDC, pen);
        RoundRect(item->hDC, r.left + 1, r.top + 1, r.right - 1, r.bottom - 1, 9, 9);
        SelectObject(item->hDC, old_brush);
        SelectObject(item->hDC, old_pen);
        DeleteObject(brush);
        DeleteObject(pen);
        SetTextColor(item->hDC,
                     disabled ? palette().muted
                              : ((primary || (!palette().light() && hover)) ? RGB(255, 255, 255)
                                                                            : palette().text));
        SelectObject(item->hDC, fonts().body);
        DrawTextW(item->hDC, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    std::wstring DefaultProjectCommand(const std::wstring &type)
    {
        return aicli::project_configuration::DefaultCommand(context.types, type);
    }
    std::wstring DefaultProjectTypeName()
    {
        const std::int64_t defaultId = context.store.DefaultProjectTypeId();
        const auto found =
            std::find_if(context.types.begin(), context.types.end(),
                         [&](const ProjectType &value) { return value.id == defaultId; });
        if (found != context.types.end())
            return found->name;
        return context.types.empty() ? L"Codex" : context.types.front().name;
    }
    std::wstring NormalizedProjectFolder(const std::wstring &folder)
    {
        std::error_code error;
        std::filesystem::path path = std::filesystem::weakly_canonical(folder, error);
        if (error)
        {
            error.clear();
            path = std::filesystem::absolute(folder, error);
            if (error)
                path = folder;
        }
        std::wstring normalized = path.lexically_normal().wstring();
        while (normalized.size() > 3 && (normalized.back() == L'\\' || normalized.back() == L'/'))
            normalized.pop_back();
        return normalized;
    }
    bool ProjectFolderAlreadyUsed(const std::wstring &folder)
    {
        const std::wstring normalized = NormalizedProjectFolder(folder);
        return std::any_of(context.projects.begin(), context.projects.end(),
                           [&](const Project &project) {
                               return !project.folder.empty() &&
                                      _wcsicmp(NormalizedProjectFolder(project.folder).c_str(),
                                               normalized.c_str()) == 0;
                           });
    }
    void LayoutProjectDialog(HWND h)
    {
        RECT client{};
        GetClientRect(h, &client);
        const int dpi = std::max(96, static_cast<int>(GetDpiForWindow(h)));
        const auto S = [dpi](int value) { return MulDiv(value, dpi, 96); };
        const int availableWidth = static_cast<int>(client.right - client.left);
        const int width = std::min(S(620), std::max(S(540), availableWidth - S(96)));
        const int height = S(558);
        const int left = (client.right - width) / 2, top = (client.bottom - height) / 2;
        panel_rect = {left, top, left + width, top + height};
        const int padding = S(34);
        name_rect = {left + padding, top + S(105), left + width - padding, top + S(140)};
        type_rect = {left + padding, top + S(179), left + width / 2 - S(12), top + S(214)};
        group_rect = {left + width / 2 + S(12), top + S(179), left + width - padding, top + S(214)};
        folder_rect = {left + padding, top + S(253), left + width - padding - S(48), top + S(288)};
        command_rect = {left + padding, top + S(327), left + width - padding, top + S(362)};
        note_rect = {left + padding, top + S(401), left + width - padding, top + S(436)};
        // Text fields keep their parent-painted one-pixel outline. Dropdowns own
        // both their fill and outline, so they occupy the complete field rectangle
        // instead of exposing a strip of modal background around the child HWND.
        auto placeEdit = [&](HWND control, RECT outer) {
            if (!control)
                return;
            aicli::ui::LayoutStyledInput(control, outer, S(1), S(5));
        };
        auto placeDropDown = [&](HWND control, RECT outer) {
            if (!control)
                return;
            const int w = outer.right - outer.left, h = outer.bottom - outer.top;
            MoveWindow(control, outer.left, outer.top, w, h, TRUE);
            ApplyRoundedRegion(control, w, h, S(12));
        };
        auto placeButton = [&](HWND control, RECT outer) {
            if (!control)
                return;
            MoveWindow(control, outer.left, outer.top, outer.right - outer.left,
                       outer.bottom - outer.top, TRUE);
            ApplyRoundedRegion(control, outer.right - outer.left, outer.bottom - outer.top, S(7));
        };
        placeEdit(this->name_control, name_rect);
        placeEdit(this->note_control, note_rect);
        placeDropDown(this->type_control, type_rect);
        placeDropDown(this->group_control, group_rect);
        placeEdit(this->folder_control, folder_rect);
        placeEdit(this->command_control, command_rect);
        RECT browse{folder_rect.right + S(10), folder_rect.top, panel_rect.right - padding,
                    folder_rect.bottom};
        placeButton(this->browse_control, browse);
        RECT cancel{left + width - padding - S(206), top + S(498), left + width - padding - S(106),
                    top + S(534)};
        RECT save{left + width - padding - S(96), top + S(498), left + width - padding,
                  top + S(534)};
        HWND child = GetWindow(h, GW_CHILD);
        while (child)
        {
            int id = GetDlgCtrlID(child);
            if (id == ID_D_OK)
            {
                MoveWindow(child, save.left, save.top, save.right - save.left,
                           save.bottom - save.top, TRUE);
                ApplyRoundedRegion(child, save.right - save.left, save.bottom - save.top, S(8));
            }
            if (id == ID_D_CANCEL)
            {
                MoveWindow(child, cancel.left, cancel.top, cancel.right - cancel.left,
                           cancel.bottom - cancel.top, TRUE);
                ApplyRoundedRegion(child, cancel.right - cancel.left, cancel.bottom - cancel.top,
                                   S(8));
            }
            child = GetWindow(child, GW_HWNDNEXT);
        }
        if (this->close_control)
        {
            MoveWindow(this->close_control, panel_rect.right - S(43), panel_rect.top + S(5), S(34),
                       S(34), TRUE);
            ApplyRoundedRegion(this->close_control, S(34), S(34), S(7));
        }
        // CreateRoundRectRgn receives an ellipse diameter, while the smooth
        // renderer below receives a true corner radius. Use the matching 28 px
        // diameter so the binary window region does not expose four unpainted
        // corner pixels outside the 14 px rendered curve.
        aicli::ui::SetModalSurfaceRegion(h, panel_rect, S(28));
        InvalidateRect(h, nullptr, FALSE);
    }
    void InvalidateProjectControlFrame(HWND dialog, HWND control)
    {
        if (!dialog || !control)
            return;
        RECT frame{};
        if (control == this->name_control)
            frame = name_rect;
        else if (control == this->note_control)
            frame = note_rect;
        else if (control == this->type_control)
            frame = type_rect;
        else if (control == this->group_control)
            frame = group_rect;
        else if (control == this->folder_control)
            frame = folder_rect;
        else if (control == this->command_control)
            frame = command_rect;
        else
        {
            GetWindowRect(control, &frame);
            MapWindowPoints(HWND_DESKTOP, dialog, reinterpret_cast<POINT *>(&frame), 2);
        }
        InflateRect(&frame, 2, 2);
        InvalidateRect(dialog, &frame, FALSE);
    }

    // The shared ModalHost surface.  Keeping this renderer palette-driven means
    // a live theme switch changes the dialog without rebuilding its controls.
    void PaintProjectDialogSurface(HDC dc, RECT client)
    {
        // The layered ModalWorkspaceScope scrim is behind this content window.
        // Do not paint an opaque backdrop here; doing so would hide the live
        // terminal/workspace and defeat modal translucency.
        (void)client;
        const auto S = [this](int value) { return Scale(value); };
        // The modal surface suppresses WM_ERASEBKGND. Seed every pixel admitted by
        // the rounded window region with the palette().panel colour before anti-aliasing the
        // visible edge, otherwise the extreme corner samples can retain the
        // window's initial white pixels.
        HBRUSH panel_background = CreateSolidBrush(palette().panel);
        FillRect(dc, &panel_rect, panel_background);
        DeleteObject(panel_background);
        DrawSmoothRoundedRect(dc, panel_rect, ScaleF(14.0f), palette().panel, palette().border,
                              ScaleF(1.0f));
        SetBkMode(dc, TRANSPARENT);
        SelectObject(dc, fonts().title ? fonts().title : fonts().medium);
        SetTextColor(dc, palette().text);
        RECT title{panel_rect.left + S(24), panel_rect.top, panel_rect.right - S(58),
                   panel_rect.top + S(44)};
        DrawTextW(dc, editing ? L"编辑项目" : L"新建项目", -1, &title,
                  DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        SelectObject(dc, fonts().subtitle ? fonts().subtitle : fonts().hint);
        SetTextColor(dc, palette().muted);
        RECT subtitle{panel_rect.left + S(34), panel_rect.top + S(53), panel_rect.right - S(34),
                      panel_rect.top + S(75)};
        DrawTextW(dc, L"配置项目信息与 AI CLI 启动环境", -1, &subtitle,
                  DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        SetTextColor(dc, palette().muted);
        auto label = [&](const wchar_t *value, int y, int x = -1) {
            if (x < 0)
                x = panel_rect.left + S(34);
            RECT rect{x, y, x + S(250), y + S(22)};
            DrawTextW(dc, value, -1, &rect, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        };
        label(L"项目名称 / Tab 标题", panel_rect.top + S(79));
        label(L"项目类型", panel_rect.top + S(153));
        label(L"分组", panel_rect.top + S(153),
              panel_rect.left + (panel_rect.right - panel_rect.left) / 2 + S(12));
        label(L"项目路径", panel_rect.top + S(227));
        label(L"启动命令", panel_rect.top + S(301));
        label(L"备注", panel_rect.top + S(375));
        HPEN separator = CreatePen(PS_SOLID, 1, palette().border);
        HGDIOBJ old_pen = SelectObject(dc, separator);
        MoveToEx(dc, panel_rect.left + S(1), panel_rect.top + S(44), nullptr);
        LineTo(dc, panel_rect.right - S(1), panel_rect.top + S(44));
        MoveToEx(dc, panel_rect.left + S(24), panel_rect.top + S(476), nullptr);
        LineTo(dc, panel_rect.right - S(24), panel_rect.top + S(476));
        SelectObject(dc, old_pen);
        DeleteObject(separator);
        auto outline = [&](RECT rect, HWND control) {
            const bool active = GetFocus() == control;
            DrawRoundedOutline(dc, rect, active ? palette().accent : palette().border, Scale(6));
        };
        outline(name_rect, this->name_control);
        outline(note_rect, this->note_control);
        // Type/group dropdowns paint a single combined fill + outline in their
        // owner-draw pass. Painting another outline underneath them caused the
        // visible unfilled rim shown around these two controls.
        outline(folder_rect, this->folder_control);
        outline(command_rect, this->command_control);
    }
    void ShowProjectDropdown(HWND dialog, HWND anchor, bool projectType)
    {
        std::vector<std::wstring> options;
        if (projectType)
        {
            for (const auto &type : context.types)
                options.push_back(type.name);
        }
        else
        {
            for (const auto &group : context.groups)
                options.push_back(group.name);
            if (!draft.group.empty() &&
                std::find(options.begin(), options.end(), draft.group) == options.end())
                options.push_back(draft.group);
        }
        wchar_t currentText[256]{};
        GetWindowTextW(anchor, currentText, 256);
        const std::wstring current = currentText;
        std::vector<aicli::ui::RoundedSelectEntry> entries;
        entries.reserve(options.size());
        const wchar_t *icon = projectType ? L"\xE756" : L"\xE8B7";
        for (const auto &option : options)
            entries.push_back({option, option, L"", icon, option == current, false});

        aicli::ui::RoundedSelectPopupOptions popup;
        popup.owner = dialog;
        popup.anchor = anchor;
        popup.palette = palette();
        popup.text_font = fonts().body;
        popup.detail_font = fonts().hint;
        popup.icon_font = fonts().icon;
        popup.dpi = std::max(96, static_cast<int>(GetDpiForWindow(dialog)));
        popup.maximum_visible_rows = 8;
        popup.match_anchor_width = true;
        popup.open_state_changed = [this, anchor](bool open) {
            open_dropdown = open ? anchor : nullptr;
            if (anchor)
                InvalidateRect(anchor, nullptr, FALSE);
        };
        SetFocus(anchor);
        const auto selected = aicli::ui::ShowRoundedSelectPopup(popup, std::move(entries));
        if (selected)
        {
            const std::wstring &value = *selected;
            wchar_t previousType[256]{};
            GetWindowTextW(anchor, previousType, 256);
            SetWindowTextW(anchor, value.c_str());
            if (projectType)
            {
                wchar_t command[1024]{};
                GetWindowTextW(this->command_control, command, 1024);
                const std::wstring currentCommand = command;
                const std::wstring previousDefault = DefaultProjectCommand(previousType);
                const bool useDefault = currentCommand.empty() || currentCommand == previousDefault;
                if (useDefault)
                    SetWindowTextW(this->command_control, DefaultProjectCommand(value).c_str());
            }
        }
        InvalidateRect(anchor, nullptr, FALSE);
    }
    LRESULT HandleMessage(HWND h, UINT m, WPARAM w, LPARAM l)
    {
        switch (m)
        {
        case WM_CREATE: {
            const std::wstring initialType = editing ? draft.type : DefaultProjectTypeName();
            this->name_control =
                aicli::ui::CreateStyledInput(h, ID_D_NAME, fonts().body, L"输入项目名称");
            this->note_control =
                aicli::ui::CreateStyledInput(h, ID_D_NOTE, fonts().body, L"输入项目备注（可选）");
            this->type_control = MakeDialogControl(
                L"BUTTON", initialType.c_str(), BS_NOTIFY | WS_TABSTOP, ID_D_TYPE, h, 0, 0, 1, 1);
            const std::wstring initialGroup =
                draft.group.empty() ? ((context.selected_group() == kAllGroupsName ||
                                        context.selected_group() == kStartedProjectsName)
                                           ? context.default_group()
                                           : context.selected_group())
                                    : draft.group;
            this->group_control = MakeDialogControl(
                L"BUTTON", initialGroup.c_str(), BS_NOTIFY | WS_TABSTOP, ID_D_GROUP, h, 0, 0, 1, 1);
            this->folder_control =
                aicli::ui::CreateStyledInput(h, ID_D_FOLDER, fonts().body, L"输入或选择项目文件夹");
            this->browse_control = MakeDialogControl(L"BUTTON", L"…", BS_PUSHBUTTON | WS_TABSTOP,
                                                     ID_D_BROWSE, h, 0, 0, 1, 1);
            this->command_control =
                aicli::ui::CreateStyledInput(h, ID_D_COMMAND, fonts().body, L"输入启动命令");
            for (HWND field : {this->name_control, this->note_control, this->folder_control,
                               this->command_control})
                aicli::ui::SetStyledInputTheme(field, palette());
            MakeDialogControl(L"BUTTON", L"确定", BS_DEFPUSHBUTTON | WS_TABSTOP, ID_D_OK, h, 0, 0,
                              1, 1);
            MakeDialogControl(L"BUTTON", L"取消", BS_PUSHBUTTON | WS_TABSTOP, ID_D_CANCEL, h, 0, 0,
                              1, 1);
            this->close_control =
                MakeDialogControl(L"BUTTON", L"×", BS_PUSHBUTTON, ID_D_CLOSE, h, 0, 0, 1, 1);
            if (editing)
            {
                SetWindowTextW(this->type_control, draft.type.c_str());
                aicli::ui::SetStyledInputText(this->name_control, draft.name);
                aicli::ui::SetStyledInputText(this->note_control, draft.note);
                aicli::ui::SetStyledInputText(this->folder_control, draft.folder);
                aicli::ui::SetStyledInputText(this->command_control, draft.command);
            }
            else
            {
                aicli::ui::SetStyledInputText(this->command_control,
                                              DefaultProjectCommand(initialType));
            }
            LayoutProjectDialog(h);
            SetFocus(this->name_control);
            return 0;
        }
        case WM_SIZE:
            LayoutProjectDialog(h);
            return 0;
        case WM_DPICHANGED:
            RefreshFonts();
            LayoutProjectDialog(h);
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            RECT client{};
            GetClientRect(h, &client);
            {
                aicli::ui::BufferedWindowPaint buffered(dc, panel_rect, ps.rcPaint);
                PaintProjectDialogSurface(buffered.dc(), client);
            }
            EndPaint(h, &ps);
            return 0;
        }
        case WM_NCHITTEST:
            return HTCLIENT;
        case WM_CTLCOLORSTATIC: {
            HDC dc = (HDC)w;
            HWND control = (HWND)l;
            if (control == this->folder_control)
            {
                const bool active = GetFocus() == control;
                const COLORREF color = active ? palette().field_focus : palette().field;
                SetTextColor(dc, active ? palette().text : palette().list_text);
                SetBkColor(dc, color);
                SetBkMode(dc, OPAQUE);
                return (LRESULT)(active ? focus_brush : field_brush);
            }
            SetTextColor(dc, palette().text);
            SetBkMode(dc, TRANSPARENT);
            return (LRESULT)GetStockObject(NULL_BRUSH);
        }
        case WM_CTLCOLOREDIT: {
            HDC dc = (HDC)w;
            HWND control = (HWND)l;
            const bool active = GetFocus() == control;
            const COLORREF color = active ? palette().field_focus : palette().field;
            SetTextColor(dc, palette().text);
            SetBkColor(dc, color);
            return (LRESULT)(active ? focus_brush : field_brush);
        }
        case WM_CTLCOLORLISTBOX: {
            HDC dc = (HDC)w;
            SetTextColor(dc, palette().text);
            SetBkColor(dc, palette().list_surface);
            return (LRESULT)list_brush;
        }
        case WM_DRAWITEM: {
            auto *item = (DRAWITEMSTRUCT *)l;
            if (item->CtlType == ODT_BUTTON)
            {
                DrawProjectButton(item);
                return TRUE;
            }
        }
        break;
        case WM_COMMAND:
            if ((HIWORD(w) == EN_SETFOCUS || HIWORD(w) == EN_KILLFOCUS ||
                 HIWORD(w) == BN_SETFOCUS || HIWORD(w) == BN_KILLFOCUS) &&
                (LOWORD(w) == ID_D_NAME || LOWORD(w) == ID_D_NOTE || LOWORD(w) == ID_D_TYPE ||
                 LOWORD(w) == ID_D_GROUP || LOWORD(w) == ID_D_FOLDER || LOWORD(w) == ID_D_COMMAND))
            {
                InvalidateProjectControlFrame(h, (HWND)l);
            }
            if (LOWORD(w) == ID_D_TYPE && HIWORD(w) == BN_CLICKED)
            {
                ShowProjectDropdown(h, this->type_control, true);
                return 0;
            }
            if (LOWORD(w) == ID_D_GROUP && HIWORD(w) == BN_CLICKED)
            {
                ShowProjectDropdown(h, this->group_control, false);
                return 0;
            }
            if (LOWORD(w) == ID_D_BROWSE)
            {
                std::wstring selected;
                if (PickFolder(h, selected))
                {
                    aicli::ui::SetStyledInputText(this->folder_control, selected);
                    if (aicli::ui::GetStyledInputText(this->name_control).empty())
                    {
                        std::filesystem::path folder(selected);
                        while (folder.filename().empty() && folder.has_parent_path())
                            folder = folder.parent_path();
                        const std::wstring folderName = folder.filename().wstring();
                        if (!folderName.empty())
                            aicli::ui::SetStyledInputText(this->name_control, folderName);
                    }
                    InvalidateProjectControlFrame(h, this->folder_control);
                    InvalidateProjectControlFrame(h, this->name_control);
                }
                return 0;
            }
            if (LOWORD(w) == ID_D_CANCEL || LOWORD(w) == ID_D_CLOSE)
            {
                aicli::ui::CloseModalSurface(h);
                return 0;
            }
            if (LOWORD(w) == ID_D_OK)
            {
                draft.name = aicli::ui::GetStyledInputText(this->name_control);
                draft.note = aicli::ui::GetStyledInputText(this->note_control);
                draft.folder = aicli::ui::GetStyledInputText(this->folder_control);
                draft.command = aicli::ui::GetStyledInputText(this->command_control);
                wchar_t type[256]{};
                GetWindowTextW(this->type_control, type, 256);
                draft.type = type;
                wchar_t group[256]{};
                GetWindowTextW(this->group_control, group, 256);
                draft.group = group;
                if (draft.name.empty())
                {
                    MessageBoxW(h, L"项目名称不能为空。", L"项目详情", MB_OK | MB_ICONWARNING);
                    return 0;
                }
                std::error_code folderError;
                if (draft.folder.empty() ||
                    !std::filesystem::is_directory(draft.folder, folderError))
                {
                    MessageBoxW(h, L"请选择一个存在的项目文件夹。", L"项目详情",
                                MB_OK | MB_ICONWARNING);
                    return 0;
                }
                if (!editing && ProjectFolderAlreadyUsed(draft.folder) &&
                    MessageBoxW(h, L"该文件夹已经创建项目，是否确认添加？", L"确认添加项目",
                                MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
                    return 0;
                if (draft.command.empty())
                    draft.command = DefaultProjectCommand(draft.type);
                const bool knownType = std::any_of(
                    context.types.begin(), context.types.end(), [&](const ProjectType &value) {
                        return _wcsicmp(value.name.c_str(), draft.type.c_str()) == 0;
                    });
                const bool knownGroup = std::any_of(
                    context.groups.begin(), context.groups.end(), [&](const ProjectGroup &value) {
                        return _wcsicmp(value.name.c_str(), draft.group.c_str()) == 0;
                    });
                if (draft.type.empty() || !knownType || draft.group.empty() || !knownGroup)
                {
                    MessageBoxW(h, L"请选择有效的项目类型和分组。", L"项目详情",
                                MB_OK | MB_ICONWARNING);
                    return 0;
                }
                accepted = true;
                aicli::ui::CloseModalSurface(h);
                return 0;
            }
            break;
        case WM_KEYDOWN:
            if (w == VK_ESCAPE)
            {
                aicli::ui::CloseModalSurface(h);
                return 0;
            }
            break;
        case WM_DESTROY:
            open_dropdown = nullptr;
            window = nullptr;
            this->name_control = this->note_control = this->type_control = this->group_control =
                this->folder_control = this->browse_control = this->command_control =
                    this->close_control = nullptr;
            return 0;
        case WM_CLOSE:
            aicli::ui::CloseModalSurface(h);
            return 0;
        }
        return DefWindowProcW(h, m, w, l);
    }
    bool Show(HWND owner, const Project *initial)
    {
        static bool registered = false;
        if (!registered)
        {
            WNDCLASSW wc{};
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpfnWndProc = WindowProc;
            wc.lpszClassName = L"AicliProjectDialog";
            wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
            wc.hbrBackground = nullptr;
            wc.style = CS_DBLCLKS;
            RegisterClassW(&wc);
            registered = true;
        }
        if (IsWindow(window))
            return false;
        RefreshTheme();
        accepted = false;
        editing = initial != nullptr;
        draft = initial ? *initial : Project{};
        aicli::ui::ModalWorkspaceScope workspace(owner, aicli::ui::ModalTopForOwner(owner),
                                                 &palette());
        window = CreateWindowExW(WS_EX_CONTROLPARENT | WS_EX_TOOLWINDOW, L"AicliProjectDialog", L"",
                                 WS_POPUP | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, 1, 1, owner,
                                 nullptr, GetModuleHandleW(nullptr), this);
        if (!window)
        {
            window = nullptr;
            context.layout_owner();
            return false;
        }
        workspace.AttachSurface(window);
        aicli::ui::ActivateModalSurface(window, this->name_control);
        MSG msg{};
        while (IsWindow(window) && GetMessageW(&msg, nullptr, 0, 0))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        window = nullptr;
        aicli::ui::RestoreModalOwner(owner);
        return accepted;
    }
};

ProjectEditorDialog::ProjectEditorDialog(ProjectEditorOptions options)
    : impl_(std::make_unique<Impl>(std::move(options)))
{
}
ProjectEditorDialog::~ProjectEditorDialog() = default;
bool ProjectEditorDialog::Show(HWND owner, Project &result, const Project *initial)
{
    if (!impl_->Show(owner, initial))
        return false;
    result = impl_->draft;
    return true;
}
HWND ProjectEditorDialog::window() const noexcept
{
    return impl_->window;
}
void ProjectEditorDialog::RefreshFonts()
{
    if (impl_->window)
        impl_->RefreshFonts();
}
void ProjectEditorDialog::RefreshTheme()
{
    if (impl_->window)
        impl_->RefreshTheme();
}

} // namespace aicli::ui
