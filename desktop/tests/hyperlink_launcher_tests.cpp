#include "terminal/hyperlink_launcher.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {
HWND launched_owner{};
std::wstring launched_url;
unsigned launches{};
INT_PTR launch_result = 33;

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

HINSTANCE WINAPI CaptureLaunch(HWND owner, LPCWSTR verb, LPCWSTR target,
                              LPCWSTR parameters, LPCWSTR directory, INT show) {
    Require(std::wstring_view{verb} == L"open", "use the registered browser");
    Require(!parameters && !directory, "URL is a target, never a shell command");
    Require(show == SW_SHOWNORMAL, "open the browser normally");
    launched_owner = owner;
    launched_url = target;
    ++launches;
    return reinterpret_cast<HINSTANCE>(launch_result);
}
}

int main() {
    try {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        const HWND owner = GetDesktopWindow();
        for (const auto* target : {
                 L"http://localhost:5173/admin/calls",
                 L"http://127.0.0.1:4280/admin?queue=1#answer",
                 L"https://example.com/admin?next=%2Fcalls%3Fid%3D1&state=a%20b#desk",
                 L"https://example.com/管理端/接听?区域=菏泽",
                 L"HTTPS://example.com/admin"}) {
            Require(aicli::terminal::OpenTerminalWebLink(owner, target, CaptureLaunch),
                    "valid web URL opens");
            Require(launched_url == target && launched_owner == owner,
                    "preserve the full URL and owner");
        }
        const auto successful_launches = launches;
        for (const auto* target : {
                 L"", L"管理端接听页面", L"https://", L"/admin/calls",
                 L"javascript:alert(1)", L"file:///C:/Windows/System32/cmd.exe",
                 L"ms-settings:privacy", L"https://example.com/\r\nadmin",
                 L" https://example.com", L"https://example.com/a b"}) {
            Require(!aicli::terminal::OpenTerminalWebLink(owner, target, CaptureLaunch),
                    "invalid or non-web target is rejected");
        }
        const std::wstring embedded_null{L"https://example.com\0/admin", 26};
        Require(!aicli::terminal::OpenTerminalWebLink(owner, embedded_null, CaptureLaunch),
                "embedded null cannot truncate the target");
        Require(launches == successful_launches, "rejected links never reach the shell");
        for (INT_PTR result : {0, 2, 5, 31, 32}) {
            launch_result = result;
            Require(!aicli::terminal::OpenTerminalWebLink(
                        owner, L"https://example.com/admin", CaptureLaunch),
                    "shell failure is reported");
        }
        winrt::uninit_apartment();
        std::cout << "Terminal hyperlink launcher tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
