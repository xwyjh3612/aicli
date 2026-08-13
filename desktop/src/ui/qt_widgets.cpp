#include "ui/qt_widgets.h"

#include <QApplication>
#include <QCursor>
#include <QDrag>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QItemSelectionModel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QStyle>
#include <QPixmap>
#include <QSignalBlocker>
#include <QStringList>
#include <QTimer>
#include <QVariant>
#include <QVBoxLayout>
#include <QWindow>

#include <algorithm>

namespace aicli::ui::qt {
namespace {

QString Css(const QColor& color) {
    return color.name(QColor::HexArgb);
}

QColor PaletteColor(const QStyleOptionViewItem& option, QPalette::ColorRole role) {
    return option.palette.color(QPalette::Normal, role);
}

} // namespace

QString ToQString(const std::wstring& value) {
    return QString::fromWCharArray(value.c_str(), static_cast<int>(value.size()));
}

std::wstring ToWideString(const QString& value) {
    return value.toStdWString();
}

QColor ToQColor(COLORREF value) {
    return QColor(GetRValue(value), GetGValue(value), GetBValue(value));
}

QString BuildApplicationStyle(const ThemePalette& theme) {
    const QColor window = ToQColor(theme.window);
    const QColor panel = ToQColor(theme.panel);
    const QColor surface = ToQColor(theme.surface);
    const QColor border = ToQColor(theme.border);
    const QColor text = ToQColor(theme.text);
    const QColor muted = ToQColor(theme.muted);
    const QColor accent = ToQColor(theme.accent);
    const QColor accentStrong = ToQColor(theme.accent_strong);
    const QColor field = ToQColor(theme.field);
    const QColor fieldFocus = ToQColor(theme.field_focus);
    const QColor selection = ToQColor(theme.selection);
    const QColor hover = ToQColor(theme.hover);
    const QColor pressed = ToQColor(theme.pressed);
    const QColor titlebar = theme.light()
        ? ToQColor(theme.titlebar)
        : QColor(23, 27, 35);

    // Use named color tokens. QString::arg() replaces the lowest numbered
    // placeholder rather than binding arguments to their literal numbers;
    // the old sheet skipped %4/%5, shifting every later color two positions.
    // That made ordinary fields, setting rows and buttons inherit the accent
    // color (#539cff in the dark theme).
    QString style = QStringLiteral(R"(
QWidget#qtRoot, QMainWindow, QWidget#mainSurface { background: @window@; color: @text@; }
QWidget#titleBar { background: @titlebar@; }
QFrame#titleBarSeparator, QFrame#settingsHeaderSeparator { background: @border@; border: none; }
QWidget#sidebar, QWidget#footer { background: @panel@; }
QLabel#appLogo { background: transparent; border: none; }
QLabel#appTitle { color: @titlebarText@; }
QPushButton { background: @surface@; color: @text@; border: 1px solid @border@; border-radius: 8px; padding: 0 12px; }
QPushButton:hover { background: @hover@; border-color: @accent@; }
QPushButton:focus { background: @surface@; border-color: @accent@; }
QPushButton:pressed { background: @pressed@; }
QPushButton#primaryButton { background: @accentStrong@; color: white; border-color: @accent@; }
QPushButton#primaryButton:hover, QPushButton#primaryButton:focus { background: @accent@; }
QPushButton#captionButton, QPushButton#closeButton {
    background: transparent; border: none; border-radius: 0; padding: 0; color: @titlebarText@;
}
QPushButton#captionButton:hover { background: @hover@; }
QPushButton#captionButton:pressed { background: @pressed@; }
QPushButton#closeButton:hover { background: #e85663; color: white; }
QPushButton#closeButton:pressed { background: #c63946; color: white; }
QLineEdit, QComboBox { background: @field@; color: @text@; border: 1px solid @border@; border-radius: 9px; padding: 0 10px; selection-background-color: @selection@; selection-color: @text@; }
QLineEdit:focus, QComboBox:focus { background: @fieldFocus@; border-color: @accent@; }
QComboBox::drop-down { width: 28px; border: none; }
QComboBox QAbstractItemView { background: @surface@; color: @text@; border: 1px solid @border@; selection-background-color: @selection@; padding: 4px; }
QListWidget { background: @surface@; color: @text@; border: none; outline: none; padding: 0; }
QListWidget::item { border: none; padding: 0; }
QListWidget#projectList { background: @listSurface@; color: @listText@; border: none; }
QWidget#workspace, QStackedWidget#workspace,
QWidget#terminalHost { background: transparent; border: none; padding: 0; }
QLabel#sidebarTitle { color: @text@; }
QLabel#mutedLabel, QLabel#footerLabel { color: @muted@; }
QFrame#separator { background: @border@; }
QScrollBar:vertical { background: transparent; width: 12px; margin: 3px 2px 3px 0; }
QScrollBar::handle:vertical { background: @border@; border-radius: 5px; min-height: 28px; }
QScrollBar::handle:vertical:hover { background: @accent@; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }
QDialog { background: @window@; color: @text@; border: 1px solid @border@; border-radius: 14px; }
  QDialog#settingsDialog, QDialog#bindingDialog { background: transparent; border: none; border-radius: 0; }
  QDialog#projectDialog, QDialog#nameCommandDialog { background: @window@; }
  QWidget#settingsSurface, QWidget#bindingSurface { background: @window@; color: @text@; border: 1px solid @border@; border-radius: 14px; }
  QWidget#settingsHeader {
      background: @titlebar@; color: @titlebarText@; border: none;
      border-top-left-radius: 13px; border-top-right-radius: 13px;
  }
  QLabel#settingsTitle { background: transparent; color: @titlebarText@; border: none; }
  QPushButton#dialogCloseButton {
      min-height: 0; background: transparent; color: @titlebarText@;
      border: none; border-radius: 6px; padding: 0;
  }
  QPushButton#dialogCloseButton:hover { background: @hover@; }
  QPushButton#dialogCloseButton:pressed { background: @pressed@; }
  QWidget#settingsBody, QStackedWidget#settingsPages, QWidget#settingsPage {
      background: transparent; color: @text@; border: none; border-radius: 0;
  }
  QFrame#settingsCard {
      background: @surface@; border: 1px solid @border@; border-radius: 12px;
  }
  QWidget#settingsCardRow { background: transparent; border: none; }
  QLabel#settingsRowLabel { color: @text@; font-size: 14px; }
  QFrame#settingsCardDivider { background: @border@; border: none; }
  QPushButton[settingsChoice="true"] {
      min-height: 34px; padding: 0 14px; background: @field@;
      border: 1px solid @border@; border-radius: 8px;
  }
  QPushButton[settingsChoice="true"]:hover { background: @hover@; border-color: @accent@; }
  QPushButton[settingsChoice="true"]:checked {
      background: @selection@; border-color: @accent@; color: @text@;
  }
  QWidget#dialogSurface { background: @window@; color: @text@; border: none; border-radius: 0; }
  QWidget#settingsNavigationPanel {
      background: @surface@; border: none; border-right: 1px solid @border@;
      border-bottom-left-radius: 13px;
  }
  QListWidget#settingsNavigation { background: transparent; border: none; }
