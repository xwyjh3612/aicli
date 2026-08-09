#include "ui/qt_widgets.h"

#include <QApplication>
#include <QDrag>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QStyle>
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

    return QStringLiteral(R"(
QWidget#qtRoot, QMainWindow, QWidget#mainSurface { background: %1; color: %2; }
QWidget#titleBar { background: %3; }
QLabel#appLogo { background: %4; color: white; border: 1px solid %5; border-radius: 9px; }
QLabel#appTitle { color: %6; }
QPushButton { background: %7; color: %8; border: 1px solid %9; border-radius: 8px; padding: 0 12px; }
QPushButton:hover { background: %10; border-color: %11; }
QPushButton:pressed { background: %12; }
QPushButton#primaryButton { background: %13; color: white; border-color: %14; }
QPushButton#primaryButton:hover { background: %15; }
QPushButton#captionButton, QPushButton#closeButton {
    background: transparent; border: none; border-radius: 0; padding: 0; color: %6;
}
QPushButton#captionButton:hover { background: %10; }
QPushButton#captionButton:pressed { background: %12; }
QPushButton#closeButton:hover { background: #e85663; color: white; }
QPushButton#closeButton:pressed { background: #c63946; color: white; }
QLineEdit, QComboBox { background: %16; color: %8; border: 1px solid %9; border-radius: 9px; padding: 0 10px; selection-background-color: %17; selection-color: %8; }
QLineEdit:focus, QComboBox:focus { background: %18; border-color: %11; }
QComboBox::drop-down { width: 28px; border: none; }
QComboBox QAbstractItemView { background: %7; color: %8; border: 1px solid %9; selection-background-color: %17; padding: 4px; }
QListWidget { background: %7; color: %8; border: none; outline: none; padding: 0; }
QListWidget::item { border: none; padding: 0; }
QListWidget#projectList { background: %20; color: %21; border: none; }
QWidget#workspace, QStackedWidget#workspace,
QWidget#terminalHost { background: transparent; border: none; padding: 0; }
QLabel#sidebarTitle { color: %8; }
QLabel#mutedLabel, QLabel#footerLabel { color: %19; }
QFrame#separator { background: %9; }
QScrollBar:vertical { background: transparent; width: 12px; margin: 3px 2px 3px 0; }
QScrollBar::handle:vertical { background: %9; border-radius: 5px; min-height: 28px; }
QScrollBar::handle:vertical:hover { background: %11; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }
QDialog { background: %1; color: %2; border: 1px solid %9; border-radius: 14px; }
  QDialog#settingsDialog, QDialog#bindingDialog { background: transparent; border: none; border-radius: 0; }
  QDialog#projectDialog, QDialog#nameCommandDialog { background: %1; }
  QWidget#settingsSurface, QWidget#bindingSurface { background: %1; color: %8; border: 1px solid %9; border-radius: 14px; }
  QWidget#settingsHeader, QWidget#settingsBody, QStackedWidget#settingsPages,
  QWidget#settingsPage {
      background: transparent; color: %8; border: none; border-radius: 0;
  }
  QWidget#dialogSurface { background: %1; color: %8; border: none; border-radius: 0; }
  QWidget#settingsNavigationPanel {
      background: %7; border: none; border-right: 1px solid %9;
      border-bottom-left-radius: 13px;
  }
  QListWidget#settingsNavigation { background: transparent; border: none; }
