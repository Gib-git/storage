#include "OrganizeView.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSplitter>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QSettings>
#include <QVBoxLayout>

#include <map>

#include "RuleDialog.h"
#include "storage/core/ai.hpp"
#include "Theme.h"
#include "storage/core/format.hpp"
#include "storage/core/paths.hpp"
#include "storage/core/volumes.hpp"

using namespace storage;

namespace {
QString qs(const std::string& s) { return QString::fromStdString(s); }

FileSystem* fsOf(const std::shared_ptr<ScanTree>& tree) { return filesystemFor(tree ? tree->volume : "local"); }

QString rulesDir() {
    const QString dir = QDir(qs(toUtf8(appDataDir()))).filePath("rules");
    QDir().mkpath(dir);
    return dir;
}

QString describe(const OrganizeRule& r) {
    QStringList what;
    for (Category c : r.match.categories) what << QString::fromUtf8(categoryName(c));
    for (const auto& e : r.match.extensions) what << "." + qs(e);
    if (!r.match.nameContains.empty()) what << QObject::tr("name has “%1”").arg(qs(r.match.nameContains));
    if (!r.match.nameRegex.empty()) what << QObject::tr("pattern");
    if (r.match.minSize || r.match.maxSize) what << QObject::tr("by size");
    if (r.match.olderThanDays || r.match.newerThanDays) what << QObject::tr("by age");
    const QString match = what.isEmpty() ? QObject::tr("all files") : what.join(", ");
    const QString target = r.leave ? QObject::tr("stay put") : qs(r.folder) + (r.rename.empty() ? "" : "/" + qs(r.rename));
    return QString("%1  —  %2 → %3").arg(qs(r.name), match, target);
}
}  // namespace

