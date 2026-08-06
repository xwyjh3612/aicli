#include <windows.h>
#include <consoleapi.h>
#include <processthreadsapi.h>
#include <string>
#include <thread>
#include <mutex>
#include <vector>
#include <memory>
#include <algorithm>

namespace {
constexpr UINT WM_PTY_OUTPUT = WM_APP + 1;
constexpr UINT WM_PTY_EXIT = WM_APP + 2;
constexpr wchar_t kClassName[] = L"AicliTerminalProbeWindow";

struct OutputMessage {
    std::wstring text;
};

class ConPtyProcess {
public:
    ConPtyProcess() = default;
    ~ConPtyProcess() { Stop(); }

    bool Start(const std::wstring& command, const std::wstring& cwd, HWND owner) {
        owner_ = owner;
        SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        HANDLE inputRead = nullptr, inputWrite = nullptr;
        HANDLE outputRead = nullptr, outputWrite = nullptr;
        if (!CreatePipe(&inputRead, &inputWrite, &sa, 0) ||
            !CreatePipe(&outputRead, &outputWrite, &sa, 0)) {
            CloseHandle(inputRead); CloseHandle(inputWrite);
            CloseHandle(outputRead); CloseHandle(outputWrite);
            return false;
        }
        SetHandleInformation(inputWrite, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(outputRead, HANDLE_FLAG_INHERIT, 0);

        COORD size{120, 32};
        HRESULT hr = CreatePseudoConsole(size, inputRead, outputWrite, 0, &pty_);
        CloseHandle(inputRead);
        CloseHandle(outputWrite);
        if (FAILED(hr)) {
            CloseHandle(inputWrite); CloseHandle(outputRead);
            return false;
        }
        input_ = inputWrite;
        output_ = outputRead;

        SIZE_T attributeSize = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
        auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
            HeapAlloc(GetProcessHeap(), 0, attributeSize));
        if (!attributes || !InitializeProcThreadAttributeList(attributes, 1, 0, &attributeSize) ||
            !UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                       pty_, sizeof(pty_), nullptr, nullptr)) {
            if (attributes) HeapFree(GetProcessHeap(), 0, attributes);
            Stop();
            return false;
        }

        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.lpAttributeList = attributes;
        PROCESS_INFORMATION process{};
        std::wstring commandLine = L"cmd.exe /D /K " + command;
        std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
        mutableCommand.push_back(L'\0');
        DWORD flags = EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT;
        BOOL created = CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE,
                                      flags, nullptr, cwd.empty() ? nullptr : cwd.c_str(),
                                      &startup.StartupInfo, &process);
        DeleteProcThreadAttributeList(attributes);
        HeapFree(GetProcessHeap(), 0, attributes);
        if (!created) {
            Stop();
            return false;
        }
        process_ = process.hProcess;
        CloseHandle(process.hThread);
        reader_ = std::thread([this] { ReadLoop(); });
        return true;
    }

    void Stop() {
        if (process_) {
            TerminateProcess(process_, 0);
            CloseHandle(process_);
            process_ = nullptr;
        }
        if (pty_) {
            ClosePseudoConsole(pty_);
            pty_ = nullptr;
        }
        if (input_) { CloseHandle(input_); input_ = nullptr; }
        if (output_) { CloseHandle(output_); output_ = nullptr; }
        if (reader_.joinable()) reader_.join();
    }

    void Write(const std::string& bytes) {
        if (!input_) return;
        DWORD written = 0;
        WriteFile(input_, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    }

    void Resize(short columns, short rows) {
        if (pty_) ResizePseudoConsole(pty_, COORD{columns, rows});
    }

private:
    void ReadLoop() {
        char buffer[8192];
        DWORD read = 0;
        std::string pending;
        while (output_ && ReadFile(output_, buffer, sizeof(buffer), &read, nullptr) && read) {
            pending.append(buffer, read);
            size_t usable = pending.size();
            int count = 0;
            while (usable > 0) {
                count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                            pending.data(), static_cast<int>(usable), nullptr, 0);
                if (count > 0) break;
                --usable;
            }
            if (usable > 0 && count > 0) {
                std::wstring text(static_cast<size_t>(count), L'\0');
                MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, pending.data(),
                                    static_cast<int>(usable), text.data(), count);
                auto* message = new OutputMessage{std::move(text)};
                PostMessageW(owner_, WM_PTY_OUTPUT, 0, reinterpret_cast<LPARAM>(message));
                pending.erase(0, usable);
            }
        }
        PostMessageW(owner_, WM_PTY_EXIT, 0, 0);
    }

    HWND owner_ = nullptr;
    HPCON pty_ = nullptr;
    HANDLE input_ = nullptr;
    HANDLE output_ = nullptr;
    HANDLE process_ = nullptr;
    std::thread reader_;
};

