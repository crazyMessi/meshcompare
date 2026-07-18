#include "coloring_panel.h"

#include "coloring_commands.h"
#include "../core/workspace_state.h"

#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayout>
#include <QPalette>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QTabBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVariant>
#include <QWidget>

namespace
{
QString meshName(const WorkspaceState& state, MeshId id)
{
    const MeshEntry* mesh = state.mesh(id);
    return mesh == nullptr ? QStringLiteral("None") : mesh->displayName;
}

QComboBox* createStyledPopupComboBox(QWidget* parent)
{
    auto* combo = new QComboBox(parent);
    combo->setItemDelegate(new QStyledItemDelegate(combo));
    return combo;
}
} // namespace

ColoringPanel::ColoringPanel(
    WorkspaceState& state,
    IColoringCommands& commands,
    QWidget* parent)
    : QFrame(parent), state_(state), commands_(commands)
{
    setObjectName(QStringLiteral("coloringPanel"));
    setWindowModality(Qt::NonModal);
    setFrameShape(QFrame::StyledPanel);
    setFrameShadow(QFrame::Raised);
    setBackgroundRole(QPalette::Window);
    setAutoFillBackground(true);
    setMinimumWidth(360);

    auto* root = new QVBoxLayout(this);
    root->setSizeConstraint(QLayout::SetFixedSize);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(8);

    auto* title = new QLabel(tr("Coloring"), this);
    QFont titleFont = title->font();
    titleFont.setBold(true);
    title->setFont(titleFont);
    root->addWidget(title);

    modeTabs_ = new QTabBar(this);
    modeTabs_->setObjectName(QStringLiteral("coloringModeTabs"));
    modeTabs_->setDrawBase(false);
    modeTabs_->setExpanding(true);
    modeTabs_->addTab(tr("Uniform Color"));
    modeTabs_->addTab(tr("Distance"));
    modeTabs_->addTab(tr("Double Layer"));
    root->addWidget(modeTabs_);

    referenceRow_ = new QWidget(this);
    referenceRow_->setObjectName(QStringLiteral("referenceRow"));
    auto* referenceLayout = new QHBoxLayout(referenceRow_);
    referenceLayout->setContentsMargins(0, 0, 0, 0);
    referenceLayout->setSpacing(6);
    referenceLayout->addWidget(new QLabel(tr("Reference"), referenceRow_));
    referenceCombo_ = createStyledPopupComboBox(referenceRow_);
    referenceCombo_->setObjectName(QStringLiteral("referenceCombo"));
    referenceLayout->addWidget(referenceCombo_, 1);
    root->addWidget(referenceRow_);

    modeStack_ = new QStackedWidget(this);
    modeStack_->addWidget(buildUniformPage());
    modeStack_->addWidget(buildAnalysisPage());
    root->addWidget(modeStack_);

    analysisStatusLabel_ = new QLabel(this);
    analysisStatusLabel_->setObjectName(QStringLiteral("analysisStatusLabel"));
    analysisStatusLabel_->setWordWrap(true);
    analysisStatusLabel_->hide();
    root->addWidget(analysisStatusLabel_);

    auto* actions = new QHBoxLayout;
    clearButton_ = new QPushButton(tr("Clear Coloring"), this);
    clearButton_->setObjectName(QStringLiteral("clearColoringButton"));
    cancelButton_ = new QPushButton(tr("Cancel"), this);
    cancelButton_->setObjectName(QStringLiteral("cancelAnalysisButton"));
    applyButton_ = new QPushButton(tr("Apply Color"), this);
    applyButton_->setObjectName(QStringLiteral("applyColoringButton"));
    actions->addWidget(clearButton_);
    actions->addStretch(1);
    actions->addWidget(cancelButton_);
    actions->addWidget(applyButton_);
    root->addLayout(actions);

    connect(
        modeTabs_,
        &QTabBar::currentChanged,
        this,
        &ColoringPanel::updateModeUi);
    connect(
        referenceCombo_,
        QOverload<int>::of(&QComboBox::currentIndexChanged),
        this,
        &ColoringPanel::changeReference);
    connect(
        uniformMeshCombo_,
        QOverload<int>::of(&QComboBox::currentIndexChanged),
        this,
        &ColoringPanel::changeSelectedMesh);
    connect(
        advancedParametersToggle_,
        &QToolButton::toggled,
        this,
        [this](bool expanded) {
            advancedParameters_->setVisible(expanded);
            advancedParametersToggle_->setArrowType(
                expanded ? Qt::DownArrow : Qt::RightArrow);
        });
    connect(
        applyButton_,
        &QPushButton::clicked,
        this,
        &ColoringPanel::applyCurrentMode);
    connect(
        chooseUniformColorButton_,
        &QPushButton::clicked,
        this,
        &ColoringPanel::chooseUniformColor);
    connect(cancelButton_, &QPushButton::clicked, this, [this] {
        commands_.cancelAnalysis();
        // Cancellation may have delivered completion synchronously. Always
        // derive the final button state from the committed workspace phase.
        refreshFromState();
    });
    connect(clearButton_, &QPushButton::clicked, this, [this] {
        const OperationResult result = commands_.clearColoring();
        reportFailure(result);
        refreshFromState();
    });

    advancedParameters_->hide();
    updateModeUi(0);
    refreshFromState();
}

