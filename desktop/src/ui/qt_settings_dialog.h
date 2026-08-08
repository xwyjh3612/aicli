#pragma once

#include <QDialog>

#include <functional>

#include "app/desktop_controller.h"

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QStackedWidget;

namespace aicli::ui::qt {

class SettingsDialog final : public QDialog {
    Q_OBJECT
public:
    using ThemeCallback = std::function<void()>;

    SettingsDialog(app::DesktopController& controller,
                   ThemeCallback themeChanged,
                   QWidget* parent = nullptr);
    void OpenPage(int row);

private slots:
    void selectPage(int row);
    void setDarkTheme();
    void setLightTheme();
    void saveNetwork();
    void addProjectType();
    void editProjectType();
    void deleteProjectType();
    void addGroup();
    void editGroup();
    void deleteGroup();
    void revokeDevice();

private:
    QWidget* buildAppearancePage();
    QWidget* buildNetworkPage();
    QWidget* buildProjectTypesPage();
    QWidget* buildGroupsPage();
    QWidget* buildDevicesPage();
    void reloadProjectTypes();
    void reloadGroups();
    void reloadDevices();
    void refreshThemeButtons();
    void showError(const std::wstring& message);

    app::DesktopController& controller_;
    ThemeCallback theme_changed_;
    QListWidget* navigation_{};
    QStackedWidget* pages_{};
    QPushButton* dark_theme_{};
    QPushButton* light_theme_{};
    QLineEdit* listen_address_{};
    QLineEdit* listen_port_{};
    QLineEdit* local_address_{};
    QLineEdit* remote_address_{};
    QLabel* network_status_{};
    QListWidget* project_types_{};
    QListWidget* groups_{};
    QListWidget* devices_{};
};

} // namespace aicli::ui::qt
