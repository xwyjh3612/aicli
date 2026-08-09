#include "ui/qt_settings_dialog.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QStackedWidget>
#include <QVBoxLayout>

#include "ui/qt_widgets.h"

namespace aicli::ui::qt {
namespace {

class NameCommandDialog final : public QDialog {
public:
    NameCommandDialog(const QString& title,
                      const QString& name,
                      const QString& command,
                      QWidget* parent)
        : QDialog(parent) {
        setObjectName(QStringLiteral("nameCommandDialog"));
        setAttribute(Qt::WA_StyledBackground, true);
        setAutoFillBackground(true);
        setWindowTitle(title);
        setModal(true);
        setMinimumWidth(480);
        auto* shell = new QVBoxLayout(this);
        shell->setContentsMargins(0, 0, 0, 0);
        shell->setSpacing(0);
        auto* surface = new QWidget(this);
        surface->setObjectName(QStringLiteral("dialogSurface"));
        surface->setAttribute(Qt::WA_StyledBackground, true);
        surface->setAutoFillBackground(true);
        shell->addWidget(surface);

        auto* root = new QVBoxLayout(surface);
        root->setContentsMargins(24, 22, 24, 20);
        root->setSpacing(14);
        auto* heading = new QLabel(title, surface);
        QFont font = heading->font();
        font.setPixelSize(20);
        font.setWeight(QFont::DemiBold);
        heading->setFont(font);
        root->addWidget(heading);
        auto* form = new QFormLayout();
        form->setVerticalSpacing(12);
        name_ = new QLineEdit(name, surface);
        name_->setMinimumHeight(42);
        name_->setPlaceholderText(QStringLiteral("名称"));
        command_ = new QLineEdit(command, surface);
        command_->setMinimumHeight(42);
        command_->setPlaceholderText(QStringLiteral("启动命令"));
        form->addRow(QStringLiteral("名称"), name_);
        form->addRow(QStringLiteral("启动命令"), command_);
        root->addLayout(form);
        auto* buttons = new QDialogButtonBox(
            QDialogButtonBox::Cancel | QDialogButtonBox::Save, surface);
        buttons->button(QDialogButtonBox::Save)->setObjectName(QStringLiteral("primaryButton"));
        buttons->button(QDialogButtonBox::Save)->setText(QStringLiteral("确定"));
        buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
        for (auto* button : buttons->buttons()) button->setMinimumSize(100, 40);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(buttons, &QDialogButtonBox::accepted, this, [this] {
            if (name_->text().trimmed().isEmpty() || command_->text().trimmed().isEmpty()) {
                name_->setFocus();
                return;
            }
            accept();
        });
        root->addWidget(buttons);
    }