OrganizeView::OrganizeView(QWidget* parent) : QWidget(parent) {
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);

    auto* header = new QWidget(this);
    header->setObjectName("Header");
    auto* h = new QHBoxLayout(header);
    h->setContentsMargins(12, 8, 12, 8);
    scopeLabel_ = new QLabel(tr("Scan a folder first"), header);
    auto* change = new QToolButton(header);
    change->setText(tr("Change…"));
    change->setAutoRaise(true);
    connect(change, &QToolButton::clicked, this, &OrganizeView::chooseFolderRequested);
    destLabel_ = new QLabel(header);
    auto* dest = new QToolButton(header);
    dest->setText(tr("Destination…"));
    dest->setAutoRaise(true);
    dest->setPopupMode(QToolButton::InstantPopup);
    auto* destMenu = new QMenu(dest);
    destMenu->addAction(tr("Organize in place"), this, [this] {
        destination_.clear();
        destLabel_->setText(tr("in place"));
        plan_.reset();
        showPlan();
    });
    destMenu->addAction(tr("Into another folder…"), this, [this] {
        const QString d = QFileDialog::getExistingDirectory(this, tr("Put organized files in"));
        if (d.isEmpty()) return;
        destination_ = QDir::toNativeSeparators(d);
        destLabel_->setText(tr("into %1").arg(destination_));
        plan_.reset();
        showPlan();
    });
    dest->setMenu(destMenu);
    destLabel_->setText(tr("in place"));
    destLabel_->setStyleSheet(QString("color:%1").arg(theme::mutedText().name()));
    h->addWidget(scopeLabel_);
    h->addWidget(change);
    h->addSpacing(16);
    h->addWidget(destLabel_, 1);
    h->addWidget(dest);
    v->addWidget(header);

    auto* split = new QSplitter(Qt::Horizontal, this);
    split->setHandleWidth(1);

    // Left: rules --------------------------------------------------------------------
    auto* left = new QWidget(split);
    auto* lv = new QVBoxLayout(left);
    lv->setContentsMargins(12, 10, 12, 10);

    auto* presetRow = new QHBoxLayout;
    presetBox_ = new QComboBox(left);
    presetBox_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    connect(presetBox_, &QComboBox::currentIndexChanged, this, &OrganizeView::selectPreset);
    auto* presetMenuButton = new QToolButton(left);
    presetMenuButton->setText(tr("⋯"));
    presetMenuButton->setPopupMode(QToolButton::InstantPopup);
    auto* presetMenu = new QMenu(presetMenuButton);
    presetMenu->addAction(tr("Suggest with AI (Ollama)…"), this, &OrganizeView::suggestWithAi);
    presetMenu->addSeparator();
    presetMenu->addAction(tr("Save As…"), this, &OrganizeView::savePreset);
    presetMenu->addAction(tr("Import…"), this, &OrganizeView::importPreset);
    presetMenu->addAction(tr("Export…"), this, &OrganizeView::exportPreset);
    presetMenu->addSeparator();
    presetMenu->addAction(tr("Delete Saved Rules"), this, &OrganizeView::deletePreset);
    presetMenuButton->setMenu(presetMenu);
    presetRow->addWidget(new QLabel(tr("Rules:"), left));
    presetRow->addWidget(presetBox_, 1);
    presetRow->addWidget(presetMenuButton);
    lv->addLayout(presetRow);
    presetDescription_ = new QLabel(left);
    presetDescription_->setWordWrap(true);
    presetDescription_->setStyleSheet(QString("color:%1").arg(theme::mutedText().name()));
    lv->addWidget(presetDescription_);

    ruleList_ = new QListWidget(left);
    ruleList_->setAlternatingRowColors(true);
    ruleList_->setWordWrap(true);
    connect(ruleList_, &QListWidget::itemDoubleClicked, this, &OrganizeView::editRule);
    lv->addWidget(ruleList_, 1);
    auto* ruleButtons = new QHBoxLayout;
    auto addButton = [&](const QString& text, auto slot) {
        auto* b = new QPushButton(text, left);
        connect(b, &QPushButton::clicked, this, slot);
        ruleButtons->addWidget(b);
    };
    addButton(tr("Add…"), &OrganizeView::addRule);
    addButton(tr("Edit…"), &OrganizeView::editRule);
    addButton(tr("Remove"), &OrganizeView::removeRule);
    addButton(tr("↑"), [this] { moveRule(-1); });
    addButton(tr("↓"), [this] { moveRule(1); });
    lv->addLayout(ruleButtons);

    auto* opts = new QFormLayout;
    fallback_ = new QLineEdit(left);
    fallback_->setPlaceholderText(tr("leave them where they are"));
    connect(fallback_, &QLineEdit::textEdited, this, &OrganizeView::rulesEdited);
    opts->addRow(tr("Everything else →"), fallback_);
    dateSource_ = new QComboBox(left);
    dateSource_->addItem(tr("Photo date taken, else modified"));
    dateSource_->addItem(tr("Modified date"));
    connect(dateSource_, &QComboBox::currentIndexChanged, this, &OrganizeView::rulesEdited);
    opts->addRow(tr("Dates from:"), dateSource_);
    conflict_ = new QComboBox(left);
    conflict_->addItem(tr("Keep both (add a number)"));
    conflict_->addItem(tr("Skip that file"));
    connect(conflict_, &QComboBox::currentIndexChanged, this, &OrganizeView::rulesEdited);
    opts->addRow(tr("Name already taken:"), conflict_);
    lv->addLayout(opts);
    subfolders_ = new QCheckBox(tr("Include files in subfolders"), left);
    removeEmpty_ = new QCheckBox(tr("Remove folders left empty"), left);
    hidden_ = new QCheckBox(tr("Include hidden files"), left);
    for (auto* cb : {subfolders_, removeEmpty_, hidden_}) {
        connect(cb, &QCheckBox::toggled, this, &OrganizeView::rulesEdited);
        lv->addWidget(cb);
    }
    auto* aiButton = new QPushButton(tr("Suggest with AI…"), left);
    aiButton->setToolTip(tr("Ask a local Ollama model to propose rules for this folder. Only names, kinds, sizes and dates "
                            "are sent — never file contents."));
    connect(aiButton, &QPushButton::clicked, this, &OrganizeView::suggestWithAi);
    lv->addWidget(aiButton);
    previewButton_ = new QPushButton(tr("Preview"), left);
    previewButton_->setDefault(true);
    connect(previewButton_, &QPushButton::clicked, this, &OrganizeView::preview);
    lv->addWidget(previewButton_);

    // Right: preview ------------------------------------------------------------------
    auto* right = new QWidget(split);
    auto* rv = new QVBoxLayout(right);
    rv->setContentsMargins(0, 0, 0, 0);
    rv->setSpacing(0);
    auto* rbar = new QWidget(right);
    auto* rh = new QHBoxLayout(rbar);
    rh->setContentsMargins(12, 10, 12, 10);
    summary_ = new QLabel(rbar);
    summary_->setWordWrap(true);
    rh->addWidget(summary_, 1);
    applyButton_ = new QPushButton(tr("Organize"), rbar);
    connect(applyButton_, &QPushButton::clicked, this, &OrganizeView::apply);
    rh->addWidget(applyButton_);
    rv->addWidget(rbar);
    progress_ = new QProgressBar(right);
    progress_->setTextVisible(false);
    progress_->setMaximumHeight(4);
    progress_->hide();
    rv->addWidget(progress_);
    result_ = new QTreeWidget(right);
    result_->setHeaderLabels({tr("After organizing"), tr("Files"), tr("Size")});
    result_->header()->setStretchLastSection(false);
    result_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    result_->header()->resizeSection(1, 80);
    result_->header()->resizeSection(2, 90);
    result_->setAlternatingRowColors(true);
    result_->setUniformRowHeights(true);
    rv->addWidget(result_, 1);

    split->addWidget(left);
    split->addWidget(right);
    split->setStretchFactor(1, 1);
    split->setSizes({420, 700});
    v->addWidget(split, 1);

    poll_ = new QTimer(this);
    poll_->setInterval(120);
    connect(poll_, &QTimer::timeout, this, [this] {
        if (!executor_) return;
        const auto p = executor_->progress();
        progress_->setRange(0, std::max(1, p.total));
        progress_->setValue(p.done);
        summary_->setText(tr("Organizing… %1 of %2  %3").arg(p.done).arg(p.total).arg(QFileInfo(qs(p.current)).fileName()));
    });

    loadPresets();
    showPlan();
}

