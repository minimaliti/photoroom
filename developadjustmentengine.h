#ifndef DEVELOPADJUSTMENTENGINE_H
#define DEVELOPADJUSTMENTENGINE_H

#include <QObject>
#include <QImage>
#include <QFuture>

#include <atomic>
#include <memory>
#include <mutex>

#include "developtypes.h"

struct DevelopAdjustmentRenderResult
{
    int requestId = 0;
    QImage image;
    bool cancelled = false;
    qint64 elapsedMs = 0;
    bool isPreview = false;
    double displayScale = 1.0;
};

struct DevelopAdjustmentRequest
{
    int requestId = 0;
    QImage image;
    DevelopAdjustments adjustments;
    bool isPreview = false;
    double displayScale = 1.0;
};

class DevelopAdjustmentEngine : public QObject
{
    Q_OBJECT

public:
    struct CancellationToken {
        std::atomic<bool> cancelled{false};
    };

    explicit DevelopAdjustmentEngine(QObject *parent = nullptr);
    ~DevelopAdjustmentEngine() override;

    QFuture<DevelopAdjustmentRenderResult> renderAsync(DevelopAdjustmentRequest request);

    void cancelActive();

private:

    QFuture<DevelopAdjustmentRenderResult> startRender(DevelopAdjustmentRequest request,
                                                       const std::shared_ptr<CancellationToken> &token);

    std::shared_ptr<CancellationToken> makeActiveToken();

    mutable std::mutex m_mutex;
    std::shared_ptr<CancellationToken> m_activeToken;
};

#endif // DEVELOPADJUSTMENTENGINE_H


