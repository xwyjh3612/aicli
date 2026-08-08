#include "app/application_activation.h"

#include <windows.h>
#include <appmodel.h>
#include <shobjidl.h>

namespace aicli::app {

int EnsurePackagedActivation(const wchar_t* application_user_model_id,
                              const wchar_t* arguments) {
    UINT32 length = 0;
    const LONG result = GetCurrentPackageFullName(&length, nullptr);
    if (result != APPMODEL_ERROR_NO_PACKAGE) return 1;

    IApplicationActivationManager* manager = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ApplicationActivationManager, nullptr,
                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&manager))) ||
        !manager) {
        return -1;
    }

    DWORD process_id = 0;
    const HRESULT activated = manager->ActivateApplication(
        application_user_model_id,
        arguments && *arguments ? arguments : nullptr,
        AO_NONE,
        &process_id);
    manager->Release();
    return SUCCEEDED(activated) ? 0 : -1;
}

} // namespace aicli::app
