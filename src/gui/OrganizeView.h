#pragma once

#include <QWidget>

#include <memory>

#include "BackgroundTask.h"
#include "storage/core/executor.hpp"
#include "storage/core/journal.hpp"
#include "storage/core/organize.hpp"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QProgressBar;
class QPushButton;
class QTimer;
class QTreeWidget;

class OrganizeView : public QWidget {
    Q_OBJECT
public:
    explicit OrganizeView(QWidget* parent = nullptr);
    ~OrganizeView() override;

    void setScope(std::shared_ptr<storage::ScanTree> tree, storage::Node* folder);
    bool busy() const { return task_.running(); }
    void previewNow() { preview(); }

signals:
    void chooseFolderRequested();
    void filesChanged();
    void message(const QString& text);
    void previewReady(int moves);

private:
    void loadPresets();
    void selectPreset(int index);
    void showRules();
    storage::RuleSet currentRules() const;
    void rulesEdited();
    void addRule();
    void editRule();
    void removeRule();
    void moveRule(int delta);
    void savePreset();
    void importPreset();
    void exportPreset();
    void deletePreset();
    void suggestWithAi();

    void preview();
    void showPlan();
    void apply();

    std::shared_ptr<storage::ScanTree> tree_;
    storage::Node* folder_ = nullptr;
    QString destination_;  // empty = organize in place

    std::vector<storage::RuleSet> presets_;
    std::vector<bool> presetIsCustom_;
    storage::RuleSet rules_;
    bool loading_ = false;

    std::unique_ptr<storage::OrganizePlan> plan_;
    std::unique_ptr<storage::Journal> journal_;
    std::unique_ptr<storage::Executor> executor_;
    BackgroundTask task_;
    QTimer* poll_ = nullptr;

    QLabel* scopeLabel_ = nullptr;
    QLabel* destLabel_ = nullptr;
    QComboBox* presetBox_ = nullptr;
    QLabel* presetDescription_ = nullptr;
    QListWidget* ruleList_ = nullptr;
    QLineEdit* fallback_ = nullptr;
    QComboBox* dateSource_ = nullptr;
    QComboBox* conflict_ = nullptr;
    QCheckBox* subfolders_ = nullptr;
    QCheckBox* removeEmpty_ = nullptr;
    QCheckBox* hidden_ = nullptr;
    QPushButton* previewButton_ = nullptr;

    QLabel* summary_ = nullptr;
    QTreeWidget* result_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QPushButton* applyButton_ = nullptr;
};