QListWidget#settingsNavigation::item { min-height: 38px; padding: 0 14px; border-radius: 7px; }
QListWidget#settingsNavigation::item:hover { background: @hover@; }
QListWidget#settingsNavigation::item:selected { background: @selection@; color: @text@; }
QListWidget#settingsProjectTypes::item, QListWidget#settingsGroups::item {
    padding: 0 12px; border-radius: 7px;
}
QListWidget#settingsProjectTypes::item:hover, QListWidget#settingsGroups::item:hover {
    background: @hover@;
}
QListWidget#settingsProjectTypes::item:selected, QListWidget#settingsGroups::item:selected {
    background: @selection@; color: @text@;
}
QDialog QLineEdit, QDialog QComboBox { min-height: 42px; padding: 0 14px; }
QDialog QCheckBox, QDialog QRadioButton { color: @text@; spacing: 9px; min-height: 28px; }
QDialog QCheckBox::indicator, QDialog QRadioButton::indicator { width: 17px; height: 17px; }
QLabel#settingsHint { color: @muted@; font-size: 12px; }
QDialog QPushButton { min-height: 40px; padding: 0 18px; }
QDialog QPushButton#captionButton { min-height: 0; padding: 0; }
QDialogButtonBox QPushButton { min-width: 100px; }
QDialog QLabel { color: @text@; }
QDialog QGroupBox { border: 1px solid @border@; border-radius: 10px; margin-top: 12px; padding: 12px; }
QDialog QTabWidget::pane { border: 1px solid @border@; border-radius: 8px; }
QListView, QTreeView, QTableView { background: @surface@; color: @text@; border: 1px solid @border@; border-radius: 8px; alternate-background-color: @window@; }
QListView::item:selected, QTreeView::item:selected, QTableView::item:selected { background: @selection@; color: @text@; }
QToolTip { background: @surface@; color: @text@; border: 1px solid @border@; padding: 5px; }
    )");

    const auto replaceColor = [&style](const char* token, const QColor& color) {
        style.replace(QString::fromLatin1(token), Css(color));
    };
    replaceColor("@window@", window);
    replaceColor("@panel@", panel);
    replaceColor("@surface@", surface);
    replaceColor("@border@", border);
    replaceColor("@text@", text);
    replaceColor("@muted@", muted);
    replaceColor("@accent@", accent);
    replaceColor("@accentStrong@", accentStrong);
    replaceColor("@field@", field);
    replaceColor("@fieldFocus@", fieldFocus);
    replaceColor("@selection@", selection);
    replaceColor("@hover@", hover);
    replaceColor("@pressed@", pressed);
    replaceColor("@titlebar@", titlebar);
    replaceColor("@titlebarText@", ToQColor(theme.titlebar_text));
    replaceColor("@listSurface@", ToQColor(theme.list_surface));
    replaceColor("@listText@", ToQColor(theme.list_text));
    return style;
}

