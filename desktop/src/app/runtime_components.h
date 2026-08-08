#pragma once

#include <filesystem>
#include <initializer_list>
#include <vector>

#include <windows.h>
#include <roapi.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>

namespace aicli::app {

// Registers the side-by-side WinUI/Windows Terminal WinRT components that are
// shipped with the desktop application.  Keeping the loader in its own
// translation unit makes the window code independent of DLL activation
// details, while preserving the existing activation order and lifetime.
class WinRtComponentRegistry {
public:
    WinRtComponentRegistry() = default;
    bool Initialize();
    ~WinRtComponentRegistry();

    WinRtComponentRegistry(const WinRtComponentRegistry&) = delete;
    WinRtComponentRegistry& operator=(const WinRtComponentRegistry&) = delete;

private:
    bool Register(const wchar_t* file,
                  std::initializer_list<const wchar_t*> class_names);

    std::filesystem::path directory_;
    DLL_DIRECTORY_COOKIE dll_directory_{};
    std::vector<HMODULE> modules_;
    std::vector<RO_REGISTRATION_COOKIE> cookies_;
};

// The packaged Terminal DLLs expose activation factories directly rather than
// through the normal package activation path.  This small template is kept
// header-only because callers request different WinRT interfaces.
template <typename Interface>
Interface LocalActivationFactory(const wchar_t* module_name,
                                  const wchar_t* class_name) {
    HMODULE module = GetModuleHandleW(module_name);
    if (!module) module = LoadLibraryW(module_name);
    winrt::check_pointer(module);
    auto callback = reinterpret_cast<PFNGETACTIVATIONFACTORY>(
        GetProcAddress(module, "DllGetActivationFactory"));
    winrt::check_pointer(callback);
    winrt::hstring id{class_name};
    winrt::Windows::Foundation::IActivationFactory activation{nullptr};
    winrt::check_hresult(callback(
        reinterpret_cast<HSTRING>(winrt::get_abi(id)),
        reinterpret_cast<::IActivationFactory**>(winrt::put_abi(activation))));
    return activation.as<Interface>();
}

} // namespace aicli::app
