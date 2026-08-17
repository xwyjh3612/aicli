#include "ui/qt_project_dialog.h"

#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QComboBox>

#include "ui/qt_widgets.h"

namespace aicli::ui::qt {

ProjectDialog::ProjectDialog(app::DesktopController& controller,
                             QWidget* parent,
                             const Project* existing)
    : QDialog(parent), controller_(controller), editing_(existing != nullptr) {
    setObjectName(QStringLiteral("projectDialog"));
    setAttribute(Qt::WA_StyledBackground, true);
    setAutoFillBackground(true);
    setWindowTitle(editing_ ? QStringLiteral("编辑项目") : QStringLiteral("新增项目"));
    setModal(true);
    setMinimumWidth(560);
    setSizeGripEnabled(false);

    if (existing) draft_ = *existing;

    auto* shell = new QVBoxLayout(this);
    shell->setContentsMargins(0, 0, 0, 0);
    shell->setSpacing(0);
    auto* surface = new QWidget(this);
    surface->setObjectName(QStringLiteral("dialogSurface"));
    surface->setAttribute(Qt::WA_StyledBackground, true);
    surface->setAutoFillBackground(true);
    shell->addWidget(surface);

    auto* root = new QVBoxLayout(surface);
    root->setContentsMargins(28, 24, 28, 22);
    root->setSpacing(16);

    auto* heading = new QLabel(editing_ ? QStringLiteral("编辑项目") : QStringLiteral("创建项目"), surface);
    QFont headingFont = heading->font();
    headingFont.setPixelSize(21);
    headingFont.setWeight(QFont::DemiBold);
    heading->setFont(headingFont);
    root->addWidget(heading);

    auto* subtitle = new QLabel(
        editing_ ? QStringLiteral("修改项目名称、分组、目录和启动命令")
                 : QStringLiteral("选择项目类型后，启动命令会自动填充，可按需修改"), surface);
    subtitle->setObjectName(QStringLiteral("mutedLabel"));
    root->addWidget(subtitle);

    auto* form = new QFormLayout();
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    form->setFormAlignment(Qt::AlignTop);
    form->setHorizontalSpacing(16);
    form->setVerticalSpacing(12);

    name_ = new QLineEdit(surface);
    name_->setMinimumHeight(42);
    name_->setPlaceholderText(QStringLiteral("例如：默认 Codex 会话"));
    name_->setObjectName(QStringLiteral("dialogField"));
    form->addRow(QStringLiteral("项目名称"), name_);

    type_ = new QComboBox(surface);
    type_->setMinimumHeight(42);
    type_->setObjectName(QStringLiteral("dialogCombo"));
    for (const auto& type : controller_.projectTypes()) {
        type_->addItem(ToQString(type.name), QVariant::fromValue<qlonglong>(type.id));
    }
    form->addRow(QStringLiteral("项目类型"), type_);

    group_ = new QComboBox(surface);
    group_->setMinimumHeight(42);
    group_->setObjectName(QStringLiteral("dialogCombo"));
    for (const auto& group : controller_.groups()) group_->addItem(ToQString(group.name));
    form->addRow(QStringLiteral("分组"), group_);

    auto* folderRow = new QWidget(surface);
    auto* folderLayout = new QHBoxLayout(folderRow);
    folderLayout->setContentsMargins(0, 0, 0, 0);
    folderLayout->setSpacing(8);
    folder_ = new QLineEdit(folderRow);
    folder_->setMinimumHeight(42);
    folder_->setPlaceholderText(QStringLiteral("选择项目文件夹"));
    auto* browse = new QPushButton(QStringLiteral("选择文件夹"), folderRow);
    browse->setObjectName(QStringLiteral("toolbarButton"));
    browse->setMinimumHeight(40);
    browse->setFixedWidth(112);
    folderLayout->addWidget(folder_, 1);
    folderLayout->addWidget(browse);
    connect(browse, &QPushButton::clicked, this, &ProjectDialog::browseFolder);
    form->addRow(QStringLiteral("项目目录"), folderRow);

    command_ = new QLineEdit(surface);
    command_->setMinimumHeight(42);
    command_->setPlaceholderText(QStringLiteral("启动命令"));
    form->addRow(QStringLiteral("启动命令"), command_);
    root->addLayout(form);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Cancel | QDialogButtonBox::Save, surface);
    buttons->button(QDialogButtonBox::Save)->setObjectName(QStringLiteral("primaryButton"));
    buttons->button(QDialogButtonBox::Save)->setText(QStringLiteral("确定"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    for (auto* button : buttons->buttons()) button->setMinimumSize(100, 40);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, this, &ProjectDialog::acceptDialog);
    root->addWidget(buttons);

    if (existing) {
        name_->setText(ToQString(existing->name));
        folder_->setText(ToQString(existing->folder));
        command_->setText(ToQString(existing->command));
        const int typeIndex = type_->findText(ToQString(existing->type));
        if (typeIndex >= 0) type_->setCurrentIndex(typeIndex);
        const int groupIndex = group_->findText(ToQString(existing->group.empty() ? L"默认分组" : existing->group));
        if (groupIndex >= 0) group_->setCurrentIndex(groupIndex);
        type_->setEnabled(false);
    } else if (type_->count() > 0) {
        const auto defaultTypeId = controller_.store().DefaultProjectTypeId();
        const int defaultIndex = type_->findData(
            QVariant::fromValue<qlonglong>(defaultTypeId));
        if (defaultIndex >= 0) type_->setCurrentIndex(defaultIndex);
        projectTypeChanged(type_->currentIndex());
    }
    previousCommand_ = command_->text();
    connect(type_, qOverload<int>(&QComboBox::currentIndexChanged), this,
            &ProjectDialog::projectTypeChanged);
    name_->setFocus();
}

void ProjectDialog::browseFolder() {
    const QString value = QFileDialog::getExistingDirectory(
        this, QStringLiteral("选择项目文件夹"), folder_->text());
    if (!value.isEmpty()) folder_->setText(value);
}

void ProjectDialog::projectTypeChanged(int index) {
    if (editing_ || index < 0 || index >= controller_.projectTypes().size()) return;
    const auto& type = controller_.projectTypes()[static_cast<std::size_t>(index)];
    if (command_->text().isEmpty() || command_->text() == previousCommand_) {
        command_->setText(ToQString(type.default_command));
    }
    previousCommand_ = command_->text();
}

void ProjectDialog::acceptDialog() {
    if (name_->text().trimmed().isEmpty()) {
        name_->setFocus();
        return;
    }
    draft_.name = ToWideString(name_->text().trimmed());
    draft_.type = ToWideString(type_->currentText());
    draft_.group = ToWideString(group_->currentText());
    draft_.folder = ToWideString(folder_->text().trimmed());
    draft_.command = ToWideString(command_->text().trimmed());
    if (draft_.command.empty()) draft_.command = controller_.DefaultCommandForType(draft_.type);
    accept();
}

} // namespace aicli::ui::qt
