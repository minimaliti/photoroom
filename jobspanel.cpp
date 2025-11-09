#include "jobspanel.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QtGlobal>

namespace {
QString statusColorForState(JobState state)
{
    switch (state) {
    case JobState::Succeeded:
        return QStringLiteral("#4ade80");
    case JobState::Failed:
        return QStringLiteral("#f87171");
    case JobState::Cancelled:
        return QStringLiteral("#a1a1aa");
    case JobState::Pending:
    case JobState::Running:
    default:
        return QStringLiteral("#38bdf8");
    }
}
} // namespace

JobRowWidget::JobRowWidget(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("jobRow"));
    setAutoFillBackground(true);
    setStyleSheet(QStringLiteral(
        "#jobRow { border: 1px solid rgba(148,163,184,0.2); border-radius: 6px; background: rgba(15,23,42,0.88); }"
        "#jobTitle { font-weight: 600; color: rgba(248,250,252,0.92); }"
        "QLabel { font-size: 11px; color: rgba(226,232,240,0.85); }"));

    auto *outerLayout = new QVBoxLayout(this);
    outerLayout->setContentsMargins(8, 6, 8, 6);
    outerLayout->setSpacing(4);

    m_titleLabel = new QLabel(this);
    m_titleLabel->setObjectName(QStringLiteral("jobTitle"));
    m_titleLabel->setWordWrap(true);
    outerLayout->addWidget(m_titleLabel);

    m_progressBar = new QProgressBar(this);
    m_progressBar->setObjectName(QStringLiteral("jobProgress"));
    m_progressBar->setTextVisible(false);
    m_progressBar->setMinimumHeight(4);
    m_progressBar->setMaximumHeight(4);
    m_progressBar->setRange(0, 100);
    m_progressBar->setStyleSheet(QStringLiteral(
        "QProgressBar { border: none; border-radius: 2px; background-color: rgba(71,85,105,0.55); }"
        "QProgressBar::chunk { border-radius: 2px; background-color: #38bdf8; }"));
    outerLayout->addWidget(m_progressBar);
}

void JobRowWidget::updateFromJob(const JobInfo &info)
{
    m_jobId = info.id;

    const QString titleText = info.title.isEmpty()
            ? QObject::tr("Background task")
            : info.title;
    const QString detailText = info.detail.trimmed();
    QString combined = titleText;
    if (!detailText.isEmpty()) {
        combined.append(QStringLiteral(" · %1").arg(detailText));
    }

    if (info.state == JobState::Failed) {
        combined.append(QStringLiteral(" — %1").arg(jobStateToDisplayText(info.state)));
    } else if (info.state == JobState::Cancelled) {
        combined.append(QStringLiteral(" — %1").arg(jobStateToDisplayText(info.state)));
    }

    m_titleLabel->setText(combined);
    m_titleLabel->setToolTip(combined);

    if (info.indeterminate || info.progress < 0) {
        m_progressBar->setRange(0, 0);
    } else {
        m_progressBar->setRange(0, 100);
        m_progressBar->setValue(qBound(0, info.progress, 100));
    }

    applyStateStyling(info);
}

QUuid JobRowWidget::jobId() const
{
    return m_jobId;
}

void JobRowWidget::applyStateStyling(const JobInfo &info)
{
    const QString color = statusColorForState(info.state);
    m_progressBar->setStyleSheet(QStringLiteral(
        "QProgressBar { border: none; border-radius: 2px; background-color: rgba(71,85,105,0.55); }"
        "QProgressBar::chunk { border-radius: 2px; background-color: %1; }").arg(color));
    QString labelColor = (info.state == JobState::Failed) ? QStringLiteral("#f87171")
                      : (info.state == JobState::Cancelled) ? QStringLiteral("#a1a1aa")
                      : QStringLiteral("rgba(248,250,252,0.92)");
    m_titleLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 11px; font-weight: 600;").arg(labelColor));
}