QWidget* ColoringPanel::buildUniformPage()
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    auto* meshRow = new QWidget(page);
    auto* meshLayout = new QHBoxLayout(meshRow);
    meshLayout->setContentsMargins(0, 0, 0, 0);
    meshLayout->setSpacing(6);
    meshLayout->addWidget(new QLabel(tr("Mesh"), meshRow));
    uniformMeshCombo_ = createStyledPopupComboBox(meshRow);
    uniformMeshCombo_->setObjectName(QStringLiteral("uniformMeshCombo"));
    meshLayout->addWidget(uniformMeshCombo_, 1);
    layout->addWidget(meshRow);

    auto* colorRow = new QWidget(page);
    auto* colorLayout = new QHBoxLayout(colorRow);
    colorLayout->setContentsMargins(0, 0, 0, 0);
    colorLayout->setSpacing(6);
    colorLayout->addWidget(new QLabel(tr("Color"), colorRow));
    uniformColorCombo_ = createStyledPopupComboBox(colorRow);
    uniformColorCombo_->setObjectName(QStringLiteral("uniformColorCombo"));
    uniformColorCombo_->addItem(
        tr("Green"), QColor(QStringLiteral("#5aaa75")));
    uniformColorCombo_->addItem(
        tr("Blue"), QColor(QStringLiteral("#4f8cc9")));
    uniformColorCombo_->addItem(
        tr("Red"), QColor(QStringLiteral("#d65c5c")));
    uniformColorCombo_->addItem(
        tr("Gold"), QColor(QStringLiteral("#d9a441")));
    uniformColorCombo_->addItem(
        tr("Gray"), QColor(QStringLiteral("#a6adb4")));
    colorLayout->addWidget(uniformColorCombo_, 1);
    chooseUniformColorButton_ = new QPushButton(tr("Choose Color…"), colorRow);
    chooseUniformColorButton_->setObjectName(
        QStringLiteral("chooseUniformColorButton"));
    colorLayout->addWidget(chooseUniformColorButton_);
    layout->addWidget(colorRow);
    layout->addStretch(1);
    return page;
}

