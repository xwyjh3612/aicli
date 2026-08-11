#include "ui/qt_main_window.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDialog>
#include <QEventLoop>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPalette>
#include <QPainter>
#include <QPushButton>
#include <QResizeEvent>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <dwmapi.h>
#include <imm.h>
#include <windowsx.h>

#include <algorithm>
#include <filesystem>

#include "features/git_repository.h"
#include "ui/qt_binding_dialog.h"
#include "ui/qt_project_dialog.h"
#include "ui/qt_settings_dialog.h"
#include "ui/qt_widgets.h"

namespace aicli::ui::qt {
namespace {

constexpr int kTitleBarHeight = 52;
constexpr int kFooterHeight = 30;
constexpr int kSidebarWidth = 286;
// Windows 11's documented sentinel for suppressing the DWM-drawn outer
// border while retaining shadow, rounded corners, and native resize support.
constexpr COLORREF kDwmColorNone = static_cast<COLORREF>(0xFFFFFFFEu);

class WorkspaceScrim final : public QWidget {
public:
    WorkspaceScrim(QWidget* owner, bool lightTheme)
        // Keep the scrim inside the main window.  The previous implementation
        // used Qt::Tool, which creates a second top-level HWND.  Showing that
        // window briefly deactivates/reactivates the owner and DWM repaints
        // the owner's border (visible as a one-frame bright flash when a
        // modal is opened).  A native child still paints above the terminal
        // child window, but never participates in window activation.
        : QWidget(owner),
          color_(0, 0, 0, lightTheme ? 88 : 138) {
        setAttribute(Qt::WA_TranslucentBackground, true);
        setAttribute(Qt::WA_ShowWithoutActivating, true);
        setAttribute(Qt::WA_NativeWindow, true);
        // The scrim is visual-only.  It must never become the hit-test target
        // for the modal above it; otherwise a second-level dialog can look
        // enabled while every input/button click is silently swallowed.
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
        setFocusPolicy(Qt::NoFocus);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(rect(), color_);
    }

private:
    QColor color_;
};

QColor Color(const ThemePalette& theme, COLORREF ThemePalette::*member) {
    return ToQColor(theme.*member);
}

} // namespace

MainWindow::MainWindow(app::DesktopController& controller)
    : controller_(controller) {
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint |
                   Qt::WindowMinimizeButtonHint | Qt::WindowMaximizeButtonHint |
                   Qt::WindowSystemMenuHint);
    setAttribute(Qt::WA_NativeWindow, true);
    setWindowTitle(QStringLiteral("AI CLI"));
    setMinimumSize(900, 600);
    resize(1100, 700);
    buildUi();
    wireController();
}

MainWindow::~MainWindow() {
    if (ime_filter_installed_ && qApp) {
        qApp->removeEventFilter(this);
        ime_filter_installed_ = false;
    }
    // DesktopController is owned by qt_main.cpp and is shut down after the
    // Qt event loop has returned.  Waiting for CivetWeb, ConPTY and
    // TermControl here would keep the native window visible while their
    // worker threads are being joined.
}

