#pragma once

#include <QFrame>

#include "../services/mesh_color_service.h"

class IColoringCommands;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QStackedWidget;
class QTabBar;
class QToolButton;
class QWidget;
class WorkspaceState;

class ColoringPanel final : public QFrame
{
    Q_OBJECT

public:
    explicit ColoringPanel(
        WorkspaceState& state,
        IColoringCommands& commands,
        QWidget* parent = nullptr);

    void refreshFromState();
    void presentAnalysisProgress(
        quint64 generation,
        quint64 batchSerial,
        MeshId meshId,
        int percent,
        const QString& message);
    void presentAnalysisFinished(const AnalysisBatchResult& result);

signals:
    void statusMessage(const QString& message);

private:
    QWidget* buildUniformPage();
    QWidget* buildAnalysisPage();
    void chooseUniformColor();
    void applyCurrentMode();
    void changeSelectedMesh(int index);
    void changeReference(int index);
    void updateModeUi(int modeIndex);
    void reportFailure(const OperationResult& result);
    SurfaceComparisonOptions comparisonOptions() const;

    WorkspaceState& state_;
    IColoringCommands& commands_;
    QTabBar* modeTabs_ = nullptr;
    QComboBox* referenceCombo_ = nullptr;
    QWidget* referenceRow_ = nullptr;
    QStackedWidget* modeStack_ = nullptr;
    QComboBox* uniformMeshCombo_ = nullptr;
    QComboBox* uniformColorCombo_ = nullptr;
    QPushButton* chooseUniformColorButton_ = nullptr;
    QLabel* referenceSummaryLabel_ = nullptr;
    QLabel* targetSummaryLabel_ = nullptr;
    QToolButton* advancedParametersToggle_ = nullptr;
    QWidget* advancedParameters_ = nullptr;
    QWidget* distanceParameters_ = nullptr;
    QWidget* doubleLayerParameters_ = nullptr;
    QSpinBox* sampleCountSpin_ = nullptr;
    QDoubleSpinBox* distanceColorMaxSpin_ = nullptr;
    QSpinBox* nearestNeighborCountSpin_ = nullptr;
    QDoubleSpinBox* oppositeNormalAngleSpin_ = nullptr;
    QLabel* analysisStatusLabel_ = nullptr;
    QPushButton* applyButton_ = nullptr;
    QPushButton* cancelButton_ = nullptr;
    QPushButton* clearButton_ = nullptr;
    bool refreshing_ = false;
};
