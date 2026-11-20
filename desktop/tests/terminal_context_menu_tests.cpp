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
#include <mutex>
#include <stdexcept>

namespace {
aicli::terminal::XamlIslandAdapter* adapter{};
int menu_row{-1};
bool cancel_menu{}, switch_project{};
unsigned menu_count{};
bool physical_pick{}, physical_pick_failed{};
std::wstring observed_input;
std::mutex observed_input_mutex;
std::wstring ObservedInput() {
    std::scoped_lock lock(observed_input_mutex);
    return observed_input;
}
constexpr UINT_PTR kMenuTestTimer = 12;
constexpr UINT_PTR kPhysicalPickTimeoutTimer = 13;

void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

LRESULT CALLBACK OwnerProc(HWND owner, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_TIMER && (wparam == kMenuTestTimer || wparam == kPhysicalPickTimeoutTimer)) {
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
        if(wparam==kPhysicalPickTimeoutTimer){
            KillTimer(owner,kPhysicalPickTimeoutTimer);physical_pick_failed=true;
            PostMessageW(popup,WM_KEYDOWN,VK_ESCAPE,0);return 0;
        }
        KillTimer(owner, kMenuTestTimer);
        ++menu_count;
        if (switch_project) adapter->Activate(L"second");
        if (cancel_menu) PostMessageW(popup, WM_KEYDOWN, VK_ESCAPE, 0);
        else {
            const int dpi = static_cast<int>(GetDpiForWindow(owner));
            const int y = MulDiv(6 + 36 * menu_row + 18 + (menu_row >= 2 ? 9 : 0), dpi, 96);
            if(physical_pick){
                RECT bounds{};GetWindowRect(popup,&bounds);
                SetCursorPos(bounds.left+MulDiv(40,dpi,96),bounds.top+y);
                INPUT input[2]{};input[0].type=input[1].type=INPUT_MOUSE;
                input[0].mi.dwFlags=MOUSEEVENTF_LEFTDOWN;input[1].mi.dwFlags=MOUSEEVENTF_LEFTUP;
                physical_pick_failed=SendInput(2,input,sizeof(INPUT))!=2;
                SetTimer(owner,kPhysicalPickTimeoutTimer,3000,nullptr);
            }else PostMessageW(popup, WM_LBUTTONUP, 0, MAKELPARAM(MulDiv(40, dpi, 96), y));
        }
        return 0;
    }
    if (message == WM_CLOSE) { DestroyWindow(owner); return 0; }
    if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(owner, message, wparam, lparam);
}

template<class Condition> void Until(Condition condition, const char* failure) {
    const auto deadline = GetTickCount64() + 10000;
    while (!condition() && GetTickCount64() < deadline) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (physical_pick && (message.message == WM_POINTERDOWN ||
                message.message == WM_POINTERUP || message.message == WM_POINTERUPDATE)) {
                POINTER_INFO pointer{};
                if (GetPointerInfo(GET_POINTERID_WPARAM(message.wParam), &pointer) &&
                    pointer.ButtonChangeType != POINTER_CHANGE_NONE) {
                    std::cout << "Physical pointer message: " << message.message
                        << "; button change: " << pointer.ButtonChangeType << '\n';
                }
            }
            const bool handled = adapter->PreTranslateMessage(message);
            if (handled) continue;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
    }
    Require(condition(), failure);
}