void MainWindow::buildUi() {
    root_ = new QWidget(this);
    root_->setObjectName(QStringLiteral("qtRoot"));
    setCentralWidget(root_);
    auto* rootLayout = new QVBoxLayout(root_);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    title_bar_ = new TitleBarWidget(root_);
    rootLayout->addWidget(title_bar_);
    auto* titleBarSeparator = new QFrame(root_);
    titleBarSeparator->setObjectName(QStringLiteral("titleBarSeparator"));
    titleBarSeparator->setFrameShape(QFrame::NoFrame);
    titleBarSeparator->setFixedHeight(1);
    titleBarSeparator->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    rootLayout->addWidget(titleBarSeparator);

    auto* content = new QWidget(root_);
    content->setObjectName(QStringLiteral("mainSurface"));
    auto* contentLayout = new QHBoxLayout(content);
    // Keep the XAML Island off the outermost right edge. HTTRANSPARENT from a
    // DesktopWindowXamlSource child does not reliably bubble through its Qt
    // native-host ancestors, so this small logical-pixel strip guarantees the
    // top-level frame itself owns the initial right-edge resize gesture.
    contentLayout->setContentsMargins(0, 0, 2, 0);
    contentLayout->setSpacing(0);

    auto* sidebar = new QWidget(content);
    sidebar->setObjectName(QStringLiteral("sidebar"));
    sidebar->setFixedWidth(kSidebarWidth);
    auto* sidebarLayout = new QVBoxLayout(sidebar);
    sidebarLayout->setContentsMargins(6, 6, 6, 6);
    sidebarLayout->setSpacing(6);

    search_ = new QLineEdit(sidebar);
    search_->setPlaceholderText(QStringLiteral("搜索会话"));
    search_->setClearButtonEnabled(true);
    search_->setFixedHeight(38);
    // The application-wide IME coordinator restores Windows text-input focus
    // for this and every other Qt editor after TermControl was active.
    search_->setAttribute(Qt::WA_InputMethodEnabled, true);
    search_->setInputMethodHints(Qt::ImhNone);
    sidebarLayout->addWidget(search_);

    auto* sidebarHeader = new QHBoxLayout();
    sidebarHeader->setContentsMargins(3, 0, 0, 0);
    auto* sidebarTitle = new QLabel(QStringLiteral("项目列表"), sidebar);
    sidebarTitle->setObjectName(QStringLiteral("sidebarTitle"));
    QFont sidebarFont = sidebarTitle->font();
    sidebarFont.setPixelSize(16);
    sidebarTitle->setFont(sidebarFont);
    add_project_ = new QPushButton(QStringLiteral("+"), sidebar);
    add_project_->setObjectName(QStringLiteral("primaryButton"));
    add_project_->setFixedSize(36, 34);
    sidebarHeader->addWidget(sidebarTitle);
    sidebarHeader->addStretch(1);
    sidebarHeader->addWidget(add_project_);
    sidebarLayout->addLayout(sidebarHeader);

    project_list_ = new ProjectListWidget(sidebar);
    project_list_->setObjectName(QStringLiteral("projectList"));
    sidebarLayout->addWidget(project_list_, 1);
    contentLayout->addWidget(sidebar);

    workspace_ = new QStackedWidget(content);
    workspace_->setObjectName(QStringLiteral("workspace"));
    workspace_->setFrameShape(QFrame::NoFrame);
    workspace_->setLineWidth(0);
    workspace_->setMidLineWidth(0);
    workspace_->setContentsMargins(0, 0, 0, 0);
    terminal_host_ = new TerminalHostWidget(workspace_);
    terminal_host_->setObjectName(QStringLiteral("terminalHost"));
    terminal_host_->setContentsMargins(0, 0, 0, 0);
    workspace_->addWidget(terminal_host_);

    prestart_ = new QWidget(workspace_);
    auto* prestartLayout = new QVBoxLayout(prestart_);
    prestartLayout->setContentsMargins(28, 28, 28, 28);
    prestartLayout->setSpacing(14);
    project_title_ = new QLabel(QStringLiteral("选择一个项目"), prestart_);
    QFont projectTitleFont = project_title_->font();
    projectTitleFont.setPixelSize(22);
    projectTitleFont.setWeight(QFont::DemiBold);
    project_title_->setFont(projectTitleFont);
    project_folder_ = new QLabel(QStringLiteral("文件夹：未设置"), prestart_);
    project_command_ = new QLabel(QStringLiteral("启动命令：未设置"), prestart_);
    project_state_ = new QLabel(QStringLiteral("状态：未运行"), prestart_);
    for (auto* label : {project_folder_, project_command_, project_state_}) {
        label->setObjectName(QStringLiteral("mutedLabel"));
        label->setWordWrap(true);
    }
    prestartLayout->addWidget(project_title_);
    prestartLayout->addSpacing(24);
    prestartLayout->addWidget(project_folder_);
    prestartLayout->addWidget(project_command_);
    prestartLayout->addWidget(project_state_);
    prestartLayout->addStretch(1);
    auto* prestartActions = new QHBoxLayout();
    start_project_ = new QPushButton(QStringLiteral("立即开始"), prestart_);
    start_project_->setObjectName(QStringLiteral("primaryButton"));
    start_project_->setFixedSize(112, 38);
    edit_project_ = new QPushButton(QStringLiteral("编辑项目"), prestart_);
    edit_project_->setFixedSize(112, 38);
    prestartActions->addWidget(start_project_);
    prestartActions->addWidget(edit_project_);
    prestartActions->addStretch(1);
    prestartLayout->addLayout(prestartActions);
    workspace_->addWidget(prestart_);
    contentLayout->addWidget(workspace_, 1);
    rootLayout->addWidget(content, 1);

    auto* footer = new QWidget(root_);
    footer->setObjectName(QStringLiteral("footer"));
    footer->setFixedHeight(kFooterHeight);
    auto* footerLayout = new QHBoxLayout(footer);
    footerLayout->setContentsMargins(14, 0, 14, 0);
    footerLayout->setSpacing(14);
    status_ = new QLabel(QStringLiteral("就绪"), footer);
    status_->setObjectName(QStringLiteral("footerLabel"));
    git_status_ = new QLabel(QStringLiteral("git 不可用"), footer);
    git_status_->setObjectName(QStringLiteral("footerLabel"));
    telemetry_ = new QLabel(QStringLiteral("Agent 离线  |  内存 0 MB  |  公网未配置"), footer);
    telemetry_->setObjectName(QStringLiteral("footerLabel"));
    footerLayout->addWidget(status_, 1);
    footerLayout->addWidget(git_status_);
    footerLayout->addWidget(telemetry_);
    rootLayout->addWidget(footer);

    connect(title_bar_->minimizeButton(), &QPushButton::clicked,
            this, &QWidget::showMinimized);
    connect(title_bar_->maximizeButton(), &QPushButton::clicked, this, [this] {
        if (isMaximized()) showNormal(); else showMaximized();
    });
    connect(title_bar_->closeButton(), &QPushButton::clicked,
            this, &QWidget::close);
    connect(title_bar_->groupButton(), &QPushButton::clicked, this, &MainWindow::showGroupMenu);
    connect(title_bar_->addGroupButton(), &QPushButton::clicked, this, &MainWindow::createGroup);
    connect(title_bar_->bindButton(), &QPushButton::clicked, this, &MainWindow::showBindingDialog);
    connect(title_bar_->devicesButton(), &QPushButton::clicked, this, [this] { showSettingsDialog(4); });
    connect(title_bar_->settingsButton(), &QPushButton::clicked, this, [this] { showSettingsDialog(0); });
    connect(add_project_, &QPushButton::clicked, this, &MainWindow::createProject);
    connect(edit_project_, &QPushButton::clicked, this, &MainWindow::editSelectedProject);
    connect(start_project_, &QPushButton::clicked, this, &MainWindow::startSelectedProject);
    connect(search_, &QLineEdit::textChanged, this, &MainWindow::refreshProjectList);
    connect(project_list_, &QListWidget::currentRowChanged, this, &MainWindow::projectSelectionChanged);
    connect(project_list_, &QListWidget::itemDoubleClicked, this, [this] { startSelectedProject(); });
    connect(project_list_, &ProjectListWidget::customContextMenuRequested,
            this, &MainWindow::showProjectContextMenu);
    connect(project_list_, &ProjectListWidget::orderChanged,
            this, &MainWindow::persistProjectOrder);
    connect(project_list_, &ProjectListWidget::selectionCommitted,
            this, [this](const QString& projectId) {
                if (refreshing_ || projectId.isEmpty()) return;
                controller_.SetSelectedProject(ToWideString(projectId));
                refreshProjectDetails();
            });
    connect(terminal_host_, &TerminalHostWidget::hostResized, this,
            [this](int width, int height) {
                if (terminal_attached_) controller_.ResizeTerminal(width, height);
            });
}