OrganizeView::~OrganizeView() {
    if (executor_) executor_->cancel();
    task_.join();
}

void OrganizeView::setScope(std::shared_ptr<ScanTree> tree, Node* folder) {
    if (busy()) return;
    tree_ = std::move(tree);
    folder_ = folder;
    plan_.reset();
    if (folder_) {
        QString name = qs(folder_->name);
        if (!folder_->parent && !QFileInfo(name).fileName().isEmpty()) name = QFileInfo(name).fileName();
        scopeLabel_->setText(tr("Organize <b>%1</b>").arg(name.toHtmlEscaped()));
        const bool local = !tree_ || isLocalVolume(tree_->volume);
        const QString p = qs(folder_->path(local ? nativeSeparator() : '/'));
        scopeLabel_->setToolTip(local ? QDir::toNativeSeparators(p) : p);
    } else {
        scopeLabel_->setText(tr("Scan a folder first"));
    }
    showPlan();
}

// --- Presets -------------------------------------------------------------------------

void OrganizeView::loadPresets() {
    loading_ = true;
    const QString keep = presetBox_->currentText();
    presets_ = builtinPresets();
    presetIsCustom_.assign(presets_.size(), false);
    QDirIterator it(rulesDir(), {"*.json"}, QDir::Files);
    while (it.hasNext()) {
        QFile f(it.next());
        if (!f.open(QIODevice::ReadOnly)) continue;
        try {
            presets_.push_back(RuleSet::fromJson(json::Value::parse(f.readAll().toStdString())));
            presetIsCustom_.push_back(true);
        } catch (const std::exception&) {
            // Ignore broken files; they stay on disk for the user to fix.
        }
    }
    presetBox_->clear();
    for (size_t i = 0; i < presets_.size(); ++i)
        presetBox_->addItem((presetIsCustom_[i] ? QString("★ ") : QString()) + qs(presets_[i].name));
    const int idx = presetBox_->findText(keep);
    loading_ = false;
    presetBox_->setCurrentIndex(idx >= 0 ? idx : 0);
    selectPreset(presetBox_->currentIndex());
}