SingleTextDialog::SingleTextDialog(const QString& title,
                                   const QString& label,
                                   const QString& initial,
                                   QWidget* parent)
    : QDialog(parent) {
    setObjectName(QStringLiteral("singleTextDialog"));
    setModal(true);
    setMinimumWidth(440);
    setAttribute(Qt::WA_StyledBackground, true);

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
    QFont headingFont = heading->font();
    headingFont.setPixelSize(20);
    headingFont.setWeight(QFont::DemiBold);
    heading->setFont(headingFont);
    root->addWidget(heading);

    auto* form = new QFormLayout();
    form->setVerticalSpacing(10);
    field_ = new QLineEdit(initial, surface);
    field_->setMinimumHeight(42);
    field_->setPlaceholderText(label);
    form->addRow(label, field_);
    root->addLayout(form);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Cancel | QDialogButtonBox::Save, surface);
    buttons->button(QDialogButtonBox::Save)->setObjectName(QStringLiteral("primaryButton"));
    buttons->button(QDialogButtonBox::Save)->setText(QStringLiteral("确定"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    for (auto* button : buttons->buttons()) button->setMinimumSize(100, 40);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, this, &SingleTextDialog::acceptDialog);
    root->addWidget(buttons);
    field_->setFocus();
    field_->selectAll();
}

QString SingleTextDialog::value() const {
    return field_ ? field_->text().trimmed() : QString();
}

void SingleTextDialog::acceptDialog() {
    if (value().isEmpty()) {
        if (field_) field_->setFocus();
        return;
    }
    accept();
}

TerminalHostWidget::TerminalHostWidget(QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_DontCreateNativeAncestors, true);
    setAttribute(Qt::WA_NativeWindow, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setFocusPolicy(Qt::StrongFocus);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void TerminalHostWidget::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    emit nativeHostReady(reinterpret_cast<HWND>(winId()));
}

void TerminalHostWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    emit hostResized(width(), height());
}

ProjectItemDelegate::ProjectItemDelegate(QObject* parent) : QStyledItemDelegate(parent) {}

QSize ProjectItemDelegate::sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const {
    return QSize(220, 40);
}

