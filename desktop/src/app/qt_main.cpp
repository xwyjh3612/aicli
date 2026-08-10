#include <windows.h>

#include <QAbstractNativeEventFilter>
#include <QApplication>
#include <QIcon>
#include <QImage>
#include <QMessageBox>
#include <QPixmap>

#include <winrt/base.h>

#include "aicli_build_identity.h"
#include "app/application_activation.h"
#include "app/desktop_controller.h"
#include "app/resource.h"
#include "ui/qt_main_window.h"
#include "ui/qt_widgets.h"

namespace {
constexpr wchar_t kApplicationUserModelId[] = AICLI_APPLICATION_USER_MODEL_ID;

class TerminalShortcutFilter final : public QAbstractNativeEventFilter {
public:
    explicit TerminalShortcutFilter(
        aicli::app::DesktopController& controller) noexcept
        : controller_(controller) {}

    bool nativeEventFilter(const QByteArray&,
                           void* message,
                           qintptr* result) override {
        const auto* native = static_cast<const MSG*>(message);
        if (!native || !controller_.PreTranslateTerminalMessage(*native)) {
            return false;
        }
        if (result) *result = 0;
        return true;
    }

private:
    aicli::app::DesktopController& controller_;
};
}

int WINAPI wWinMain(HINSTANCE instance,
                    HINSTANCE,
                    PWSTR arguments,
                    int show_command) {
    winrt::init_apartment(winrt::apartment_type::single_threaded);

    int activation = 1;
#ifndef AICLI_SKIP_PACKAGED_ACTIVATION
    activation = aicli::app::EnsurePackagedActivation(
        kApplicationUserModelId, arguments);
#else
    (void)arguments;
#endif
    if (activation < 0) {
        MessageBoxW(nullptr,
                    L"无法启动已注册的 AI CLI 桌面实例，请先完成包注册。",
                    L"AI CLI", MB_OK | MB_ICONERROR);
        winrt::uninit_apartment();
        return 1;
    }
    if (activation == 0) {
        winrt::uninit_apartment();
        return 0;
    }

    // Register the side-by-side WinUI/Windows Terminal activation factories
    // before Qt creates its first native window. RoRegisterActivationFactories
    // can no longer register Microsoft.UI.Xaml reliably after the Qt Windows
    // platform plugin has initialized HWND/COM services.
    aicli::app::DesktopController controller;
    std::wstring runtime_error;
    if (!controller.PrepareRuntime(&runtime_error)) {
        MessageBoxW(nullptr,
                    runtime_error.empty() ? L"Windows Terminal 组件加载失败"
                                          : runtime_error.c_str(),
                    L"AI CLI", MB_OK | MB_ICONERROR);
        winrt::uninit_apartment();
        return 1;
    }

    QApplication application(__argc, __argv);
    application.setApplicationName(QStringLiteral("AI CLI"));
    application.setOrganizationName(QStringLiteral("AI CLI"));
    QApplication::setStyle(QStringLiteral("Fusion"));
    if (const auto icon = static_cast<HICON>(LoadImageW(
            instance, MAKEINTRESOURCEW(IDI_AICLI_APP), IMAGE_ICON,
            GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON),
            LR_DEFAULTCOLOR | LR_SHARED))) {
        application.setWindowIcon(
            QIcon(QPixmap::fromImage(QImage::fromHICON(icon))));
    }

    aicli::ui::qt::MainWindow window(controller);
    std::wstring error;
    if (!window.Initialize(&error)) {
        QMessageBox::critical(nullptr, QStringLiteral("AI CLI"),
                              aicli::ui::qt::ToQString(
                                  error.empty() ? L"桌面端初始化失败" : error));
        controller.Shutdown();
        winrt::uninit_apartment();
        return 1;
    }

    // TermControl keyboard focus is hosted below Qt's HWND hierarchy.  A
    // process-wide native filter is the Qt equivalent of the legacy Win32
    // message-loop pre-translation stage: Ctrl+V pastes, while Ctrl+C copies
    // only when the terminal has a selection and otherwise remains SIGINT.
    TerminalShortcutFilter terminal_shortcuts(controller);
    application.installNativeEventFilter(&terminal_shortcuts);

    if (show_command == SW_SHOWMAXIMIZED) window.showMaximized();
    else window.show();
    const int result = application.exec();
    application.removeNativeEventFilter(&terminal_shortcuts);
    controller.Shutdown();
    winrt::uninit_apartment();
    return result;
}
