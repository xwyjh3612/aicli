#include "ui/qt_binding_dialog.h"

#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QVBoxLayout>

#include <nlohmann/json.hpp>
#include <qrcodegen.hpp>

#include "core/text_utils.h"
#include "ui/qt_widgets.h"

namespace aicli::ui::qt {

BindingDialog::BindingDialog(app::DesktopController& controller, QWidget* parent)
    : QDialog(parent), controller_(controller) {
    setObjectName(QStringLiteral("bindingDialog"));
    setAttribute(Qt::WA_StyledBackground, true);
    // executeModal() makes the top-level dialog translucent.  Keep that outer
    // window transparent and let one styled child own the complete modal
    // background/border, matching the project and settings dialogs.
    setAutoFillBackground(false);
    setWindowTitle(QStringLiteral("绑定 Android 手机"));
    setModal(true);
    resize(820, 560);
    setMinimumSize(720, 520);

    auto* shell = new QVBoxLayout(this);
    shell->setContentsMargins(0, 0, 0, 0);
    shell->setSpacing(0);
    auto* surface = new QWidget(this);
    surface->setObjectName(QStringLiteral("bindingSurface"));
    surface->setAttribute(Qt::WA_StyledBackground, true);
    surface->setAutoFillBackground(true);
    shell->addWidget(surface);

    auto* root = new QVBoxLayout(surface);
    root->setContentsMargins(28, 24, 28, 22);
    root->setSpacing(12);
    auto* heading = new QLabel(QStringLiteral("绑定 Android 手机"), surface);
    QFont font = heading->font();
    font.setPixelSize(21);
    font.setWeight(QFont::DemiBold);
    heading->setFont(font);
    root->addWidget(heading);
    auto* subtitle = new QLabel(
        QStringLiteral("请使用 AI CLI App 内的扫码入口；电脑只需确认这一次。"), surface);
    subtitle->setObjectName(QStringLiteral("mutedLabel"));
    root->addWidget(subtitle);

    auto* content = new QHBoxLayout();
    content->setSpacing(28);
    qr_ = new QLabel(surface);
    qr_->setFixedSize(360, 360);
    qr_->setAlignment(Qt::AlignCenter);
    qr_->setStyleSheet(QStringLiteral("background:#f5f7fa;border-radius:8px;"));
    content->addWidget(qr_);

    auto* information = new QVBoxLayout();
    information->setSpacing(10);
    auto* wait = new QLabel(QStringLiteral("等待手机扫码"), surface);
    QFont waitFont = wait->font();
    waitFont.setPixelSize(17);
    waitFont.setWeight(QFont::DemiBold);
    wait->setFont(waitFont);
    information->addWidget(wait);
    details_ = new QLabel(surface);
    details_->setObjectName(QStringLiteral("mutedLabel"));
    details_->setWordWrap(true);
    information->addWidget(details_);
    device_ = new QLabel(surface);
    device_->setWordWrap(true);
    information->addWidget(device_);
    information->addStretch(1);
    content->addLayout(information, 1);
    root->addLayout(content, 1);

    auto* actions = new QHBoxLayout();
    auto* refresh = new QPushButton(QStringLiteral("刷新二维码"), surface);
    approve_ = new QPushButton(QStringLiteral("确认绑定"), surface);
    approve_->setObjectName(QStringLiteral("primaryButton"));
    reject_ = new QPushButton(QStringLiteral("拒绝"), surface);
    auto* close = new QPushButton(QStringLiteral("关闭"), surface);
    actions->addWidget(refresh);
    actions->addStretch(1);
    actions->addWidget(approve_);
    actions->addWidget(reject_);
    actions->addWidget(close);
    root->addLayout(actions);

    connect(refresh, &QPushButton::clicked, this, &BindingDialog::refreshPairing);
    connect(approve_, &QPushButton::clicked, this, &BindingDialog::approvePairing);
    connect(reject_, &QPushButton::clicked, this, &BindingDialog::rejectPairing);
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    refreshPairing();
}

void BindingDialog::refreshPairing() {
    pending_.reset();
    pairing_ = controller_.agent().CreatePairing();
    renderPairing();
}

void BindingDialog::renderPairing() {
    try {
        const auto payload = aicli::text::Utf8Text(pairing_.qr_payload);
        const auto code = qrcodegen::QrCode::encodeText(payload.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
        constexpr int imageSize = 360;
        constexpr int quiet = 4;
        const int modules = code.getSize();
        const int scale = std::max(1, imageSize / (modules + quiet * 2));
        const int actual = scale * (modules + quiet * 2);
        QImage image(actual, actual, QImage::Format_RGB32);
        image.fill(QColor(245, 247, 250));
        for (int y = 0; y < modules; ++y) {
            for (int x = 0; x < modules; ++x) {
                if (!code.getModule(x, y)) continue;
                for (int py = 0; py < scale; ++py) {
                    for (int px = 0; px < scale; ++px) {
                        image.setPixelColor((x + quiet) * scale + px,
                                            (y + quiet) * scale + py,
                                            QColor(18, 20, 24));
                    }
                }
            }
        }
        qr_->setPixmap(QPixmap::fromImage(image).scaled(
            qr_->size(), Qt::KeepAspectRatio, Qt::FastTransformation));
    } catch (...) {
        qr_->setText(QStringLiteral("二维码生成失败"));
    }

    std::wstring local = controller_.store().GetSetting(L"local_address");
    if (local.empty()) {
        try {
            const auto json = nlohmann::json::parse(aicli::text::Utf8Text(pairing_.qr_payload));
            local = aicli::text::WideText(json.value("localAddress", std::string{}));
        } catch (...) {}
    }
    auto fingerprint = pairing_.certificate_fingerprint;
    if (fingerprint.size() > 28) fingerprint = fingerprint.substr(0, 28) + L"…";
    details_->setText(
        QStringLiteral("本地地址\n") + ToQString(local) +
        QStringLiteral("\n\n证书指纹\n") + ToQString(fingerprint) +
        QStringLiteral("\n\n二维码 5 分钟内有效。已绑定手机断网或电脑重启后不需要重新确认。"));
    approve_->setVisible(false);
    reject_->setVisible(false);
    device_->setText(QStringLiteral("手机扫码并提交设备信息后，将在这里显示确认信息。"));
}

void BindingDialog::UpdatePairing(const agent::PendingPairing& pairing) {
    if (pairing.info.pairing_id != pairing_.pairing_id) return;
    pending_ = pairing;
    const QString state = pairing.authorized ? QStringLiteral("绑定完成")
        : pairing.rejected ? QStringLiteral("已拒绝") : QStringLiteral("等待电脑确认");
    device_->setText(state + QStringLiteral("\n\n设备：") + ToQString(pairing.device.name) +
                     QStringLiteral("\n型号：") + ToQString(pairing.device.model));
    const bool canConfirm = pairing.submitted && !pairing.authorized && !pairing.rejected;
    approve_->setVisible(canConfirm);
    reject_->setVisible(canConfirm);
}

void BindingDialog::approvePairing() { confirm(true); }
void BindingDialog::rejectPairing() { confirm(false); }

void BindingDialog::confirm(bool approve) {
    if (!pending_) return;
    std::wstring error;
    if (!controller_.agent().ConfirmPairing(pending_->info.pairing_id, approve, &error)) {
        QMessageBox::warning(this, QStringLiteral("手机绑定"), ToQString(error));
        return;
    }
    pending_->authorized = approve;
    pending_->rejected = !approve;
    UpdatePairing(*pending_);
}

} // namespace aicli::ui::qt
