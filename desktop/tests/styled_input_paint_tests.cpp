#include "ui/styled_input.h"
#include "ui/drawing_utils.h"
#include <commctrl.h>
#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace {
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
LRESULT CALLBACK ParentProc(HWND window, UINT message, WPARAM w, LPARAM l) {
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_CTLCOLOREDIT || message == WM_CTLCOLORSTATIC) {
        // Reproduce stale/default parent DC colors. A themed edit must remain
        // legible even when its host does not supply a matching foreground.
        SetTextColor(reinterpret_cast<HDC>(w), RGB(37, 40, 47));
        SetBkColor(reinterpret_cast<HDC>(w), RGB(37, 40, 47));
        return reinterpret_cast<LRESULT>(GetStockObject(BLACK_BRUSH));
    }
    return DefWindowProcW(window, message, w, l);
}
struct Image {
    HDC dc{};
    HBITMAP bitmap{};
    HGDIOBJ previous{};
    RECT client{};
    explicit Image(HWND edit) {
        GetClientRect(edit, &client);
        const auto screen = GetDC(nullptr);
        dc = CreateCompatibleDC(screen);
        bitmap = CreateCompatibleBitmap(screen, client.right, client.bottom);
        ReleaseDC(nullptr, screen);
        Require(dc && bitmap, "create render bitmap");
        previous = SelectObject(dc, bitmap);
        FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        SetTextColor(dc, RGB(255, 0, 255));
        SetBkColor(dc, RGB(0, 255, 0));
        SetBkMode(dc, TRANSPARENT);
        SendMessageW(edit, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT);
    }
    ~Image() { SelectObject(dc, previous); DeleteObject(bitmap); DeleteDC(dc); }
    unsigned PixelsNear(COLORREF color) const {
        unsigned count{};
        for (int y = 5; y < client.bottom - 5; ++y) {
            for (int x = 8; x < client.right - 8; ++x) {
                const auto value = GetPixel(dc, x, y);
                if (std::abs(int(GetRValue(value)) - int(GetRValue(color))) < 8 &&
                    std::abs(int(GetGValue(value)) - int(GetGValue(color))) < 8 &&
                    std::abs(int(GetBValue(value)) - int(GetBValue(color))) < 8) ++count;
            }
        }
        return count;
    }
};
void CheckText(HWND edit, const aicli::ui::ThemePalette& palette, bool focused) {
    Image image(edit);
    Require(GetPixel(image.dc, image.client.right / 2, 2) ==
        (focused ? palette.field_focus : palette.field), "edit uses its own themed background");
    Require(image.PixelsNear(palette.text) > 40, "actual text pixels remain visible in theme foreground");
    Require(GetTextColor(image.dc) == RGB(255, 0, 255) &&
        GetBkColor(image.dc) == RGB(0, 255, 0) && GetBkMode(image.dc) == TRANSPARENT,
        "printing must not leak edit/placeholder colors into the caller DC");
}
}

// Render real native EDIT controls into bitmaps; validate glyph pixels rather
// than just checking that the text remains stored in the database/control.
int main() {
    HWND parent{};
    HFONT font{};
    try {
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
        InitCommonControlsEx(&controls);
        aicli::ui::GdiplusRuntime gdiplus;
        WNDCLASSW klass{};
        klass.hInstance = GetModuleHandleW(nullptr);
        klass.lpszClassName = L"AiCliInputPaintTest";
        klass.lpfnWndProc = ParentProc;
        Require(RegisterClassW(&klass) != 0, "register test parent");
        font = CreateFontW(-18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        for (bool composited : {false, true}) {
            parent = CreateWindowExW(composited ? WS_EX_COMPOSITED : 0,
                klass.lpszClassName, L"", WS_POPUP | WS_CLIPCHILDREN,
                -30000, -30000, 500, 160, nullptr, nullptr, klass.hInstance, nullptr);
            Require(parent != nullptr, "create parent");
            ShowWindow(parent, SW_SHOWNOACTIVATE);
            for (bool read_only : {false, true}) {
                const auto edit = aicli::ui::CreateStyledInput(parent, 1, font, L"Placeholder MMMWWW", read_only);
                aicli::ui::LayoutStyledInput(edit, RECT{10, 10, 440, 54});
                aicli::ui::SetStyledInputText(edit, L"MMM WWW ABC 123 中文输入");
                for (bool light : {false, true, false}) {
                    const auto palette = aicli::ui::MakeThemePalette(light);
                    aicli::ui::SetStyledInputTheme(edit, palette);
                    SetFocus(parent);
                    CheckText(edit, palette, false);
                    SetFocus(edit);
                    CheckText(edit, palette, true);
                    if (!read_only) {
                        SendMessageW(edit, EM_SETSEL, static_cast<WPARAM>(-1), -1);
                        for (wchar_t character : std::wstring(L" 新文字 ABC"))
                            SendMessageW(edit, WM_CHAR, character, 1);
                        CheckText(edit, palette, true);
                        SendMessageW(edit, EM_SETSEL, 0, 3);
                        SendMessageW(edit, EM_REPLACESEL, TRUE,
                            reinterpret_cast<LPARAM>(L"替换 MMM"));
                        CheckText(edit, palette, true);
                        SendMessageW(edit, WM_CHAR, VK_BACK, 1);
                        CheckText(edit, palette, true);
                    }
                    const auto saved = aicli::ui::GetStyledInputText(edit);
                    aicli::ui::SetStyledInputText(edit, L"");
                    { Image placeholder(edit);
                      Require(placeholder.PixelsNear(palette.muted) > 40, "placeholder remains visible"); }
                    aicli::ui::SetStyledInputText(edit, saved);
                    CheckText(edit, palette, true);
                    SendMessageW(edit, EM_SETSEL, 0, -1);
                    { Image selected(edit);
                      Require(selected.PixelsNear(GetSysColor(COLOR_HIGHLIGHTTEXT)) > 40,
                          "selected text retains native contrasting highlight foreground"); }
                    SendMessageW(edit, EM_SETSEL, static_cast<WPARAM>(-1), -1);
                    SetFocus(parent);
                    CheckText(edit, palette, false);
                }
                DestroyWindow(edit);
            }
            DestroyWindow(parent);
            parent = nullptr;
        }
        DeleteObject(font);
        std::cout << "Styled input glyph-color tests passed: typing, replacement, selection, readonly, theme and focus changes.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        if (parent) DestroyWindow(parent);
        if (font) DeleteObject(font);
        return 1;
    }
}
