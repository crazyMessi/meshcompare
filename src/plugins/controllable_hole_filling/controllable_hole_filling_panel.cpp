#include "controllable_hole_filling_panel.h"

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

namespace controllable_hole_filling
{
namespace
{

QComboBox* createCombo(QWidget* parent)
{
    auto* combo = new QComboBox(parent);
    combo->setItemDelegate(new QStyledItemDelegate(combo));
    return combo;
}

QLabel* hintLabel(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("holeFillingHint"));
    label->setWordWrap(true);
    return label;
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
    setMinimumWidth(390);

    auto* root = new QVBoxLayout(this);
    root->setSizeConstraint(QLayout::SetFixedSize);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(9);

    auto* title = new QLabel(tr("Release-1536 Hole Filling"), this);
    title->setObjectName(QStringLiteral("holeFillingTitle"));
    QFont titleFont = title->font();
    titleFont.setBold(true);
    title->setFont(titleFont);
    root->addWidget(title);
    root->addWidget(hintLabel(
        tr("Reconstruct a closed surface from a sparse unsigned-distance field."),
        this));

    auto* parameters = new QGroupBox(tr("Parameters"), this);
    parameters->setObjectName(QStringLiteral("holeFillingParametersGroup"));
    auto* form = new QFormLayout(parameters);
    form->setContentsMargins(10, 12, 10, 10);
    form->setSpacing(7);

    meshCombo_ = createCombo(parameters);
    meshCombo_->setObjectName(QStringLiteral("holeFillingMeshCombo"));
    form->addRow(tr("Mesh"), meshCombo_);

    resolutionSpin_ = new QSpinBox(parameters);
    resolutionSpin_->setObjectName(
        QStringLiteral("holeFillingResolutionSpin"));
    resolutionSpin_->setRange(16, 1536);
    resolutionSpin_->setSingleStep(64);
    resolutionSpin_->setValue(512);
    resolutionSpin_->setToolTip(tr(
        "Grid resolution r. Memory and runtime grow approximately with r squared "
        "for the sparse band."));
    form->addRow(tr("r"), resolutionSpin_);

    cclIterationsSpin_ = new QSpinBox(parameters);
    cclIterationsSpin_->setObjectName(
        QStringLiteral("holeFillingCclIterationsSpin"));
    cclIterationsSpin_->setRange(0, 31);
    cclIterationsSpin_->setValue(3);
    cclIterationsSpin_->setToolTip(tr(
        "Closing radius, band-growth iterations, and minimum sparse-band radius."));
    form->addRow(tr("ccl-iter"), cclIterationsSpin_);

    epsFactorSpin_ = new QDoubleSpinBox(parameters);
    epsFactorSpin_->setObjectName(
        QStringLiteral("holeFillingEpsFactorSpin"));
    epsFactorSpin_->setDecimals(3);
    epsFactorSpin_->setRange(0.001, 32.0);
    epsFactorSpin_->setSingleStep(0.25);
    epsFactorSpin_->setValue(2.0);
    epsFactorSpin_->setToolTip(tr(
        "Isosurface factor. The signed-field level is eps / r."));
    form->addRow(tr("eps"), epsFactorSpin_);

    outputCombo_ = createCombo(parameters);
    outputCombo_->setObjectName(QStringLiteral("holeFillingOutputCombo"));
    outputCombo_->addItem(
        tr("New reconstructed layer"),
        static_cast<int>(OutputMode::NewLayer));
    outputCombo_->addItem(
        tr("Replace current mesh"),
        static_cast<int>(OutputMode::CurrentMesh));
    form->addRow(tr("Output"), outputCombo_);
    root->addWidget(parameters);

    root->addWidget(hintLabel(
        tr("The native C++ implementation does not require Python or CUDA."),
        this));

    statusLabel_ = new QLabel(this);
    statusLabel_->setObjectName(QStringLiteral("holeFillingStatusLabel"));
    statusLabel_->setWordWrap(true);
    statusLabel_->hide();
    root->addWidget(statusLabel_);

    auto* actions = new QHBoxLayout;
    actions->addStretch(1);
    applyButton_ = new QPushButton(tr("Fill Holes"), this);
    applyButton_->setObjectName(QStringLiteral("generateHolePatchButton"));
    actions->addWidget(applyButton_);
    root->addLayout(actions);

    connect(
        applyButton_,
        &QPushButton::clicked,
        this,
        &Panel::apply);
    refreshFromState();
}

void Panel::refreshFromState()
{
    const QSignalBlocker meshBlocker(meshCombo_);
    const MeshId selected = state_.selectedMeshId();
    meshCombo_->clear();
    int selectedIndex = -1;
    for (const MeshEntry& mesh : state_.meshes()) {
        meshCombo_->addItem(
            mesh.displayName,
            QVariant::fromValue<qulonglong>(mesh.id));
        if (mesh.id == selected)
            selectedIndex = meshCombo_->count() - 1;
    }
    meshCombo_->setCurrentIndex(selectedIndex);

    const bool ready = state_.phase() == WorkspacePhase::Ready;
    meshCombo_->setEnabled(ready);
    resolutionSpin_->setEnabled(ready);
    cclIterationsSpin_->setEnabled(ready);
    epsFactorSpin_->setEnabled(ready);
    outputCombo_->setEnabled(ready);
    applyButton_->setEnabled(ready && meshCombo_->currentIndex() >= 0);

    if (state_.meshes().size() >= 8 &&
        static_cast<OutputMode>(outputCombo_->currentData().toInt()) ==
            OutputMode::NewLayer) {
        outputCombo_->setCurrentIndex(
            outputCombo_->findData(
                static_cast<int>(OutputMode::CurrentMesh)));
    }
    outputCombo_->setToolTip(
        state_.meshes().size() >= 8
            ? tr("The workspace already has 8 layers; replace the current mesh.")
            : QString());
}

FillRequest Panel::request() const
{
    FillRequest value;
    value.meshId = meshCombo_->currentData().toULongLong();
    value.config.resolution = resolutionSpin_->value();
    value.config.cclIterations = cclIterationsSpin_->value();
    value.config.epsFactor = epsFactorSpin_->value();
    value.outputMode =
        static_cast<OutputMode>(outputCombo_->currentData().toInt());
    return value;
}

void Panel::apply()
{
    if (state_.phase() != WorkspacePhase::Ready)
        return;

    applyButton_->setEnabled(false);
    statusLabel_->setText(tr("Reconstructing surface..."));
    statusLabel_->show();
    FillSummary summary;
    const OperationResult result =
        commands_.fillHoles(request(), &summary);
    if (!result.ok) {
        statusLabel_->setText(result.error);
        emit operationFailed(result.error);
    }
    else {
        statusLabel_->setText(
            tr("%1 vertices, %2 triangles, %3 active cells in %4 s.")
                .arg(summary.vertexCount)
                .arg(summary.faceCount)
                .arg(summary.activeCellCount)
                .arg(summary.elapsedMilliseconds / 1000.0, 0, 'f', 2));
    }
    refreshFromState();
}

} // namespace controllable_hole_filling