QWidget* ColoringPanel::buildAnalysisPage()
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    referenceSummaryLabel_ = new QLabel(page);
    referenceSummaryLabel_->setObjectName(
        QStringLiteral("referenceSummaryLabel"));
    targetSummaryLabel_ = new QLabel(page);
    targetSummaryLabel_->setObjectName(QStringLiteral("targetSummaryLabel"));
    targetSummaryLabel_->setWordWrap(true);
    layout->addWidget(referenceSummaryLabel_);
    layout->addWidget(targetSummaryLabel_);

    advancedParametersToggle_ = new QToolButton(page);
    advancedParametersToggle_->setObjectName(
        QStringLiteral("advancedParametersToggle"));
    advancedParametersToggle_->setText(tr("Advanced Parameters"));
    advancedParametersToggle_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    advancedParametersToggle_->setArrowType(Qt::RightArrow);
    advancedParametersToggle_->setCheckable(true);
    advancedParametersToggle_->setChecked(false);
    layout->addWidget(advancedParametersToggle_);

    advancedParameters_ = new QWidget(page);
    advancedParameters_->setObjectName(QStringLiteral("advancedParameters"));
    auto* advancedLayout = new QVBoxLayout(advancedParameters_);
    advancedLayout->setContentsMargins(12, 0, 0, 0);
    advancedLayout->setSpacing(6);

    distanceParameters_ = new QWidget(advancedParameters_);
    distanceParameters_->setObjectName(QStringLiteral("distanceParameters"));
    auto* distanceLayout = new QFormLayout(distanceParameters_);
    distanceLayout->setContentsMargins(0, 0, 0, 0);
    distanceLayout->setSpacing(6);
    distanceMappingCombo_ = createStyledPopupComboBox(distanceParameters_);
    distanceMappingCombo_->setObjectName(
        QStringLiteral("distanceMappingCombo"));
    distanceMappingCombo_->addItem(
        tr("Square root"),
        static_cast<int>(DistanceColorMapping::SquareRoot));
    distanceMappingCombo_->addItem(
        tr("Linear"),
        static_cast<int>(DistanceColorMapping::Linear));
    distanceLayout->addRow(tr("Mapping"), distanceMappingCombo_);
    distanceColorMaxSpin_ = new QDoubleSpinBox(distanceParameters_);
    distanceColorMaxSpin_->setObjectName(
        QStringLiteral("distanceColorMaxSpin"));
    distanceColorMaxSpin_->setDecimals(6);
    distanceColorMaxSpin_->setRange(0.0, 1000000.0);
    distanceColorMaxSpin_->setValue(0.04);
    distanceLayout->addRow(tr("Maximum distance"), distanceColorMaxSpin_);
    advancedLayout->addWidget(distanceParameters_);

    doubleLayerParameters_ = new QWidget(advancedParameters_);
    doubleLayerParameters_->setObjectName(
        QStringLiteral("doubleLayerParameters"));
    auto* doubleLayerLayout = new QFormLayout(doubleLayerParameters_);
    doubleLayerLayout->setContentsMargins(0, 0, 0, 0);
    doubleLayerLayout->setSpacing(6);

    sampleCountSpin_ = new QSpinBox(doubleLayerParameters_);
    sampleCountSpin_->setObjectName(QStringLiteral("sampleCountSpin"));
    sampleCountSpin_->setRange(1, 5000000);
    sampleCountSpin_->setValue(500000);
    doubleLayerLayout->addRow(tr("Samples"), sampleCountSpin_);

    nearestNeighborCountSpin_ = new QSpinBox(doubleLayerParameters_);
    nearestNeighborCountSpin_->setObjectName(
        QStringLiteral("nearestNeighborCountSpin"));
    nearestNeighborCountSpin_->setRange(1, 1000);
    nearestNeighborCountSpin_->setValue(20);
    doubleLayerLayout->addRow(
        tr("Nearest neighbors"), nearestNeighborCountSpin_);

    oppositeNormalAngleSpin_ = new QDoubleSpinBox(doubleLayerParameters_);
    oppositeNormalAngleSpin_->setObjectName(
        QStringLiteral("oppositeNormalAngleSpin"));
    oppositeNormalAngleSpin_->setDecimals(1);
    oppositeNormalAngleSpin_->setRange(0.0, 180.0);
    oppositeNormalAngleSpin_->setValue(170.0);
    oppositeNormalAngleSpin_->setSuffix(tr("°"));
    doubleLayerLayout->addRow(
        tr("Opposite angle"), oppositeNormalAngleSpin_);
    advancedLayout->addWidget(doubleLayerParameters_);

    layout->addWidget(advancedParameters_);
    layout->addStretch(1);
    return page;
}