    QString name() const { return name_->text().trimmed(); }
    QString command() const { return command_->text().trimmed(); }

private:
    QLineEdit* name_{};
    QLineEdit* command_{};
};

QWidget* PageWithHeading(const QString& title,
                         const QString& subtitle,
                         QVBoxLayout*& layout,
                         QWidget* parent) {
    auto* page = new QWidget(parent);
    page->setObjectName(QStringLiteral("settingsPage"));
    page->setAttribute(Qt::WA_StyledBackground, true);
    page->setAutoFillBackground(false);
    layout = new QVBoxLayout(page);
    layout->setContentsMargins(28, 24, 28, 24);
    layout->setSpacing(14);
    auto* heading = new QLabel(title, page);
    QFont font = heading->font();
    font.setPixelSize(21);
    font.setWeight(QFont::DemiBold);
    heading->setFont(font);
    layout->addWidget(heading);
    auto* description = new QLabel(subtitle, page);
    description->setObjectName(QStringLiteral("mutedLabel"));
    description->setWordWrap(true);
    layout->addWidget(description);
    return page;
}

QHBoxLayout* ActionRow(QWidget* parent,
                       QPushButton*& add,
                       QPushButton*& edit,
                       QPushButton*& remove) {
    auto* row = new QHBoxLayout();
    row->setSpacing(8);
    add = new QPushButton(QStringLiteral("新增"), parent);
    add->setObjectName(QStringLiteral("primaryButton"));
    edit = new QPushButton(QStringLiteral("编辑"), parent);
    remove = new QPushButton(QStringLiteral("删除"), parent);
    for (auto* button : {add, edit, remove}) button->setMinimumSize(100, 40);
    row->addWidget(add);
    row->addWidget(edit);
    row->addWidget(remove);
    row->addStretch(1);
    return row;
}

} // namespace

SettingsDialog::SettingsDialog(app::DesktopController& controller,
                               ThemeCallback themeChanged,
                               QWidget* parent)
    : QDialog(parent), controller_(controller), theme_changed_(std::move(themeChanged)) {
    setObjectName(QStringLiteral("settingsDialog"));
    setAttribute(Qt::WA_StyledBackground, true);
    // The outer frameless window is translucent. Filling its palette paints a
    // rectangular layer behind settingsSurface, which is especially visible
    // beneath the rounded corners in the light theme.
    setAutoFillBackground(false);
    setWindowTitle(QStringLiteral("设置"));
    setModal(true);
    resize(900, 620);
    setMinimumSize(780, 540);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // One surface owns the complete dialog silhouette.  Header, navigation,
    // and page content are deliberately flat children; giving each child its
    // own radius creates visible seams and double corners at the joins.
    auto* surface = new QWidget(this);
    surface->setObjectName(QStringLiteral("settingsSurface"));
    surface->setAttribute(Qt::WA_StyledBackground, true);
    surface->setAutoFillBackground(true);
    auto* surfaceLayout = new QVBoxLayout(surface);
    surfaceLayout->setContentsMargins(1, 1, 1, 1);
    surfaceLayout->setSpacing(0);
    root->addWidget(surface);

    auto* header = new QWidget(surface);
    header->setObjectName(QStringLiteral("settingsHeader"));
    auto* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(22, 12, 12, 12);
    auto* title = new QLabel(QStringLiteral("设置"), header);
    QFont titleFont = title->font();
    titleFont.setPixelSize(18);
    titleFont.setWeight(QFont::DemiBold);
    title->setFont(titleFont);
    auto* close = new QPushButton(QStringLiteral("×"), header);
    close->setObjectName(QStringLiteral("captionButton"));
    close->setFixedSize(38, 32);
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    headerLayout->addWidget(title);
    headerLayout->addStretch(1);
    headerLayout->addWidget(close);
    surfaceLayout->addWidget(header);

    auto* body = new QWidget(surface);
    body->setObjectName(QStringLiteral("settingsBody"));
    body->setAttribute(Qt::WA_StyledBackground, true);
    body->setAutoFillBackground(false);
    auto* bodyLayout = new QHBoxLayout(body);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(0);
    auto* navigationPanel = new QWidget(body);
    navigationPanel->setObjectName(QStringLiteral("settingsNavigationPanel"));
    navigationPanel->setAttribute(Qt::WA_StyledBackground, true);
    navigationPanel->setAutoFillBackground(true);
    navigationPanel->setFixedWidth(236);
    auto* navigationLayout = new QVBoxLayout(navigationPanel);
    navigationLayout->setContentsMargins(0, 0, 0, 0);
    navigationLayout->setSpacing(0);

    navigation_ = new QListWidget(navigationPanel);
    navigation_->setObjectName(QStringLiteral("settingsNavigation"));
    navigation_->setFrameShape(QFrame::NoFrame);
    navigation_->viewport()->setAutoFillBackground(false);
    navigation_->addItems({QStringLiteral("外观"), QStringLiteral("访问地址"),
                           QStringLiteral("项目类型"), QStringLiteral("分组管理"),
                           QStringLiteral("已绑定设备")});
    navigation_->setCurrentRow(0);
    navigation_->setSpacing(6);
    navigation_->setUniformItemSizes(true);
    QFont navigationFont = navigation_->font();
    navigationFont.setPixelSize(15);
    navigation_->setFont(navigationFont);
    navigationLayout->addWidget(navigation_);
    pages_ = new QStackedWidget(body);
    pages_->setObjectName(QStringLiteral("settingsPages"));
    pages_->setFrameShape(QFrame::NoFrame);
    pages_->setLineWidth(0);
    pages_->setMidLineWidth(0);
    pages_->setAttribute(Qt::WA_StyledBackground, true);
    pages_->setAutoFillBackground(false);
    pages_->addWidget(buildAppearancePage());
    pages_->addWidget(buildNetworkPage());
    pages_->addWidget(buildProjectTypesPage());
    pages_->addWidget(buildGroupsPage());
    pages_->addWidget(buildDevicesPage());
    bodyLayout->addWidget(navigationPanel);
    bodyLayout->addWidget(pages_, 1);
    surfaceLayout->addWidget(body, 1);
    connect(navigation_, &QListWidget::currentRowChanged, this, &SettingsDialog::selectPage);

    reloadProjectTypes();
    reloadGroups();
    reloadDevices();
    refreshThemeButtons();
}

void SettingsDialog::OpenPage(int row) {
    if (row < 0 || row >= navigation_->count()) return;
    navigation_->setCurrentRow(row);
    pages_->setCurrentIndex(row);
}

void SettingsDialog::selectPage(int row) {
    if (row >= 0 && row < pages_->count()) pages_->setCurrentIndex(row);
}

QWidget* SettingsDialog::buildAppearancePage() {
    QVBoxLayout* layout{};
    auto* page = PageWithHeading(
        QStringLiteral("外观"), QStringLiteral("主题会立即应用到主界面、设置和当前终端。"),
        layout, pages_);
    auto* row = new QHBoxLayout();
    dark_theme_ = new QPushButton(QStringLiteral("夜间模式"), page);
    light_theme_ = new QPushButton(QStringLiteral("白天模式"), page);
    dark_theme_->setCheckable(true);
    light_theme_->setCheckable(true);
    row->addWidget(dark_theme_);
    row->addWidget(light_theme_);
    row->addStretch(1);
    layout->addLayout(row);
    layout->addStretch(1);
    connect(dark_theme_, &QPushButton::clicked, this, &SettingsDialog::setDarkTheme);
    connect(light_theme_, &QPushButton::clicked, this, &SettingsDialog::setLightTheme);
    return page;
}

QWidget* SettingsDialog::buildNetworkPage() {
    QVBoxLayout* layout{};
    auto* page = PageWithHeading(
        QStringLiteral("访问地址"),
        QStringLiteral("本地地址用于局域网连接，远程地址用于公网或 Relay。保存后 Agent 会自动重启。"),
        layout, pages_);
    auto* form = new QFormLayout();
    form->setVerticalSpacing(12);
    listen_address_ = new QLineEdit(ToQString(controller_.store().GetSetting(L"listen_address", L"0.0.0.0")), page);
    listen_port_ = new QLineEdit(ToQString(controller_.store().GetSetting(L"listen_port", L"4280")), page);
    local_address_ = new QLineEdit(ToQString(controller_.store().GetSetting(L"local_address")), page);
    remote_address_ = new QLineEdit(ToQString(controller_.store().GetSetting(L"remote_address")), page);
    for (auto* field : {listen_address_, listen_port_, local_address_, remote_address_}) {
        field->setMinimumHeight(42);
    }
    form->addRow(QStringLiteral("监听地址"), listen_address_);
    form->addRow(QStringLiteral("监听端口"), listen_port_);
    form->addRow(QStringLiteral("本地访问地址"), local_address_);
    form->addRow(QStringLiteral("远程访问地址"), remote_address_);
    layout->addLayout(form);
    auto* row = new QHBoxLayout();
    auto* save = new QPushButton(QStringLiteral("保存并重启 Agent"), page);
    save->setObjectName(QStringLiteral("primaryButton"));
    save->setMinimumSize(180, 40);
    network_status_ = new QLabel(page);
    network_status_->setObjectName(QStringLiteral("mutedLabel"));
    row->addWidget(save);
    row->addWidget(network_status_);
    row->addStretch(1);
    layout->addLayout(row);
    layout->addStretch(1);
    connect(save, &QPushButton::clicked, this, &SettingsDialog::saveNetwork);
    return page;
}

QWidget* SettingsDialog::buildProjectTypesPage() {
    QVBoxLayout* layout{};
    auto* page = PageWithHeading(
        QStringLiteral("项目类型"),
        QStringLiteral("Codex 和 Claude 是预置类型，也可以修改名称和启动命令。"),
        layout, pages_);
    project_types_ = new QListWidget(page);
    project_types_->setObjectName(QStringLiteral("settingsProjectTypes"));
    project_types_->setAlternatingRowColors(true);
    project_types_->setUniformItemSizes(true);
    project_types_->setSpacing(4);
    layout->addWidget(project_types_, 1);
    QPushButton *add{}, *edit{}, *remove{};
    layout->addLayout(ActionRow(page, add, edit, remove));
    connect(add, &QPushButton::clicked, this, &SettingsDialog::addProjectType);
    connect(edit, &QPushButton::clicked, this, &SettingsDialog::editProjectType);
    connect(remove, &QPushButton::clicked, this, &SettingsDialog::deleteProjectType);
    connect(project_types_, &QListWidget::itemDoubleClicked, this, [this] { editProjectType(); });
    return page;
}

QWidget* SettingsDialog::buildGroupsPage() {
    QVBoxLayout* layout{};
    auto* page = PageWithHeading(
        QStringLiteral("分组管理"), QStringLiteral("分组用于整理项目，项目可以随时移动到其他分组。"),
        layout, pages_);
    groups_ = new QListWidget(page);
    groups_->setObjectName(QStringLiteral("settingsGroups"));
    groups_->setAlternatingRowColors(true);
    groups_->setUniformItemSizes(true);
    groups_->setSpacing(4);
    layout->addWidget(groups_, 1);
    QPushButton *add{}, *edit{}, *remove{};
    layout->addLayout(ActionRow(page, add, edit, remove));
    connect(add, &QPushButton::clicked, this, &SettingsDialog::addGroup);
    connect(edit, &QPushButton::clicked, this, &SettingsDialog::editGroup);
    connect(remove, &QPushButton::clicked, this, &SettingsDialog::deleteGroup);
    connect(groups_, &QListWidget::itemDoubleClicked, this, [this] { editGroup(); });
    return page;
}

QWidget* SettingsDialog::buildDevicesPage() {
    QVBoxLayout* layout{};
    auto* page = PageWithHeading(
        QStringLiteral("已绑定设备"),
        QStringLiteral("解绑后，该手机保存的私钥将无法再控制此电脑。"), layout, pages_);
    devices_ = new QListWidget(page);
    layout->addWidget(devices_, 1);
    auto* row = new QHBoxLayout();
    auto* refresh = new QPushButton(QStringLiteral("刷新"), page);
    auto* revoke = new QPushButton(QStringLiteral("解绑设备"), page);
    row->addWidget(refresh);
    row->addWidget(revoke);
    row->addStretch(1);
    layout->addLayout(row);
    connect(refresh, &QPushButton::clicked, this, &SettingsDialog::reloadDevices);
    connect(revoke, &QPushButton::clicked, this, &SettingsDialog::revokeDevice);
    return page;
}

void SettingsDialog::setDarkTheme() {
    controller_.SetTheme(false);
    if (theme_changed_) theme_changed_();
    refreshThemeButtons();
}

void SettingsDialog::setLightTheme() {
    controller_.SetTheme(true);
    if (theme_changed_) theme_changed_();
    refreshThemeButtons();
}

void SettingsDialog::refreshThemeButtons() {
    dark_theme_->setChecked(!controller_.lightTheme());
    light_theme_->setChecked(controller_.lightTheme());
    dark_theme_->setObjectName(!controller_.lightTheme() ? QStringLiteral("primaryButton") : QString());
    light_theme_->setObjectName(controller_.lightTheme() ? QStringLiteral("primaryButton") : QString());
    dark_theme_->style()->unpolish(dark_theme_);
    dark_theme_->style()->polish(dark_theme_);
    light_theme_->style()->unpolish(light_theme_);
    light_theme_->style()->polish(light_theme_);
}

void SettingsDialog::saveNetwork() {
    bool portOk = false;
    const int port = listen_port_->text().toInt(&portOk);
    if (!portOk || port < 1 || port > 65535) {
        network_status_->setText(QStringLiteral("端口必须在 1–65535 之间"));
        listen_port_->setFocus();
        return;
    }
    controller_.store().SetSetting(L"listen_address", ToWideString(listen_address_->text().trimmed()));
    controller_.store().SetSetting(L"listen_port", ToWideString(listen_port_->text().trimmed()));
    controller_.store().SetSetting(L"local_address", ToWideString(local_address_->text().trimmed()));
    controller_.store().SetSetting(L"remote_address", ToWideString(remote_address_->text().trimmed()));
    std::wstring error;
    if (!controller_.RestartAgent(controller_.CurrentAgentConfig(), &error)) {
        network_status_->setText(QStringLiteral("保存失败：") + ToQString(error));
        return;
    }
    network_status_->setText(QStringLiteral("已保存，Agent 已重启"));
}

void SettingsDialog::reloadProjectTypes() {
    project_types_->clear();
    for (const auto& type : controller_.store().LoadProjectTypes()) {
        auto* item = new QListWidgetItem(
            ToQString(type.name) + QStringLiteral("\n") + ToQString(type.default_command), project_types_);
        item->setSizeHint(QSize(0, 58));
        item->setData(Qt::UserRole, QVariant::fromValue<qlonglong>(type.id));
        item->setData(Qt::UserRole + 1, type.builtin);
        item->setToolTip(ToQString(type.default_command));
    }
}

void SettingsDialog::reloadGroups() {
    groups_->clear();
    for (const auto& group : controller_.store().LoadGroups()) {
        auto* item = new QListWidgetItem(
            ToQString(group.name) + QStringLiteral("  (") +
                QString::number(controller_.ProjectCount(group.name)) + QStringLiteral(")"), groups_);
        item->setSizeHint(QSize(0, 44));
        item->setData(Qt::UserRole, QVariant::fromValue<qlonglong>(group.id));
        item->setData(Qt::UserRole + 1, group.builtin);
        item->setData(Qt::UserRole + 2, ToQString(group.name));
    }
}

void SettingsDialog::reloadDevices() {
    devices_->clear();
    for (const auto& device : controller_.store().LoadDevices()) {
        auto* item = new QListWidgetItem(
            ToQString(device.name) + QStringLiteral("  ·  ") + ToQString(device.model) +
                QStringLiteral("\n") + ToQString(device.phone_id), devices_);
        item->setData(Qt::UserRole, ToQString(device.phone_id));
    }
}

void SettingsDialog::addProjectType() {
    NameCommandDialog dialog(QStringLiteral("新增项目类型"), {}, {}, this);
    if (dialog.exec() != QDialog::Accepted) return;
    std::wstring error;
    if (!controller_.store().AddProjectType(ToWideString(dialog.name()),
                                             ToWideString(dialog.command()), &error)) {
        showError(error); return;
    }
    controller_.ReloadModels();
    reloadProjectTypes();
}

void SettingsDialog::editProjectType() {
    const int row = project_types_->currentRow();
    const auto types = controller_.store().LoadProjectTypes();
    if (row < 0 || row >= static_cast<int>(types.size())) return;
    const auto& type = types[static_cast<std::size_t>(row)];
    NameCommandDialog dialog(QStringLiteral("编辑项目类型"), ToQString(type.name),
                             ToQString(type.default_command), this);
    if (dialog.exec() != QDialog::Accepted) return;
    std::wstring error;
    if (!controller_.store().UpdateProjectType(type.id, ToWideString(dialog.name()),
                                                ToWideString(dialog.command()), &error)) {
        showError(error); return;
    }
    controller_.ReloadModels();
    reloadProjectTypes();
}

void SettingsDialog::deleteProjectType() {
    const int row = project_types_->currentRow();
    const auto types = controller_.store().LoadProjectTypes();
    if (row < 0 || row >= static_cast<int>(types.size())) return;
    const auto& type = types[static_cast<std::size_t>(row)];
    if (type.builtin) { showError(L"预置项目类型不能删除，但可以编辑"); return; }
    if (QMessageBox::question(this, QStringLiteral("删除项目类型"),
                              QStringLiteral("确定删除“") + ToQString(type.name) + QStringLiteral("”吗？"))
        != QMessageBox::Yes) return;
    std::wstring error;
    if (!controller_.store().DeleteProjectType(type.id, &error)) { showError(error); return; }
    controller_.ReloadModels();
    reloadProjectTypes();
}

void SettingsDialog::addGroup() {
    SingleTextDialog dialog(QStringLiteral("新增分组"), QStringLiteral("分组名称"), {}, this);
    dialog.setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint |
                          Qt::NoDropShadowWindowHint);
    dialog.setAttribute(Qt::WA_TranslucentBackground, true);
    if (dialog.exec() != QDialog::Accepted) return;
    const QString name = dialog.value();
    std::wstring error;
    if (!controller_.store().AddGroup(ToWideString(name), &error)) { showError(error); return; }
    controller_.ReloadModels();
    reloadGroups();
}

