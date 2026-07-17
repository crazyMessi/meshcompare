#pragma once

#include <utility>

#include <QElapsedTimer>
#include <QMutex>
#include <QMutexLocker>
#include <QThread>
#include <QWaitCondition>
#include <QVector>

#include "services/surface_comparison.h"

class FakeSurfaceComparer final : public ISurfaceComparer
{
public:
    struct Call
    {
        SurfaceMeshSnapshot source;
        SurfaceMeshSnapshot reference;
        SurfaceComparisonMetric metric = SurfaceComparisonMetric::PrecisionAtThreshold;
        SurfaceComparisonOptions options;
        QThread* thread = nullptr;
    };

    void enqueueSuccess(
        double globalScore,
        QVector<double> faceScores = {1.0})
    {
        SurfaceComparisonOutcome outcome;
        outcome.result = OperationResult::success();
        outcome.comparison.sampleCount = 1;
        outcome.comparison.coloredFaceCount = faceScores.size();
        outcome.comparison.globalScore = globalScore;
        outcome.comparison.faceScores = std::move(faceScores);
        enqueue(std::move(outcome));
    }

    void enqueueFailure(const QString& error)
    {
        enqueue({OperationResult::failure(error), {}});
    }

    void enqueueDistanceSuccess(
        QVector<double> vertexDistances,
        DistanceToReferenceStatistics statistics = {})
    {
        SurfaceComparisonOutcome outcome;
        outcome.result = OperationResult::success();
        outcome.comparison.vertexDistances = std::move(vertexDistances);
        if (statistics.vertexCount == 0) {
            statistics.vertexCount = outcome.comparison.vertexDistances.size();
            statistics.finiteVertexCount =
                outcome.comparison.vertexDistances.size();
            for (double distance : outcome.comparison.vertexDistances) {
                statistics.meanDistance += distance;
                statistics.maxDistance =
                    qMax(statistics.maxDistance, distance);
            }
            if (!outcome.comparison.vertexDistances.isEmpty()) {
                statistics.meanDistance /=
                    outcome.comparison.vertexDistances.size();
                statistics.percentile99Distance = statistics.maxDistance;
            }
        }
        outcome.comparison.distanceStatistics = statistics;
        enqueue(std::move(outcome));
    }

    void enqueueDoubleLayerSuccess(
        QVector<double> vertexScores,
        DoubleLayerStatistics statistics = {})
    {
        SurfaceComparisonOutcome outcome;
        outcome.result = OperationResult::success();
        outcome.comparison.vertexScores = std::move(vertexScores);
        if (statistics.sampleCount == 0) {
            statistics.sampleCount = 1;
            statistics.affectedVertexFraction = 0.0;
            for (double score : outcome.comparison.vertexScores) {
                if (score > 0.0)
                    statistics.affectedVertexFraction += 1.0;
            }
            if (!outcome.comparison.vertexScores.isEmpty()) {
                statistics.affectedVertexFraction /=
                    outcome.comparison.vertexScores.size();
            }
        }
        outcome.comparison.doubleLayerStatistics = statistics;
        enqueue(std::move(outcome));
    }

    void pauseNextComparison()
    {
        QMutexLocker lock(&mutex_);
        pauseNext_ = true;
        resumeRequested_ = false;
    }

    void requireDedicatedCancellationForNextComparison()
    {
        QMutexLocker lock(&mutex_);
        requireDedicatedCancellationNext_ = true;
        ignoredProgressCancellationCount_ = 0;
        dedicatedCancellationObservationCount_ = 0;
    }

    void clearPendingOutcomes()
    {
        QMutexLocker lock(&mutex_);
        outcomes_.clear();
    }

    bool waitUntilEntered(int expectedCount = 1, int timeoutMs = 5000) const
    {
        QElapsedTimer timer;
        timer.start();
        QMutexLocker lock(&mutex_);
        while (enteredCount_ < expectedCount) {
            const int remaining = timeoutMs - int(timer.elapsed());
            if (remaining <= 0 || !entered_.wait(&mutex_, remaining))
                return enteredCount_ >= expectedCount;
        }
        return true;
    }

    bool waitUntilReturned(int expectedCount = 1, int timeoutMs = 5000) const
    {
        QElapsedTimer timer;
        timer.start();
        QMutexLocker lock(&mutex_);
        while (returnedCount_ < expectedCount) {
            const int remaining = timeoutMs - int(timer.elapsed());
            if (remaining <= 0 || !returned_.wait(&mutex_, remaining))
                return returnedCount_ >= expectedCount;
        }
        return true;
    }

    void resume()
    {
        QMutexLocker lock(&mutex_);
        resumeRequested_ = true;
        resume_.wakeAll();
    }

