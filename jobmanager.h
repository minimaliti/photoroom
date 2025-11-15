#ifndef JOBMANAGER_H
#define JOBMANAGER_H

#include <QObject>
#include <QDateTime>
#include <QHash>
#include <QList>
#include <QString>
#include <QUuid>
#include <QVector>

enum class JobState {
    Pending,
    Running,
    Succeeded,
    Failed,
    Cancelled
};

enum class JobCategory {
    Import,
    PreviewGeneration,
    MetadataExtraction,
    Develop,
    Histogram,
    Export,
    Misc
};

struct JobInfo {
    QUuid id;
    JobCategory category = JobCategory::Misc;
    JobState state = JobState::Pending;
    QString title;
    QString detail;
    int progress = -1;          // 0 - 100, -1 for indeterminate
    int completedSteps = 0;
    int totalSteps = -1;
    bool indeterminate = true;
    QDateTime startedAt;
    QDateTime finishedAt;
};

class JobManager : public QObject
{
    Q_OBJECT
public:
    explicit JobManager(QObject *parent = nullptr);

    QUuid startJob(JobCategory category, const QString &title, const QString &detail = QString());
    void updateDetail(const QUuid &id, const QString &detail);
    void updateProgress(const QUuid &id, int completedSteps, int totalSteps = -1);
    void setIndeterminate(const QUuid &id, bool indeterminate);
    void completeJob(const QUuid &id, const QString &detail = QString());
    void failJob(const QUuid &id, const QString &errorDetail);
    void cancelJob(const QUuid &id, const QString &detail = QString());
    QVector<JobInfo> jobs() const;
    int activeJobCount() const;

signals:
    void jobAdded(const JobInfo &info);
    void jobUpdated(const JobInfo &info);
    void jobRemoved(const QUuid &id);

private:
    struct JobEntry {
        JobInfo info;
        bool removalScheduled = false;
    };

    JobInfo *findJob(const QUuid &id);
    const JobInfo *findJob(const QUuid &id) const;
    void updateJobState(JobInfo &info, JobState newState);
    void publishUpdate(const JobInfo &info);
    void scheduleRemoval(const QUuid &id, int delayMs);

    QHash<QUuid, JobEntry> m_jobs;
    QList<QUuid> m_order;
};

QString jobStateToDisplayText(JobState state);
QString jobCategoryToDisplayText(JobCategory category);

#endif // JOBMANAGER_H

