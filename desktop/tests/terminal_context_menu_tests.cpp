#include "app/runtime_components.h"
#include "core/text_utils.h"
#include "terminal/terminal_context_menu.h"
#include "terminal/xaml_island_adapter.h"
#include "ui/drawing_utils.h"

#include <windowsx.h>
#include <ole2.h>
#include <appmodel.h>
#include <shobjidl.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
aicli::terminal::XamlIslandAdapter* adapter{};
int menu_row{-1};
bool cancel_menu{}, switch_project{};
unsigned menu_count{};
std::wstring observed_input;
constexpr UINT_PTR kMenuTestTimer = 12;

void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

LRESULT CALLBACK OwnerProc(HWND owner, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_TIMER && wparam == kMenuTestTimer) {
        HWND popup{};
        EnumThreadWindows(GetCurrentThreadId(), [](HWND window, LPARAM value) -> BOOL {
            wchar_t name[64]{};
            GetClassNameW(window, name, 64);
            if (std::wstring_view(name) == L"AicliRoundedContextMenu") {
                *reinterpret_cast<HWND*>(value) = window;
                return FALSE;
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&popup));
        if (!popup) return 0;
        KillTimer(owner, kMenuTestTimer);
        ++menu_count;
        if (switch_project) adapter->Activate(L"second");
        if (cancel_menu) PostMessageW(popup, WM_KEYDOWN, VK_ESCAPE, 0);
        else {
            const int dpi = static_cast<int>(GetDpiForWindow(owner));
            const int y = MulDiv(6 + 36 * menu_row + 18 + (menu_row >= 2 ? 9 : 0), dpi, 96);
            PostMessageW(popup, WM_LBUTTONUP, 0, MAKELPARAM(MulDiv(40, dpi, 96), y));
        }
        return 0;
    }
    if (message == WM_CLOSE) { DestroyWindow(owner); return 0; }
    if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(owner, message, wparam, lparam);
}

template<class Condition> void Until(Condition condition) {
    const auto deadline = GetTickCount64() + 10000;
    while (!condition() && GetTickCount64() < deadline) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (adapter->PreTranslateMessage(message)) continue;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
    }
    Require(condition(), "terminal output did not become available");
}

void OpenTestClipboard(HWND owner) {
    const auto deadline = GetTickCount64() + 2000;
    while (!OpenClipboard(owner)) {
        Require(GetTickCount64() < deadline, "test clipboard stayed busy");
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
    }
}
void ClipboardText(HWND owner, std::wstring_view value) {
    OpenTestClipboard(owner);
    EmptyClipboard();
    const auto memory = GlobalAlloc(GMEM_MOVEABLE, (value.size() + 1) * sizeof(wchar_t));
    auto* text = static_cast<wchar_t*>(GlobalLock(memory));
    std::copy(value.begin(), value.end(), text);
    text[value.size()] = L'\0';
    GlobalUnlock(memory);
    SetClipboardData(CF_UNICODETEXT, memory);
    CloseClipboard();
}
std::wstring ClipboardText(HWND owner) {
    OpenTestClipboard(owner);
    const auto memory = GetClipboardData(CF_UNICODETEXT);
    const auto* text = memory ? static_cast<const wchar_t*>(GlobalLock(memory)) : nullptr;
    const std::wstring value = text ? text : L"";
    if (text) GlobalUnlock(memory);
    CloseClipboard();
    return value;
}

void Menu(HWND owner, int row, bool cancel = false, bool keyboard = false, HWND target = nullptr) {
    menu_row = row;
    cancel_menu = cancel;
    const auto count = menu_count;
    SetTimer(owner, kMenuTestTimer, 10, nullptr);
    MSG message{};
    message.hwnd = target ? target : adapter->Window();
    message.message = keyboard ? WM_KEYDOWN : WM_RBUTTONUP;
    message.wParam = keyboard ? VK_APPS : 0;
    message.lParam = keyboard ? 0 : MAKELPARAM(60, 70);
    Require(adapter->PreTranslateMessage(message), "terminal handles context menu input");
    KillTimer(owner, kMenuTestTimer);
    Require(menu_count == count + 1, "one right-click or menu key opens exactly one popup");
}