void ProjectItemDelegate::paint(QPainter* painter,
                                const QStyleOptionViewItem& option,
                                const QModelIndex& index) const {
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    const QRect card = option.rect.adjusted(0, 1, 0, -1);
    const bool selected = option.state & QStyle::State_Selected;
    const bool hovered = option.state & QStyle::State_MouseOver;
    const QPalette palette = option.palette;
    QColor fill = PaletteColor(option, QPalette::Base);
    if (option.widget) {
        const QVariant surface = option.widget->property("projectItemSurface");
        if (surface.canConvert<QColor>()) fill = surface.value<QColor>();
    }
    if (selected) {
        fill = PaletteColor(option, QPalette::Highlight);
    } else if (hovered) {
        fill = palette.color(QPalette::AlternateBase);
    }
    painter->setPen(Qt::NoPen);
    painter->setBrush(fill);
    painter->drawRoundedRect(QRectF(card), 2, 2);

    const bool running = index.data(ProjectSessionRole).toBool();
    const bool working = index.data(ProjectWorkingRole).toBool();
    const QColor stateColor = running ? QColor(73, 203, 145)
                                      : QColor(245, 178, 72);

    QPen gripPen(palette.color(QPalette::PlaceholderText), 1.2);
    gripPen.setCapStyle(Qt::RoundCap);
    painter->setPen(gripPen);
    for (int line = 0; line < 3; ++line) {
        const int y = card.center().y() - 5 + line * 5;
        painter->drawLine(card.left() + 11, y, card.left() + 19, y);
    }

    const QRectF icon(card.right() - 31, card.center().y() - 10, 20, 20);
    if (!running) {
        painter->setPen(Qt::NoPen);
        painter->setBrush(stateColor);
        QPolygonF triangle;
        triangle << QPointF(icon.left() + 5, icon.top() + 4)
                 << QPointF(icon.right() - 4, icon.center().y())
                 << QPointF(icon.left() + 5, icon.bottom() - 4);
        painter->drawPolygon(triangle);
    } else if (working) {
        QColor track = stateColor;
        track.setAlpha(65);
        QPen trackPen(track, 1.65);
        trackPen.setCosmetic(true);
        trackPen.setCapStyle(Qt::RoundCap);
        painter->setPen(trackPen);
        painter->setBrush(Qt::NoBrush);
        const QRectF ring = icon.adjusted(2, 2, -2, -2);
        painter->drawEllipse(ring);

        qreal angle = -90.0;
        if (const auto* list = qobject_cast<const ProjectListWidget*>(option.widget)) {
            angle = list->spinnerAngle();
        }
        QPen spinnerPen(stateColor, 2.35);
        spinnerPen.setCosmetic(true);
        spinnerPen.setCapStyle(Qt::RoundCap);
        painter->setPen(spinnerPen);
        painter->drawArc(ring, static_cast<int>(-angle * 16.0),
                         static_cast<int>(-112.0 * 16.0));
    } else {
        QPen iconPen(stateColor, 1.65);
        iconPen.setCosmetic(true);
        painter->setPen(iconPen);
        painter->setBrush(Qt::NoBrush);
        painter->drawEllipse(icon.adjusted(2, 2, -2, -2));
        painter->setPen(Qt::NoPen);
        painter->setBrush(stateColor);
        painter->drawEllipse(QRectF(icon.center().x() - 3, icon.center().y() - 3, 6, 6));
    }

    QString name = index.data(Qt::DisplayRole).toString();
    if (name.isEmpty()) name = QStringLiteral("未命名项目");
    QColor itemText = palette.color(QPalette::Text);
    if (option.widget) {
        const QVariant text = option.widget->property("projectItemText");
        if (text.canConvert<QColor>()) itemText = text.value<QColor>();
    }
    painter->setPen(itemText);
    QFont font = option.font;
    font.setPixelSize(14);
    painter->setFont(font);
    const QRect textRect(card.left() + 32, card.top(), card.width() - 68, card.height());
    painter->drawText(textRect, Qt::AlignVCenter | Qt::TextSingleLine,
                      QFontMetrics(font).elidedText(name, Qt::ElideRight, textRect.width()));
    painter->restore();
}

ProjectListWidget::ProjectListWidget(QWidget* parent) : QListWidget(parent) {
    setItemDelegate(new ProjectItemDelegate(this));
    setSelectionMode(QAbstractItemView::SingleSelection);
    setSelectionBehavior(QAbstractItemView::SelectRows);
    setDragEnabled(true);
    setAcceptDrops(true);
    setDropIndicatorShown(true);
    setDragDropMode(QAbstractItemView::InternalMove);
    setDefaultDropAction(Qt::MoveAction);
    setMouseTracking(true);
    viewport()->setMouseTracking(true);
    setSpacing(2);
    setUniformItemSizes(true);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setContextMenuPolicy(Qt::CustomContextMenu);
    spinner_timer_ = new QTimer(this);
    spinner_timer_->setInterval(75);
    connect(spinner_timer_, &QTimer::timeout,
            this, &ProjectListWidget::advanceWorkingAnimation);
}