void MainWindow::wireController() {
    controller_.SetStateChangedCallback([this] {
        if (shutting_down_) return;
        QMetaObject::invokeMethod(this, &MainWindow::refreshAll, Qt::QueuedConnection);
    });
    controller_.SetStatusCallback([this](const std::wstring& text) {
        if (shutting_down_) return;
        QMetaObject::invokeMethod(this, [this, text] { setStatus(text); }, Qt::QueuedConnection);
    });
    controller_.SetPairingCallback([this](const agent::PendingPairing& pairing) {
        if (shutting_down_) return;
        QMetaObject::invokeMethod(this, [this, pairing] { handlePairing(pairing); }, Qt::QueuedConnection);
    });
    controller_.SetAgentCallback([this](bool, const std::wstring&) {
        if (shutting_down_) return;
        QMetaObject::invokeMethod(this, &MainWindow::updateTelemetry, Qt::QueuedConnection);
    });
}

bool MainWindow::Initialize(std::wstring* error) {
    const HWND window = reinterpret_cast<HWND>(winId());
    taskbar_button_created_message_ = RegisterWindowMessageW(L"TaskbarButtonCreated");
    if (taskbar_button_created_message_) {
        ChangeWindowMessageFilterEx(window, taskbar_button_created_message_,
                                    MSGFLT_ALLOW, nullptr);
    }
    taskbar_working_indicator_.Attach(window);
    applyNativeWindowStyle();
    // Capture Qt's working input context before WindowsXamlManager and
    // TermControl initialize text services on this UI thread. The XAML stack
    // changes the default compatibility context used by Qt's IMM32 backend;
    // retaining the original context lets Qt editors restore it on focus.
    captureQtInputContext(window);
    if (!controller_.Initialize(window, error)) return false;

    const HWND terminalParent = reinterpret_cast<HWND>(terminal_host_->winId());
    terminal_attached_ = controller_.AttachTerminal(
        terminalParent, std::max(1, terminal_host_->width()),
        std::max(1, terminal_host_->height()), error);
    if (!terminal_attached_) {
        if (error && error->empty()) *error = L"无法初始化 Windows Terminal 控件";
        return false;
    }

    applyTheme();
    refreshAll();
    telemetry_timer_ = new QTimer(this);
    telemetry_timer_->setInterval(2000);
    connect(telemetry_timer_, &QTimer::timeout, this, &MainWindow::updateTelemetry);
    telemetry_timer_->start();

    terminal_snapshot_timer_ = new QTimer(this);
    terminal_snapshot_timer_->setInterval(200);
    connect(terminal_snapshot_timer_, &QTimer::timeout,
            this, &MainWindow::publishTerminalSnapshots);

    // Prime Qt's native input context on the first event-loop turn, before the
    // delayed terminal-session restore takes focus below. Without this first
    // activation, TermControl can remain the only initialized text-service
    // client on the shared UI thread and every later QLineEdit receives raw
    // Latin keystrokes. When there is no terminal to restore, leaving search
    // focused is the natural initial state.
    QTimer::singleShot(0, this, [this] {
        if (search_) search_->setFocus(Qt::OtherFocusReason);
    });

    QTimer::singleShot(160, this, [this] {
        const auto id = controller_.selectedProjectId();
        if (id.empty()) return;

        // Make Qt finish showing and sizing the native terminal host before
        // TermControl is created.  Starting it while QStackedWidget still has
        // the pre-start page visible lets ConPTY run, but the first composed
        // frame can remain black until the session is restarted.
        prepareTerminalSurface();
        setStatus(L"正在恢复上次会话…");
        QTimer::singleShot(0, this, [this, id] {
            if (shutting_down_ || controller_.selectedProjectId() != id) {
                refreshAll();
                return;
            }
            if (controller_.StartProject(id)) showRunningProject(id);
            refreshAll();
        });
    });
    return true;
}

void MainWindow::applyTheme() {
    const auto& theme = controller_.theme();
    QPalette palette;
    palette.setColor(QPalette::Window, ToQColor(theme.window));
    palette.setColor(QPalette::WindowText, ToQColor(theme.text));
    palette.setColor(QPalette::Base, ToQColor(theme.panel));
    palette.setColor(QPalette::AlternateBase, ToQColor(theme.hover));
    palette.setColor(QPalette::Text, ToQColor(theme.text));
    palette.setColor(QPalette::Button, ToQColor(theme.surface));
    palette.setColor(QPalette::ButtonText, ToQColor(theme.text));
    palette.setColor(QPalette::Highlight, ToQColor(theme.selection));
    palette.setColor(QPalette::HighlightedText, ToQColor(theme.text));
    palette.setColor(QPalette::Link, ToQColor(theme.accent));
    palette.setColor(QPalette::Mid, ToQColor(theme.border));
    palette.setColor(QPalette::PlaceholderText, ToQColor(theme.muted));
    palette.setColor(QPalette::ToolTipBase, ToQColor(theme.surface));
    palette.setColor(QPalette::ToolTipText, ToQColor(theme.text));
    qApp->setPalette(palette);
    qApp->setStyleSheet(BuildApplicationStyle(theme));
    applyNativeWindowStyle();
    // The delegate uses this semantic surface instead of inheriting the
    // platform's (often black) QPalette::Base.  Keep it explicit so both
    // themes render unselected project cards with a deliberate background.
    project_list_->setProperty("projectItemSurface", ToQColor(theme.list_surface));
    project_list_->setProperty("projectItemText", ToQColor(theme.list_text));
    QPalette listPalette = project_list_->palette();
    listPalette.setColor(QPalette::Base, ToQColor(theme.list_surface));
    listPalette.setColor(QPalette::Window, ToQColor(theme.list_surface));
    listPalette.setColor(QPalette::Text, ToQColor(theme.list_text));
    listPalette.setColor(QPalette::AlternateBase, ToQColor(theme.hover));
    project_list_->setPalette(listPalette);
    project_list_->viewport()->update();

    // The native XAML child is laid out again by Qt after this function
    // returns (the settings dialog also queues a state refresh).  Repaint the
    // active Codex viewport once that pass is complete.  Coalesce the two
    // applyTheme() calls made while the modal is open and after it closes.
    const auto refresh_serial = ++theme_refresh_serial_;
    const bool expected_light_theme = controller_.lightTheme();
    QTimer::singleShot(180, this, [this, refresh_serial, expected_light_theme] {
        if (shutting_down_ || refresh_serial != theme_refresh_serial_ ||
            controller_.lightTheme() != expected_light_theme) {
            return;
        }
        controller_.RefreshTerminalViewport();
    });
}