    int callCount() const
    {
        QMutexLocker lock(&mutex_);
        return calls_.size();
    }

    QVector<Call> calls() const
    {
        QMutexLocker lock(&mutex_);
        return calls_;
    }

    int returnedCount() const
    {
        QMutexLocker lock(&mutex_);
        return returnedCount_;
    }

    int ignoredProgressCancellationCount() const
    {
        QMutexLocker lock(&mutex_);
        return ignoredProgressCancellationCount_;
    }

    int dedicatedCancellationObservationCount() const
    {
        QMutexLocker lock(&mutex_);
        return dedicatedCancellationObservationCount_;
    }

    SurfaceComparisonOutcome compare(
        const SurfaceMeshSnapshot& source,
        const SurfaceMeshSnapshot& reference,
        SurfaceComparisonMetric metric,
        const SurfaceComparisonOptions& options,
        AnalysisProgress progress = {},
        AnalysisCancellation cancellationRequested = {}) const override
    {
        SurfaceComparisonOutcome scripted;
        bool pause = false;
        bool requireDedicatedCancellation = false;
        {
            QMutexLocker lock(&mutex_);
            if (outcomes_.isEmpty()) {
                scripted = {
                    OperationResult::failure(
                        QStringLiteral("Fake comparer has no scripted outcome.")),
                    {}};
            }
            else {
                scripted = outcomes_.takeFirst();
            }
            pause = pauseNext_;
            pauseNext_ = false;
            requireDedicatedCancellation = requireDedicatedCancellationNext_;
            requireDedicatedCancellationNext_ = false;
            calls_.append({source, reference, metric, options, QThread::currentThread()});
            ++enteredCount_;
            entered_.wakeAll();
        }

        if (requireDedicatedCancellation) {
            QElapsedTimer timeout;
            timeout.start();
            while (timeout.elapsed() < 250) {
                const bool progressRequestedCancellation =
                    progress && !progress(
                        50, QStringLiteral("Fake comparison awaiting cancellation."));
                if (progressRequestedCancellation) {
                    QMutexLocker lock(&mutex_);
                    ++ignoredProgressCancellationCount_;
                }

                if (cancellationRequested && cancellationRequested()) {
                    {
                        QMutexLocker lock(&mutex_);
                        ++dedicatedCancellationObservationCount_;
                    }
                    return finish(cancelledOutcome());
                }
                QThread::msleep(1);
            }
            return finish({
                OperationResult::failure(QStringLiteral(
                    "Fake comparer did not observe dedicated cancellation.")),
                {}});
        }

        if ((cancellationRequested && cancellationRequested()) ||
            (progress && !progress(10, QStringLiteral("Fake comparison entered."))))
            return finish(cancelledOutcome());

        if (pause) {
            for (;;) {
                {
                    QMutexLocker lock(&mutex_);
                    if (resumeRequested_) {
                        resumeRequested_ = false;
                        break;
                    }
                    resume_.wait(&mutex_, 10);
                }
                if (cancellationRequested && cancellationRequested())
                    return finish(cancelledOutcome());
                if (progress && !progress(50, QStringLiteral("Fake comparison paused.")))
                    return finish(cancelledOutcome());
            }
        }

        if ((cancellationRequested && cancellationRequested()) ||
            (progress && !progress(100, QStringLiteral("Fake comparison complete."))))
            return finish(cancelledOutcome());
        return finish(scripted);
    }

private:
    static SurfaceComparisonOutcome cancelledOutcome()
    {
        return {OperationResult::failure(QStringLiteral("Analysis cancelled.")), {}};
    }

    void enqueue(SurfaceComparisonOutcome outcome)
    {
        QMutexLocker lock(&mutex_);
        outcomes_.append(std::move(outcome));
    }

    SurfaceComparisonOutcome finish(SurfaceComparisonOutcome outcome) const
    {
        QMutexLocker lock(&mutex_);
        ++returnedCount_;
        returned_.wakeAll();
        return outcome;
    }

    mutable QMutex mutex_;
    mutable QWaitCondition entered_;
    mutable QWaitCondition returned_;
    mutable QWaitCondition resume_;
    mutable QVector<SurfaceComparisonOutcome> outcomes_;
    mutable QVector<Call> calls_;
    mutable int enteredCount_ = 0;
    mutable int returnedCount_ = 0;
    mutable bool pauseNext_ = false;
    mutable bool resumeRequested_ = false;
    mutable bool requireDedicatedCancellationNext_ = false;
    mutable int ignoredProgressCancellationCount_ = 0;
    mutable int dedicatedCancellationObservationCount_ = 0;
};
