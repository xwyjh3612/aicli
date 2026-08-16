#include "ui/qt_settings_dialog.h"

#include <QDialogButtonBox>
#include <QButtonGroup>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPushButton>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QWindow>

#include <initializer_list>

#include "core/text_utils.h"
#include "features/mobile_project_creation.h"
#include "ui/qt_widgets.h"

namespace aicli::ui::qt {
namespace {

class DialogTitleBar final : public QWidget {
public:
    explicit DialogTitleBar(QWidget* parent = nullptr) : QWidget(parent) {
        setObjectName(QStringLiteral("settingsHeader"));
        setAttribute(Qt::WA_StyledBackground, true);
        setAutoFillBackground(true);
        setFixedHeight(56);
        setMouseTracking(true);
    }

protected:
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton) {
            QWidget::mousePressEvent(event);
            return;
        }
        if (auto* top = window(); top && top->windowHandle() &&
            top->windowHandle()->startSystemMove()) {
            dragging_ = false;
            event->accept();
            return;
        }
        dragging_ = true;
        drag_offset_ = event->globalPosition().toPoint() - window()->frameGeometry().topLeft();
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        if (dragging_ && (event->buttons() & Qt::LeftButton)) {
            window()->move(event->globalPosition().toPoint() - drag_offset_);
            event->accept();
            return;
        }
        QWidget::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton) dragging_ = false;
        QWidget::mouseReleaseEvent(event);
    }