void TestMenu(HWND owner) {
    using namespace aicli::terminal;
    for (bool selection : {false, true}) for (bool paste : {false, true}) {
        const auto entries = TerminalContextMenuEntries(selection, paste);
        Require(entries.size() == 4 && entries[0].enabled == selection && entries[1].enabled == paste &&
            entries[2].enabled && entries[3].enabled, "menu availability follows selection and clipboard");
    }
    ClipboardText(owner, L"clipboard sentinel");
    const auto before_menu = observed_input;
    Menu(owner, 0);
    Require(ClipboardText(owner) == L"clipboard sentinel", "disabled copy does not change clipboard");
    Require(observed_input.find(L"clipboard sentinel", before_menu.size()) == std::wstring::npos,
        "right-click does not paste clipboard text into the terminal");
    Menu(owner, 2);
    Menu(owner, 0);
    const auto copied = ClipboardText(owner);
    Require(copied.find(L"AICLI_MENU_LINE_ONE") != std::wstring::npos &&
        copied.find(L"复制第二行") != std::wstring::npos && copied.find(L'\n') != std::wstring::npos,
        "selection copy preserves Unicode and multiple lines");

    const HWND deeper = CreateWindowW(L"STATIC", L"", WS_CHILD, 0, 0, 1, 1,
        adapter->Window(), nullptr, GetModuleHandleW(nullptr), nullptr);
    Require(deeper != nullptr, "create deeper terminal input target");
    ClipboardText(owner, L"cancel sentinel");
    Menu(owner, 0, true, false, deeper);
    Require(ClipboardText(owner) == L"cancel sentinel", "cancelled deeper-window menu has no clipboard side effect");
    DestroyWindow(deeper);
    Menu(owner, 3, false, true);
    Require(ClipboardText(owner).find(L"AICLI_MENU_LINE_ONE") != std::wstring::npos,
        "keyboard context menu can copy the full terminal buffer");
    ClipboardText(owner, L"AICLI_PASTE_CHECK");
    observed_input.clear();
    Menu(owner, 1);
    Require(observed_input.find(L"AICLI_PASTE_CHECK") != std::wstring::npos &&
        observed_input.find(L'\r') == std::wstring::npos && observed_input.find(L'\n') == std::wstring::npos,
        "menu paste uses terminal input without submitting a command");

    ClipboardText(owner, L"stale session sentinel");
    switch_project = true;
    Menu(owner, 3);
    switch_project = false;
    Require(ClipboardText(owner) == L"stale session sentinel", "session switch invalidates pending menu action");
    MSG unrelated{};
    unrelated.hwnd = owner;
    unrelated.message = WM_RBUTTONUP;
    Require(!adapter->PreTranslateMessage(unrelated), "terminal focus does not swallow right-clicks outside terminal");
    std::cout << "Real TermControl context menu: Unicode selection copy, full buffer copy, disabled copy, cancellation, keyboard, deeper HWND and session switching passed\n";
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    const bool interactive = argc > 1 && std::wstring_view(argv[1]) == L"--interactive";
    wchar_t executable[32768]{};
    GetModuleFileNameW(nullptr, executable, 32768);
    const auto directory = std::filesystem::path(executable).parent_path();
    UINT32 identity_size{};
    if (GetCurrentPackageFullName(&identity_size, nullptr) == APPMODEL_ERROR_NO_PACKAGE) {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        winrt::com_ptr<IApplicationActivationManager> activation;
        HRESULT result = CoCreateInstance(CLSID_ApplicationActivationManager, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(activation.put()));
        DWORD pid{};
        if (SUCCEEDED(result)) result = activation->ActivateApplication(
            L"AiCli.Desktop.MenuProbe_jw3zr00rmb0ym!App", interactive ? L"--interactive" : nullptr,
            AO_NOERRORUI, &pid);
        if (FAILED(result)) {
            std::cerr << "Register the isolated MenuProbe manifest before running this test\n";
            return 1;
        }
        const HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!process) return 1;
        const DWORD wait = WaitForSingleObject(process, interactive ? INFINITE : 60000);
        DWORD code{1};
        if (wait == WAIT_OBJECT_0) GetExitCodeProcess(process, &code);
        CloseHandle(process);
        if (!interactive) {
            std::ifstream report(directory / L"terminal-menu-result.txt");
            std::cout << report.rdbuf();
        }
        return static_cast<int>(code);
    }
    ShowWindow(GetConsoleWindow(), SW_HIDE);
    std::ofstream report(directory / L"terminal-menu-result.txt");
    struct Redirect {
        std::streambuf* out;
        std::streambuf* err;
        ~Redirect() { std::cout.rdbuf(out); std::cerr.rdbuf(err); }
    } redirect{std::cout.rdbuf(report.rdbuf()), std::cerr.rdbuf(report.rdbuf())};
    std::cout << std::unitbuf;
    HWND owner{};
    winrt::com_ptr<IDataObject> clipboard_backup;
    try {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        aicli::ui::GdiplusRuntime drawing;
        aicli::app::WinRtComponentRegistry components;
        // Match the native app: local DLL activation remains available when
        // the optional RoRegisterActivationFactories path is unavailable.
        (void)components.Initialize();
        std::cout << "Initialized isolated runtime\n";
        WNDCLASSW type{};
        type.lpfnWndProc = OwnerProc;
        type.hInstance = GetModuleHandleW(nullptr);
        type.lpszClassName = L"AiCliTerminalMenuTestOwner";
        RegisterClassW(&type);
        owner = CreateWindowW(type.lpszClassName, L"AiCli CLI 右键测试", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
            interactive ? 100 : -30000, interactive ? 100 : -30000, 920, 600,
            nullptr, nullptr, type.hInstance, nullptr);
        aicli::terminal::XamlIslandAdapter island;
        adapter = &island;
        RECT client{};
        GetClientRect(owner, &client);
        if (!island.Attach(owner, client)) throw std::runtime_error(
            "attach isolated terminal island: " + aicli::text::Utf8Text(island.LastError()));
        std::cout << "Attached isolated XAML island\n";
        const auto cwd = std::filesystem::temp_directory_path().wstring();
        Require(island.Start(L"first", L"Write-Output 'AICLI_MENU_LINE_ONE'; Write-Output '复制第二行'",
            cwd, L"右键测试", owner, nullptr, {}, {},
            [](std::wstring_view value){observed_input.append(value);}), "start isolated test shell");
        std::cout << "Started isolated test terminal\n";
        if (interactive) {
            MSG message{};
            while (GetMessageW(&message, nullptr, 0, 0) > 0) {
                if (island.PreTranslateMessage(message)) continue;
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        } else {
            Until([&]{return island.ReadEntireBuffer().find(L"复制第二行") != std::wstring::npos;});
            Require(island.Start(L"second", L"Write-Output 'SECOND_ISOLATED_SESSION'", cwd,
                L"第二测试终端", owner, nullptr), "start second isolated shell");
            Until([&]{return island.ReadEntireBuffer().find(L"SECOND_ISOLATED_SESSION") != std::wstring::npos;});
            island.Activate(L"first");
            Require(SUCCEEDED(OleGetClipboard(clipboard_backup.put())), "preserve original clipboard");
            TestMenu(owner);
            (void)OleSetClipboard(clipboard_backup.get());
        }
        island.Shutdown();
        if (IsWindow(owner)) DestroyWindow(owner);
        components.Shutdown();
        return 0;
    } catch (const std::exception& error) {
        if (clipboard_backup) (void)OleSetClipboard(clipboard_backup.get());
        std::cerr << error.what() << '\n';
        return 1;
    } catch (const winrt::hresult_error& error) {
        if (clipboard_backup) (void)OleSetClipboard(clipboard_backup.get());
        std::wcerr << error.message().c_str() << '\n';
        return 1;
    }
}
