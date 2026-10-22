#pragma once

#include <QDialog>

#include "storage/core/organize.hpp"

class QCheckBox;
class QLineEdit;
class QRadioButton;
class QSpinBox;
class QLabel;

// Edit one organize rule: which files it matches and where they go.
class RuleDialog : public QDialog {
    Q_OBJECT
public:
    RuleDialog(const storage::OrganizeRule& rule, QWidget* parent = nullptr);
    storage::OrganizeRule rule() const;

private:
    void accept() override;
    void updateExample();

    QLineEdit* name_;
    QList<QCheckBox*> types_;
    QLineEdit* extensions_;
    QLineEdit* contains_;
    QLineEdit* regex_;
    QSpinBox* minMb_;
    QSpinBox* maxMb_;
    QSpinBox* older_;
    QSpinBox* newer_;
    QRadioButton* moveRadio_;
    QRadioButton* leaveRadio_;
    QLineEdit* folder_;
    QLineEdit* rename_;
    QLabel* example_;
};
