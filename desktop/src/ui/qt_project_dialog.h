#pragma once

#include <QDialog>

#include "app/desktop_controller.h"

class QComboBox;
class QLineEdit;

namespace aicli::ui::qt {

class ProjectDialog final : public QDialog {
    Q_OBJECT
public:
    ProjectDialog(app::DesktopController& controller,
                  QWidget* parent,
                  const Project* existing = nullptr);

    const Project& project() const noexcept { return draft_; }

private slots:
    void browseFolder();
    void projectTypeChanged(int index);
    void acceptDialog();

private:
    app::DesktopController& controller_;
    Project draft_;
    bool editing_{};
    QString previousCommand_;
    QLineEdit* name_{};
    QComboBox* type_{};
    QComboBox* group_{};
    QLineEdit* folder_{};
    QLineEdit* command_{};
};

} // namespace aicli::ui::qt
