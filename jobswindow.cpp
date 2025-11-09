#include "jobswindow.h"

#include "jobspanel.h"
#include "jobmanager.h"

#include <QGuiApplication>
#include <QScreen>
#include <QVBoxLayout>

namespace {
QRect availableGeometryFor(QWidget *widget)
{
    if (!widget) {
        const QList<QScreen *> screens = QGuiApplication::screens();
        if (!screens.isEmpty()) {
            return screens.first()->availableGeometry();
        }
        return QRect();
    }

    if (QScreen *screen = widget->screen()) {
        return screen->availableGeometry();
    }

    return QGuiApplication::primaryScreen()
        ? QGuiApplication::primaryScreen()->availableGeometry()
        : QRect();
}
} // namespace

JobsWindow::JobsWindow(QWidget *parent)
    : QWidget(parent, Qt::Tool | Qt::WindowStaysOnTopHint | Qt::CustomizeWindowHint | Qt::WindowTitleHint)
{
    setAttribute(Qt::WA_DeleteOnClose, false);
    setAttribute(Qt::WA_ShowWithoutActivating, true);
    setWindowTitle(tr("Background Tasks"));
    setMinimumWidth(280);
    setMaximumWidth(340);
    setMinimumHeight(160);
    setMaximumHeight(320);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_panel = new JobsPanel(this);
    layout->addWidget(m_panel);
}

void JobsWindow::setJobManager(JobManager *manager)
{
    if (m_panel) {
        m_panel->setJobManager(manager);
    }
}

void JobsWindow::showRelativeTo(QWidget *reference, int margin)
{
    QWidget *anchor = reference ? reference : parentWidget();
    const QRect screenGeom = availableGeometryFor(anchor);

    QSize desired = sizeHint();
    desired.setWidth(qBound(minimumWidth(), desired.width(), maximumWidth()));
    desired.setHeight(qBound(minimumHeight(), desired.height(), maximumHeight()));
    resize(desired);

    QPoint targetPos;
    if (anchor) {
        const QRect anchorRect = anchor->rect();
        QPoint base = anchor->mapToGlobal(anchorRect.bottomRight());
        if (anchor == parentWidget()) {
            base = anchor->mapToGlobal(anchorRect.topRight());
        }
        targetPos = QPoint(base.x() - width(), base.y() + margin);
    } else {
        targetPos = QPoint(screenGeom.right() - width() - margin,
                           screenGeom.top() + margin);
    }

    if (!screenGeom.isNull()) {
        if (targetPos.x() + width() > screenGeom.right() - margin) {
            targetPos.setX(screenGeom.right() - width() - margin);
        }
        if (targetPos.x() < screenGeom.left() + margin) {
            targetPos.setX(screenGeom.left() + margin);
        }
        if (targetPos.y() + height() > screenGeom.bottom() - margin) {
            targetPos.setY(screenGeom.bottom() - height() - margin);
        }
        if (targetPos.y() < screenGeom.top() + margin) {
            targetPos.setY(screenGeom.top() + margin);
        }
    }

    move(targetPos);
    show();
    raise();
}

void JobsWindow::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    emit visibilityChanged(true);
}

void JobsWindow::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    emit visibilityChanged(false);
}