ConPtyProcess g_process;
HWND g_output = nullptr;
HFONT g_font = nullptr;
WNDPROC g_originalEditProc = nullptr;
std::wstring g_command;
std::wstring g_cwd;

std::string Utf8(const wchar_t* text, int length) {
    if (length <= 0) return {};
    int bytes = WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr, 0, nullptr, nullptr);
    std::string result(bytes, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, length, result.data(), bytes, nullptr, nullptr);
    return result;
}

void AppendOutput(const std::wstring& text) {
    if (!g_output) return;
    int length = GetWindowTextLengthW(g_output);
    std::wstring current(static_cast<size_t>(length) + 1, L'\0');
    if (length) {
        GetWindowTextW(g_output, current.data(), length + 1);
        current.resize(static_cast<size_t>(length));
    } else {
        current.clear();
    }
    if (current.size() > 2 * 1024 * 1024) current.erase(0, 256 * 1024);
    current += text;
    SetWindowTextW(g_output, current.c_str());
    SendMessageW(g_output, EM_SETSEL, static_cast<WPARAM>(-1), static_cast<LPARAM>(-1));
    SendMessageW(g_output, EM_SCROLLCARET, static_cast<WPARAM>(0), static_cast<LPARAM>(0));
}

bool HandleTerminalInput(UINT message, WPARAM wParam) {
    if (message == WM_KEYDOWN) {
        if (wParam == VK_UP) { g_process.Write("\x1b[A"); return true; }
        if (wParam == VK_DOWN) { g_process.Write("\x1b[B"); return true; }
        if (wParam == VK_RIGHT) { g_process.Write("\x1b[C"); return true; }
        if (wParam == VK_LEFT) { g_process.Write("\x1b[D"); return true; }
        if (wParam == VK_ESCAPE) { g_process.Write("\x1b"); return true; }
        if ((GetKeyState(VK_CONTROL) & 0x8000) && wParam == 'C') { g_process.Write("\x03"); return true; }
        if ((GetKeyState(VK_CONTROL) & 0x8000) && wParam == 'D') { g_process.Write("\x04"); return true; }
    }
    if (message == WM_CHAR) {
        wchar_t character = static_cast<wchar_t>(wParam);
        if (character == L'\r') { g_process.Write("\r"); return true; }
        if (character == L'\b') { g_process.Write("\x7f"); return true; }
        if (character >= 0x20 || character == L'\t') {
            g_process.Write(Utf8(&character, 1));
            return true;
        }
    }
    return false;
}

LRESULT CALLBACK EditProc(HWND control, UINT message, WPARAM wParam, LPARAM lParam) {
    if (HandleTerminalInput(message, wParam)) return 0;
    return CallWindowProcW(g_originalEditProc, control, message, wParam, lParam);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        g_output = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL |
            ES_NOHIDESEL | ES_WANTRETURN,
            0, 0, 0, 0, window, nullptr, GetModuleHandleW(nullptr), nullptr);
        g_originalEditProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
            g_output, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(EditProc)));
        SendMessageW(g_output, EM_SETLIMITTEXT, static_cast<WPARAM>(0), 0);
        g_font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             FIXED_PITCH | FF_MODERN, L"Cascadia Mono");
        SendMessageW(g_output, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
        SetFocus(g_output);
        if (!g_process.Start(g_command.empty() ? L"" : g_command, g_cwd, window)) {
            AppendOutput(L"Unable to start ConPTY process.\r\n");
        } else {
            AppendOutput(L"[ConPTY started]\r\n");
        }
        return 0;
    }
    case WM_SIZE:
        if (g_output) MoveWindow(g_output, 0, 0, LOWORD(lParam), HIWORD(lParam), TRUE);
        if (g_output) {
            short cols = static_cast<short>(std::max(20, static_cast<int>(LOWORD(lParam)) / 8));
            short rows = static_cast<short>(std::max(5, static_cast<int>(HIWORD(lParam)) / 18));
            g_process.Resize(cols, rows);
        }
        return 0;
    case WM_PTY_OUTPUT: {
        auto* output = reinterpret_cast<OutputMessage*>(lParam);
        if (output) { AppendOutput(output->text); delete output; }
        return 0;
    }
    case WM_PTY_EXIT:
        AppendOutput(L"\r\n[process exited]\r\n");
        return 0;
    case WM_DESTROY:
        g_process.Stop();
        if (g_font) DeleteObject(g_font);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int show) {
    g_command = commandLine ? commandLine : L"";
    WNDCLASSW windowClass{};
    windowClass.hInstance = instance;
    windowClass.lpfnWndProc = WindowProc;
    windowClass.lpszClassName = kClassName;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassW(&windowClass);
    HWND window = CreateWindowExW(0, kClassName, L"AI CLI - ConPTY stage 0",
                                  WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                  1200, 760, nullptr, nullptr, instance, nullptr);
    if (!window) return 1;
    ShowWindow(window, show);
    UpdateWindow(window);
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}