void OrganizeView::selectPreset(int index) {
    if (loading_ || index < 0 || static_cast<size_t>(index) >= presets_.size()) return;
    rules_ = presets_[static_cast<size_t>(index)];
    loading_ = true;
    presetDescription_->setText(qs(rules_.description));
    fallback_->setText(qs(rules_.fallbackFolder));
    dateSource_->setCurrentIndex(rules_.dateSource == DateSource::Modified ? 1 : 0);
    conflict_->setCurrentIndex(rules_.conflict == ConflictPolicy::Skip ? 1 : 0);
    subfolders_->setChecked(rules_.includeSubfolders);
    removeEmpty_->setChecked(rules_.removeEmptyFolders);
    hidden_->setChecked(rules_.includeHidden);
    loading_ = false;
    showRules();
    plan_.reset();
    showPlan();
}

void OrganizeView::showRules() {
    ruleList_->clear();
    for (const auto& r : rules_.rules) ruleList_->addItem(describe(r));
    if (rules_.rules.empty()) {
        auto* item = new QListWidgetItem(tr("No specific rules — everything follows “Everything else”."));
        item->setFlags(Qt::NoItemFlags);
        ruleList_->addItem(item);
    }
}

RuleSet OrganizeView::currentRules() const {
    RuleSet r = rules_;
    r.fallbackFolder = fallback_->text().trimmed().toStdString();
    r.dateSource = dateSource_->currentIndex() == 1 ? DateSource::Modified : DateSource::CaptureThenModified;
    r.conflict = conflict_->currentIndex() == 1 ? ConflictPolicy::Skip : ConflictPolicy::Rename;
    r.includeSubfolders = subfolders_->isChecked();
    r.removeEmptyFolders = removeEmpty_->isChecked();
    r.includeHidden = hidden_->isChecked();
    return r;
}

void OrganizeView::rulesEdited() {
    if (loading_) return;
    rules_ = currentRules();
    plan_.reset();
    showPlan();
}

void OrganizeView::addRule() {
    OrganizeRule r;
    r.name = tr("New rule").toStdString();
    RuleDialog dlg(r, this);
    if (dlg.exec() != QDialog::Accepted) return;
    rules_ = currentRules();
    rules_.rules.push_back(dlg.rule());
    showRules();
    rulesEdited();
}

void OrganizeView::editRule() {
    const int row = ruleList_->currentRow();
    if (row < 0 || static_cast<size_t>(row) >= rules_.rules.size()) return;
    RuleDialog dlg(rules_.rules[static_cast<size_t>(row)], this);
    if (dlg.exec() != QDialog::Accepted) return;
    rules_ = currentRules();
    rules_.rules[static_cast<size_t>(row)] = dlg.rule();
    showRules();
    ruleList_->setCurrentRow(row);
    rulesEdited();
}

void OrganizeView::removeRule() {
    const int row = ruleList_->currentRow();
    if (row < 0 || static_cast<size_t>(row) >= rules_.rules.size()) return;
    rules_ = currentRules();
    rules_.rules.erase(rules_.rules.begin() + row);
    showRules();
    rulesEdited();
}

void OrganizeView::moveRule(int delta) {
    const int row = ruleList_->currentRow();
    const int to = row + delta;
    if (row < 0 || to < 0 || static_cast<size_t>(to) >= rules_.rules.size()) return;
    rules_ = currentRules();
    std::swap(rules_.rules[static_cast<size_t>(row)], rules_.rules[static_cast<size_t>(to)]);
    showRules();
    ruleList_->setCurrentRow(to);
    rulesEdited();
}

void OrganizeView::savePreset() {
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Save Rules"), tr("Name:"), QLineEdit::Normal,
                                               qs(rules_.name) + tr(" (custom)"), &ok)
                             .trimmed();
    if (!ok || name.isEmpty()) return;
    RuleSet r = currentRules();
    r.name = name.toStdString();
    QString file = name;
    file.replace(QRegularExpression("[^A-Za-z0-9 _-]"), "_");
    QFile f(QDir(rulesDir()).filePath(file + ".json"));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(QByteArray::fromStdString(r.toJson().dump(2))) < 0) {
        QMessageBox::warning(this, tr("Save Rules"), tr("Could not write %1").arg(f.fileName()));
        return;
    }
    f.close();
    loadPresets();
    presetBox_->setCurrentIndex(presetBox_->findText("★ " + name));
    emit message(tr("Saved rules “%1”").arg(name));
}