private:
    bool dragging_{};
    QPoint drag_offset_{};
};

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
    if (!subtitle.isEmpty()) {
        auto* description = new QLabel(subtitle, page);
        description->setObjectName(QStringLiteral("mutedLabel"));
        description->setWordWrap(true);
        layout->addWidget(description);
    }
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

    // One surface owns the complete dialog silhouette. The inset header only
    // repeats the matching top radii so its distinct chrome fill remains
    // inside that silhouette; navigation and page content stay flat.
    auto* surface = new QWidget(this);
    surface->setObjectName(QStringLiteral("settingsSurface"));
    surface->setAttribute(Qt::WA_StyledBackground, true);
    surface->setAutoFillBackground(true);
    auto* surfaceLayout = new QVBoxLayout(surface);
    surfaceLayout->setContentsMargins(1, 1, 1, 1);
    surfaceLayout->setSpacing(0);
    root->addWidget(surface);

    auto* header = new DialogTitleBar(surface);
    auto* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(22, 12, 12, 12);
    auto* title = new QLabel(QStringLiteral("设置"), header);
    title->setObjectName(QStringLiteral("settingsTitle"));
    title->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    QFont titleFont = title->font();
    titleFont.setPixelSize(18);
    titleFont.setWeight(QFont::DemiBold);
    title->setFont(titleFont);
    auto* close = new QPushButton(QStringLiteral("×"), header);
    close->setObjectName(QStringLiteral("dialogCloseButton"));
    close->setFixedSize(38, 32);
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    headerLayout->addWidget(title);
    headerLayout->addStretch(1);
    headerLayout->addWidget(close);
    surfaceLayout->addWidget(header);
    auto* headerSeparator = new QFrame(surface);
    headerSeparator->setObjectName(QStringLiteral("settingsHeaderSeparator"));
    headerSeparator->setFrameShape(QFrame::NoFrame);
    headerSeparator->setFixedHeight(1);
    headerSeparator->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    surfaceLayout->addWidget(headerSeparator);

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
    navigation_->addItems({QStringLiteral("外观"), QStringLiteral("本机与网络"),
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
    auto* page = PageWithHeading(QStringLiteral("外观设置"), QString(), layout, pages_);

    auto* card = new QFrame(page);
    card->setObjectName(QStringLiteral("settingsCard"));
    card->setAttribute(Qt::WA_StyledBackground, true);
    auto* cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(20, 8, 20, 8);
    cardLayout->setSpacing(0);

    const auto prepareChoice = [](QPushButton* button, int minimumWidth = 94) {
        button->setCheckable(true);
        button->setProperty("settingsChoice", true);
        button->setMinimumWidth(minimumWidth);
        button->setCursor(Qt::PointingHandCursor);
    };
    const auto addRow = [card, cardLayout](
        const QString& text, std::initializer_list<QPushButton*> buttons,
        bool divider) {
        auto* row = new QWidget(card);
        row->setObjectName(QStringLiteral("settingsCardRow"));
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 8, 0, 8);
        rowLayout->setSpacing(8);
        auto* label = new QLabel(text, row);
        label->setObjectName(QStringLiteral("settingsRowLabel"));
        rowLayout->addWidget(label);
        rowLayout->addStretch(1);
        for (auto* button : buttons) rowLayout->addWidget(button);
        cardLayout->addWidget(row);
        if (divider) {
            auto* line = new QFrame(card);
            line->setObjectName(QStringLiteral("settingsCardDivider"));
            line->setFrameShape(QFrame::NoFrame);
            line->setFixedHeight(1);
            cardLayout->addWidget(line);
        }
    };

    dark_theme_ = new QPushButton(QStringLiteral("夜间模式"), page);
    light_theme_ = new QPushButton(QStringLiteral("白天模式"), page);
    prepareChoice(dark_theme_);
    prepareChoice(light_theme_);

    taskbar_working_ = new QPushButton(page);
    prepareChoice(taskbar_working_, 86);
    taskbar_working_->setChecked(
        controller_.store().GetSetting(L"taskbar_working_enabled", L"1") != L"0");
    taskbar_working_->setText(taskbar_working_->isChecked()
        ? QStringLiteral("开启") : QStringLiteral("关闭"));

    taskbar_style_progress_ = new QPushButton(QStringLiteral("进度条"), page);
    taskbar_style_overlay_ = new QPushButton(QStringLiteral("图标转圈"), page);
    prepareChoice(taskbar_style_progress_);
    prepareChoice(taskbar_style_overlay_, 104);
    const bool overlayStyle = controller_.store().GetSetting(
        L"taskbar_working_style", L"progress") == L"overlay";
    taskbar_style_progress_->setChecked(!overlayStyle);
    taskbar_style_overlay_->setChecked(overlayStyle);
    auto* styleGroup = new QButtonGroup(page);
    styleGroup->addButton(taskbar_style_progress_);
    styleGroup->addButton(taskbar_style_overlay_);
    taskbar_scope_current_ = new QPushButton(QStringLiteral("当前项目"), page);
    taskbar_scope_all_ = new QPushButton(QStringLiteral("所有项目"), page);
    prepareChoice(taskbar_scope_current_);
    prepareChoice(taskbar_scope_all_);
    const bool currentOnly = controller_.store().GetSetting(
        L"taskbar_working_scope", L"all") == L"current";
    taskbar_scope_current_->setChecked(currentOnly);
    taskbar_scope_all_->setChecked(!currentOnly);
    auto* scopeGroup = new QButtonGroup(page);
    scopeGroup->addButton(taskbar_scope_current_);
    scopeGroup->addButton(taskbar_scope_all_);
    addRow(QStringLiteral("应用主题"), {dark_theme_, light_theme_}, true);
    addRow(QStringLiteral("任务栏缓冲动画"), {taskbar_working_}, true);
    addRow(QStringLiteral("缓冲样式"),
           {taskbar_style_progress_, taskbar_style_overlay_}, true);
    addRow(QStringLiteral("作用范围"),
           {taskbar_scope_current_, taskbar_scope_all_}, false);
    layout->addWidget(card);
    layout->addStretch(1);
    connect(dark_theme_, &QPushButton::clicked, this, &SettingsDialog::setDarkTheme);
    connect(light_theme_, &QPushButton::clicked, this, &SettingsDialog::setLightTheme);
    connect(taskbar_working_, &QPushButton::clicked,
            this, &SettingsDialog::saveTaskbarWorkingPreference);
    connect(taskbar_style_progress_, &QPushButton::clicked,
            this, &SettingsDialog::saveTaskbarWorkingPreference);
    connect(taskbar_style_overlay_, &QPushButton::clicked,
            this, &SettingsDialog::saveTaskbarWorkingPreference);
    connect(taskbar_scope_current_, &QPushButton::clicked,
            this, &SettingsDialog::saveTaskbarWorkingPreference);
    connect(taskbar_scope_all_, &QPushButton::clicked,
            this, &SettingsDialog::saveTaskbarWorkingPreference);
    return page;
}

