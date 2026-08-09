#pragma once

#include <cstdint>

#include <QMainWindow>
#include <QPointer>

#include "app/desktop_controller.h"

class QLabel;
class QDialog;
class QLineEdit;
class QPushButton;
class QStackedWidget;
class QTimer;

namespace aicli::ui::qt {

class BindingDialog;
class ProjectListWidget;
class TerminalHostWidget;
class TitleBarWidget;

class MainWindow final : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(app::DesktopController& controller);
    ~MainWindow() override;

    bool Initialize(std::wstring* error = nullptr);

protected:
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;
    void resizeEvent(QResizeEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void changeEvent(QEvent* event) override;

private slots:
    void refreshAll();
    void refreshProjectList();
    void refreshProjectDetails();
    void updateTelemetry();
    void projectSelectionChanged();
    void startSelectedProject();
    void createProject();
    void editSelectedProject();
    void deleteSelectedProject();
    void showProjectContextMenu(const QPoint& point);
    void showGroupMenu();
    void createGroup();
    void showBindingDialog();
    void showSettingsDialog(int page = 0);
    void persistProjectOrder();
    void publishTerminalSnapshots();

private:
    void buildUi();
    void wireController();
    void applyTheme();
    void applyNativeWindowStyle();
    void handlePairing(const agent::PendingPairing& pairing);
    int executeModal(QDialog& dialog);
    void setStatus(const std::wstring& text);
    const Project* selectedProject() const;
    std::wstring selectedProjectIdFromList() const;
    void openProjectFolder();
    void moveSelectedProject(const std::wstring& group);
    void stopSelectedProject(const std::wstring& status);
    void prepareTerminalSurface();
    void showRunningProject(const std::wstring& project_id);

    app::DesktopController& controller_;
    QWidget* root_{};
    TitleBarWidget* title_bar_{};
    QLineEdit* search_{};
    ProjectListWidget* project_list_{};
    QPushButton* add_project_{};
    QStackedWidget* workspace_{};
    TerminalHostWidget* terminal_host_{};
    QWidget* prestart_{};
    QLabel* project_title_{};
    QLabel* project_folder_{};
    QLabel* project_command_{};
    QLabel* project_state_{};
    QPushButton* start_project_{};
    QPushButton* edit_project_{};
    QLabel* status_{};
    QLabel* git_status_{};
    QLabel* telemetry_{};
    QTimer* telemetry_timer_{};
    QTimer* terminal_snapshot_timer_{};
    QPointer<BindingDialog> binding_dialog_;
    bool terminal_attached_{};
    bool refreshing_{};
    bool shutting_down_{};
    bool native_frame_configured_{};
    std::uint64_t theme_refresh_serial_{};
};

} // namespace aicli::ui::qt
