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
    void executeNativeDrag(Qt::DropActions) override {
        drag_started = true;
        auto* moved = takeItem(1);
        insertItem(0, moved);
    }
};

class ProjectListDragTests final : public QObject {
    Q_OBJECT

private slots:
    void unselectedRowDragDoesNotCommitTerminalSelection();
    void gripClickStillCommitsSelection();
};

void ProjectListDragTests::unselectedRowDragDoesNotCommitTerminalSelection() {
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
    QSignalSpy selection_committed(&list, &ProjectListWidget::selectionCommitted);
    QSignalSpy order_changed(&list, &ProjectListWidget::orderChanged);
    QSignalSpy drag_activity(&list, &ProjectListWidget::dragActivityChanged);
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
    QTRY_COMPARE(order_changed.count(), 1);
    QCOMPARE(selection_committed.count(), 0);
    QCOMPARE(list.currentItem()->data(ProjectIdRole).toString(), QStringLiteral("project-1"));
    QCOMPARE(list.item(0)->data(ProjectIdRole).toString(), QStringLiteral("project-2"));
    QCOMPARE(drag_activity.count(), 2);
    QCOMPARE(drag_activity.at(0).at(0).toBool(), true);
    QCOMPARE(drag_activity.at(1).at(0).toBool(), false);
}

void ProjectListDragTests::gripClickStillCommitsSelection() {
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

    QSignalSpy selection_committed(&list, &ProjectListWidget::selectionCommitted);
    QSignalSpy order_changed(&list, &ProjectListWidget::orderChanged);
    const QRect target = list.visualItemRect(list.item(2));
    const QPoint position(target.left() + 15, target.center().y());
    QTest::mouseClick(list.viewport(), Qt::LeftButton, Qt::NoModifier, position);

    QCOMPARE(selection_committed.count(), 1);
    QCOMPARE(selection_committed.at(0).at(0).toString(), QStringLiteral("project-3"));
    QCOMPARE(order_changed.count(), 0);
}

} // namespace

QTEST_MAIN(ProjectListDragTests)

#include "qt_project_list_tests.moc"