void ProjectListWidget::SetDragEnabledForQuery(bool enabled) {
    dragAllowed_ = enabled;
    setDragEnabled(enabled);
    if (!enabled && !dragInProgress_) {
        const bool wasActive = dragArmed_;
        dragArmed_ = false;
        dragProjectId_.clear();
        selectionBeforeDragId_.clear();
        unsetCursor();
        viewport()->unsetCursor();
        if (wasActive) emit dragActivityChanged(false);
    }
}

void ProjectListWidget::SetWorkingAnimationActive(bool active) {
    if (!spinner_timer_) return;
    if (active) {
        if (!spinner_timer_->isActive()) {
            spinner_angle_ = -90.0;
            spinner_timer_->start();
        }
    } else {
        spinner_timer_->stop();
    }
}

void ProjectListWidget::advanceWorkingAnimation() {
    spinner_angle_ += 18.0;
    if (spinner_angle_ >= 360.0) spinner_angle_ -= 360.0;
    for (int row = 0; row < count(); ++row) {
        const auto* project = item(row);
        if (!project || !project->data(ProjectWorkingRole).toBool()) continue;
        const QRect item_rect = visualItemRect(project);
        if (item_rect.intersects(viewport()->rect())) viewport()->update(item_rect);
    }
}

void ProjectListWidget::updateGripCursor(const QPoint& position) {
    const auto* item = itemAt(position);
    bool over_grip = false;
    if (dragAllowed_ && item) {
        const QRect row = visualItemRect(item);
        // Keep the hit area slightly wider than the three painted bars.  This
        // makes the native four-way move cursor easy to acquire without
        // turning the whole item into a drag handle.
        const QRect grip(row.left(), row.top(), 34, row.height());
        over_grip = grip.contains(position);
    }
    const auto shape = over_grip || dragArmed_ ? Qt::SizeAllCursor : Qt::ArrowCursor;
    setCursor(shape);
    viewport()->setCursor(shape);
}

void ProjectListWidget::mousePressEvent(QMouseEvent* event) {
    const QPoint position = event->position().toPoint();
    auto* pressedItem = itemAt(position);
    const QRect pressedRow = pressedItem ? visualItemRect(pressedItem) : QRect{};
    const bool overGrip = pressedItem &&
        QRect(pressedRow.left(), pressedRow.top(), 34, pressedRow.height()).contains(position);
    dragArmed_ = dragAllowed_ && event->button() == Qt::LeftButton && overGrip;

    // QAbstractItemView only arms its native drag state when the pressed row
    // was already selected.  Select the row before forwarding the press so a
    // first press on the grip can drag immediately instead of requiring a
    // separate click to select it first.  Suppress currentRowChanged for this
    // preparatory selection: the controller otherwise rebuilds the list on a
    // queued state notification and destroys Qt's pressed drag index before
    // the first mouse-move event arrives.
    if (dragArmed_) {
        dragProjectId_ = pressedItem->data(ProjectIdRole).toString();
        selectionBeforeDragId_ = currentItem()
            ? currentItem()->data(ProjectIdRole).toString()
            : QString{};
        {
            const QSignalBlocker blocker(this);
            setCurrentItem(pressedItem, QItemSelectionModel::ClearAndSelect);
        }
        emit dragActivityChanged(true);
        updateGripCursor(position);
        QListWidget::mousePressEvent(event);
        return;
    } else {
        dragProjectId_.clear();
        selectionBeforeDragId_.clear();
    }
    updateGripCursor(position);
    QListWidget::mousePressEvent(event);
}

void ProjectListWidget::mouseMoveEvent(QMouseEvent* event) {
    updateGripCursor(event->position().toPoint());
    QListWidget::mouseMoveEvent(event);
}

void ProjectListWidget::mouseReleaseEvent(QMouseEvent* event) {
    const bool commitSelection = dragArmed_ && !dragInProgress_ &&
                                 event->button() == Qt::LeftButton;
    const QString projectId = dragProjectId_;
    QListWidget::mouseReleaseEvent(event);
    if (commitSelection) {
        dragArmed_ = false;
        dragProjectId_.clear();
        selectionBeforeDragId_.clear();
        if (!projectId.isEmpty()) emit selectionCommitted(projectId);
        emit dragActivityChanged(false);
    }
    updateGripCursor(event->position().toPoint());
}

void ProjectListWidget::leaveEvent(QEvent* event) {
    if (!dragArmed_) {
        unsetCursor();
        viewport()->unsetCursor();
    }
    QListWidget::leaveEvent(event);
}