void WaitForOutput(std::wstring_view line, const char* failure) {
    std::wstring buffer;
    const std::wstring marker = L"\n" + std::wstring(line);
    try {
        Until([&] {
            buffer = adapter->ReadEntireBuffer();
            return buffer.find(marker) != std::wstring::npos;
        }, failure);
    } catch (...) {
        std::cout << "Fixture buffer characters: " << buffer.size()
            << "; contains output marker: " << (buffer.find(line) != std::wstring::npos)
            << "; contains line feed: " << (buffer.find(L'\n') != std::wstring::npos) << '\n';
        throw;
    }
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

struct VisibleFixture {
    HWND owner;
    HWND previous_foreground{GetForegroundWindow()};
    POINT previous_cursor{};
    explicit VisibleFixture(HWND window) : owner(window) { GetCursorPos(&previous_cursor); }
    ~VisibleFixture() {
        physical_pick = false;
        KillTimer(owner, kMenuTestTimer);
        KillTimer(owner, kPhysicalPickTimeoutTimer);
        SetWindowPos(owner, HWND_NOTOPMOST, -30000, -30000, 0, 0,
            SWP_NOSIZE | SWP_NOACTIVATE);
        SetCursorPos(previous_cursor.x, previous_cursor.y);
        if (IsWindow(previous_foreground)) SetForegroundWindow(previous_foreground);
    }
    void Show() {
        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        SetWindowPos(owner, HWND_TOPMOST, work.left + 30, work.top + 30, 920, 600, SWP_SHOWWINDOW);
        RECT client{};
        GetClientRect(owner, &client);
        adapter->Resize(client);
        adapter->SetVisible(true);
        Focus(owner);
    }
    static void Focus(HWND owner) {
        SetWindowPos(owner, HWND_TOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        SetForegroundWindow(owner);
        // Windows may deny programmatic activation of this packaged fixture.
        // Click its own title bar once, and keep the frame visible until all
        // asynchronous menu actions have completed.
        if (GetForegroundWindow() != owner) {
            RECT frame{};
            GetWindowRect(owner, &frame);
            const int dpi = static_cast<int>(GetDpiForWindow(owner));
            POINT caption{frame.left + MulDiv(120, dpi, 96), frame.top + MulDiv(16, dpi, 96)};
            Require(GetAncestor(WindowFromPoint(caption), GA_ROOT) == owner,
                "isolated fixture title bar must be unobscured");
            SetCursorPos(caption.x, caption.y);
            INPUT activation[2]{};
            activation[0].type = activation[1].type = INPUT_MOUSE;
            activation[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
            activation[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
            Require(SendInput(2, activation, sizeof(INPUT)) == 2,
                "activate isolated fixture with a title bar click");
        }
        Until([&]{return GetForegroundWindow() == owner;}, "isolated terminal did not become foreground");
        SetFocus(adapter->Window());
    }
};

void PhysicalMenu(HWND owner,int row,bool cancel=false,bool keyboard=false,
    std::function<bool()> action_completed={}){
    VisibleFixture::Focus(owner);
    Require(adapter->Activate(L"first"), "focus isolated terminal for physical input");
    RECT bounds{};GetWindowRect(adapter->Window(),&bounds);
    POINT target{bounds.left+120,bounds.top+100};
    const HWND hit=WindowFromPoint(target);
    Require(hit==adapter->Window()||IsChild(adapter->Window(),hit)||hit==owner,"physical right-click targets isolated terminal");
    SetCursorPos(target.x,target.y);
    menu_row=row;cancel_menu=cancel;physical_pick=true;physical_pick_failed=false;
    const auto count=menu_count;
    SetTimer(owner,kMenuTestTimer,10,nullptr);
    INPUT input[2]{};
    if(keyboard){
        input[0].type=input[1].type=INPUT_KEYBOARD;
        input[0].ki.wVk=input[1].ki.wVk=VK_APPS;
        input[1].ki.dwFlags=KEYEVENTF_KEYUP;
    }else{
        input[0].type=input[1].type=INPUT_MOUSE;
        input[0].mi.dwFlags=MOUSEEVENTF_RIGHTDOWN;
        input[1].mi.dwFlags=MOUSEEVENTF_RIGHTUP;
    }
    const UINT sent=SendInput(2,input,sizeof(INPUT));
    try{
        Require(sent==2,"inject real right-click into isolated terminal");
        Until([&]{return menu_count==count+1;}, "physical context menu did not open");
        Require(!physical_pick_failed,"real left-click selects the terminal menu option");
        if (action_completed) Until(action_completed, "physical menu action did not complete");
    }catch(...){
        physical_pick=false;KillTimer(owner,kMenuTestTimer);KillTimer(owner,kPhysicalPickTimeoutTimer);
        throw;
    }
    physical_pick=false;KillTimer(owner,kMenuTestTimer);KillTimer(owner,kPhysicalPickTimeoutTimer);
}

void TestMenu(HWND owner) {
    using namespace aicli::terminal;
    for (bool selection : {false, true}) for (bool paste : {false, true}) {
        const auto entries = TerminalContextMenuEntries(selection, paste);
        Require(entries.size() == 4 && entries[0].enabled == selection && entries[1].enabled == paste &&
            entries[2].enabled && entries[3].enabled, "menu availability follows selection and clipboard");
    }
    ClipboardText(owner, L"clipboard sentinel");
    const auto before_menu = ObservedInput();
    Menu(owner, 0);
    Require(ClipboardText(owner) == L"clipboard sentinel", "disabled copy does not change clipboard");
    Require(ObservedInput().find(L"clipboard sentinel", before_menu.size()) == std::wstring::npos,
        "right-click does not paste clipboard text into the terminal");
    Menu(owner, 2);
    std::cout << "Testing physical right-click selection copy\n";
    PhysicalMenu(owner,0);
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
    std::cout << "Testing physical keyboard full-buffer copy\n";
    PhysicalMenu(owner, 3, false, true);
    Require(ClipboardText(owner).find(L"AICLI_MENU_LINE_ONE") != std::wstring::npos,
        "keyboard context menu can copy the full terminal buffer");
    ClipboardText(owner, L"AICLI_PASTE_CHECK");
    {
        std::scoped_lock lock(observed_input_mutex);
        observed_input.clear();
    }
    std::cout << "Testing physical right-click paste\n";
    try {
        PhysicalMenu(owner, 1, false, false,
            []{return ObservedInput().find(L"AICLI_PASTE_CHECK") != std::wstring::npos;});
    } catch (...) {
        std::cout << "Paste input characters: " << ObservedInput().size()
            << "; clipboard still has paste fixture: " << (ClipboardText(owner) == L"AICLI_PASTE_CHECK")
            << "; buffer contains paste fixture: "
            << (adapter->ReadEntireBuffer().find(L"AICLI_PASTE_CHECK") != std::wstring::npos) << '\n';
        throw;
    }
    const auto pasted = ObservedInput();
    Require(pasted.find(L"AICLI_PASTE_CHECK") != std::wstring::npos &&
        pasted.find(L'\r') == std::wstring::npos && pasted.find(L'\n') == std::wstring::npos,
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
    std::cout << "Real TermControl context menu: hardware right-click and menu clicks, Unicode selection copy, paste, keyboard, cancellation, deeper HWND and session switching passed\n";
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
        std::unique_ptr<VisibleFixture> visible;
        if (!interactive) {
            visible = std::make_unique<VisibleFixture>(owner);
            visible->Show();
        }
        const auto cwd = std::filesystem::temp_directory_path().wstring();
        Require(island.Start(L"first", L"Write-Output 'AICLI_MENU_LINE_ONE'; Write-Output '复制第二行'",
            cwd, L"右键测试", owner, nullptr, {}, {},
            [](std::wstring_view value){
                std::scoped_lock lock(observed_input_mutex);
                observed_input.append(value);
            }), "start isolated test shell");
        std::cout << "Started isolated test terminal\n";
        if (interactive) {
            MSG message{};
            while (GetMessageW(&message, nullptr, 0, 0) > 0) {
                if (island.PreTranslateMessage(message)) continue;
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        } else {
            WaitForOutput(L"复制第二行", "first terminal output did not become available");
            std::cout << "First terminal output ready\n";
            Require(island.Start(L"second", L"Write-Output 'SECOND_ISOLATED_SESSION'", cwd,
                L"第二测试终端", owner, nullptr), "start second isolated shell");
            WaitForOutput(L"SECOND_ISOLATED_SESSION", "second terminal output did not become available");
            std::cout << "Second terminal output ready\n";
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