QWidget* SettingsDialog::buildNetworkPage() {
    QVBoxLayout* layout{};
    auto* page = PageWithHeading(
        QStringLiteral("本机与访问地址"),
        QStringLiteral("配置手机访问服务、访问地址和手机工作目录。"),
        layout, pages_);
    auto* form = new QFormLayout();
    form->setVerticalSpacing(12);
    computer_name_ = new QLineEdit(ToQString(controller_.store().GetSetting(
        L"computer_name", aicli::text::LocalComputerName())), page);
    listen_address_ = new QLineEdit(ToQString(controller_.store().GetSetting(L"listen_address", L"0.0.0.0")), page);
    listen_port_ = new QLineEdit(ToQString(controller_.store().GetSetting(L"listen_port", L"4280")), page);
    local_address_ = new QLineEdit(ToQString(controller_.store().GetSetting(L"local_address")), page);
    remote_address_ = new QLineEdit(ToQString(controller_.store().GetSetting(L"remote_address")), page);
    mobile_workspace_root_ = new QLineEdit(ToQString(
        mobile_projects::WorkspaceDirectory(controller_.store()).wstring()), page);
    mobile_workspace_root_->setPlaceholderText(QStringLiteral("选择手机工作目录"));
    for (auto* field : {computer_name_, listen_address_, listen_port_, local_address_,
                        remote_address_, mobile_workspace_root_}) {
        field->setMinimumHeight(42);
    }
    computer_name_->setMaxLength(64);
    form->addRow(QStringLiteral("本机名称"), computer_name_);
    form->addRow(QStringLiteral("监听地址"), listen_address_);
    form->addRow(QStringLiteral("监听端口"), listen_port_);
    form->addRow(QStringLiteral("本地访问地址"), local_address_);
    form->addRow(QStringLiteral("远程访问地址"), remote_address_);
    auto* mobileRootRow = new QWidget(page);
    auto* mobileRootLayout = new QHBoxLayout(mobileRootRow);
    mobileRootLayout->setContentsMargins(0, 0, 0, 0);
    mobileRootLayout->setSpacing(8);
    auto* browseMobileRoot = new QPushButton(QStringLiteral("选择"), mobileRootRow);
    browseMobileRoot->setMinimumSize(86, 42);
    mobileRootLayout->addWidget(mobile_workspace_root_, 1);
    mobileRootLayout->addWidget(browseMobileRoot);
    form->addRow(QStringLiteral("手机工作目录"), mobileRootRow);
    mobile_access_ = new QPushButton(page);
    mobile_access_->setCheckable(true);
    mobile_access_->setChecked(aicli::agent::IsMobileAccessEnabled(controller_.store()));
    mobile_access_->setText(mobile_access_->isChecked()
        ? QStringLiteral("开启") : QStringLiteral("关闭"));
    mobile_access_->setCursor(Qt::PointingHandCursor);
    mobile_access_->setMinimumSize(96, 40);
    form->addRow(QStringLiteral("手机访问服务"), mobile_access_);
    layout->addLayout(form);
    auto* row = new QHBoxLayout();
    auto* save = new QPushButton(QStringLiteral("保存设置"), page);
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
    connect(mobile_access_, &QPushButton::toggled, this, [this](bool enabled) {
        mobile_access_->setText(enabled ? QStringLiteral("开启")
                                        : QStringLiteral("关闭"));
    });
    connect(browseMobileRoot, &QPushButton::clicked, this, [this] {
        const QString selected = QFileDialog::getExistingDirectory(
            this, QStringLiteral("选择手机工作目录"),
            mobile_workspace_root_->text().trimmed());
        if (!selected.isEmpty()) {
            mobile_workspace_root_->setText(QDir::toNativeSeparators(selected));
        }
    });
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

void SettingsDialog::saveTaskbarWorkingPreference() {
    if (!taskbar_working_ || !taskbar_scope_current_ || !taskbar_style_overlay_) return;
    taskbar_working_->setText(taskbar_working_->isChecked()
        ? QStringLiteral("开启") : QStringLiteral("关闭"));
    controller_.SetTaskbarWorkingPreference(
        taskbar_working_->isChecked(), taskbar_scope_current_->isChecked(),
        taskbar_style_overlay_->isChecked());
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
    if (taskbar_working_) {
        taskbar_working_->setText(taskbar_working_->isChecked()
            ? QStringLiteral("开启") : QStringLiteral("关闭"));
    }
}

void SettingsDialog::saveNetwork() {
    const QString computerName = computer_name_->text().trimmed();
    if (computerName.isEmpty()) {
        network_status_->setText(QStringLiteral("本机名称不能为空"));
        computer_name_->setFocus();
        return;
    }
    bool portOk = false;
    const int port = listen_port_->text().toInt(&portOk);
    if (!portOk || port < 1 || port > 65535) {
        network_status_->setText(QStringLiteral("端口必须在 1–65535 之间"));
        listen_port_->setFocus();
        return;
    }
    QString mobileWorkspaceRoot = mobile_workspace_root_->text().trimmed();
    if (!mobileWorkspaceRoot.isEmpty()) {
        mobileWorkspaceRoot = QDir::cleanPath(mobileWorkspaceRoot);
    }
    if ((!mobileWorkspaceRoot.isEmpty() &&
         !QDir::isAbsolutePath(mobileWorkspaceRoot)) ||
        (mobile_access_->isChecked() && mobileWorkspaceRoot.isEmpty())) {
        network_status_->setText(
            mobile_access_->isChecked() && mobileWorkspaceRoot.isEmpty()
                ? QStringLiteral("开启手机访问前请选择手机工作目录")
                : QStringLiteral("手机工作目录必须是有效的绝对路径"));
        mobile_workspace_root_->setFocus();
        return;
    }
    controller_.store().SetSetting(L"computer_name", ToWideString(computerName));
    controller_.store().SetSetting(L"listen_address", ToWideString(listen_address_->text().trimmed()));
    controller_.store().SetSetting(L"listen_port", ToWideString(listen_port_->text().trimmed()));
    controller_.store().SetSetting(L"local_address", ToWideString(local_address_->text().trimmed()));
    controller_.store().SetSetting(L"remote_address", ToWideString(remote_address_->text().trimmed()));
    controller_.store().SetSetting(
        L"mobile_workspace_root",
        mobileWorkspaceRoot.isEmpty()
            ? L"" : ToWideString(QDir::toNativeSeparators(mobileWorkspaceRoot)));
    aicli::agent::SetMobileAccessEnabled(controller_.store(), mobile_access_->isChecked());
    std::wstring error;
    if (!controller_.RestartAgent(controller_.CurrentAgentConfig(), &error)) {
        network_status_->setText(QStringLiteral("保存失败：") + ToQString(error));
        return;
    }
    network_status_->setText(mobile_access_->isChecked()
        ? QStringLiteral("已保存，手机访问服务已启动")
        : QStringLiteral("已保存，手机访问服务已关闭"));
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
