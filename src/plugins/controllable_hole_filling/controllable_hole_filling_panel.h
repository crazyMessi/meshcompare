#pragma once

#include <QFrame>

#include "controllable_hole_filling.h"

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QWidget;
class WorkspaceState;

namespace controllable_hole_filling
{

class Panel final : public QFrame
{
    Q_OBJECT

public:
    explicit Panel(
        WorkspaceState& state,
        ICommands& commands,
        QWidget* parent = nullptr);

    void refreshFromState();

signals:
    void operationFailed(const QString& message);

private:
    void apply();
    FillRequest request() const;

    WorkspaceState& state_;
    ICommands& commands_;
    QComboBox* meshCombo_ = nullptr;
    QSpinBox* resolutionSpin_ = nullptr;
    QSpinBox* cclIterationsSpin_ = nullptr;
    QDoubleSpinBox* epsFactorSpin_ = nullptr;
    QComboBox* outputCombo_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QPushButton* applyButton_ = nullptr;
};

} // namespace controllable_hole_filling
