#pragma once

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QSpinBox;

// Preferences, stored in QSettings. Keys:
//   ui/theme, dupes/minSizeIndex, ai/host, ai/model, api/enabled, api/port, scans/autoRescanDays
class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(QWidget* parent = nullptr);

signals:
    void themeChanged(int mode);
    void apiSettingsChanged();

private:
    void save();

    QComboBox* theme_;
    QComboBox* minSize_;
    QSpinBox* rescanDays_;
    QLineEdit* ollamaHost_;
    QLineEdit* ollamaModel_;
    QCheckBox* apiEnabled_;
    QSpinBox* apiPort_;
};