void ColoringPanel::refreshFromState()
{
    QScopedValueRollback<bool> guard(refreshing_, true);
    const QSignalBlocker referenceBlocker(referenceCombo_);
    const QSignalBlocker meshBlocker(uniformMeshCombo_);
    referenceCombo_->clear();
    uniformMeshCombo_->clear();
    int referenceIndex = -1;
    int selectedIndex = -1;
    for (const MeshEntry& mesh : state_.meshes()) {
        referenceCombo_->addItem(
            mesh.displayName,
            QVariant::fromValue<qulonglong>(mesh.id));
        uniformMeshCombo_->addItem(
            mesh.displayName,
            QVariant::fromValue<qulonglong>(mesh.id));
        if (mesh.id == state_.referenceId())
            referenceIndex = referenceCombo_->count() - 1;
        if (mesh.id == state_.selectedMeshId())
            selectedIndex = uniformMeshCombo_->count() - 1;
    }
    referenceCombo_->setCurrentIndex(referenceIndex);
    uniformMeshCombo_->setCurrentIndex(selectedIndex);
    referenceSummaryLabel_->setText(
        tr("Reference: %1").arg(meshName(state_, state_.referenceId())));

    const bool ready = state_.phase() == WorkspacePhase::Ready;
    const bool analyzing = state_.phase() == WorkspacePhase::Analyzing;
    referenceCombo_->setEnabled(ready);
    uniformMeshCombo_->setEnabled(ready);
    uniformColorCombo_->setEnabled(ready);
    chooseUniformColorButton_->setEnabled(ready);
    clearButton_->setEnabled(ready);
    advancedParametersToggle_->setEnabled(ready);
    sampleCountSpin_->setEnabled(ready);
    distanceMappingCombo_->setEnabled(ready);
    distanceColorMaxSpin_->setEnabled(ready);
    nearestNeighborCountSpin_->setEnabled(ready);
    oppositeNormalAngleSpin_->setEnabled(ready);
    applyButton_->setVisible(!analyzing);
    applyButton_->setEnabled(ready);
    cancelButton_->setVisible(analyzing);
    cancelButton_->setEnabled(analyzing);
    updateModeUi(modeTabs_->currentIndex());
}

void ColoringPanel::presentAnalysisProgress(
    quint64 generation,
    quint64 batchSerial,
    MeshId meshId,
    int percent,
    const QString& message)
{
    Q_UNUSED(batchSerial);
    if (generation != state_.generation() ||
        state_.phase() != WorkspacePhase::Analyzing) {
        return;
    }
    const MeshEntry* mesh = state_.mesh(meshId);
    if (mesh == nullptr)
        return;

    QString text = tr("%1 — %2%").arg(mesh->displayName).arg(percent);
    if (!message.isEmpty())
        text.append(tr(" — %1").arg(message));
    analysisStatusLabel_->setText(text);
    analysisStatusLabel_->show();
}

void ColoringPanel::presentAnalysisFinished(const AnalysisBatchResult& result)
{
    if (result.generation != 0 && result.generation != state_.generation())
        return;
    analysisStatusLabel_->setText(
        result.result.ok
            ? tr("Analysis complete.")
            : (result.result.error.isEmpty()
                   ? tr("Analysis failed.")
                   : result.result.error));
    analysisStatusLabel_->show();
    // The controller owns global error publication. This callback only keeps
    // the panel's local, modeless status in sync with the completed batch.
    refreshFromState();
}

void ColoringPanel::chooseUniformColor()
{
    const QColor current = uniformColorCombo_->currentData().value<QColor>();
    const QColor selected = QColorDialog::getColor(
        current,
        this,
        tr("Choose Uniform Color"));
    if (!selected.isValid())
        return;

    int index = uniformColorCombo_->findData(selected);
    if (index < 0) {
        uniformColorCombo_->addItem(
            tr("Custom %1").arg(selected.name(QColor::HexRgb).toUpper()),
            selected);
        index = uniformColorCombo_->count() - 1;
    }
    uniformColorCombo_->setCurrentIndex(index);
}

