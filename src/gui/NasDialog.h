#pragma once

#include <QDialog>

#include "BackgroundTask.h"
#include "storage/core/ssh_fs.hpp"

class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;

// Add or edit a NAS connection, with a live "Test Connection".
class NasDialog : public QDialog {
    Q_OBJECT
public:
    NasDialog(const storage::NasConfig& config, QWidget* parent = nullptr);
    storage::NasConfig config() const;

private:
    void test();
    void accept() override;

    QLineEdit* name_;
    QLineEdit* host_;
    QLineEdit* user_;
    QSpinBox* port_;
    QLineEdit* key_;
    QLineEdit* root_;
    QLabel* status_;
    QPushButton* testButton_;
    std::vector<std::string> transport_;
    BackgroundTask task_;
};