void ProjectListWidget::startDrag(Qt::DropActions supportedActions) {
    if (!dragAllowed_ || !dragArmed_) return;
    dragInProgress_ = true;
    const QStringList orderBefore = projectOrder();
    executeNativeDrag(supportedActions);
    const bool orderChangedByDrop = projectOrder() != orderBefore;

    // The temporary selection is only required to let QAbstractItemView start
    // a native drag on the first grip press. It must never become the active
    // terminal session. Restore the real selection while signals are blocked.
    {
        const QSignalBlocker blocker(this);
        if (auto* selected = findProjectItem(selectionBeforeDragId_)) {
            setCurrentItem(selected, QItemSelectionModel::ClearAndSelect);
        } else {
            setCurrentItem(nullptr);
        }
    }
    dragArmed_ = false;
    dragProjectId_.clear();
    selectionBeforeDragId_.clear();

    // QDrag runs a nested event loop. Do not save, reload, or clear the list
    // until both that loop and QAbstractItemView's outer mouse event have
    // returned. Background terminal notifications are deferred by MainWindow
    // while dragInProgress_ remains true.
    QTimer::singleShot(0, this, [this, orderChangedByDrop] {
        dragInProgress_ = false;
        if (orderChangedByDrop) emit orderChanged();
        emit dragActivityChanged(false);
        updateGripCursor(viewport()->mapFromGlobal(QCursor::pos()));
    });
}

void ProjectListWidget::executeNativeDrag(Qt::DropActions supportedActions) {
    QListWidget::startDrag(supportedActions);
}

QListWidgetItem* ProjectListWidget::findProjectItem(
    const QString& projectId) const {
    if (projectId.isEmpty()) return nullptr;
    for (int row = 0; row < count(); ++row) {
        auto* candidate = item(row);
        if (candidate && candidate->data(ProjectIdRole).toString() == projectId)
            return candidate;
    }
    return nullptr;
}

QStringList ProjectListWidget::projectOrder() const {
    QStringList result;
    result.reserve(count());
    for (int row = 0; row < count(); ++row) {
        if (auto* project = item(row))
            result.push_back(project->data(ProjectIdRole).toString());
    }
    return result;
}

TitleBarWidget::TitleBarWidget(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("titleBar"));
    setFixedHeight(40);
    setMouseTracking(true);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(14, 0, 0, 0);
    layout->setSpacing(6);

    auto* logo = new QLabel(this);
    logo->setObjectName(QStringLiteral("appLogo"));
    logo->setAlignment(Qt::AlignCenter);
    logo->setFixedSize(28, 28);
    const QPixmap app_icon = qApp->windowIcon().pixmap(22, 22);
    if (!app_icon.isNull()) logo->setPixmap(app_icon);
    else logo->setText(QStringLiteral(">_"));
    layout->addWidget(logo);

    title_label_ = new QLabel(QStringLiteral("AI CLI"), this);
    title_label_->setObjectName(QStringLiteral("appTitle"));
    QFont title_font = title_label_->font();
    title_font.setPixelSize(16);
    title_label_->setFont(title_font);
    layout->addWidget(title_label_);

    auto makeButton = [this](const QString& text, const QString& name, int width) {
        auto* button = new QPushButton(text, this);
        button->setObjectName(name);
        button->setFixedSize(width, 30);
        button->setFocusPolicy(Qt::NoFocus);
        return button;
    };
    group_button_ = makeButton(QStringLiteral("全部 (0)  ▾"), QStringLiteral("groupButton"), 154);
    add_group_button_ = makeButton(QStringLiteral("+"), QStringLiteral("smallButton"), 34);
    layout->addSpacing(18);
    layout->addWidget(group_button_);
    layout->addWidget(add_group_button_);

    layout->addStretch(1);
    bind_button_ = makeButton(QStringLiteral("绑定手机"), QStringLiteral("toolbarButton"), 96);
    devices_button_ = makeButton(QStringLiteral("已绑定 0 台"), QStringLiteral("toolbarButton"), 106);
    settings_button_ = makeButton(QStringLiteral("设置"), QStringLiteral("toolbarButton"), 72);
    layout->addWidget(bind_button_);
    layout->addWidget(devices_button_);
    layout->addWidget(settings_button_);

    // Keep the entire title bar on one row. These are client-side buttons,
    // but use the same glyphs, dimensions and hover treatment as the Windows
    // 11 caption controls so the custom title bar remains visually native.
    auto* caption_buttons = new QWidget(this);
    caption_buttons->setObjectName(QStringLiteral("captionButtons"));
    caption_buttons->setFixedSize(138, 40);
    auto* caption_layout = new QHBoxLayout(caption_buttons);
    caption_layout->setContentsMargins(0, 0, 0, 0);
    caption_layout->setSpacing(0);

    QFont caption_font;
    caption_font.setFamilies({QStringLiteral("Segoe Fluent Icons"),
                              QStringLiteral("Segoe MDL2 Assets")});
    caption_font.setPixelSize(10);

    auto makeCaptionButton = [&](QChar glyph, const QString& name,
                                 const QString& accessibleName) {
        auto* button = new QPushButton(QString(glyph), caption_buttons);
        button->setObjectName(name);
        button->setFixedSize(46, 40);
        button->setFocusPolicy(Qt::NoFocus);
        button->setFont(caption_font);
        button->setAccessibleName(accessibleName);
        button->setToolTip(accessibleName);
        return button;
    };
    minimize_button_ = makeCaptionButton(QChar(0xE921),
                                         QStringLiteral("captionButton"),
                                         QStringLiteral("最小化"));
    maximize_button_ = makeCaptionButton(QChar(0xE922),
                                         QStringLiteral("captionButton"),
                                         QStringLiteral("最大化"));
    close_button_ = makeCaptionButton(QChar(0xE8BB),
                                      QStringLiteral("closeButton"),
                                      QStringLiteral("关闭"));
    caption_layout->addWidget(minimize_button_);
    caption_layout->addWidget(maximize_button_);
    caption_layout->addWidget(close_button_);
    layout->addSpacing(8);
    layout->addWidget(caption_buttons);
}

