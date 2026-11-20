#include "app/runtime_components.h"
#include "core/text_utils.h"
#include "terminal/terminal_context_menu.h"
#include "terminal/codex_composer_state.h"
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
bool headless_input{};
std::wstring observed_input;
std::mutex observed_input_mutex;
std::vector<std::wstring> opened_links;
std::wstring ObservedInput() {
    std::scoped_lock lock(observed_input_mutex);
    return observed_input;
}
constexpr UINT_PTR kMenuTestTimer = 12;
constexpr UINT_PTR kPhysicalPickTimeoutTimer = 13;

UINT ClickAt(POINT point, bool right = false) {
    INPUT input[2]{};
    const int left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int width = std::max(1, GetSystemMetrics(SM_CXVIRTUALSCREEN));
    const int height = std::max(1, GetSystemMetrics(SM_CYVIRTUALSCREEN));
    for (auto& event : input) {
        event.type = INPUT_MOUSE;
        // Anchor both injected packets, instead of depending on a shared
        // cursor position between SetCursorPos and SendInput.
        event.mi.dx = static_cast<LONG>((static_cast<long long>(point.x - left) * 65536 + 32768) / width);
        event.mi.dy = static_cast<LONG>((static_cast<long long>(point.y - top) * 65536 + 32768) / height);
        event.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    }
    input[0].mi.dwFlags |= right ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_LEFTDOWN;
    input[1].mi.dwFlags |= right ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_LEFTUP;
    return SendInput(2, input, sizeof(INPUT));
}

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
            const int y = MulDiv(6 + 36 * menu_row + 18 + (menu_row >= 2 ? 9 : 0) +
                (menu_row >= 4 ? 9 : 0), dpi, 96);
            if(physical_pick){
                RECT bounds{};GetWindowRect(popup,&bounds);
                physical_pick_failed=ClickAt({bounds.left+MulDiv(40,dpi,96),bounds.top+y})!=2;
                SetTimer(owner,kPhysicalPickTimeoutTimer,3000,nullptr);
            }else PostMessageW(popup, WM_LBUTTONUP, 0, MAKELPARAM(MulDiv(40, dpi, 96), y));
        }
        return 0;
    }
    if (message == WM_CLOSE) { DestroyWindow(owner); return 0; }
    if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(owner, message, wparam, lparam);
}

template<class Condition> void Until(Condition condition, const char* failure, ULONGLONG timeout=10000) {
    const auto deadline = GetTickCount64() + timeout;
    while (!condition() && GetTickCount64() < deadline) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
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

void Menu(HWND owner, int row, bool cancel = false, bool keyboard = false, HWND target = nullptr,
    POINT position = {60, 70}) {
    menu_row = row;
    cancel_menu = cancel;
    const auto count = menu_count;
    SetTimer(owner, kMenuTestTimer, 10, nullptr);
    MSG message{};
    message.hwnd = target ? target : adapter->Window();
    message.message = keyboard ? WM_KEYDOWN : WM_RBUTTONUP;
    message.wParam = keyboard ? VK_APPS : 0;
    message.lParam = keyboard ? 0 : MAKELPARAM(position.x, position.y);
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
            const HWND caption_root = GetAncestor(WindowFromPoint(caption), GA_ROOT);
            if (caption_root != owner) {
                wchar_t name[96]{};
                GetClassNameW(caption_root, name, 96);
                std::cout << "Fixture title-bar hit class: " << aicli::text::Utf8Text(name)
                    << "; frame origin: " << frame.left << ',' << frame.top
                    << "; dpi: " << dpi << '\n';
            }
            Require(caption_root == owner,
                "isolated fixture title bar must be unobscured");
            Require(ClickAt(caption) == 2,
                "activate isolated fixture with a title bar click");
        }
        Until([&]{return GetForegroundWindow() == owner;}, "isolated terminal did not become foreground");
        SetFocus(adapter->Window());
    }
};

