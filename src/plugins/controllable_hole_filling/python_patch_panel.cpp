#include "python_patch_panel.h"

#include "core/workspace_state.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayout>
#include <QPalette>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStyledItemDelegate>
#include <QVBoxLayout>
#include <QVariant>

namespace python_hole_filling
{
namespace
{

QComboBox* comboBox(QWidget* parent)
{
    auto* combo = new QComboBox(parent);
    combo->setItemDelegate(new QStyledItemDelegate(combo));
    return combo;
}

} // namespace

Panel::Panel(
    WorkspaceState& state,
    ICommands& commands,
    QWidget* parent)
    : QFrame(parent), state_(state), commands_(commands)
{
    setObjectName(QStringLiteral("controllableHoleFillingPanel"));
    setWindowModality(Qt::NonModal);
    setFrameShape(QFrame::StyledPanel);
    setFrameShadow(QFrame::Raised);
    setBackgroundRole(QPalette::Window);
    setAutoFillBackground(true);
    setMinimumWidth(400);

    auto* root = new QVBoxLayout(this);
    root->setSizeConstraint(QLayout::SetFixedSize);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(9);

    auto* title = new QLabel(tr("Controllable Hole Filling"), this);
    title->setObjectName(QStringLiteral("holeFillingTitle"));
    QFont titleFont = title->font();
    titleFont.setBold(true);
    title->setFont(titleFont);
    root->addWidget(title);

    auto* hint = new QLabel(
        tr("Patch manifold boundary loops with fixed boundary vertices."),
        this);
    hint->setObjectName(QStringLiteral("holeFillingHint"));
    hint->setWordWrap(true);
    root->addWidget(hint);

    auto* processing = new QGroupBox(tr("Processing"), this);
    auto* processingForm = new QFormLayout(processing);
    processingForm->setContentsMargins(10, 12, 10, 10);
    processingForm->setSpacing(7);

    meshCombo_ = comboBox(processing);
    meshCombo_->setObjectName(QStringLiteral("holeFillingMeshCombo"));
    processingForm->addRow(tr("Mesh"), meshCombo_);

    methodCombo_ = comboBox(processing);
    methodCombo_->setObjectName(QStringLiteral("holeFillingMethodCombo"));
    methodCombo_->addItem(
        tr("Curvature constrained"),
        static_cast<int>(PatchMethod::Hybrid));
    methodCombo_->addItem(
        tr("Planar"),
        static_cast<int>(PatchMethod::Planar));
    processingForm->addRow(tr("Patch"), methodCombo_);

    scopeCombo_ = comboBox(processing);
    scopeCombo_->setObjectName(QStringLiteral("holeFillingScopeCombo"));
    scopeCombo_->addItem(
        tr("Largest eligible hole"),
        static_cast<int>(HoleScope::Largest));
    scopeCombo_->addItem(
        tr("Up to limit"),
        static_cast<int>(HoleScope::UpToLimit));
    scopeCombo_->addItem(
        tr("All eligible holes"),
        static_cast<int>(HoleScope::All));
    scopeCombo_->setCurrentIndex(1);
    processingForm->addRow(tr("Holes"), scopeCombo_);

    maxHolesSpin_ = new QSpinBox(processing);
    maxHolesSpin_->setObjectName(QStringLiteral("holeFillingMaxHolesSpin"));
    maxHolesSpin_->setRange(1, 128);
    maxHolesSpin_->setValue(8);
    processingForm->addRow(tr("Hole limit"), maxHolesSpin_);

    minLoopVerticesSpin_ = new QSpinBox(processing);
    minLoopVerticesSpin_->setObjectName(
        QStringLiteral("holeFillingMinLoopVerticesSpin"));
    minLoopVerticesSpin_->setRange(3, 256);
    minLoopVerticesSpin_->setValue(6);
    processingForm->addRow(
        tr("Minimum loop vertices"), minLoopVerticesSpin_);

    minPerimeterSpin_ = new QDoubleSpinBox(processing);
    minPerimeterSpin_->setObjectName(
        QStringLiteral("holeFillingMinPerimeterSpin"));
    minPerimeterSpin_->setDecimals(6);
    minPerimeterSpin_->setRange(0.0, 1000000000.0);
    processingForm->addRow(
        tr("Minimum perimeter"), minPerimeterSpin_);
    root->addWidget(processing);

    auto* geometry = new QGroupBox(tr("Patch Geometry"), this);
    auto* geometryForm = new QFormLayout(geometry);
    geometryForm->setContentsMargins(10, 12, 10, 10);
    geometryForm->setSpacing(7);

    targetVerticesSpin_ = new QSpinBox(geometry);
    targetVerticesSpin_->setObjectName(
        QStringLiteral("holeFillingTargetVerticesSpin"));
    targetVerticesSpin_->setRange(3, 4096);
    targetVerticesSpin_->setValue(512);
    geometryForm->addRow(tr("Target vertices"), targetVerticesSpin_);

    normalWeightSpin_ = new QDoubleSpinBox(geometry);
    normalWeightSpin_->setObjectName(
        QStringLiteral("holeFillingNormalWeightSpin"));
    normalWeightSpin_->setDecimals(3);
    normalWeightSpin_->setRange(0.0, 160.0);
    normalWeightSpin_->setValue(40.0);
    geometryForm->addRow(
        tr("Boundary normal weight"), normalWeightSpin_);

    anchorWeightSpin_ = new QDoubleSpinBox(geometry);
    anchorWeightSpin_->setObjectName(
        QStringLiteral("holeFillingAnchorWeightSpin"));
    anchorWeightSpin_->setDecimals(5);
    anchorWeightSpin_->setSingleStep(0.0005);
    anchorWeightSpin_->setRange(0.0, 0.02);
    anchorWeightSpin_->setValue(0.002);
    geometryForm->addRow(
        tr("Shape anchor weight"), anchorWeightSpin_);

    maxTangentRmsSpin_ = new QDoubleSpinBox(geometry);
    maxTangentRmsSpin_->setObjectName(
        QStringLiteral("holeFillingMaxTangentRmsSpin"));
    maxTangentRmsSpin_->setDecimals(3);
    maxTangentRmsSpin_->setSingleStep(0.01);
    maxTangentRmsSpin_->setRange(0.001, 1.0);
    maxTangentRmsSpin_->setValue(0.15);
    geometryForm->addRow(
        tr("Maximum tangent RMS"), maxTangentRmsSpin_);
    root->addWidget(geometry);

    auto* outputForm = new QFormLayout;
    outputCombo_ = comboBox(this);
    outputCombo_->setObjectName(QStringLiteral("holeFillingOutputCombo"));
    outputCombo_->addItem(
        tr("New patch layer"),
        static_cast<int>(OutputMode::NewLayer));
    outputCombo_->addItem(
        tr("Update current mesh"),
        static_cast<int>(OutputMode::CurrentMesh));
    outputForm->addRow(tr("Output"), outputCombo_);
    root->addLayout(outputForm);

    auto* collisionHint = new QLabel(
        tr("Collision and self-intersection checks are not performed."),
        this);
    collisionHint->setObjectName(QStringLiteral("holeFillingHint"));
    root->addWidget(collisionHint);

    statusLabel_ = new QLabel(this);
    statusLabel_->setObjectName(QStringLiteral("holeFillingStatusLabel"));
    statusLabel_->setWordWrap(true);
    statusLabel_->hide();
    root->addWidget(statusLabel_);

    auto* actions = new QHBoxLayout;
    actions->addStretch(1);
    applyButton_ = new QPushButton(tr("Generate Patch"), this);
    applyButton_->setObjectName(QStringLiteral("generateHolePatchButton"));
    actions->addWidget(applyButton_);
    root->addLayout(actions);

    connect(
        methodCombo_,
        QOverload<int>::of(&QComboBox::currentIndexChanged),
        this,
        [this] { updateMethodUi(); });
    connect(
        scopeCombo_,
        QOverload<int>::of(&QComboBox::currentIndexChanged),
        this,
        [this] {
            maxHolesSpin_->setEnabled(
                static_cast<HoleScope>(
                    scopeCombo_->currentData().toInt()) ==
                HoleScope::UpToLimit);
        });
    connect(
        applyButton_,
        &QPushButton::clicked,
        this,
        &Panel::apply);
    refreshFromState();
}

void Panel::refreshFromState()
{
    const QSignalBlocker blocker(meshCombo_);
    meshCombo_->clear();
    int selectedIndex = -1;
    for (const MeshEntry& mesh : state_.meshes()) {
        meshCombo_->addItem(
            mesh.displayName,
            QVariant::fromValue<qulonglong>(mesh.id));
        if (mesh.id == state_.selectedMeshId())
            selectedIndex = meshCombo_->count() - 1;
    }
    meshCombo_->setCurrentIndex(selectedIndex);

    const bool ready = state_.phase() == WorkspacePhase::Ready;
    meshCombo_->setEnabled(ready);
    methodCombo_->setEnabled(ready);
    scopeCombo_->setEnabled(ready);
    maxHolesSpin_->setEnabled(
        ready &&
        static_cast<HoleScope>(scopeCombo_->currentData().toInt()) ==
            HoleScope::UpToLimit);
    minLoopVerticesSpin_->setEnabled(ready);
    minPerimeterSpin_->setEnabled(ready);
    outputCombo_->setEnabled(ready);
    applyButton_->setEnabled(ready && selectedIndex >= 0);
    if (state_.meshes().size() >= 8 &&
        static_cast<OutputMode>(outputCombo_->currentData().toInt()) ==
            OutputMode::NewLayer) {
        outputCombo_->setCurrentIndex(
            outputCombo_->findData(
                static_cast<int>(OutputMode::CurrentMesh)));
    }
    updateMethodUi();
}

void Panel::updateMethodUi()
{
    const bool enabled =
        state_.phase() == WorkspacePhase::Ready &&
        static_cast<PatchMethod>(methodCombo_->currentData().toInt()) ==
            PatchMethod::Hybrid;
    targetVerticesSpin_->setEnabled(enabled);
    normalWeightSpin_->setEnabled(enabled);
    anchorWeightSpin_->setEnabled(enabled);
    maxTangentRmsSpin_->setEnabled(enabled);
}

FillRequest Panel::request() const
{
    FillRequest request;
    request.meshId = meshCombo_->currentData().toULongLong();
    request.config.method =
        static_cast<PatchMethod>(methodCombo_->currentData().toInt());
    request.config.scope =
        static_cast<HoleScope>(scopeCombo_->currentData().toInt());
    request.config.targetVertices = targetVerticesSpin_->value();
    request.config.maxHoles = maxHolesSpin_->value();
    request.config.minLoopVertices = minLoopVerticesSpin_->value();
    request.config.minHolePerimeter = minPerimeterSpin_->value();
    request.config.normalWeight = normalWeightSpin_->value();
    request.config.anchorWeight = anchorWeightSpin_->value();
    request.config.maxTangentRms = maxTangentRmsSpin_->value();
    request.outputMode =
        static_cast<OutputMode>(outputCombo_->currentData().toInt());
    return request;
}

void Panel::apply()
{
    if (state_.phase() != WorkspacePhase::Ready)
        return;
    applyButton_->setEnabled(false);
    statusLabel_->setText(tr("Generating patch..."));
    statusLabel_->show();
    FillSummary summary;
    const OperationResult result =
        commands_.fillPythonHoles(request(), &summary);
    if (!result.ok) {
        statusLabel_->setText(result.error);
        emit operationFailed(result.error);
    }
    else {
        statusLabel_->setText(
            tr("Patched %1 hole(s): %2 vertices, %3 triangles.")
                .arg(summary.patchedHoleCount)
                .arg(summary.patchVertexCount)
                .arg(summary.patchFaceCount));
    }
    refreshFromState();
}

} // namespace python_hole_filling
