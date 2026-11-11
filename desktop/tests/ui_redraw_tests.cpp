#include "ui/modal_host.h"
#include "ui/redraw_scope.h"
#include "ui/styled_input.h"
#include "ui/drawing_utils.h"
#include <iostream>
#include <stdexcept>

namespace {
void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
HWND owner{};
bool changing_page{};
unsigned owner_redraw_toggles{}, partial_page_paints{}, completed_paints{};
HBRUSH background{};

LRESULT CALLBACK TestWindow(HWND window, UINT message, WPARAM w, LPARAM l) {
    if (message == WM_SETREDRAW && window == owner) ++owner_redraw_toggles;
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_CTLCOLOREDIT) {
        SetBkColor(reinterpret_cast<HDC>(w), RGB(32, 36, 43));
        SetTextColor(reinterpret_cast<HDC>(w), RGB(240, 240, 240));
        return reinterpret_cast<LRESULT>(background);
    }
    if (message == WM_PAINT) {
        if (changing_page) ++partial_page_paints;
        else ++completed_paints;
        PAINTSTRUCT paint{};
        auto dc = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        FillRect(dc, &client, background);
        EndPaint(window, &paint);
        return 0;
    }
    return DefWindowProcW(window, message, w, l);
}
HWND Create(HWND parent = nullptr, bool child = false) {
    auto window = CreateWindowExW(WS_EX_TOOLWINDOW, L"AiCliRedrawRegression", L"",
        (child ? WS_CHILD | WS_VISIBLE : WS_POPUP) | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        child ? 0 : -30000, child ? 0 : -30000, 640, 480, parent, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    Require(window != nullptr, "create test window");
    return window;
}
unsigned VisibleScrims() {
    unsigned count{};
    EnumThreadWindows(GetCurrentThreadId(), [](HWND window, LPARAM context) -> BOOL {
        wchar_t name[64]{};
        GetClassNameW(window, name, 64);
        if (wcscmp(name, L"AicliModalScrim") == 0 && IsWindowVisible(window))
            ++*reinterpret_cast<unsigned*>(context);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&count));
    return count;
}
void PaintQueued(HWND root) {
    RedrawWindow(root, nullptr, nullptr,
        RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW | RDW_NOERASE);
}
void CheckPanelBuffer() {
    const auto screen = GetDC(nullptr);
    const auto target = CreateCompatibleDC(screen);
    const auto bitmap = CreateCompatibleBitmap(screen, 120, 100);
    ReleaseDC(nullptr, screen);
    const auto previous = SelectObject(target, bitmap);
    const RECT whole{0, 0, 120, 100};
    FillRect(target, &whole, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    const RECT panel{25, 30, 95, 80};
    const RECT dirty{40, 45, 80, 70};
    {
        aicli::ui::BufferedWindowPaint buffer(target, panel, dirty);
        FillRect(buffer.dc(), &panel, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        SetPixel(buffer.dc(), 60, 55, RGB(255, 0, 0));
    }
    const bool coordinates = GetPixel(target, 60, 55) == RGB(255, 0, 0) &&
        GetPixel(target, 79, 69) == RGB(0, 0, 0);
    const bool clipping = GetPixel(target, 30, 35) == RGB(255, 255, 255) &&
        GetPixel(target, 100, 90) == RGB(255, 255, 255);
    SelectObject(target, previous);
    DeleteObject(bitmap);
    DeleteDC(target);
    Require(coordinates && clipping, "panel buffer preserves coordinates and dirty-region clipping");
}
}

// In-process HWND regression tests. All windows stay off-screen; this does
// not inspect, activate or send input to a user's running application.
int main() {
    try {
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
        InitCommonControlsEx(&controls);
        aicli::ui::GdiplusRuntime graphics;
        CheckPanelBuffer();
        background = CreateSolidBrush(RGB(32, 36, 43));
        WNDCLASSW klass{};
        klass.hInstance = GetModuleHandleW(nullptr);
        klass.lpszClassName = L"AiCliRedrawRegression";
        klass.lpfnWndProc = TestWindow;
        Require(RegisterClassW(&klass) != 0, "register test class");
        owner = Create();
        ShowWindow(owner, SW_SHOWNOACTIVATE);
        PaintQueued(owner);
        const auto original_style = GetWindowLongPtrW(owner, GWL_EXSTYLE);
        for (int iteration = 0; iteration < 30; ++iteration) {
            const auto palette = aicli::ui::MakeThemePalette(iteration % 2 != 0);
            {
                aicli::ui::ModalWorkspaceScope modal(owner, 0, &palette);
                const auto form = Create(owner);
                const auto pane = Create(form, true);
                const auto field = aicli::ui::CreateStyledInput(pane, 1, nullptr, L"项目名称");
                aicli::ui::SetStyledInputTheme(field, palette);
                aicli::ui::LayoutStyledInput(field, RECT{20, 20, 420, 60});
                modal.AttachSurface(form);
                aicli::ui::ActivateModalSurface(form, field);
                Require(IsWindowVisible(form) && IsWindowVisible(field), "modal first presentation includes fields");
                Require(VisibleScrims() == 1, "one visible backdrop");
                Require(GetFocus() == field, "initial edit focus preserved");
                for (int page = 0; page < 6; ++page) {
                    changing_page = true;
                    {
                        aicli::ui::ChildRedrawScope transaction(pane);
                        ShowWindow(field, SW_HIDE);
                        aicli::ui::SetStyledInputText(field, L"中文项目 C:\\work\\example");
                        aicli::ui::LayoutStyledInput(field, RECT{20, 40 + page * 5, 420, 80 + page * 5});
                        {
                            aicli::ui::ChildRedrawScope nested(pane);
                            ShowWindow(field, SW_SHOWNOACTIVATE);
                        }
                        Require(!IsWindowVisible(pane), "nested redraw must not publish incomplete page");
                    }
                    changing_page = false;
                    PaintQueued(form);
                    Require(IsWindowVisible(field), "page switch preserves requested field visibility");
                    Require(aicli::ui::GetStyledInputText(field) == L"中文项目 C:\\work\\example", "edit text preserved");
                }
                ShowWindow(field, SW_HIDE);
                { aicli::ui::ChildRedrawScope hidden(field); }
                Require(!IsWindowVisible(field), "redraw guard must not reveal hidden controls");
                {
                    aicli::ui::ModalWorkspaceScope nested(form, 0, &palette);
                    const auto group = Create(form);
                    nested.AttachSurface(group);
                    aicli::ui::ActivateModalSurface(group);
                    Require(VisibleScrims() == 2, "nested modal backdrop");
                    aicli::ui::CloseModalSurface(group);
                    Require(VisibleScrims() == 1 && IsWindowVisible(form), "nested close retains outer form without an extra backdrop frame");
                }
                aicli::ui::CloseModalSurface(form);
                Require(!IsWindow(form) && VisibleScrims() == 0, "form and backdrop dismissed together before scope unwinds");
            }
            Require(IsWindowVisible(owner), "owner remains visible after modal dismissal");
            Require(!aicli::ui::HasAnyActiveModal(), "modal bookkeeping released");
        }
        Require(owner_redraw_toggles == 0, "modal close must never suspend the workspace or terminal");
        Require(GetWindowLongPtrW(owner, GWL_EXSTYLE) == original_style, "workspace rendering styles preserved");
        Require(partial_page_paints == 0, "no parent paint while page visibility and layout are incomplete");
        Require(completed_paints > 0, "final frames actually painted");
        DestroyWindow(owner);
        DeleteObject(background);
        std::cout << "UI redraw tests passed: 30 modal cycles, 180 page changes, nested dialogs, both themes.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        if (owner) DestroyWindow(owner);
        if (background) DeleteObject(background);
        return 1;
    }
}
