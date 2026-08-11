#include <QApplication>
#include <QMouseEvent>
#include <QSignalSpy>
#include <QtTest/QTest>

#include "ui/qt_widgets.h"

namespace {

class DragProbeProjectList final : public aicli::ui::qt::ProjectListWidget {
public:
    using ProjectListWidget::ProjectListWidget;

    bool drag_started{};

protected:
    void startDrag(Qt::DropActions) override {
        drag_started = true;
    }
};

class ProjectListDragTests final : public QObject {
    Q_OBJECT

private slots:
    void unselectedRowStartsDraggingOnFirstGripPress();
};

void ProjectListDragTests::unselectedRowStartsDraggingOnFirstGripPress() {
    using namespace aicli::ui::qt;

    DragProbeProjectList list;
    list.resize(260, 160);
    for (int row = 0; row < 3; ++row) {
        auto* item = new QListWidgetItem(QStringLiteral("Project %1").arg(row + 1), &list);
        item->setData(ProjectIdRole, QStringLiteral("project-%1").arg(row + 1));
    }
    list.setCurrentRow(0);
    list.show();
    QVERIFY(QTest::qWaitForWindowExposed(&list));

    QSignalSpy row_changed(&list, &QListWidget::currentRowChanged);
    const QRect target = list.visualItemRect(list.item(1));
    const QPoint press_position(target.left() + 15, target.center().y());

    QTest::mousePress(list.viewport(), Qt::LeftButton, Qt::NoModifier, press_position);
    QCOMPARE(list.currentRow(), 1);
    QCOMPARE(row_changed.count(), 0);

    const QPoint move_position(
        press_position.x(),
        press_position.y() - QApplication::startDragDistance() - 8);
    QMouseEvent move_event(
        QEvent::MouseMove,
        move_position,
        list.viewport()->mapToGlobal(move_position),
        Qt::NoButton,
        Qt::LeftButton,
        Qt::NoModifier);
    QApplication::sendEvent(list.viewport(), &move_event);

    QVERIFY2(list.drag_started,
             "The first grip press on an unselected project did not start a drag.");
    QTest::mouseRelease(list.viewport(), Qt::LeftButton, Qt::NoModifier, move_position);
}

} // namespace

QTEST_MAIN(ProjectListDragTests)

#include "qt_project_list_tests.moc"
