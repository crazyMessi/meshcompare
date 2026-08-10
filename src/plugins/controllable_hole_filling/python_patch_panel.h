#pragma once

#include <QFrame>

#include "python_patch.h"

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QWidget;
class WorkspaceState;

namespace python_hole_filling
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
    void updateMethodUi();
    FillRequest request() const;

    WorkspaceState& state_;
    ICommands& commands_;
    QComboBox* meshCombo_ = nullptr;
    QComboBox* methodCombo_ = nullptr;
    QComboBox* scopeCombo_ = nullptr;
    QSpinBox* maxHolesSpin_ = nullptr;
    QSpinBox* minLoopVerticesSpin_ = nullptr;
    QDoubleSpinBox* minPerimeterSpin_ = nullptr;
    QSpinBox* targetVerticesSpin_ = nullptr;
    QDoubleSpinBox* normalWeightSpin_ = nullptr;
    QDoubleSpinBox* anchorWeightSpin_ = nullptr;
    QDoubleSpinBox* maxTangentRmsSpin_ = nullptr;
    QComboBox* outputCombo_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QPushButton* applyButton_ = nullptr;
};

} // namespace python_hole_filling
