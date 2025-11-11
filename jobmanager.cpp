#include "jobmanager.h"

#include <QtGlobal>
#include <QTimer>

namespace {
constexpr int kSuccessRetentionMs = 4000;
constexpr int kFailureRetentionMs = 8000;
}

JobManager::JobManager(QObject *parent)
    : QObject(parent)
{
}

QUuid JobManager::startJob(JobCategory category, const QString &title, const QString &detail)
{
    JobInfo info;
    info.id = QUuid::createUuid();
    info.category = category;
    info.state = JobState::Running;
    info.title = title;
    info.detail = detail;
    info.progress = -1;
    info.indeterminate = true;
    info.startedAt = QDateTime::currentDateTime();

    JobEntry entry;
    entry.info = info;

    m_jobs.insert(info.id, entry);
    m_order.append(info.id);

    emit jobAdded(info);
    return info.id;
}

void JobManager::updateDetail(const QUuid &id, const QString &detail)
{
    JobInfo *info = findJob(id);
    if (!info) {
        return;
    }
    info->detail = detail;
    publishUpdate(*info);
}

void JobManager::updateProgress(const QUuid &id, int completedSteps, int totalSteps)
{
    JobInfo *info = findJob(id);
    if (!info) {
        return;
    }
    if (totalSteps >= 0) {
        info->totalSteps = totalSteps;
        info->indeterminate = false;
        info->completedSteps = qMax(0, completedSteps);
        const int clampedTotal = qMax(1, totalSteps);
        const double ratio = static_cast<double>(info->completedSteps) / static_cast<double>(clampedTotal);
        info->progress = static_cast<int>(qBound(0.0, ratio, 1.0) * 100.0);
    } else {
        info->totalSteps = -1;
        info->completedSteps = completedSteps;
        info->progress = completedSteps;
    }
    publishUpdate(*info);
}

void JobManager::setIndeterminate(const QUuid &id, bool indeterminate)
{
    JobInfo *info = findJob(id);
    if (!info) {
        return;
    }
    info->indeterminate = indeterminate;
    if (indeterminate) {
        info->progress = -1;
    }
    publishUpdate(*info);
}

void JobManager::completeJob(const QUuid &id, const QString &detail)
{
    auto it = m_jobs.find(id);
    if (it == m_jobs.end()) {
        return;
    }

    JobInfo &info = it.value().info;
    if (!detail.isEmpty()) {
        info.detail = detail;
    }
    updateJobState(info, JobState::Succeeded);

    m_jobs.erase(it);
    m_order.removeAll(id);
    emit jobRemoved(id);
}

void JobManager::failJob(const QUuid &id, const QString &errorDetail)
{
    JobInfo *info = findJob(id);
    if (!info) {
        return;
    }
    if (!errorDetail.isEmpty()) {
        info->detail = errorDetail;
    }
    updateJobState(*info, JobState::Failed);
    scheduleRemoval(id, kFailureRetentionMs);
}

void JobManager::cancelJob(const QUuid &id, const QString &detail)
{
    JobInfo *info = findJob(id);
    if (!info) {
        return;
    }
    if (!detail.isEmpty()) {
        info->detail = detail;
    }
    updateJobState(*info, JobState::Cancelled);
    scheduleRemoval(id, kSuccessRetentionMs);
}

QVector<JobInfo> JobManager::jobs() const
{
    QVector<JobInfo> list;
    list.reserve(m_order.size());
    for (const QUuid &id : m_order) {
        const JobInfo *info = findJob(id);
        if (!info) {
            continue;
        }
        list.append(*info);
    }
    return list;
}

int JobManager::activeJobCount() const
{
    int count = 0;
    for (const auto &entry : m_jobs) {
        if (entry.info.state == JobState::Running || entry.info.state == JobState::Pending) {
            ++count;
        }
    }
    return count;
}

JobInfo *JobManager::findJob(const QUuid &id)
{
    auto it = m_jobs.find(id);
    if (it == m_jobs.end()) {
        return nullptr;
    }
    return &it.value().info;
}

const JobInfo *JobManager::findJob(const QUuid &id) const
{
    auto it = m_jobs.constFind(id);
    if (it == m_jobs.constEnd()) {
        return nullptr;
    }
    return &it.value().info;
}

void JobManager::updateJobState(JobInfo &info, JobState newState)
{
    if (info.state == newState) {
        return;
    }
    info.state = newState;
    if (newState == JobState::Succeeded) {
        info.progress = 100;
        info.indeterminate = false;
    }
    info.finishedAt = QDateTime::currentDateTime();
    publishUpdate(info);
}

void JobManager::publishUpdate(const JobInfo &info)
{
    emit jobUpdated(info);
}

void JobManager::scheduleRemoval(const QUuid &id, int delayMs)
{
    auto it = m_jobs.find(id);
    if (it == m_jobs.end()) {
        return;
    }
    if (it->removalScheduled) {
        return;
    }
    it->removalScheduled = true;
    QTimer::singleShot(delayMs, this, [this, id]() {
        auto entryIt = m_jobs.find(id);
        if (entryIt == m_jobs.end()) {
            return;
        }
        m_jobs.erase(entryIt);
        m_order.removeAll(id);
        emit jobRemoved(id);
    });
}

QString jobStateToDisplayText(JobState state)
{
    switch (state) {
    case JobState::Pending:
        return QObject::tr("Pending");
    case JobState::Running:
        return QObject::tr("Working…");
    case JobState::Succeeded:
        return QObject::tr("Completed");
    case JobState::Failed:
        return QObject::tr("Failed");
    case JobState::Cancelled:
        return QObject::tr("Cancelled");
    }
    return {};
}

QString jobCategoryToDisplayText(JobCategory category)
{
    switch (category) {
    case JobCategory::Import:
        return QObject::tr("Import");
    case JobCategory::PreviewGeneration:
        return QObject::tr("Preview");
    case JobCategory::Develop:
        return QObject::tr("Develop");
    case JobCategory::Histogram:
        return QObject::tr("Histogram");
    case JobCategory::Export:
        return QObject::tr("Export");
    case JobCategory::Misc:
    default:
        return QObject::tr("Task");
    }
}