void OrganizeView::importPreset() {
    const QString path = QFileDialog::getOpenFileName(this, tr("Import Rules"), {}, tr("Rules (*.json)"));
    if (path.isEmpty()) return;
    QFile f(path);
    try {
        if (!f.open(QIODevice::ReadOnly)) throw std::runtime_error("cannot read file");
        const RuleSet r = RuleSet::fromJson(json::Value::parse(f.readAll().toStdString()));
        QFile::copy(path, QDir(rulesDir()).filePath(QFileInfo(path).fileName()));
        loadPresets();
        presetBox_->setCurrentIndex(presetBox_->findText("★ " + qs(r.name)));
    } catch (const std::exception& e) {
        QMessageBox::warning(this, tr("Import Rules"), tr("This file is not a valid rule set:\n%1").arg(qs(e.what())));
    }
}

void OrganizeView::exportPreset() {
    const QString path = QFileDialog::getSaveFileName(this, tr("Export Rules"), qs(rules_.name) + ".json", tr("Rules (*.json)"));
    if (path.isEmpty()) return;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    f.write(QByteArray::fromStdString(currentRules().toJson().dump(2)));
}

void OrganizeView::deletePreset() {
    const int idx = presetBox_->currentIndex();
    if (idx < 0 || !presetIsCustom_[static_cast<size_t>(idx)]) {
        emit message(tr("Built-in rules cannot be deleted."));
        return;
    }
    QDirIterator it(rulesDir(), {"*.json"}, QDir::Files);
    while (it.hasNext()) {
        const QString file = it.next();
        QFile f(file);
        if (!f.open(QIODevice::ReadOnly)) continue;
        try {
            if (RuleSet::fromJson(json::Value::parse(f.readAll().toStdString())).name == rules_.name) {
                f.close();
                QFile::remove(file);
            }
        } catch (const std::exception&) {
        }
    }
    loadPresets();
}

void OrganizeView::suggestWithAi() {
    if (!folder_ || busy()) {
        emit message(tr("Choose a folder first."));
        return;
    }
    QSettings settings;
    OllamaSettings ai;
    ai.host = settings.value("ai/host", qs(ai.host)).toString().toStdString();
    ai.model = settings.value("ai/model", qs(ai.model)).toString().toStdString();

    // Offer the installed models; fall back to free text when Ollama is not reachable.
    std::vector<std::string> models;
    ollamaModels(ai, models);
    QStringList choices;
    for (const auto& m : models) choices << qs(m);
    if (!choices.contains(qs(ai.model))) choices.prepend(qs(ai.model));
    bool ok = false;
    const QString model = QInputDialog::getItem(this, tr("Suggest with AI"),
                                                models.empty() ? tr("Ollama model (Ollama does not seem to be running at %1):").arg(qs(ai.host))
                                                               : tr("Ollama model:"),
                                                choices, choices.indexOf(qs(ai.model)), true, &ok);
    if (!ok || model.isEmpty()) return;
    const QString goal = QInputDialog::getText(this, tr("Suggest with AI"),
                                               tr("What would you like? (optional — e.g. “photos by year, documents by topic”)"),
                                               QLineEdit::Normal, {}, &ok);
    if (!ok) return;
    ai.model = model.toStdString();
    settings.setValue("ai/model", model);

    summary_->setText(tr("Asking %1 for rules… (names and sizes only — no file contents are sent)").arg(model));
    progress_->setRange(0, 0);
    progress_->show();
    previewButton_->setEnabled(false);
    auto keepAlive = tree_;
    Node* folder = folder_;
    const std::string goalText = goal.toStdString();
    task_.start<std::pair<std::shared_ptr<RuleSet>, QString>>(
        this,
        [keepAlive, folder, ai, goalText] {
            try {
                return std::make_pair(std::make_shared<RuleSet>(suggestRules(ai, *folder, goalText)), QString());
            } catch (const std::exception& e) {
                return std::make_pair(std::shared_ptr<RuleSet>(), QString::fromStdString(e.what()));
            }
        },
        [this](std::pair<std::shared_ptr<RuleSet>, QString> result) {
            progress_->hide();
            previewButton_->setEnabled(true);
            if (!result.first) {
                showPlan();
                QMessageBox::warning(this, tr("Suggest with AI"), result.second);
                return;
            }
            presets_.push_back(*result.first);
            presetIsCustom_.push_back(false);
            loading_ = true;
            presetBox_->addItem(qs(result.first->name));
            loading_ = false;
            presetBox_->setCurrentIndex(presetBox_->count() - 1);
            emit message(tr("AI suggested %n rule(s) — review them, then Preview. Nothing moves until you click Organize.",
                            nullptr, static_cast<int>(result.first->rules.size())));
            preview();
        });
}

