#pragma once

#include <QAbstractItemView>
#include <QColor>
#include <QDialog>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QStyledItemDelegate>
#include <QWidget>

#include <windows.h>

#include "core/models.h"
#include "ui/theme.h"

class QLineEdit;
class QTimer;

namespace aicli::ui::qt {

QString ToQString(const std::wstring& value);
std::wstring ToWideString(const QString& value);
QColor ToQColor(COLORREF value);
QString BuildApplicationStyle(const ThemePalette& theme);

enum ProjectItemRole {
    ProjectIdRole = Qt::UserRole,
    ProjectStatusRole,
    ProjectSessionRole,
    ProjectWorkingRole,
};

class TerminalHostWidget final : public QWidget {
    Q_OBJECT
public:
    explicit TerminalHostWidget(QWidget* parent = nullptr);

signals:
    void nativeHostReady(HWND window);
    void hostResized(int width, int height);

protected:
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
};

// Small, reusable modal used for names (groups, labels, etc.).  Keeping this
// here makes all one-line forms use the same opaque surface, field geometry,
// placeholder behavior, and Chinese confirmation buttons.
class SingleTextDialog final : public QDialog {
    Q_OBJECT
public:
    SingleTextDialog(const QString& title,
                     const QString& label,
                     const QString& initial,
                     QWidget* parent = nullptr);

    QString value() const;

private slots:
    void acceptDialog();

private:
    QLineEdit* field_{};
};

class ProjectItemDelegate final : public QStyledItemDelegate {
    Q_OBJECT
public:
    explicit ProjectItemDelegate(QObject* parent = nullptr);
    void paint(QPainter* painter,
               const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option,
                   const QModelIndex& index) const override;
};

class ProjectListWidget : public QListWidget {
    Q_OBJECT
public:
    explicit ProjectListWidget(QWidget* parent = nullptr);
    void SetDragEnabledForQuery(bool enabled);
    void SetWorkingAnimationActive(bool active);
    qreal spinnerAngle() const noexcept { return spinner_angle_; }

signals:
    void orderChanged();
    void selectionCommitted(const QString& projectId);

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void startDrag(Qt::DropActions supportedActions) override;
    void dropEvent(QDropEvent* event) override;

private:
    void updateGripCursor(const QPoint& position);
    void advanceWorkingAnimation();
    bool dragArmed_{};
    bool dragAllowed_{true};
    QString dragProjectId_;
    QTimer* spinner_timer_{};
    qreal spinner_angle_{-90.0};
};

class TitleBarWidget final : public QWidget {
    Q_OBJECT
public:
    explicit TitleBarWidget(QWidget* parent = nullptr);

    QPushButton* groupButton() const noexcept { return group_button_; }
    QPushButton* addGroupButton() const noexcept { return add_group_button_; }
    QPushButton* bindButton() const noexcept { return bind_button_; }
    QPushButton* devicesButton() const noexcept { return devices_button_; }
    QPushButton* settingsButton() const noexcept { return settings_button_; }
    QPushButton* minimizeButton() const noexcept { return minimize_button_; }
    QPushButton* maximizeButton() const noexcept { return maximize_button_; }
    QPushButton* closeButton() const noexcept { return close_button_; }
    QLabel* titleLabel() const noexcept { return title_label_; }

    void UpdateMaximizeGlyph(bool maximized);

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    QLabel* title_label_{};
    QPushButton* group_button_{};
    QPushButton* add_group_button_{};
    QPushButton* bind_button_{};
    QPushButton* devices_button_{};
    QPushButton* settings_button_{};
    QPushButton* minimize_button_{};
    QPushButton* maximize_button_{};
    QPushButton* close_button_{};
    bool drag_pending_{};
    bool dragging_{};
    QPoint drag_press_global_;
    QPoint drag_offset_;
};

} // namespace aicli::ui::qt
