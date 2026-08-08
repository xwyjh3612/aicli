#include "app/runtime_components.h"

#include <fstream>

namespace aicli::app {

bool WinRtComponentRegistry::Initialize() {
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) return false;
    directory_ = std::filesystem::path(path).parent_path();
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS |
                             LOAD_LIBRARY_SEARCH_USER_DIRS);
    dll_directory_ = AddDllDirectory(directory_.c_str());

    return Register(L"Microsoft.UI.Xaml.dll", {
               L"Microsoft.UI.Xaml.Controls.CommandBarFlyout",
               L"Microsoft.UI.Xaml.Controls.XamlControlsResources",
               L"Microsoft.UI.Xaml.XamlTypeInfo.XamlControlsXamlMetaDataProvider"}) &&
           Register(L"Microsoft.Terminal.Settings.Model.dll", {
               L"Microsoft.Terminal.Settings.Model.CascadiaSettings",
               L"Microsoft.Terminal.Settings.Model.Profile",
               L"Microsoft.Terminal.Settings.Model.TerminalSettings",
               L"Microsoft.Terminal.Settings.Model.TerminalSettingsCreateResult"}) &&
           Register(L"Microsoft.Terminal.UI.dll", {
               L"Microsoft.Terminal.UI.TextMenuFlyout",
               L"Microsoft.Terminal.UI.ResourceString",
               L"Microsoft.Terminal.UI.Converters",
               L"Microsoft.Terminal.UI.IconPathConverter"}) &&
           Register(L"Microsoft.Terminal.Control.dll", {
               L"Microsoft.Terminal.Control.ControlCore",
               L"Microsoft.Terminal.Control.ControlInteractivity",
               L"Microsoft.Terminal.Control.KeyChord",
               L"Microsoft.Terminal.Control.OpenHyperlinkEventArgs",
               L"Microsoft.Terminal.Control.ScrollBarVisualStateManager",
               L"Microsoft.Terminal.Control.SearchBoxControl",
               L"Microsoft.Terminal.Control.SelectionColor",
               L"Microsoft.Terminal.Control.TSFInputControl",
               L"Microsoft.Terminal.Control.TermControl",
               L"Microsoft.Terminal.Control.VisualBellLight",
               L"Microsoft.Terminal.Control.XamlMetaDataProvider"});
}

WinRtComponentRegistry::~WinRtComponentRegistry() {
    for (auto cookie : cookies_) RoRevokeActivationFactories(cookie);
    if (dll_directory_) RemoveDllDirectory(dll_directory_);
}

bool WinRtComponentRegistry::Register(
    const wchar_t* file, std::initializer_list<const wchar_t*> class_names) {
    const auto full_path = directory_ / file;
    HMODULE module = LoadLibraryExW(
        full_path.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS |
            LOAD_LIBRARY_SEARCH_USER_DIRS);
    if (!module) {
        std::ofstream log(directory_ / L"component-load-error.txt", std::ios::trunc);
        log << "LoadLibraryEx failed for " << std::filesystem::path(file).string()
            << ": " << GetLastError() << "\n";
        return false;
    }

    auto callback = reinterpret_cast<PFNGETACTIVATIONFACTORY>(
        GetProcAddress(module, "DllGetActivationFactory"));
    if (!callback) {
        std::ofstream log(directory_ / L"component-load-error.txt", std::ios::trunc);
        log << "DllGetActivationFactory missing for "
            << std::filesystem::path(file).string() << "\n";
        FreeLibrary(module);
        return false;
    }

    std::vector<winrt::hstring> names;
    std::vector<HSTRING> ids;
    std::vector<PFNGETACTIVATIONFACTORY> callbacks;
    names.reserve(class_names.size());
    ids.reserve(class_names.size());
    callbacks.reserve(class_names.size());
    for (const auto* name : class_names) {
        names.emplace_back(name);
        ids.push_back(reinterpret_cast<HSTRING>(winrt::get_abi(names.back())));
        callbacks.push_back(callback);
    }

    RO_REGISTRATION_COOKIE cookie{};
    const HRESULT registration = RoRegisterActivationFactories(
        ids.data(), callbacks.data(), static_cast<UINT32>(ids.size()), &cookie);
    if (FAILED(registration)) {
        std::ofstream log(directory_ / L"component-load-error.txt", std::ios::trunc);
        log << "RoRegisterActivationFactories failed for "
            << std::filesystem::path(file).string() << ": 0x" << std::hex
            << static_cast<unsigned long>(registration) << "\n";
        FreeLibrary(module);
        return false;
    }

    modules_.push_back(module);
    cookies_.push_back(cookie);
    return true;
}

} // namespace aicli::app