void MainWindow::applyNativeWindowStyle() {
    const HWND window = reinterpret_cast<HWND>(winId());
    if (!window) return;

    LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
    const LONG_PTR requiredStyle = style | WS_THICKFRAME | WS_MINIMIZEBOX |
                                   WS_MAXIMIZEBOX | WS_SYSMENU;
    const bool styleChanged = style != requiredStyle;
    if (styleChanged) SetWindowLongPtrW(window, GWL_STYLE, requiredStyle);
    if (styleChanged || !native_frame_configured_) {
        const MARGINS margins{0, 0, 0, 0};
        DwmExtendFrameIntoClientArea(window, &margins);
        SetWindowPos(window, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                         SWP_NOACTIVATE | SWP_FRAMECHANGED);
        native_frame_configured_ = true;
    }

    const BOOL dark = controller_.lightTheme() ? FALSE : TRUE;
    DwmSetWindowAttribute(window, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    DwmSetWindowAttribute(window, DWMWA_BORDER_COLOR, &kDwmColorNone, sizeof(kDwmColorNone));
    const DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_ROUND;
    DwmSetWindowAttribute(window, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
}

void MainWindow::captureQtInputContext(HWND window) {
    if (!window || !IsWindow(window)) return;
    if (const HIMC context = ImmGetContext(window)) {
        qt_input_context_ = context;
        ImmReleaseContext(window, context);
    }
    if (qt_input_context_ && !ime_filter_installed_ && qApp) {
        qApp->installEventFilter(this);
        ime_filter_installed_ = true;
    }
}

void MainWindow::restoreQtInputContext(QWidget* editor) {
    if (!qt_input_context_ || !editor || restoring_ime_) return;
    QWidget* top_level = editor->window();
    if (!top_level) return;

    const HWND target = reinterpret_cast<HWND>(top_level->winId());
    if (!target || !IsWindow(target)) return;

    restoring_ime_ = true;
    // Qt keeps logical focus on the child editor while Windows routes IME
    // messages through its top-level HWND. Native focus must be back on that
    // HWND before the preserved context is associated; otherwise a no-op
    // SetFocus can leave the TermControl context active.
    if (GetFocus() != target) SetFocus(target);
    ImmAssociateContext(target, qt_input_context_);
    restoring_ime_ = false;
}

void MainWindow::refreshAll() {
    if (refreshing_ || shutting_down_) return;
    refreshing_ = true;
    title_bar_->groupButton()->setText(
        ToQString(controller_.group()) + QStringLiteral(" (") +
        QString::number(controller_.ProjectCount(controller_.group())) + QStringLiteral(")  ▾"));
    title_bar_->devicesButton()->setText(
        QStringLiteral("已绑定 ") + QString::number(controller_.BoundDeviceCount()) + QStringLiteral(" 台"));
    taskbar_working_indicator_.SetWorking(controller_.AnyProjectWorking());
    refreshing_ = false;
    refreshProjectList();
    refreshProjectDetails();
    updateTelemetry();
}

void MainWindow::refreshProjectList() {
    if (refreshing_ || shutting_down_) return;
    refreshing_ = true;
    const QSignalBlocker blocker(project_list_);
    const std::wstring preferred = controller_.selectedProjectId();
    const bool filtering = !search_->text().trimmed().isEmpty();
    project_list_->clear();
    const auto indices = controller_.VisibleProjectIndices(ToWideString(search_->text()));
    int selectedRow = -1;
    bool hasVisibleWorking = false;
    for (int projectIndex : indices) {
        if (projectIndex < 0 || projectIndex >= static_cast<int>(controller_.projects().size())) continue;
        const auto& project = controller_.projects()[static_cast<std::size_t>(projectIndex)];
        auto* item = new QListWidgetItem(ToQString(project.name), project_list_);
        const bool working = controller_.IsProjectWorking(project.id);
        item->setData(ProjectIdRole, ToQString(project.id));
        item->setData(ProjectStatusRole, ToQString(project.status));
        item->setData(ProjectSessionRole, controller_.HasSession(project.id));
        item->setData(ProjectWorkingRole, working);
        hasVisibleWorking = hasVisibleWorking || working;
        item->setToolTip(ToQString(project.folder));
        if (project.id == preferred) selectedRow = project_list_->count() - 1;
    }
    project_list_->SetWorkingAnimationActive(hasVisibleWorking);
    project_list_->SetDragEnabledForQuery(search_->text().trimmed().isEmpty());
    // While a user is composing a search query (notably with a Chinese IME),
    // rebuilding the filtered list must not silently activate its first row.
    // Activating another TermControl transfers TSF focus away from QLineEdit
    // and cancels the composition. Leave the list unselected until the user
    // explicitly clicks a result; clearing the query restores normal selection.
    if (selectedRow < 0 && project_list_->count() > 0 && !filtering) selectedRow = 0;
    project_list_->setCurrentRow(selectedRow);
    const std::wstring nextId = selectedRow >= 0
        ? ToWideString(project_list_->item(selectedRow)->data(ProjectIdRole).toString())
        : std::wstring{};
    refreshing_ = false;
    if (!filtering && nextId != controller_.selectedProjectId()) {
        controller_.SetSelectedProject(nextId);
    }
}

const Project* MainWindow::selectedProject() const {
    return controller_.FindProject(controller_.selectedProjectId());
}

std::wstring MainWindow::selectedProjectIdFromList() const {
    auto* item = project_list_->currentItem();
    return item ? ToWideString(item->data(ProjectIdRole).toString()) : std::wstring{};
}

void MainWindow::refreshProjectDetails() {
    const auto* project = selectedProject();
    if (!project) {
        workspace_->setCurrentWidget(prestart_);
        controller_.SetTerminalVisible(false);
        project_title_->setText(QStringLiteral("未选择项目"));
        project_folder_->setText(QStringLiteral("文件夹：未设置"));
        project_command_->setText(QStringLiteral("启动命令：未设置"));
        project_state_->setText(QStringLiteral("状态：未运行"));
        start_project_->setEnabled(false);
        edit_project_->setEnabled(false);
        return;
    }

    const bool running = controller_.HasSession(project->id);
    if (running) {
        workspace_->setCurrentWidget(terminal_host_);
        controller_.SetTerminalVisible(true);
        controller_.ResizeTerminal(terminal_host_->width(), terminal_host_->height());
    } else {
        workspace_->setCurrentWidget(prestart_);
        controller_.SetTerminalVisible(false);
    }
    project_title_->setText(ToQString(project->name));
    project_folder_->setText(QStringLiteral("文件夹：") +
                             (project->folder.empty() ? QStringLiteral("未设置") : ToQString(project->folder)));
    project_command_->setText(QStringLiteral("启动命令：") +
                              (project->command.empty()
                                   ? ToQString(controller_.DefaultCommandForType(project->type))
                                   : ToQString(project->command)));
    const QString state = running ? QStringLiteral("运行中")
        : project->status == L"paused" ? QStringLiteral("已暂停") : QStringLiteral("未运行");
    project_state_->setText(QStringLiteral("状态：") + state);
    start_project_->setText(project->status == L"paused" ? QStringLiteral("继续") : QStringLiteral("立即开始"));
    start_project_->setEnabled(true);
    edit_project_->setEnabled(true);

    const auto git = controller_.GitStatus(project->folder);
    QString gitLabel;
    switch (git.state) {
    case aicli::git::RepositoryState::NotInitialized: gitLabel = QStringLiteral("git 未创建"); break;
    case aicli::git::RepositoryState::NoCommits: gitLabel = QStringLiteral("git 未提交"); break;
    case aicli::git::RepositoryState::HasCommits: gitLabel = QStringLiteral("git 已提交"); break;
    default: gitLabel = QStringLiteral("git 不可用"); break;
    }
    git_status_->setText(gitLabel);
    git_status_->setToolTip(
        QStringLiteral("Git 目录：") + (git.root.empty() ? QStringLiteral("无") : ToQString(git.root)) +
        QStringLiteral("\n最后推送：") + (git.last_push.empty() ? QStringLiteral("未知") : ToQString(git.last_push)) +
        QStringLiteral("\n最后拉取：") + (git.last_pull.empty() ? QStringLiteral("未知") : ToQString(git.last_pull)));
}

void MainWindow::projectSelectionChanged() {
    if (refreshing_) return;
    const auto id = selectedProjectIdFromList();
    controller_.SetSelectedProject(id);
    refreshProjectDetails();
}

void MainWindow::startSelectedProject() {
    auto id = selectedProjectIdFromList();
    if (id.empty()) id = controller_.selectedProjectId();
    if (id.empty()) return;

    // Keep the action tied to the row the user actually clicked. State-change
    // notifications rebuild the list asynchronously, so relying only on the
    // controller's previous selection can briefly start another project.
    if (id != controller_.selectedProjectId()) controller_.SetSelectedProject(id);

    start_project_->setEnabled(false);
    start_project_->setText(QStringLiteral("正在启动…"));
    setStatus(L"正在启动终端…");
    QApplication::setOverrideCursor(Qt::WaitCursor);
    prepareTerminalSurface();
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    const bool started = controller_.StartProject(id);
    QApplication::restoreOverrideCursor();

    if (!started) {
        QMessageBox::warning(this, QStringLiteral("启动项目"),
                             QStringLiteral("终端启动失败，请查看底部状态。"));
        refreshAll();
        return;
    }

    // A successful StartProject already owns a real TermControl/ConPTY
    // session. Switch the Qt stack immediately instead of waiting for a
    // queued state refresh; otherwise the process starts behind the prestart
    // page and the button appears to have done nothing.
    showRunningProject(id);
    setStatus(L"原生终端已连接");
    refreshAll();
}

void MainWindow::createProject() {
    ProjectDialog dialog(controller_, this);
    if (executeModal(dialog) != QDialog::Accepted) return;
    std::wstring error;
    if (!controller_.SaveProject(dialog.project(), &error)) {
        QMessageBox::warning(this, QStringLiteral("保存项目"), ToQString(error));
        return;
    }
    setStatus(L"项目已创建    |    选择右侧立即开始");
    refreshAll();
}

void MainWindow::editSelectedProject() {
    const auto* project = selectedProject();
    if (!project) return;
    ProjectDialog dialog(controller_, this, project);
    if (executeModal(dialog) != QDialog::Accepted) return;
    std::wstring error;
    if (!controller_.SaveProject(dialog.project(), &error)) {
        QMessageBox::warning(this, QStringLiteral("保存项目"), ToQString(error));
        return;
    }
    setStatus(L"项目配置已更新");
    refreshAll();
}

void MainWindow::deleteSelectedProject() {
    const auto* project = selectedProject();
    if (!project) return;
    QString text = QStringLiteral("确定从 AI CLI 中删除项目“") + ToQString(project->name) +
        QStringLiteral("”吗？\n\n只会删除项目配置，不会删除项目文件夹。" );
    if (controller_.HasSession(project->id)) {
        text.insert(text.indexOf(QStringLiteral("\n\n")),
                    QStringLiteral("\n\n该项目正在运行，删除后会同时结束终端会话。"));
    }
    if (QMessageBox::question(this, QStringLiteral("删除项目"), text) != QMessageBox::Yes) return;
    const auto name = project->name;
    std::wstring error;
    if (!controller_.DeleteProject(project->id, &error)) {
        QMessageBox::warning(this, QStringLiteral("删除项目"), ToQString(error));
        return;
    }
    setStatus(L"项目“" + name + L"”已删除，项目文件夹未受影响");
    refreshAll();
}

void MainWindow::showProjectContextMenu(const QPoint& point) {
    auto* item = project_list_->itemAt(point);
    if (!item) return;
    project_list_->setCurrentItem(item);
    projectSelectionChanged();
    const auto* project = selectedProject();
    if (!project) return;

    QMenu menu(this);
    QAction* start = menu.addAction(controller_.HasSession(project->id)
        ? QStringLiteral("激活会话") : QStringLiteral("开始 / 继续"));
    QAction* restart = menu.addAction(QStringLiteral("重新开始"));
    QAction* pause = nullptr;
    if (controller_.HasSession(project->id)) {
        pause = menu.addAction(QStringLiteral("暂停"));
    }
    menu.addSeparator();
    QAction* edit = menu.addAction(QStringLiteral("编辑项目"));
    QAction* open = menu.addAction(QStringLiteral("打开项目文件夹"));
    auto* move = menu.addMenu(QStringLiteral("移动到分组"));
    for (const auto& group : controller_.groups()) {
        QAction* action = move->addAction(ToQString(group.name));
        connect(action, &QAction::triggered, this, [this, name = group.name] { moveSelectedProject(name); });
    }
    QAction* remove = menu.addAction(QStringLiteral("删除项目"));
    QAction* chosen = menu.exec(project_list_->viewport()->mapToGlobal(point));
    if (chosen == start) startSelectedProject();
    else if (chosen == restart) controller_.StartProject(project->id, true);
    else if (pause && chosen == pause) pauseSelectedProject();
    else if (chosen == edit) editSelectedProject();
    else if (chosen == open) openProjectFolder();
    else if (chosen == remove) deleteSelectedProject();
}

void MainWindow::showGroupMenu() {
    QMenu menu(this);
    QAction* all = menu.addAction(
        QStringLiteral("全部 (") + QString::number(controller_.ProjectCount(L"全部")) + QStringLiteral(")"));
    all->setCheckable(true);
    all->setChecked(controller_.group() == L"全部");
    connect(all, &QAction::triggered, this, [this] { controller_.SetGroup(L"全部"); });
    menu.addSeparator();
    for (const auto& group : controller_.groups()) {
        QAction* action = menu.addAction(
            ToQString(group.name) + QStringLiteral(" (") +
            QString::number(controller_.ProjectCount(group.name)) + QStringLiteral(")"));
        action->setCheckable(true);
        action->setChecked(controller_.group() == group.name);
        connect(action, &QAction::triggered, this, [this, name = group.name] {
            controller_.SetGroup(name);
        });
    }
    menu.addSeparator();
    QAction* manage = menu.addAction(QStringLiteral("管理分组…"));
    connect(manage, &QAction::triggered, this, [this] { showSettingsDialog(3); });
    menu.exec(title_bar_->groupButton()->mapToGlobal(
        QPoint(0, title_bar_->groupButton()->height() + 4)));
}

void MainWindow::createGroup() {
    SingleTextDialog dialog(QStringLiteral("创建分组"), QStringLiteral("分组名称"), {}, this);
    if (executeModal(dialog) != QDialog::Accepted) return;
    const QString name = dialog.value();
    std::wstring error;
    if (!controller_.store().AddGroup(ToWideString(name), &error)) {
        QMessageBox::warning(this, QStringLiteral("添加分组"), ToQString(error));
        return;
    }
    controller_.ReloadModels();
    controller_.SetGroup(ToWideString(name));
    setStatus(L"已创建分组 " + ToWideString(name));
}

void MainWindow::showBindingDialog() {
    if (!controller_.agent().Online()) {
        QMessageBox::warning(this, QStringLiteral("绑定手机"),
                             QStringLiteral("HTTPS/WSS Agent 未启动，请先检查设置中的端口。"));
        return;
    }
    BindingDialog dialog(controller_, this);
    binding_dialog_ = &dialog;
    executeModal(dialog);
    binding_dialog_.clear();
    refreshAll();
}

void MainWindow::showSettingsDialog(int page) {
    SettingsDialog dialog(controller_, [this] {
        applyTheme();
        refreshAll();
    }, this);
    dialog.OpenPage(page);
    executeModal(dialog);
    controller_.ReloadModels();
    applyTheme();
    refreshAll();
}

int MainWindow::executeModal(QDialog& dialog) {
    WorkspaceScrim scrim(this, controller_.lightTheme());
    // Configure the dialog before showing the scrim.  Recreating a native
    // dialog window after another top-level overlay is visible can cause an
    // extra activation cycle on Windows and a visible DWM border repaint.
    dialog.setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint |
                          Qt::NoDropShadowWindowHint);
    dialog.setWindowModality(Qt::WindowModal);
    dialog.setAttribute(Qt::WA_TranslucentBackground, true);
    const QSize desired = dialog.size().expandedTo(dialog.minimumSizeHint());
    dialog.resize(
        std::min(desired.width(), std::max(420, width() - 80)),
        std::min(desired.height(), std::max(320, height() - 110)));
    const QRect owner = frameGeometry();
    dialog.move(owner.center() - QPoint(dialog.width() / 2, dialog.height() / 2));

    // The scrim is a child of the main window, so use client coordinates and
    // never show/raise a second top-level HWND.  This keeps the owner active
    // while the modal is being prepared and removes the border flash.
    scrim.setGeometry(0, kTitleBarHeight, width(),
                      std::max(1, height() - kTitleBarHeight));
    scrim.show();
    scrim.raise();
    const int result = dialog.exec();
    scrim.hide();
    return result;
}

void MainWindow::persistProjectOrder() {
    if (!search_->text().trimmed().isEmpty()) return;
    std::vector<std::wstring> ids;
    ids.reserve(static_cast<std::size_t>(project_list_->count()));
    for (int row = 0; row < project_list_->count(); ++row) {
        ids.push_back(ToWideString(project_list_->item(row)->data(ProjectIdRole).toString()));
    }
    std::wstring error;
    if (!controller_.ReorderVisibleProjects(controller_.group(), ids, &error) && !error.empty()) {
        QMessageBox::warning(this, QStringLiteral("项目排序"), ToQString(error));
    }
    refreshProjectList();
}

void MainWindow::moveSelectedProject(const std::wstring& group) {
    const auto id = controller_.selectedProjectId();
    if (id.empty()) return;
    std::wstring error;
    if (!controller_.MoveProjectToGroup(id, group, &error)) {
        QMessageBox::warning(this, QStringLiteral("移动项目"), ToQString(error));
        return;
    }
    controller_.SetGroup(group);
    setStatus(L"项目已移动到 " + group);
}

void MainWindow::pauseSelectedProject() {
    const auto id = controller_.selectedProjectId();
    if (id.empty()) return;
    controller_.StopProject(id, L"paused");
    setStatus(L"会话已暂停");
}

void MainWindow::prepareTerminalSurface() {
    workspace_->setCurrentWidget(terminal_host_);
    terminal_host_->show();
    terminal_host_->raise();
    terminal_host_->updateGeometry();
    controller_.ResizeTerminal(
        std::max(1, terminal_host_->width()),
        std::max(1, terminal_host_->height()));
}

void MainWindow::showRunningProject(const std::wstring& project_id) {
    if (project_id.empty() || !controller_.HasSession(project_id)) return;
    prepareTerminalSurface();
    controller_.ActivateProject(project_id, true);
    controller_.SetTerminalVisible(true);
    controller_.ResizeTerminal(terminal_host_->width(), terminal_host_->height());
}

void MainWindow::openProjectFolder() {
    const auto* project = selectedProject();
    if (!project || project->folder.empty()) return;
    QDesktopServices::openUrl(QUrl::fromLocalFile(ToQString(project->folder)));
}

void MainWindow::handlePairing(const agent::PendingPairing& pairing) {
    if (binding_dialog_) {
        binding_dialog_->UpdatePairing(pairing);
        return;
    }
    if (!pairing.submitted || pairing.authorized || pairing.rejected) return;
    const QString message = QStringLiteral("手机名称：") + ToQString(pairing.device.name) +
        QStringLiteral("\n型号：") + ToQString(pairing.device.model) +
        QStringLiteral("\n设备 ID：") + ToQString(pairing.device.phone_id) +
        QStringLiteral("\n\n确认后该手机可远程操作管理员终端。" );
    const bool approve = QMessageBox::warning(
        this, QStringLiteral("确认绑定手机"), message,
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes;
    std::wstring error;
    if (!controller_.agent().ConfirmPairing(pairing.info.pairing_id, approve, &error) && !error.empty()) {
        QMessageBox::warning(this, QStringLiteral("手机绑定"), ToQString(error));
    }
    refreshAll();
}

void MainWindow::setStatus(const std::wstring& text) {
    status_->setText(ToQString(text));
}

void MainWindow::updateTelemetry() {
    const QString agent = controller_.agent().Online() ? QStringLiteral("Agent 在线") : QStringLiteral("Agent 离线");
    const bool remote = !controller_.store().GetSetting(L"remote_address").empty();
    telemetry_->setText(agent + QStringLiteral("  |  内存 ") +
                        QString::number(controller_.WorkingSetMegabytes()) +
                        QStringLiteral(" MB  |  ") +
                        (remote ? QStringLiteral("公网已配置") : QStringLiteral("公网未配置")));
    refreshProjectDetails();
}

void MainWindow::publishTerminalSnapshots() {
    if (controller_.TakeTerminalDirty()) {
        controller_.PublishDirtySnapshots();
    } else if (terminal_snapshot_timer_) {
        terminal_snapshot_timer_->stop();
    }
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    if (event && event->type() == QEvent::FocusIn) {
        if (auto* editor = qobject_cast<QLineEdit*>(watched)) {
            // QWindowsInputContext handles the same FocusIn after this event
            // filter returns and calls ImmAssociateContextEx(IACE_DEFAULT).
            // Restoring synchronously here is therefore immediately undone.
            // Queue the operation so it runs after Qt's focus/input-method
            // update, and ignore stale requests when focus has moved again.
            const auto serial = ++ime_focus_serial_;
            const QPointer<QLineEdit> guarded_editor(editor);
            QMetaObject::invokeMethod(
                this,
                [this, guarded_editor, serial] {
                    if (shutting_down_ || serial != ime_focus_serial_ ||
                        !guarded_editor || !guarded_editor->hasFocus()) {
                        return;
                    }
                    restoreQtInputContext(guarded_editor);
                },
                Qt::QueuedConnection);
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

bool MainWindow::nativeEvent(const QByteArray& eventType, void* message, qintptr* result) {
    auto* native = static_cast<MSG*>(message);
    if (native && taskbar_button_created_message_ &&
        native->message == taskbar_button_created_message_) {
        taskbar_working_indicator_.OnTaskbarButtonCreated();
        *result = 0;
        return true;
    }
    if (native && controller_.HandleNativeMessage(
            native->message, native->wParam, native->lParam)) {
        if (native->message == app::kQtTerminalDirtyMessage && terminal_snapshot_timer_ &&
            !terminal_snapshot_timer_->isActive()) {
            terminal_snapshot_timer_->start();
        }
        *result = 0;
        return true;
    }
    if (!native) return QMainWindow::nativeEvent(eventType, message, result);

    // MSG already carries the native window handle. Calling winId() from
    // inside nativeEvent can synchronously create/reconfigure the HWND and
    // re-enter this handler while Qt is processing a window-position message;
    // on Windows that re-entry can crash TextShaping.dll. Never create an
    // HWND from the event handler; use the handle supplied by Windows.
    const HWND window = native->hwnd;
    switch (native->message) {
    case WM_MOUSEACTIVATE:
        // Deliver the first title-bar click even when the window begins in
        // the background. The client-side drag handler needs that press to
        // start moving immediately instead of requiring a second gesture.
        *result = MA_ACTIVATE;
        return true;
    case WM_NCACTIVATE:
        // The whole title bar and frame are client-rendered. Never let the
        // default procedure paint an active/inactive non-client frame.
        DwmSetWindowAttribute(window, DWMWA_BORDER_COLOR,
                              &kDwmColorNone, sizeof(kDwmColorNone));
        *result = TRUE;
        return true;
    case WM_NCCALCSIZE:
        if (native->wParam) {
            // WS_THICKFRAME remains enabled for resize/snap, while the entire
            // outer rectangle is still client space for the custom title bar.
            *result = 0;
            return true;
        }
        break;
    case WM_NCHITTEST: {
        const POINT screen{GET_X_LPARAM(native->lParam), GET_Y_LPARAM(native->lParam)};
        RECT rect{};
        GetWindowRect(window, &rect);
        const int frame = std::max(
            6, GetSystemMetricsForDpi(SM_CXSIZEFRAME, GetDpiForWindow(window)) +
                   GetSystemMetricsForDpi(SM_CXPADDEDBORDER, GetDpiForWindow(window)));
        if (!isMaximized()) {
            const bool left = screen.x < rect.left + frame;
            const bool right = screen.x >= rect.right - frame;
            const bool top = screen.y < rect.top + frame;
            const bool bottom = screen.y >= rect.bottom - frame;
            if (top && left) { *result = HTTOPLEFT; return true; }
            if (top && right) { *result = HTTOPRIGHT; return true; }
            if (bottom && left) { *result = HTBOTTOMLEFT; return true; }
            if (bottom && right) { *result = HTBOTTOMRIGHT; return true; }
            if (left) { *result = HTLEFT; return true; }
            if (right) { *result = HTRIGHT; return true; }
            if (top) { *result = HTTOP; return true; }
            if (bottom) { *result = HTBOTTOM; return true; }
        }
        // TitleBarWidget owns dragging and double-click maximize. Returning
        // HTCLIENT prevents Windows from drawing a second non-client title.
        *result = HTCLIENT;
        return true;
    }
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(native->lParam);
        info->ptMinTrackSize.x = 900;
        info->ptMinTrackSize.y = 600;
        const HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
        MONITORINFO monitor_info{sizeof(monitor_info)};
        if (monitor && GetMonitorInfoW(monitor, &monitor_info)) {
            info->ptMaxPosition.x = monitor_info.rcWork.left - monitor_info.rcMonitor.left;
            info->ptMaxPosition.y = monitor_info.rcWork.top - monitor_info.rcMonitor.top;
            info->ptMaxSize.x = monitor_info.rcWork.right - monitor_info.rcWork.left;
            info->ptMaxSize.y = monitor_info.rcWork.bottom - monitor_info.rcWork.top;
            info->ptMaxTrackSize = info->ptMaxSize;
        }
        break;
    }
    case WM_DWMCOMPOSITIONCHANGED:
        native_frame_configured_ = false;
        applyNativeWindowStyle();
        break;
    case WM_SIZE:
        QTimer::singleShot(0, this, [this] {
            if (title_bar_) title_bar_->UpdateMaximizeGlyph(isMaximized());
            if (!isMinimized()) {
                if (project_title_) refreshProjectDetails();
            } else if (terminal_attached_) {
                controller_.SetTerminalVisible(false);
            }
        });
        break;
    default:
        break;
    }
    return QMainWindow::nativeEvent(eventType, message, result);
}

void MainWindow::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);
    if (title_bar_) title_bar_->UpdateMaximizeGlyph(isMaximized());
    if (terminal_attached_ && workspace_ && terminal_host_ &&
        workspace_->currentWidget() == terminal_host_) {
        controller_.ResizeTerminal(terminal_host_->width(), terminal_host_->height());
    }
}

void MainWindow::changeEvent(QEvent* event) {
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange) {
        if (title_bar_) title_bar_->UpdateMaximizeGlyph(isMaximized());
        if (!isMinimized() && project_title_) refreshProjectDetails();
    }
}

void MainWindow::closeEvent(QCloseEvent* event) {
    // Let the native window disappear first. qt_main.cpp performs the same
    // complete, synchronous controller shutdown immediately after exec()
    // returns, while this object is still alive. This preserves orderly
    // cleanup without making the close button appear unresponsive.
    shutting_down_ = true;
    if (telemetry_timer_) telemetry_timer_->stop();
    if (terminal_snapshot_timer_) terminal_snapshot_timer_->stop();
    if (project_list_) project_list_->SetWorkingAnimationActive(false);
    taskbar_working_indicator_.Detach();
    QMainWindow::closeEvent(event);
}

} // namespace aicli::ui::qt