void TitleBarWidget::UpdateMaximizeGlyph(bool maximized) {
    if (!maximize_button_) return;
    maximize_button_->setText(QString(QChar(maximized ? 0xE923 : 0xE922)));
    const QString action = maximized ? QStringLiteral("还原") : QStringLiteral("最大化");
    maximize_button_->setAccessibleName(action);
    maximize_button_->setToolTip(action);
}

void TitleBarWidget::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    // Hand dragging to Windows whenever possible.  The native move loop is
    // what enables edge/corner Snap, Snap Assist and dragging a maximized
    // window back to its restored size.  Keep the manual path below only as a
    // fallback for platforms where system move is unavailable.
    if (auto* top = window(); top && top->windowHandle()) {
        top->activateWindow();
        if (top->windowHandle()->startSystemMove()) {
            drag_pending_ = false;
            dragging_ = false;
            event->accept();
            return;
        }
    }

    drag_pending_ = true;
    dragging_ = false;
    drag_press_global_ = event->globalPosition().toPoint();
    drag_offset_ = drag_press_global_ - window()->frameGeometry().topLeft();
    event->accept();
}

void TitleBarWidget::mouseMoveEvent(QMouseEvent* event) {
    if (!(event->buttons() & Qt::LeftButton)) {
        drag_pending_ = false;
        dragging_ = false;
        QWidget::mouseMoveEvent(event);
        return;
    }

    const QPoint global = event->globalPosition().toPoint();
    if (drag_pending_ &&
        (global - drag_press_global_).manhattanLength() >= QApplication::startDragDistance()) {
        drag_pending_ = false;
        dragging_ = true;

        if (window()->isMaximized()) {
            const QRect normal = window()->normalGeometry();
            const int currentWidth = std::max(1, window()->width());
            const qreal horizontalRatio = std::clamp(
                static_cast<qreal>(drag_press_global_.x() - window()->frameGeometry().left()) /
                    static_cast<qreal>(currentWidth),
                0.08, 0.92);
            window()->showNormal();
            const QPoint restoredTopLeft(
                global.x() - static_cast<int>(normal.width() * horizontalRatio),
                global.y() - std::min(height() / 2, 24));
            window()->move(restoredTopLeft);
            drag_offset_ = global - restoredTopLeft;
        }
    }

    if (dragging_) {
        window()->move(global - drag_offset_);
        event->accept();
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void TitleBarWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        drag_pending_ = false;
        dragging_ = false;
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void TitleBarWidget::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        drag_pending_ = false;
        dragging_ = false;
        if (window()->isMaximized()) window()->showNormal();
        else window()->showMaximized();
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

} // namespace aicli::ui::qt