// --- Preview & apply ----------------------------------------------------------------

void OrganizeView::preview() {
    if (!folder_ || busy()) return;
    rules_ = currentRules();
    previewButton_->setEnabled(false);
    applyButton_->setEnabled(false);
    summary_->setText(tr("Working out where everything goes…"));
    progress_->setRange(0, 0);
    progress_->show();
    FileSystem* fs = fsOf(tree_);
    if (!fs) {
        emit message(tr("This scan belongs to a NAS that is no longer saved."));
        previewButton_->setEnabled(true);
        progress_->hide();
        return;
    }
    auto keepAlive = tree_;
    Node* folder = folder_;
    const RuleSet rules = rules_;
    const std::string dest = destination_.toStdString();
    task_.start<std::shared_ptr<OrganizePlan>>(
        this,
        [keepAlive, folder, rules, dest, fs] {
            std::shared_ptr<OrganizePlan> plan;
            try {
                plan = std::make_shared<OrganizePlan>(Organizer(*fs, rules).plan(*folder, dest));
            } catch (const std::exception&) {
            }
            return plan;
        },
        [this](std::shared_ptr<OrganizePlan> plan) {
            progress_->hide();
            previewButton_->setEnabled(true);
            plan_ = plan ? std::make_unique<OrganizePlan>(*plan) : nullptr;
            showPlan();
            emit previewReady(plan_ ? static_cast<int>(plan_->moves.size()) : 0);
        });
}