void PhysicalMenu(HWND owner,int row,bool cancel=false,bool keyboard=false,
    std::function<bool()> action_completed={}, std::wstring_view project=L"first",
    POINT position={120,100}){
    if (headless_input) {
        Require(adapter->Activate(project,false), "activate isolated headless terminal");
        Menu(owner,row,cancel,keyboard,nullptr,position);
        if (action_completed) Until(action_completed,"headless menu action did not complete");
        return;
    }
    VisibleFixture::Focus(owner);
    Require(adapter->Activate(project), "focus isolated terminal for physical input");
    RECT bounds{};GetWindowRect(adapter->Window(),&bounds);
    POINT target{bounds.left+position.x,bounds.top+position.y};
    const HWND hit=WindowFromPoint(target);
    Require(hit==adapter->Window()||IsChild(adapter->Window(),hit)||hit==owner,"physical right-click targets isolated terminal");
    menu_row=row;cancel_menu=cancel;physical_pick=true;physical_pick_failed=false;
    const auto count=menu_count;
    SetTimer(owner,kMenuTestTimer,10,nullptr);
    INPUT input[2]{};
    if(keyboard){
        input[0].type=input[1].type=INPUT_KEYBOARD;
        input[0].ki.wVk=input[1].ki.wVk=VK_APPS;
        input[1].ki.dwFlags=KEYEVENTF_KEYUP;
    }
    const UINT sent=keyboard?SendInput(2,input,sizeof(INPUT)):ClickAt(target,true);
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

void TestMenuAvailability() {
    using namespace aicli::terminal;
    Require(HasLiveCodexComposer(L"› draft\r\n\r\n100% context left\r\n"), "recognize default composer");
    Require(HasLiveCodexComposer(L"› draft\r\n\r\nmodel · provider · folder\r\n"), "recognize custom Codex status line");
    Require(!HasLiveCodexComposer(L"› old draft\r\n100% context left\r\nPS C:\\test>\r\n"),
        "a former composer does not make a PowerShell prompt clearable");
    Require(!HasLiveCodexComposer(L"› draft\r\nNORMAL · model\r\n"), "modal editing does not take native clear path");
    Require(!HasLiveCodexComposer(L"› draft\r\nEnter to confirm · Esc to cancel\r\n"),
        "question dialogs do not take draft cancellation path");
    Require(HasLiveCodexComposer(L"› draft\r\n100% context left · esc to interrupt\r\n"),
        "a running task still permits clearing its ordinary draft");
    for (bool selection : {false, true}) for (bool paste : {false, true}) {
        const auto entries = TerminalContextMenuEntries(selection, paste);
        Require(entries.size() == 4 && entries[0].enabled == selection && entries[1].enabled == paste &&
            entries[2].enabled && entries[3].enabled, "menu availability follows selection and clipboard");
    }
    for (bool clear : {false, true}) for (bool link : {false, true}) {
        const auto entries = TerminalContextMenuEntries(false, false, clear, link);
        Require(entries.size() == 4 + static_cast<size_t>(clear) + static_cast<size_t>(link),
            "context actions only appear when available");
        if (clear) Require(entries[4].command == static_cast<int>(TerminalMenuCommand::ClearInput),
            "Codex clear action follows terminal copy actions");
        if (link) Require(entries.back().command == static_cast<int>(TerminalMenuCommand::OpenLink),
            "link action uses its own command");
    }
}
void TestMenu(HWND owner) {
    TestMenuAvailability();
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
        const auto input = ObservedInput();
        std::cout << "Paste input characters: " << input.size()
            << "; starts with mouse VT sequence: " << (input.starts_with(L"\x1b[M") || input.starts_with(L"\x1b[<"))
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
    std::cout << (headless_input ? "Real TermControl message-driven context menu: " :
        "Real TermControl hardware right-click and menu clicks: ")
        << "Unicode selection copy, paste, keyboard, cancellation, deeper HWND and session switching passed\n";
}

std::wstring Literal(std::wstring_view value) {
    std::wstring result=L"'";
    for (auto ch : value) { result.push_back(ch); if (ch==L'\'') result.push_back(ch); }
    return result+L"'";
}

void TestLinks(HWND owner, const std::wstring& cwd) {
    const std::wstring uri=L"https://example.org/a%2Fb?x=1&y=%E4%B8%AD#section";
    const auto command=L"$e=[char]27;[Console]::Write($e+'[2J'+$e+'[H'+$e+']8;;' + "+
        Literal(uri)+L" + $e+'\\'+ 'LINK_FIXTURE' + $e+']8;;'+$e+'\\'+\"`r`nhttps://example.org/plain?a=1`r`nPLAIN_FIXTURE`r`n\")";
    Require(adapter->Start(L"link",command,cwd,L"链接测试",owner,nullptr), "start link fixture");
    Until([]{ const auto text=adapter->ReadEntireBuffer();
        return text.find(L"\nPLAIN_FIXTURE")!=std::wstring::npos;
    }, "link fixture renders literal hyperlink");
    // TermControl updates its plain-URL regex after 100 ms of output idle.
    const auto patterns_ready=GetTickCount64()+250;
    Until([&]{return GetTickCount64()>=patterns_ready;},"plain URL detection reaches output idle");
    const auto count=opened_links.size();
    PhysicalMenu(owner,4,false,false,{},L"link",{40,14});
    Require(opened_links.size()==count+1 && opened_links.back()==uri,
        "right-click opens OSC 8 display text using the complete original URL");
    PhysicalMenu(owner,4,false,false,{},L"link",{40,35});
    Require(opened_links.size()==count+2 && opened_links.back()==L"https://example.org/plain?a=1",
        "right-click opens a detected plain-text URL");
    Menu(owner,4,false,false,nullptr,{40,55});
    Require(opened_links.size()==count+2,"plain text does not reuse a previously hovered link");
    switch_project=true;
    adapter->Activate(L"link");
    Menu(owner,4,false,false,nullptr,{40,14});
    switch_project=false;
    Require(opened_links.size()==count+2,"switching project cancels link launch");
    std::cout << "OSC 8 link hit test, original query/fragment, plain text and stale project passed\n";
}

void TestCodexInput(HWND owner, const std::filesystem::path& capture) {
    try {
    Until([]{const auto text=adapter->ReadEntireBuffer();
        return aicli::terminal::HasLiveCodexComposer(text);
    },"fresh Codex composer did not become ready",30000);
    } catch (...) {
        std::ofstream(capture.parent_path()/L"startup-buffer.txt") << aicli::text::Utf8Text(adapter->ReadEntireBuffer());
        throw;
    }
    auto draft=[&] {
        std::error_code error;
        std::filesystem::remove(capture,error); // This exact fixture-owned file only.
        Require(adapter->WriteInput(std::wstring(1,L'\x07')),"open fixture capture editor without submitting");
        Until([&]{return std::filesystem::exists(capture);},"Codex external editor did not capture draft");
        std::ifstream stream(capture,std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(stream)),{});
        const auto ready=GetTickCount64()+350;
        Until([&]{return GetTickCount64()>=ready;},"capture editor exits before the next edit");
        Until([]{return aicli::terminal::HasLiveCodexComposer(adapter->ReadEntireBuffer());},
            "Codex composer did not return after capture");
        return aicli::text::WideText(text);
    };
    std::vector<std::wstring> values{L"AICLI_CLEAR_中文",L"第一行\n第二行\n第三行",L""};
    std::wstring long_input;
    for (int line=0;line<80;++line) long_input+=L"AICLI_LONG_"+std::to_wstring(line)+L"\n";
    values.push_back(std::move(long_input));
    values.push_back(std::wstring(6000,L'A'));
    PhysicalMenu(owner,4);
    Require(draft().empty(), "fresh fixture starts with an empty Codex composer");
    Require(adapter->SendKey(L"first",VK_RETURN,SHIFT_PRESSED),"insert an editing newline into empty composer");
    Require(draft()==L"\n","Shift+Enter edits without submitting before test text is entered");
    PhysicalMenu(owner,4);
    for (size_t index=0;index<values.size();++index) {
        std::cout << "Testing real Codex draft clear case " << index << '\n';
        if(!values[index].empty()) {
            // Build multiline drafts with real editing newlines. Raw CR/LF in
            // Windows console paste can be translated to submission events;
            // the empty-composer check above validates Shift+Enter first.
            size_t start=0;
            while(start<values[index].size()) {
                const auto end=values[index].find(L'\n',start);
                const auto line=values[index].substr(start,end==std::wstring::npos?std::wstring::npos:end-start);
                if(!line.empty()) {
                    if(index<3 && !headless_input){
                        ClipboardText(owner,line);
                        PhysicalMenu(owner,1);
                    } else Require(adapter->WriteInput(line),"feed long fixture text without a clipboard race");
                    const auto ready=GetTickCount64()+150;
                    Until([&]{return GetTickCount64()>=ready;},"paste burst settles before editing newline");
                }
                if(end==std::wstring::npos)break;
                Require(adapter->SendKey(L"first",VK_RETURN,SHIFT_PRESSED),"insert unsubmitted multiline editing newline");
                start=end+1;
            }
            Require(draft()==values[index],"independent editor capture matches original test draft");
        }
        if(index==1) {
            Require(adapter->SendKey(L"first",VK_UP),"move multiline cursor to middle");
            Require(adapter->SendKey(L"first",VK_LEFT),"move cursor inside line");
        }
        PhysicalMenu(owner,4);
        Require(draft().empty(),"clear removes the entire Codex draft without submitting");
        PhysicalMenu(owner,4);
        Require(draft().empty(),"repeated clear on an empty composer leaves Codex running");
    }
    std::cout << "Real Codex: Unicode, multiline, middle cursor, hidden long paste, empty and repeated clear passed\n";
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    bool interactive=false,codex=false;
    for(int index=1;index<argc;++index){
        interactive |= std::wstring_view(argv[index])==L"--interactive";
        codex |= std::wstring_view(argv[index])==L"--codex";
        headless_input |= std::wstring_view(argv[index])==L"--headless";
    }
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
        const std::wstring arguments=(interactive?L"--interactive ":L"")+
            std::wstring(codex?L"--codex ":L"")+std::wstring(headless_input?L"--headless":L"");
        if (SUCCEEDED(result)) result = activation->ActivateApplication(
            L"AiCli.Desktop.MenuProbe_jw3zr00rmb0ym!App", arguments.c_str(),
            AO_NOERRORUI, &pid);
        if (FAILED(result)) {
            std::cerr << "Register the isolated MenuProbe manifest before running this test\n";
            return 1;
        }
        const HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!process) return 1;
        const DWORD wait = WaitForSingleObject(process, interactive ? INFINITE : (codex?180000:60000));
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
        owner = CreateWindowW(type.lpszClassName, interactive?L"AiCli 右键功能体验":L"AiCli 自动回归测试", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
            interactive ? 100 : -30000, interactive ? 100 : -30000, 920, 600,
            nullptr, nullptr, type.hInstance, nullptr);
        aicli::terminal::XamlIslandAdapter island(interactive ? aicli::terminal::XamlIslandAdapter::LinkOpener{} :
            [](HWND,std::wstring_view uri){opened_links.emplace_back(uri);return true;});
        adapter = &island;
        RECT client{};
        GetClientRect(owner, &client);
        if (!island.Attach(owner, client)) throw std::runtime_error(
            "attach isolated terminal island: " + aicli::text::Utf8Text(island.LastError()));
        std::cout << "Attached isolated XAML island\n";
        std::unique_ptr<VisibleFixture> visible;
        if (!interactive && !headless_input) {
            visible = std::make_unique<VisibleFixture>(owner);
            visible->Show();
        } else if (headless_input) {
            island.SetVisible(true);
        }
        const auto fixture_path=directory/(L"codex-fixture-"+std::to_wstring(GetCurrentProcessId()));
        if(codex)std::filesystem::create_directories(fixture_path);
        const auto cwd = codex?fixture_path.wstring():std::filesystem::temp_directory_path().wstring();
        const auto capture=fixture_path/L"captured-draft.txt";
        std::wstring command=L"Write-Output 'AICLI_MENU_LINE_ONE'; Write-Output '复制第二行'";
        if(codex){
            if(!interactive){
                const auto editor=fixture_path/L"capture-editor.ps1";
                std::ofstream script(editor);
                const auto writing=capture.wstring()+L".writing";
                script << "param([string]$Draft)\n[IO.File]::WriteAllText(" << aicli::text::Utf8Text(Literal(writing))
                    << ",[IO.File]::ReadAllText($Draft),[Text.UTF8Encoding]::new($false))\n[IO.File]::Move("
                    << aicli::text::Utf8Text(Literal(writing)) << "," << aicli::text::Utf8Text(Literal(capture.wstring())) << ")\n";
                command+=L"; $env:VISUAL="+Literal(L"powershell.exe -NoProfile -ExecutionPolicy Bypass -File \""+editor.wstring()+L"\"")+L"; $env:EDITOR=$env:VISUAL";
            }
            command+=L"; $e=[char]27;[Console]::Write($e+']8;;https://developers.openai.com/codex/cli/features/'+$e+'\\'+'打开 Codex 使用说明'+$e+']8;;'+$e+'\\'+\"`r`n\"); codex --yolo --no-alt-screen -c ('projects={' + (ConvertTo-Json -InputObject (Get-Location).Path -Compress) + '={trust_level=\"trusted\"}}')";
        }
        Require(island.Start(L"first", command,
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
        } else if(codex){
            if (!headless_input)
                Require(SUCCEEDED(OleGetClipboard(clipboard_backup.put())), "preserve original clipboard for Codex paste tests");
            TestCodexInput(owner,capture);
            if (clipboard_backup) (void)OleSetClipboard(clipboard_backup.get());
        } else {
            WaitForOutput(L"复制第二行", "first terminal output did not become available");
            std::cout << "First terminal output ready\n";
            Require(island.Start(L"second", L"Write-Output 'SECOND_ISOLATED_SESSION'", cwd,
                L"第二测试终端", owner, nullptr), "start second isolated shell");
            WaitForOutput(L"SECOND_ISOLATED_SESSION", "second terminal output did not become available");
            std::cout << "Second terminal output ready\n";
            island.Activate(L"first");
            if (headless_input) {
                TestMenuAvailability();
                std::cout << "Headless menu availability passed; clipboard/hardware actions require an unlocked desktop\n";
            } else {
                Require(SUCCEEDED(OleGetClipboard(clipboard_backup.put())), "preserve original clipboard");
                TestMenu(owner);
            }
            TestLinks(owner,cwd);
            if (clipboard_backup) (void)OleSetClipboard(clipboard_backup.get());
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