QListWidget#settingsNavigation::item { min-height: 38px; padding: 0 14px; border-radius: 7px; }
QListWidget#settingsNavigation::item:hover { background: %10; }
QListWidget#settingsNavigation::item:selected { background: %17; color: %8; }
QListWidget#settingsProjectTypes::item, QListWidget#settingsGroups::item {
    padding: 0 12px; border-radius: 7px;
}
QListWidget#settingsProjectTypes::item:hover, QListWidget#settingsGroups::item:hover {
    background: %10;
}
QListWidget#settingsProjectTypes::item:selected, QListWidget#settingsGroups::item:selected {
    background: %17; color: %8;
}
QDialog QLineEdit, QDialog QComboBox { min-height: 42px; padding: 0 14px; }
QDialog QPushButton { min-height: 40px; padding: 0 18px; }
QDialog QPushButton#captionButton { min-height: 0; padding: 0; }
QDialogButtonBox QPushButton { min-width: 100px; }
QDialog QLabel { color: %8; }
QDialog QGroupBox { border: 1px solid %9; border-radius: 10px; margin-top: 12px; padding: 12px; }
QDialog QTabWidget::pane { border: 1px solid %9; border-radius: 8px; }
QListView, QTreeView, QTableView { background: %7; color: %8; border: 1px solid %9; border-radius: 8px; alternate-background-color: %1; }
QListView::item:selected, QTreeView::item:selected, QTableView::item:selected { background: %17; color: %8; }
QToolTip { background: %7; color: %8; border: 1px solid %9; padding: 5px; }
    )")
        .arg(Css(window), Css(text), Css(ToQColor(theme.titlebar)), Css(accentStrong),
             Css(accent), Css(ToQColor(theme.titlebar_text)), Css(surface), Css(text),
             Css(border), Css(hover), Css(accent), Css(pressed), Css(accentStrong),
             Css(accent), Css(accent), Css(field), Css(selection), Css(fieldFocus), Css(muted),
             Css(ToQColor(theme.list_surface)), Css(ToQColor(theme.list_text)));
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
    return QSize(220, 50);
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
    QColor stroke = PaletteColor(option, QPalette::Mid);
    if (selected) {
        fill = PaletteColor(option, QPalette::Highlight);
        stroke = PaletteColor(option, QPalette::Link);
    } else if (hovered) {
        fill = palette.color(QPalette::AlternateBase);
    }
    QPen cardPen(stroke, 1.0);
    // Cosmetic + half-pixel alignment keeps the outline to exactly one
    // device pixel.  Drawing a 1 px pen on an integer rectangle straddles two
    // pixels under antialiasing and looks like a thicker double border.
    cardPen.setCosmetic(true);
    painter->setPen(cardPen);
    painter->setBrush(fill);
    painter->drawRoundedRect(QRectF(card).adjusted(0.5, 0.5, -0.5, -0.5), 2, 2);

    const bool running = index.data(Qt::UserRole + 2).toBool();
    const QColor stateColor = running ? QColor(73, 203, 145)
                                      : QColor(245, 178, 72);

    QPen gripPen(palette.color(QPalette::PlaceholderText), 1.2);
    gripPen.setCapStyle(Qt::RoundCap);
    painter->setPen(gripPen);
    for (int line = 0; line < 3; ++line) {
        const int y = card.center().y() - 5 + line * 5;
        painter->drawLine(card.left() + 11, y, card.left() + 19, y);
    }

    painter->setPen(Qt::NoPen);
    painter->setBrush(stateColor);
    const QRectF dot(card.right() - 27, card.center().y() - 5, 10, 10);
    if (!running) {
        QPolygonF triangle;
        triangle << QPointF(dot.left(), dot.top())
                 << QPointF(dot.right(), dot.center().y())
                 << QPointF(dot.left(), dot.bottom());
        painter->drawPolygon(triangle);
    } else {
        painter->drawEllipse(dot);
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
    setContextMenuPolicy(Qt::CustomContextMenu);
}

void ProjectListWidget::SetDragEnabledForQuery(bool enabled) {
    dragAllowed_ = enabled;
    setDragEnabled(enabled);
    if (!enabled) {
        unsetCursor();
        viewport()->unsetCursor();
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
    dragArmed_ = event->button() == Qt::LeftButton && event->position().x() <= 34;
    updateGripCursor(event->position().toPoint());
    QListWidget::mousePressEvent(event);
}

void ProjectListWidget::mouseMoveEvent(QMouseEvent* event) {
    updateGripCursor(event->position().toPoint());
    QListWidget::mouseMoveEvent(event);
}

void ProjectListWidget::mouseReleaseEvent(QMouseEvent* event) {
    QListWidget::mouseReleaseEvent(event);
    if (event->button() == Qt::LeftButton) dragArmed_ = false;
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
    QListWidget::startDrag(supportedActions);
}

void ProjectListWidget::dropEvent(QDropEvent* event) {
    QListWidget::dropEvent(event);
    dragArmed_ = false;
    if (event->isAccepted()) emit orderChanged();
}

TitleBarWidget::TitleBarWidget(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("titleBar"));
    setFixedHeight(52);
    setMouseTracking(true);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(14, 0, 0, 0);
    layout->setSpacing(6);

    auto* logo = new QLabel(QStringLiteral(">_"), this);
    logo->setObjectName(QStringLiteral("appLogo"));
    logo->setAlignment(Qt::AlignCenter);
    logo->setFixedSize(32, 32);
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
        button->setFixedSize(width, 32);
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
    caption_buttons->setFixedSize(138, 52);
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
        button->setFixedSize(46, 52);
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
    // Do not use QWindow::startSystemMove() here.  On Windows that enters the
    // native move loop; when this app is initially inactive (or the system's
    // "show window contents while dragging" option is disabled), DWM draws a
    // thick gray/white wire-frame around the window.  Track the pointer and
    // move the complete window ourselves so the rendered window remains the
    // only drag feedback.
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
