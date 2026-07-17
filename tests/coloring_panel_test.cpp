#include <QtTest>

#include <functional>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QPalette>
#include <QPushButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTabBar>
#include <QToolButton>
#include <QWidget>

#include "app/coloring_commands.h"
#include "app/coloring_panel.h"
#include "core/workspace_state.h"

namespace
{
MeshEntry entry(MeshId id)
{
    MeshEntry result;
    result.id = id;
    result.resourceId = id + 100;
    result.sourcePath = QStringLiteral("/tmp/mesh-%1.obj").arg(id);
    result.displayName = QStringLiteral("mesh-%1.obj").arg(id);
    return result;
}

void makeReady(WorkspaceState& state)
{
    const OperationResult committed =
        state.commitWorkspace({entry(1), entry(2), entry(3)}, 1);
    Q_ASSERT(committed.ok);
}

int comboIndexForMesh(const QComboBox& combo, MeshId meshId)
{
    for (int index = 0; index < combo.count(); ++index) {
        if (combo.itemData(index).toULongLong() == meshId)
            return index;
    }
    return -1;
}

class RecordingColoringCommands final : public IColoringCommands
{
public:
    explicit RecordingColoringCommands(WorkspaceState& state)
        : state_(state)
    {
    }

    OperationResult setReference(MeshId meshId) override
    {
        referenceCalls.append(meshId);
        if (!referenceResult.ok)
            return referenceResult;
        return state_.setReference(meshId);
    }

    OperationResult selectMesh(MeshId meshId) override
    {
        selectionCalls.append(meshId);
        if (!selectionResult.ok)
            return selectionResult;
        return state_.setSelectedMesh(meshId);
    }

    OperationResult setUniformColor(MeshId meshId, const QColor& color) override
    {
        uniformMeshCalls.append(meshId);
        uniformColorCalls.append(color);
        return uniformResult;
    }

    OperationResult startAnalysis(
        SurfaceComparisonMetric metric,
        const SurfaceComparisonOptions& options) override
    {
        analysisMetrics.append(metric);
        analysisOptions.append(options);
        if (!analysisResult.ok)
            return analysisResult;
        return state_.beginAnalysis();
    }

    void cancelAnalysis() override
    {
        ++cancelCount;
        if (cancelAction)
            cancelAction();
        else
            state_.finishAnalysis();
    }

    OperationResult clearColoring() override
    {
        ++clearCount;
        return clearResult;
    }

    WorkspaceState& state_;
    QVector<MeshId> referenceCalls;
    QVector<MeshId> selectionCalls;
    QVector<MeshId> uniformMeshCalls;
    QVector<QColor> uniformColorCalls;
    QVector<SurfaceComparisonMetric> analysisMetrics;
    QVector<SurfaceComparisonOptions> analysisOptions;
    OperationResult referenceResult = OperationResult::success();
    OperationResult selectionResult = OperationResult::success();
    OperationResult uniformResult = OperationResult::success();
    OperationResult analysisResult = OperationResult::success();
    OperationResult clearResult = OperationResult::success();
    std::function<void()> cancelAction;
    int cancelCount = 0;
    int clearCount = 0;
};
} // namespace

class ColoringPanelTest : public QObject
{
    Q_OBJECT

private slots:
    void exposesEnglishModesAndExactAdvancedDefaults()
    {
        WorkspaceState state;
        makeReady(state);
        RecordingColoringCommands commands(state);
        ColoringPanel panel(state, commands);

        QVERIFY(panel.autoFillBackground());
        QCOMPARE(panel.backgroundRole(), QPalette::Window);
        QVERIFY(panel.palette().brush(QPalette::Window).isOpaque());

        auto* tabs = panel.findChild<QTabBar*>(QStringLiteral("coloringModeTabs"));
        auto* samples = panel.findChild<QSpinBox*>(QStringLiteral("sampleCountSpin"));
        auto* threshold =
            panel.findChild<QDoubleSpinBox*>(QStringLiteral("distanceThresholdSpin"));
        auto* absolute =
            panel.findChild<QCheckBox*>(QStringLiteral("absoluteNormalDotCheck"));
        auto* advancedToggle =
            panel.findChild<QToolButton*>(QStringLiteral("advancedParametersToggle"));
        auto* advanced =
            panel.findChild<QWidget*>(QStringLiteral("advancedParameters"));
        auto* chooseColor =
            panel.findChild<QPushButton*>(QStringLiteral("chooseUniformColorButton"));

        QVERIFY(tabs != nullptr);
        QCOMPARE(tabs->count(), 3);
        QCOMPARE(tabs->tabText(0), QStringLiteral("Uniform Color"));
        QCOMPARE(tabs->tabText(1), QStringLiteral("Precision"));
        QCOMPARE(tabs->tabText(2), QStringLiteral("Normal Agreement"));
        QVERIFY(samples != nullptr);
        QCOMPARE(samples->minimum(), 1);
        QCOMPARE(samples->maximum(), 5000000);
        QCOMPARE(samples->value(), 500000);
        QVERIFY(threshold != nullptr);
        QCOMPARE(threshold->decimals(), 6);
        QCOMPARE(threshold->minimum(), 0.0);
        QCOMPARE(threshold->maximum(), 1000000.0);
        QCOMPARE(threshold->value(), 0.004);
        QVERIFY(absolute != nullptr);
        QVERIFY(absolute->isChecked());
        QVERIFY(advancedToggle != nullptr);
        QVERIFY(advanced != nullptr);
        QVERIFY(advanced->isHidden());
        QVERIFY(chooseColor != nullptr);
        QCOMPARE(chooseColor->text(), QStringLiteral("Choose Color…"));

        QTest::mouseClick(advancedToggle, Qt::LeftButton);

        QVERIFY(!advanced->isHidden());
    }

