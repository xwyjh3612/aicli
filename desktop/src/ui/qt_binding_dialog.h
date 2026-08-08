#pragma once

#include <QDialog>

#include "app/desktop_controller.h"

class QLabel;
class QPushButton;

namespace aicli::ui::qt {

class BindingDialog final : public QDialog {
    Q_OBJECT
public:
    explicit BindingDialog(app::DesktopController& controller, QWidget* parent = nullptr);
    void UpdatePairing(const agent::PendingPairing& pairing);

private slots:
    void refreshPairing();
    void approvePairing();
    void rejectPairing();

private:
    void renderPairing();
    void confirm(bool approve);

    app::DesktopController& controller_;
    agent::PairingInfo pairing_;
    std::optional<agent::PendingPairing> pending_;
    QLabel* qr_{};
    QLabel* details_{};
    QLabel* device_{};
    QPushButton* approve_{};
    QPushButton* reject_{};
};

} // namespace aicli::ui::qt