void SettingsDialog::editGroup() {
    const int row = groups_->currentRow();
    const auto values = controller_.store().LoadGroups();
    if (row < 0 || row >= static_cast<int>(values.size())) return;
    const auto& group = values[static_cast<std::size_t>(row)];
    SingleTextDialog dialog(QStringLiteral("编辑分组"), QStringLiteral("分组名称"),
                            ToQString(group.name), this);
    dialog.setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint |
                          Qt::NoDropShadowWindowHint);
    dialog.setAttribute(Qt::WA_TranslucentBackground, true);
    if (dialog.exec() != QDialog::Accepted) return;
    const QString name = dialog.value();
    std::wstring error;
    if (!controller_.store().RenameGroup(group.id, ToWideString(name), &error)) { showError(error); return; }
    controller_.ReloadModels();
    reloadGroups();
}

void SettingsDialog::deleteGroup() {
    const int row = groups_->currentRow();
    const auto values = controller_.store().LoadGroups();
    if (row < 0 || row >= static_cast<int>(values.size())) return;
    const auto& group = values[static_cast<std::size_t>(row)];
    if (group.builtin) { showError(L"默认分组不能删除，但可以编辑名称"); return; }
    if (QMessageBox::question(this, QStringLiteral("删除分组"),
                              QStringLiteral("确定删除“") + ToQString(group.name) + QStringLiteral("”吗？"))
        != QMessageBox::Yes) return;
    std::wstring error;
    if (!controller_.store().DeleteGroup(group.id, &error)) { showError(error); return; }
    controller_.ReloadModels();
    reloadGroups();
}

void SettingsDialog::revokeDevice() {
    auto* item = devices_->currentItem();
    if (!item) return;
    const QString id = item->data(Qt::UserRole).toString();
    if (QMessageBox::question(this, QStringLiteral("解绑设备"),
                              QStringLiteral("解绑后该手机将立即失去控制权限，确定继续吗？"))
        != QMessageBox::Yes) return;
    if (!controller_.agent().RevokeDevice(ToWideString(id))) {
        showError(L"解绑失败"); return;
    }
    reloadDevices();
}

void SettingsDialog::showError(const std::wstring& message) {
    QMessageBox::warning(this, QStringLiteral("AI CLI"), ToQString(message));
}

} // namespace aicli::ui::qt