    void uniformUsesTheMeshSelectedAtApplyTime()
    {
        WorkspaceState state;
        makeReady(state);
        QVERIFY(state.setSelectedMesh(2).ok);
        RecordingColoringCommands commands(state);
        ColoringPanel panel(state, commands);
        auto* tabs = panel.findChild<QTabBar*>(QStringLiteral("coloringModeTabs"));
        auto* palette =
            panel.findChild<QComboBox*>(QStringLiteral("uniformColorCombo"));
        auto* apply =
            panel.findChild<QPushButton*>(QStringLiteral("applyColoringButton"));
        QVERIFY(tabs != nullptr);
        QVERIFY(palette != nullptr);
        QVERIFY(apply != nullptr);
        tabs->setCurrentIndex(0);
        const QColor green(QStringLiteral("#5aaa75"));
        const int greenIndex = palette->findData(green);
        QVERIFY(greenIndex >= 0);
        palette->setCurrentIndex(greenIndex);
        QVERIFY(state.setSelectedMesh(3).ok);

        QTest::mouseClick(apply, Qt::LeftButton);

        QCOMPARE(commands.uniformMeshCalls, QVector<MeshId>({3}));
        QCOMPARE(commands.uniformColorCalls, QVector<QColor>({green}));
    }

    void uniformMeshSelectorChangesTheTargetBeforeApplyingColor()
    {
        WorkspaceState state;
        makeReady(state);
        RecordingColoringCommands commands(state);
        ColoringPanel panel(state, commands);
        auto* meshSelector =
            panel.findChild<QComboBox*>(QStringLiteral("uniformMeshCombo"));
        auto* apply =
            panel.findChild<QPushButton*>(QStringLiteral("applyColoringButton"));
        QVERIFY(meshSelector != nullptr);
        QVERIFY(apply != nullptr);

        const int thirdMesh = comboIndexForMesh(*meshSelector, 3);
        QVERIFY(thirdMesh >= 0);
        meshSelector->setCurrentIndex(thirdMesh);
        QTest::mouseClick(apply, Qt::LeftButton);

        QCOMPARE(commands.selectionCalls, QVector<MeshId>({3}));
        QCOMPARE(state.selectedMeshId(), MeshId(3));
        QCOMPARE(commands.uniformMeshCalls, QVector<MeshId>({3}));
    }

    void precisionRoutesTheExactDefaultOptions()
    {
        WorkspaceState state;
        makeReady(state);
        RecordingColoringCommands commands(state);
        ColoringPanel panel(state, commands);
        auto* tabs = panel.findChild<QTabBar*>(QStringLiteral("coloringModeTabs"));
        auto* apply =
            panel.findChild<QPushButton*>(QStringLiteral("applyColoringButton"));
        QVERIFY(tabs != nullptr);
        QVERIFY(apply != nullptr);
        tabs->setCurrentIndex(1);

        QTest::mouseClick(apply, Qt::LeftButton);

        QCOMPARE(commands.analysisMetrics.size(), 1);
        QCOMPARE(commands.analysisMetrics.front(),
                 SurfaceComparisonMetric::PrecisionAtThreshold);
        QCOMPARE(commands.analysisOptions.front().sampleCount, 500000);
        QCOMPARE(commands.analysisOptions.front().distanceThreshold, 0.004f);
        QVERIFY(commands.analysisOptions.front().useAbsoluteNormalDot);
    }