void ColoringPanel::applyCurrentMode()
{
    if (state_.phase() != WorkspacePhase::Ready) {
        refreshFromState();
        return;
    }

    OperationResult result;
    const int modeIndex = modeTabs_->currentIndex();
    if (modeIndex == 0) {
        const QColor color = uniformColorCombo_->currentData().value<QColor>();
        if (!color.isValid()) {
            result = OperationResult::failure(
                tr("Select a valid uniform color."));
        }
        else {
            // Selection is intentionally resolved here, not when the panel is
            // opened, so viewport activation cannot leave a stale target.
            result = commands_.setUniformColor(state_.selectedMeshId(), color);
        }
    }
    else {
        const SurfaceComparisonMetric metric = modeIndex == 1
            ? SurfaceComparisonMetric::DistanceToReference
            : SurfaceComparisonMetric::DoubleLayer;
        result = commands_.startAnalysis(metric, comparisonOptions());
        if (result.ok) {
            analysisStatusLabel_->setText(tr("Starting analysis…"));
            analysisStatusLabel_->show();
        }
    }

    reportFailure(result);
    refreshFromState();
}

void ColoringPanel::changeReference(int index)
{
    if (refreshing_ || index < 0)
        return;
    const MeshId meshId = referenceCombo_->itemData(index).toULongLong();
    const OperationResult result = commands_.setReference(meshId);
    reportFailure(result);
    // Both success and failure are derived from committed state. In
    // particular, a renderer-rejected command snaps the selector back.
    refreshFromState();
}

void ColoringPanel::changeSelectedMesh(int index)
{
    if (refreshing_ || index < 0)
        return;
    const MeshId meshId = uniformMeshCombo_->itemData(index).toULongLong();
    const OperationResult result = commands_.selectMesh(meshId);
    reportFailure(result);
    refreshFromState();
}

void ColoringPanel::updateModeUi(int modeIndex)
{
    const bool uniform = modeIndex == 0;
    const bool distance = modeIndex == 1;
    const bool doubleLayer = modeIndex == 2;
    modeStack_->setCurrentIndex(uniform ? 0 : 1);
    distanceParameters_->setVisible(distance);
    doubleLayerParameters_->setVisible(doubleLayer);
    referenceRow_->setVisible(!doubleLayer);
    referenceCombo_->setEnabled(
        state_.phase() == WorkspacePhase::Ready && !doubleLayer);
    referenceSummaryLabel_->setVisible(!doubleLayer);

    QStringList targets;
    for (const MeshEntry& mesh : state_.meshes()) {
        if (doubleLayer || mesh.id != state_.referenceId())
            targets.append(mesh.displayName);
    }
    targetSummaryLabel_->setText(
        tr("Targets: %1").arg(
            targets.isEmpty() ? tr("None") : targets.join(QStringLiteral(", "))));
    applyButton_->setText(uniform ? tr("Apply Color") : tr("Run Analysis"));
}

void ColoringPanel::reportFailure(const OperationResult& result)
{
    if (!result.ok && !result.error.isEmpty())
        emit statusMessage(result.error);
}

SurfaceComparisonOptions ColoringPanel::comparisonOptions() const
{
    SurfaceComparisonOptions options;
    options.sampleCount = sampleCountSpin_->value();
    options.distanceColorMax = distanceColorMaxSpin_->value();
    options.distanceColorMapping = static_cast<DistanceColorMapping>(
        distanceMappingCombo_->currentData().toInt());
    options.nearestNeighborCount = nearestNeighborCountSpin_->value();
    options.oppositeNormalAngleDegrees = oppositeNormalAngleSpin_->value();
    options.doubleLayerRandomSeed = 0;
    return options;
}
