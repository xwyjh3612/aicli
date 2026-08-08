#include "ui/qt_widgets.h"

#include <QApplication>
#include <QDrag>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QStyle>
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
QPushButton#captionButton { background: transparent; border: none; border-radius: 0; padding: 0; color: %6; font-size: 16px; }
QPushButton#captionButton:hover { background: %10; }
QPushButton#closeButton:hover { background: #e85663; color: white; }
QPushButton#closeButton:pressed { background: #c63946; color: white; }
QLineEdit, QComboBox { background: %16; color: %8; border: 1px solid %9; border-radius: 9px; padding: 0 10px; selection-background-color: %17; selection-color: %8; }
QLineEdit:focus, QComboBox:focus { background: %18; border-color: %11; }
QComboBox::drop-down { width: 28px; border: none; }
QComboBox QAbstractItemView { background: %7; color: %8; border: 1px solid %9; selection-background-color: %17; padding: 4px; }
QListWidget { background: %7; color: %8; border: none; outline: none; padding: 0; }
QListWidget::item { border: none; padding: 0; }
QLabel#sidebarTitle { color: %8; }
QLabel#mutedLabel, QLabel#footerLabel { color: %19; }
QFrame#separator { background: %9; }
QScrollBar:vertical { background: transparent; width: 12px; margin: 3px 2px 3px 0; }
QScrollBar::handle:vertical { background: %9; border-radius: 5px; min-height: 28px; }
QScrollBar::handle:vertical:hover { background: %11; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }
QDialog { background: %1; color: %2; border: 1px solid %9; border-radius: 14px; }
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
             Css(accent), Css(accent), Css(field), Css(selection), Css(fieldFocus), Css(muted));
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
    QColor stroke = PaletteColor(option, QPalette::Mid);
    if (selected) {
        fill = PaletteColor(option, QPalette::Highlight);
        stroke = PaletteColor(option, QPalette::Link);
    } else if (hovered) {
        fill = palette.color(QPalette::AlternateBase);
    }
    painter->setPen(QPen(stroke, 1));
    painter->setBrush(fill);
    painter->drawRoundedRect(card, 2, 2);

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
    painter->setPen(palette.color(QPalette::Text));
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
    setSpacing(2);
    setUniformItemSizes(true);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    setContextMenuPolicy(Qt::CustomContextMenu);
}

void ProjectListWidget::SetDragEnabledForQuery(bool enabled) {
    dragAllowed_ = enabled;
    setDragEnabled(enabled);
}

void ProjectListWidget::mousePressEvent(QMouseEvent* event) {
    dragArmed_ = event->button() == Qt::LeftButton && event->position().x() <= 34;
    QListWidget::mousePressEvent(event);
}

void ProjectListWidget::mouseReleaseEvent(QMouseEvent* event) {
    QListWidget::mouseReleaseEvent(event);
    if (event->button() == Qt::LeftButton) dragArmed_ = false;
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

    minimize_button_ = makeButton(QStringLiteral("—"), QStringLiteral("captionButton"), 46);
    maximize_button_ = makeButton(QStringLiteral("□"), QStringLiteral("captionButton"), 46);
    close_button_ = makeButton(QStringLiteral("×"), QStringLiteral("closeButton"), 46);
    layout->addSpacing(8);
    layout->addWidget(minimize_button_);
    layout->addWidget(maximize_button_);
    layout->addWidget(close_button_);
}

void TitleBarWidget::UpdateMaximizeGlyph(bool maximized) {
    maximize_button_->setText(maximized ? QStringLiteral("❐") : QStringLiteral("□"));
}

void TitleBarWidget::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    if (auto* handle = window()->windowHandle()) {
        handle->startSystemMove();
    }
    event->accept();
}

void TitleBarWidget::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        if (window()->isMaximized()) window()->showNormal();
        else window()->showMaximized();
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

} // namespace aicli::ui::qt