    void normalAgreementRoutesTheExactDefaultOptions()
    {
        WorkspaceState state;
        makeReady(state);
        RecordingColoringCommands commands(state);
        ColoringPanel panel(state, commands);
        auto* tabs = panel.findChild<QTabBar*>(QStringLiteral("coloringModeTabs"));
        auto* apply =
            panel.findChild<QPushButton*>(QStringLiteral("applyColoringButton"));
        QVERIFY(tabs != nullptr);
        QVERIFY(apply != nullptr);
        tabs->setCurrentIndex(2);

        QTest::mouseClick(apply, Qt::LeftButton);

        QCOMPARE(commands.analysisMetrics.size(), 1);
        QCOMPARE(commands.analysisMetrics.front(),
                 SurfaceComparisonMetric::NormalAgreement);
        QCOMPARE(commands.analysisOptions.front().sampleCount, 500000);
        QCOMPARE(commands.analysisOptions.front().distanceThreshold, 0.004f);
        QVERIFY(commands.analysisOptions.front().useAbsoluteNormalDot);
    }

    void referenceSelectionUsesTheCommandAndRefreshesAfterSuccess()
    {
        WorkspaceState state;
        makeReady(state);
        RecordingColoringCommands commands(state);
        ColoringPanel panel(state, commands);
        auto* reference =
            panel.findChild<QComboBox*>(QStringLiteral("referenceCombo"));
        auto* summary =
            panel.findChild<QLabel*>(QStringLiteral("referenceSummaryLabel"));
        QVERIFY(reference != nullptr);
        QVERIFY(summary != nullptr);
        const int index = comboIndexForMesh(*reference, 3);
        QVERIFY(index >= 0);

        reference->setCurrentIndex(index);

        QCOMPARE(commands.referenceCalls, QVector<MeshId>({3}));
        QCOMPARE(state.referenceId(), MeshId(3));
        QCOMPARE(reference->currentData().toULongLong(), MeshId(3));
        QVERIFY(summary->text().contains(QStringLiteral("mesh-3.obj")));
    }

    void failedReferenceSelectionRestoresCommittedStateAndEmitsStatus()
    {
        WorkspaceState state;
        makeReady(state);
        RecordingColoringCommands commands(state);
        commands.referenceResult =
            OperationResult::failure(QStringLiteral("Reference update failed."));
        ColoringPanel panel(state, commands);
        QSignalSpy status(&panel, &ColoringPanel::statusMessage);
        auto* reference =
            panel.findChild<QComboBox*>(QStringLiteral("referenceCombo"));
        QVERIFY(reference != nullptr);
        const int index = comboIndexForMesh(*reference, 3);
        QVERIFY(index >= 0);

        reference->setCurrentIndex(index);

        QCOMPARE(commands.referenceCalls, QVector<MeshId>({3}));
        QCOMPARE(state.referenceId(), MeshId(1));
        QCOMPARE(reference->currentData().toULongLong(), MeshId(1));
        QCOMPARE(status.size(), 1);
        QCOMPARE(status.takeFirst().front().toString(),
                 QStringLiteral("Reference update failed."));
    }

    void analyzingShowsCancelAndDisablesMutatingControls()
    {
        WorkspaceState state;
        makeReady(state);
        QVERIFY(state.beginAnalysis().ok);
        RecordingColoringCommands commands(state);
        ColoringPanel panel(state, commands);
        auto* apply =
            panel.findChild<QPushButton*>(QStringLiteral("applyColoringButton"));
        auto* cancel =
            panel.findChild<QPushButton*>(QStringLiteral("cancelAnalysisButton"));
        auto* clear =
            panel.findChild<QPushButton*>(QStringLiteral("clearColoringButton"));
        auto* reference =
            panel.findChild<QComboBox*>(QStringLiteral("referenceCombo"));

        QVERIFY(apply != nullptr);
        QVERIFY(cancel != nullptr);
        QVERIFY(clear != nullptr);
        QVERIFY(reference != nullptr);
        QVERIFY(apply->isHidden());
        QVERIFY(!cancel->isHidden());
        QVERIFY(cancel->isEnabled());
        QVERIFY(!clear->isEnabled());
        QVERIFY(!reference->isEnabled());
    }

    void cancelToleratesSynchronousCompletionBeforeTheCommandReturns()
    {
        WorkspaceState state;
        makeReady(state);
        QVERIFY(state.beginAnalysis().ok);
        RecordingColoringCommands commands(state);
        ColoringPanel panel(state, commands);
        commands.cancelAction = [&] {
            state.finishAnalysis();
            AnalysisBatchResult cancelled;
            cancelled.result =
                OperationResult::failure(QStringLiteral("Analysis cancelled."));
            panel.presentAnalysisFinished(cancelled);
        };
        auto* apply =
            panel.findChild<QPushButton*>(QStringLiteral("applyColoringButton"));
        auto* cancel =
            panel.findChild<QPushButton*>(QStringLiteral("cancelAnalysisButton"));
        auto* reference =
            panel.findChild<QComboBox*>(QStringLiteral("referenceCombo"));
        QVERIFY(apply != nullptr);
        QVERIFY(cancel != nullptr);
        QVERIFY(reference != nullptr);

        QTest::mouseClick(cancel, Qt::LeftButton);

        QCOMPARE(commands.cancelCount, 1);
        QCOMPARE(state.phase(), WorkspacePhase::Ready);
        QVERIFY(!apply->isHidden());
        QVERIFY(apply->isEnabled());
        QVERIFY(cancel->isHidden());
        QVERIFY(reference->isEnabled());
    }