JobsPanel::JobsPanel(QWidget *parent)
    : QWidget(parent)
{
    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(12, 10, 12, 10);
    mainLayout->setSpacing(6);

    auto *headerLayout = new QHBoxLayout();
    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->setSpacing(6);

    //// why waste space with double title? (window title and label)
    // auto *title = new QLabel(tr("Background Activity"), this);
    // title->setStyleSheet(QStringLiteral("font-size: 12px; font-weight: 600;"));
    // headerLayout->addWidget(title);

    headerLayout->addStretch(1);

    mainLayout->addLayout(headerLayout);

    m_scrollArea = new QScrollArea(this);
    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scrollArea->setFrameShape(QFrame::NoFrame);
    m_scrollArea->setMaximumHeight(200);

    m_listContainer = new QWidget(m_scrollArea);
    m_listLayout = new QVBoxLayout(m_listContainer);
    m_listLayout->setContentsMargins(0, 0, 0, 0);
    m_listLayout->setSpacing(6);
    m_listLayout->addStretch(1);

    m_scrollArea->setWidget(m_listContainer);
    mainLayout->addWidget(m_scrollArea, 1);

    m_emptyStateLabel = new QLabel(tr("All caught up.\nBackground tasks will appear here."), this);
    m_emptyStateLabel->setAlignment(Qt::AlignCenter);
    m_emptyStateLabel->setStyleSheet(QStringLiteral("color: rgba(241,245,249,0.65); font-style: italic; font-size: 11px;"));
    mainLayout->addWidget(m_emptyStateLabel);

    setFixedWidth(280);

    ensureEmptyStateVisible();
}

void JobsPanel::setJobManager(JobManager *manager)
{
    if (manager == m_jobManager) {
        return;
    }

    if (m_jobManager) {
        disconnect(m_jobManager, nullptr, this, nullptr);
    }

    m_jobManager = manager;

    if (m_jobManager) {
        connect(m_jobManager, &JobManager::jobAdded, this, &JobsPanel::handleJobAdded);
        connect(m_jobManager, &JobManager::jobUpdated, this, &JobsPanel::handleJobUpdated);
        connect(m_jobManager, &JobManager::jobRemoved, this, &JobsPanel::handleJobRemoved);
    }

    rebuildFromManager();
}

JobManager *JobsPanel::jobManager() const
{
    return m_jobManager;
}

void JobsPanel::rebuildFromManager()
{
    qDeleteAll(m_rows);
    m_rows.clear();

    if (m_jobManager) {
        const QVector<JobInfo> infos = m_jobManager->jobs();
        for (const JobInfo &info : infos) {
            handleJobAdded(info);
        }
    }
    ensureEmptyStateVisible();
}

void JobsPanel::handleJobAdded(const JobInfo &info)
{
    if (!m_listLayout) {
        return;
    }

    if (m_rows.contains(info.id)) {
        handleJobUpdated(info);
        return;
    }

    auto *row = new JobRowWidget(m_listContainer);
    row->updateFromJob(info);

    const int stretchIndex = m_listLayout->count() - 1;
    m_listLayout->insertWidget(stretchIndex, row);
    m_rows.insert(info.id, row);
    ensureEmptyStateVisible();
}

void JobsPanel::handleJobUpdated(const JobInfo &info)
{
    JobRowWidget *row = m_rows.value(info.id, nullptr);
    if (!row) {
        handleJobAdded(info);
        return;
    }
    row->updateFromJob(info);
    ensureEmptyStateVisible();
}

void JobsPanel::handleJobRemoved(const QUuid &id)
{
    JobRowWidget *row = m_rows.take(id);
    if (row) {
        row->deleteLater();
    }
    ensureEmptyStateVisible();
}

void JobsPanel::ensureEmptyStateVisible()
{
    const bool hasRows = !m_rows.isEmpty();
    if (m_emptyStateLabel) {
        m_emptyStateLabel->setVisible(!hasRows);
    }
    if (m_scrollArea) {
        m_scrollArea->setVisible(hasRows);
    }
}