void OrganizeView::showPlan() {
    result_->clear();
    applyButton_->setEnabled(plan_ && !plan_->moves.empty() && !busy());
    if (!folder_) {
        summary_->setText(tr("Choose a folder in Disk Usage (or click Change…) to organize it."));
        return;
    }
    if (!plan_) {
        summary_->setText(tr("Choose rules, then click <b>Preview</b>. Nothing moves until you click Organize."));
        return;
    }
    const auto& p = *plan_;
    QString s = tr("<b>%n file(s)</b> will move (%1)", nullptr, static_cast<int>(p.moves.size()))
                    .arg(qs(humanSize(p.bytes)));
    QStringList extra;
    if (p.alreadyInPlace) extra << tr("%n already in place", nullptr, static_cast<int>(p.alreadyInPlace));
    if (p.unmatched) extra << tr("%n not matched", nullptr, static_cast<int>(p.unmatched));
    if (p.conflictsSkipped) extra << tr("%n skipped (name taken)", nullptr, static_cast<int>(p.conflictsSkipped));
    if (p.protectedSkipped) extra << tr("%n inside apps/libraries left alone", nullptr, static_cast<int>(p.protectedSkipped));
    if (!p.removeDirs.empty()) extra << tr("%n emptied folder(s) removed", nullptr, static_cast<int>(p.removeDirs.size()));
    if (!extra.isEmpty()) s += "<br><span style='color:" + theme::mutedText().name() + "'>" + extra.join(" · ") + "</span>";
    summary_->setText(s);

    // Build the "after" tree from destination-relative paths.
    const QString base = qs(p.destination);
    std::map<QString, QTreeWidgetItem*> folders;
    std::map<QTreeWidgetItem*, std::pair<int, uint64_t>> totals;
    auto folderItem = [&](const QString& rel) {
        QTreeWidgetItem* parent = nullptr;
        QString path;
        for (const QString& part : rel.split('/', Qt::SkipEmptyParts)) {
            path += "/" + part;
            auto it = folders.find(path);
            if (it == folders.end()) {
                auto* item = parent ? new QTreeWidgetItem(parent, {part}) : new QTreeWidgetItem(result_, {part});
                item->setIcon(0, style()->standardIcon(QStyle::SP_DirIcon));
                it = folders.emplace(path, item).first;
            }
            parent = it->second;
        }
        return parent;
    };
    constexpr int kMaxFiles = 3000;  // keep the preview responsive on huge folders
    int shown = 0;
    for (const auto& m : p.moves) {
        QString rel = QDir(base).relativeFilePath(qs(m.dst));
        rel = QDir::fromNativeSeparators(rel);
        const int slash = rel.lastIndexOf('/');
        QTreeWidgetItem* parent = slash > 0 ? folderItem(rel.left(slash)) : nullptr;
        for (QTreeWidgetItem* a = parent; a; a = a->parent()) {
            totals[a].first++;
            totals[a].second += m.size;
        }
        if (shown++ >= kMaxFiles) continue;
        const QString name = rel.mid(slash + 1);
        auto* item = parent ? new QTreeWidgetItem(parent, {name}) : new QTreeWidgetItem(result_, {name});
        item->setText(2, qs(humanSize(m.size)));
        item->setToolTip(0, tr("From: %1\nRule: %2").arg(QDir::toNativeSeparators(qs(m.src)), qs(m.rule)));
        if (m.renamed) item->setForeground(0, QColor(0xF5, 0x9E, 0x0B));
    }
    for (auto& [item, t] : totals) {
        item->setText(1, QString::number(t.first));
        item->setText(2, qs(humanSize(t.second)));
    }
    result_->sortItems(0, Qt::AscendingOrder);
    for (int i = 0; i < result_->topLevelItemCount(); ++i) result_->topLevelItem(i)->setExpanded(true);
    applyButton_->setText(p.moves.empty() ? tr("Organize") : tr("Organize %n File(s)", nullptr, static_cast<int>(p.moves.size())));
}

void OrganizeView::apply() {
    if (!plan_ || plan_->moves.empty() || busy()) return;
    const auto answer = QMessageBox::question(
        this, tr("Organize"),
        tr("Move %n file(s)? Every move is recorded and can be undone from History.", nullptr,
           static_cast<int>(plan_->moves.size())));
    if (answer != QMessageBox::Yes) return;

    int64_t batch = 0;
    try {
        journal_ = std::make_unique<Journal>(Journal::defaultPath());
        const QString where = QFileInfo(qs(plan_->scope)).fileName();
        FileSystem* fs = fsOf(tree_);
        if (!fs) throw std::runtime_error("This scan belongs to a NAS that is no longer saved.");
        batch = journal_->createBatch("organize", tr("Organize %1 (%2)").arg(where, qs(rules_.name)).toStdString(),
                                      fs->volumeName(), plan_->toOps());
    } catch (const std::exception& e) {
        QMessageBox::critical(this, tr("Organize"), qs(e.what()));
        return;
    }
    executor_ = std::make_unique<Executor>(*fsOf(tree_), *journal_);
    previewButton_->setEnabled(false);
    applyButton_->setEnabled(false);
    progress_->show();
    poll_->start();
    Executor* ex = executor_.get();
    task_.start<ExecResult>(this, [ex, batch] { return ex->run(batch); }, [this](ExecResult r) {
        poll_->stop();
        progress_->hide();
        executor_.reset();
        journal_.reset();
        plan_.reset();
        previewButton_->setEnabled(true);
        showPlan();
        QString msg = tr("Organized: %n operation(s) done", nullptr, r.done);
        if (r.skipped) msg += tr(" · %n skipped", nullptr, r.skipped);
        if (r.failed) msg += tr(" · %n failed — see History", nullptr, r.failed);
        summary_->setText(msg + tr(" · Undo is available in History."));
        emit message(msg);
        if (r.done) emit filesChanged();
    });
}