    void analysisProgressShowsTheTargetPercentAndStatusMessage()
    {
        WorkspaceState state;
        makeReady(state);
        QVERIFY(state.beginAnalysis().ok);
        RecordingColoringCommands commands(state);
        ColoringPanel panel(state, commands);
        auto* statusLabel =
            panel.findChild<QLabel*>(QStringLiteral("analysisStatusLabel"));
        QVERIFY(statusLabel != nullptr);

        panel.presentAnalysisProgress(
            state.generation(),
            7,
            2,
            42,
            QStringLiteral("Sampling target surface..."));

        QVERIFY(!statusLabel->isHidden());
        QVERIFY(statusLabel->text().contains(QStringLiteral("mesh-2.obj")));
        QVERIFY(statusLabel->text().contains(QStringLiteral("42%")));
        QVERIFY(statusLabel->text().contains(
            QStringLiteral("Sampling target surface...")));
    }

    void successfulAnalysisCompletionReplacesProgressAndRestoresReadyControls()
    {
        WorkspaceState state;
        makeReady(state);
        QVERIFY(state.beginAnalysis().ok);
        RecordingColoringCommands commands(state);
        ColoringPanel panel(state, commands);
        QSignalSpy status(&panel, &ColoringPanel::statusMessage);
        auto* statusLabel =
            panel.findChild<QLabel*>(QStringLiteral("analysisStatusLabel"));
        auto* apply =
            panel.findChild<QPushButton*>(QStringLiteral("applyColoringButton"));
        auto* cancel =
            panel.findChild<QPushButton*>(QStringLiteral("cancelAnalysisButton"));
        QVERIFY(statusLabel != nullptr);
        QVERIFY(apply != nullptr);
        QVERIFY(cancel != nullptr);
        panel.presentAnalysisProgress(
            state.generation(),
            8,
            2,
            73,
            QStringLiteral("Computing scores..."));
        state.finishAnalysis();
        AnalysisBatchResult finished;
        finished.result = OperationResult::success();
        finished.generation = state.generation();
        finished.batchSerial = 8;

        panel.presentAnalysisFinished(finished);

        QCOMPARE(statusLabel->text(), QStringLiteral("Analysis complete."));
        QVERIFY(!statusLabel->isHidden());
        QCOMPARE(status.size(), 0);
        QVERIFY(!apply->isHidden());
        QVERIFY(apply->isEnabled());
        QVERIFY(cancel->isHidden());
    }

    void failedAnalysisCompletionUpdatesOnlyTheLocalNonModalError()
    {
        WorkspaceState state;
        makeReady(state);
        QVERIFY(state.beginAnalysis().ok);
        RecordingColoringCommands commands(state);
        ColoringPanel panel(state, commands);
        QSignalSpy status(&panel, &ColoringPanel::statusMessage);
        auto* statusLabel =
            panel.findChild<QLabel*>(QStringLiteral("analysisStatusLabel"));
        QVERIFY(statusLabel != nullptr);
        state.finishAnalysis();
        AnalysisBatchResult failed;
        failed.result =
            OperationResult::failure(QStringLiteral("Comparison failed."));
        failed.generation = state.generation();
        failed.batchSerial = 9;

        panel.presentAnalysisFinished(failed);

        QCOMPARE(statusLabel->text(), QStringLiteral("Comparison failed."));
        QVERIFY(!statusLabel->isHidden());
        QCOMPARE(status.size(), 0);
        QVERIFY(QApplication::activeModalWidget() == nullptr);
    }

    void clearFailureIsReportedWithoutOpeningADialog()
    {
        WorkspaceState state;
        makeReady(state);
        RecordingColoringCommands commands(state);
        commands.clearResult =
            OperationResult::failure(QStringLiteral("Renderer rejected clear."));
        ColoringPanel panel(state, commands);
        QSignalSpy status(&panel, &ColoringPanel::statusMessage);
        auto* clear =
            panel.findChild<QPushButton*>(QStringLiteral("clearColoringButton"));
        QVERIFY(clear != nullptr);

        QTest::mouseClick(clear, Qt::LeftButton);

        QCOMPARE(commands.clearCount, 1);
        QCOMPARE(status.size(), 1);
        QCOMPARE(status.takeFirst().front().toString(),
                 QStringLiteral("Renderer rejected clear."));
        QVERIFY(QApplication::activeModalWidget() == nullptr);
    }
};

QTEST_MAIN(ColoringPanelTest)
#include "coloring_panel_test.moc"
