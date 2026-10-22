#include "CleanupView.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSplitter>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <functional>
#include <map>

#include "Theme.h"
#include "storage/core/format.hpp"
#include "storage/core/paths.hpp"
#include "storage/core/volumes.hpp"

using namespace storage;

namespace {
QString qs(const std::string& s) { return QString::fromStdString(s); }

LocalFileSystem& localFs() {
    static LocalFileSystem fs;
    return fs;
}

QColor safetyColor(Safety s) {
    switch (s) {
        case Safety::Safe: return QColor(0x22, 0xA0, 0x6B);
        case Safety::Review: return QColor(0xF5, 0x9E, 0x0B);
        case Safety::Caution: return QColor(0xEF, 0x44, 0x44);
    }
    return theme::mutedText();
}

enum { IndexRole = Qt::UserRole + 1, SizeRole, PathRole };
enum FindingKind { JunkKind = 0, LargeKind = 1, EmptyKind = 2 };
}  // namespace

CleanupView::CleanupView(QWidget* parent) : QWidget(parent) {
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    tabs_ = new QTabWidget(this);
    tabs_->setDocumentMode(true);
    v->addWidget(tabs_, 1);
    progress_ = new QProgressBar(this);
    progress_->setTextVisible(false);
    progress_->setMaximumHeight(4);
    progress_->hide();
    v->addWidget(progress_);

    // --- System & apps --------------------------------------------------------------
    auto* sys = new QWidget(tabs_);
    auto* sv = new QVBoxLayout(sys);
    sv->setContentsMargins(0, 0, 0, 0);
    sv->setSpacing(0);
    auto* split = new QSplitter(Qt::Vertical, sys);
    split->setHandleWidth(1);

    targets_ = new QTreeWidget(split);
    targets_->setHeaderLabels({tr("Location"), tr("Size"), tr("Safety"), tr("What it is")});
    targets_->header()->setStretchLastSection(true);
    targets_->header()->resizeSection(0, 260);
    targets_->header()->resizeSection(1, 90);
    targets_->header()->resizeSection(2, 80);
    targets_->setAlternatingRowColors(true);
    connect(targets_, &QTreeWidget::itemChanged, this, [this] { updateSystemSummary(); });

    commands_ = new QTreeWidget(split);
    commands_->setHeaderLabels({tr("Run it yourself"), tr("Command"), QString()});
    commands_->setRootIsDecorated(false);
    commands_->header()->resizeSection(0, 260);
    commands_->header()->resizeSection(1, 420);
    for (const auto& c : cleanCommands()) {
        auto* it = new QTreeWidgetItem(commands_, {qs(c.name), qs(c.command)});
        it->setToolTip(0, qs(c.description));
        it->setToolTip(1, qs(c.description));
        QFont mono("Menlo");
        mono.setStyleHint(QFont::Monospace);
        it->setFont(1, mono);
        auto* copy = new QToolButton(commands_);
        copy->setText(tr("Copy"));
        copy->setAutoRaise(true);
        const QString cmd = qs(c.command);
        connect(copy, &QToolButton::clicked, this, [this, cmd] {
            QApplication::clipboard()->setText(cmd);
            emit message(tr("Copied: %1").arg(cmd));
        });
        commands_->setItemWidget(it, 2, copy);
    }
    split->addWidget(targets_);
    split->addWidget(commands_);
    split->setSizes({480, 150});
    sv->addWidget(split, 1);

    auto* sbar = new QWidget(sys);
    auto* sh = new QHBoxLayout(sbar);
    sh->setContentsMargins(12, 8, 12, 8);
    permanent_ = new QCheckBox(tr("Delete caches permanently (they rebuild automatically, frees space now)"), sbar);
    permanent_->setChecked(true);
    permanent_->setToolTip(tr("Items marked Review or Caution always go to the Trash first."));
    connect(permanent_, &QCheckBox::toggled, this, &CleanupView::updateSystemSummary);
    systemSummary_ = new QLabel(sbar);
    rescanButton_ = new QPushButton(tr("Measure Again"), sbar);
    connect(rescanButton_, &QPushButton::clicked, this, &CleanupView::analyze);
    cleanButton_ = new QPushButton(tr("Clean Up"), sbar);
    cleanButton_->setDefault(true);
    connect(cleanButton_, &QPushButton::clicked, this, &CleanupView::cleanSystem);
    sh->addWidget(permanent_);
    sh->addStretch(1);
    sh->addWidget(systemSummary_);
    sh->addWidget(rescanButton_);
    sh->addWidget(cleanButton_);
    sv->addWidget(sbar);
    tabs_->addTab(sys, tr("System && Apps"));

    // --- In this folder -------------------------------------------------------------
    auto* fold = new QWidget(tabs_);
    auto* fv = new QVBoxLayout(fold);
    fv->setContentsMargins(0, 0, 0, 0);
    fv->setSpacing(0);
    auto* fbar = new QWidget(fold);
    auto* fh = new QHBoxLayout(fbar);
    fh->setContentsMargins(12, 8, 12, 8);
    folderLabel_ = new QLabel(tr("Scan a folder first"), fbar);
    age_ = new QComboBox(fbar);
    for (const auto& [label, days] : std::vector<std::pair<QString, int>>{
             {tr("untouched for 3 months"), 90}, {tr("untouched for 6 months"), 180}, {tr("untouched for 1 year"), 365},
             {tr("untouched for 2 years"), 730}})
        age_->addItem(label, days);
    age_->setCurrentIndex(1);
    minSize_ = new QComboBox(fbar);
    for (const auto& [label, mb] : std::vector<std::pair<QString, int>>{
             {tr("large = 50 MB+"), 50}, {tr("large = 100 MB+"), 100}, {tr("large = 500 MB+"), 500}, {tr("large = 1 GB+"), 1024}})
        minSize_->addItem(label, mb);
    minSize_->setCurrentIndex(1);
    auto* find = new QPushButton(tr("Find"), fbar);
    connect(find, &QPushButton::clicked, this, &CleanupView::findInFolder);
    fh->addWidget(folderLabel_, 1);
    fh->addWidget(age_);
    fh->addWidget(minSize_);
    fh->addWidget(find);
    fv->addWidget(fbar);

    findings_ = new QTreeWidget(fold);
    findings_->setHeaderLabels({tr("Item"), tr("Size"), tr("Last changed"), tr("Why")});
    findings_->header()->resizeSection(0, 460);
    findings_->header()->resizeSection(1, 90);
    findings_->header()->resizeSection(2, 120);
    findings_->setAlternatingRowColors(true);
    findings_->setUniformRowHeights(true);
    connect(findings_, &QTreeWidget::itemChanged, this, [this] { updateFolderSummary(); });
    fv->addWidget(findings_, 1);

    auto* fbar2 = new QWidget(fold);
    auto* fh2 = new QHBoxLayout(fbar2);
    fh2->setContentsMargins(12, 8, 12, 8);
    folderSummary_ = new QLabel(fbar2);
    folderClean_ = new QPushButton(tr("Move to Trash"), fbar2);
    connect(folderClean_, &QPushButton::clicked, this, &CleanupView::cleanFolder);
    fh2->addWidget(folderSummary_, 1);
    fh2->addWidget(folderClean_);
    fv->addWidget(fbar2);
    tabs_->addTab(fold, tr("In This Folder"));

    poll_ = new QTimer(this);
    poll_->setInterval(120);
    connect(poll_, &QTimer::timeout, this, [this] {
        if (!executor_) return;
        const auto p = executor_->progress();
        progress_->setRange(0, std::max(1, p.total));
        progress_->setValue(p.done);
        emit message(tr("Cleaning… %1 of %2  %3").arg(p.done).arg(p.total).arg(QFileInfo(qs(p.current)).fileName()));
    });
    updateSystemSummary();
    updateFolderSummary();
}

CleanupView::~CleanupView() {
    cancel_ = true;
    if (executor_) executor_->cancel();
    task_.join();
}

void CleanupView::showEvent(QShowEvent* e) {
    QWidget::showEvent(e);
    ensureAnalyzed();
}

void CleanupView::ensureAnalyzed() {
    if (!analyzedOnce_ && !busy()) analyze();
}

void CleanupView::setWorking(bool on, const QString& text) {
    cleanButton_->setEnabled(!on);
    rescanButton_->setEnabled(!on);
    folderClean_->setEnabled(!on);
    if (on) {
        progress_->setRange(0, 0);
        progress_->show();
        if (!text.isEmpty()) emit message(text);
    } else {
        progress_->hide();
        updateSystemSummary();
        updateFolderSummary();
    }
}

// --- System & apps -----------------------------------------------------------------

void CleanupView::analyze() {
    if (busy()) return;
    analyzedOnce_ = true;
    setWorking(true, tr("Measuring caches, logs and other junk…"));
    targets_->clear();
    auto* cancel = &cancel_;
    task_.start<std::vector<CleanItem>>(
        this, [cancel] { return analyzeCleanTargets(localFs(), cleanCatalogue(), cancel); },
        [this](std::vector<CleanItem> items) {
            items_ = std::move(items);
            showItems();
            setWorking(false);
            uint64_t total = 0;
            for (const auto& i : items_) total += i.bytes;
            emit message(tr("Found %1 that can be cleaned").arg(qs(humanSize(total))));
            emit analyzed();
        });
}

void CleanupView::showItems() {
    const QSignalBlocker block(targets_);
    targets_->clear();
    std::map<std::string, QTreeWidgetItem*> groups;
    for (size_t i = 0; i < items_.size(); ++i) {
        const auto& item = items_[i];
        QTreeWidgetItem*& g = groups[item.target.group];
        if (!g) {
            g = new QTreeWidgetItem(targets_, {qs(item.target.group)});
            QFont f = g->font(0);
            f.setBold(true);
            g->setFont(0, f);
            g->setFlags(Qt::ItemIsEnabled);
            g->setExpanded(true);
        }
        auto* it = new QTreeWidgetItem(g, {qs(item.target.name), qs(humanSize(item.bytes)), safetyName(item.target.safety),
                                           qs(item.target.description + (item.target.hint.empty() ? "" : " " + item.target.hint))});
        it->setData(0, IndexRole, static_cast<int>(i));
        it->setForeground(2, safetyColor(item.target.safety));
        it->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
        it->setToolTip(3, it->text(3));
        QStringList paths;
        for (const auto& p : item.target.paths) paths << QDir::toNativeSeparators(qs(expandUserPath(p)));
        it->setToolTip(0, paths.join("\n"));
        const bool empty = item.entries.empty();
        it->setFlags(empty ? Qt::ItemIsEnabled : Qt::ItemIsEnabled | Qt::ItemIsUserCheckable | Qt::ItemIsSelectable);
        if (!empty) it->setCheckState(0, item.target.defaultOn ? Qt::Checked : Qt::Unchecked);
        if (empty) {
            for (int c = 0; c < 4; ++c) it->setForeground(c, theme::mutedText());
            it->setText(1, tr("—"));
        }
    }
    updateSystemSummary();
}

void CleanupView::updateSystemSummary() {
    uint64_t bytes = 0;
    int count = 0;
    for (QTreeWidgetItemIterator it(targets_); *it; ++it) {
        if (!((*it)->flags() & Qt::ItemIsUserCheckable) || (*it)->checkState(0) != Qt::Checked) continue;
        bytes += items_[static_cast<size_t>((*it)->data(0, IndexRole).toInt())].bytes;
        ++count;
    }
    systemSummary_->setText(count ? tr("<b>%1</b> selected").arg(qs(humanSize(bytes))) : tr("Nothing selected"));
    cleanButton_->setEnabled(count > 0 && !busy());
}

void CleanupView::cleanSystem() {
    std::vector<CleanItem> chosen;
    bool caution = false;
    for (QTreeWidgetItemIterator it(targets_); *it; ++it) {
        if (!((*it)->flags() & Qt::ItemIsUserCheckable) || (*it)->checkState(0) != Qt::Checked) continue;
        const auto& item = items_[static_cast<size_t>((*it)->data(0, IndexRole).toInt())];
        caution |= item.target.safety == Safety::Caution;
        chosen.push_back(item);
    }
    if (chosen.empty()) return;
    uint64_t bytes = 0;
    QStringList names;
    for (const auto& c : chosen) {
        bytes += c.bytes;
        names << QString("• %1 (%2)").arg(qs(c.target.name), qs(humanSize(c.bytes)));
    }
    QString text = tr("Clean up %1?\n\n%2").arg(qs(humanSize(bytes)), names.join("\n"));
    if (permanent_->isChecked()) text += tr("\n\nCaches marked Safe are deleted permanently; everything else goes to the Trash.");
    if (caution) text += tr("\n\nYou selected items marked Caution — make sure you don't need them.");
    if (QMessageBox::question(this, tr("Clean Up"), text) != QMessageBox::Yes) return;
    run(planClean(chosen, permanent_->isChecked()), tr("Clean up %1").arg(qs(humanSize(bytes))), "local", [this] { analyze(); });
}

// --- In this folder ----------------------------------------------------------------

void CleanupView::setScope(std::shared_ptr<ScanTree> tree, Node* folder) {
    if (busy()) return;
    tree_ = std::move(tree);
    folder_ = folder;
    findings_->clear();
    if (folder_) {
        QString name = qs(folder_->name);
        if (!folder_->parent && !QFileInfo(name).fileName().isEmpty()) name = QFileInfo(name).fileName();
        folderLabel_->setText(tr("Look in <b>%1</b> for files and folders that are:").arg(name.toHtmlEscaped()));
    } else {
        folderLabel_->setText(tr("Scan a folder in Disk Usage first"));
    }
    updateFolderSummary();
}

void CleanupView::findInFolder() {
    if (!folder_) return;
    const QSignalBlocker block(findings_);
    findings_->clear();
    FileSystem* fs = filesystemFor(tree_ ? tree_->volume : "local");
    const char sep = fs ? fs->separator() : nativeSeparator();
    const int days = age_->currentData().toInt();
    const uint64_t minSize = static_cast<uint64_t>(minSize_->currentData().toInt()) * 1024 * 1024;

    auto section = [&](const QString& title, const std::vector<Finding>& list, int kind, bool sized) {
        uint64_t total = 0;
        for (const auto& f : list) total += f.size;
        auto* top = new QTreeWidgetItem(findings_, {title + QString("  (%1)").arg(list.size())});
        QFont f = top->font(0);
        f.setBold(true);
        top->setFont(0, f);
        if (sized) top->setText(1, qs(humanSize(total)));
        top->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable | Qt::ItemIsAutoTristate);
        top->setCheckState(0, Qt::Unchecked);
        top->setData(0, IndexRole, kind);
        const QString base = qs(folder_->path(sep));
        for (const auto& fd : list) {
            const QString path = qs(fd.path);
            QString shown = QDir(base).relativeFilePath(path);
            auto* it = new QTreeWidgetItem(top, {QDir::toNativeSeparators(shown), sized ? qs(humanSize(fd.size)) : QString(),
                                                 QLocale().toString(QDateTime::fromSecsSinceEpoch(fd.mtime).date(), QLocale::ShortFormat),
                                                 qs(fd.reason)});
            it->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable | Qt::ItemIsSelectable);
            it->setCheckState(0, Qt::Unchecked);
            it->setData(0, PathRole, path);
            it->setData(0, SizeRole, static_cast<qulonglong>(fd.size));
            it->setData(0, IndexRole, kind);
            it->setData(1, Qt::UserRole, fd.isDir);
            it->setData(2, Qt::UserRole, static_cast<qlonglong>(fd.mtime));
            it->setToolTip(0, QDir::toNativeSeparators(path));
            it->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
        }
        top->setExpanded(!list.empty() && list.size() < 200);
    };
    section(tr("Rebuildable project folders in idle projects"), findProjectJunk(*folder_, sep, days), JunkKind, true);
    section(tr("Large files not modified recently"), findLargeOldFiles(*folder_, sep, minSize, days), LargeKind, true);
    section(tr("Empty folders"), findEmptyFolders(*folder_, sep), EmptyKind, false);
    updateFolderSummary();
}

void CleanupView::updateFolderSummary() {
    uint64_t bytes = 0;
    int count = 0;
    for (QTreeWidgetItemIterator it(findings_); *it; ++it) {
        if ((*it)->data(0, PathRole).isNull() || (*it)->checkState(0) != Qt::Checked) continue;
        bytes += (*it)->data(0, SizeRole).toULongLong();
        ++count;
    }
    folderSummary_->setText(count ? tr("<b>%n item(s)</b> selected · %1", nullptr, count).arg(qs(humanSize(bytes)))
                                  : (folder_ ? tr("Tick items to move them to the Trash.") : QString()));
    folderClean_->setEnabled(count > 0 && !busy());
}

void CleanupView::cleanFolder() {
    std::vector<JournalOp> ops;
    uint64_t bytes = 0;
    for (QTreeWidgetItemIterator it(findings_); *it; ++it) {
        if ((*it)->data(0, PathRole).isNull() || (*it)->checkState(0) != Qt::Checked) continue;
        JournalOp op;
        op.src = (*it)->data(0, PathRole).toString().toStdString();
        op.size = (*it)->data(0, SizeRole).toULongLong();
        const bool isDir = (*it)->data(1, Qt::UserRole).toBool();
        op.action = (*it)->data(0, IndexRole).toInt() == EmptyKind ? Action::RemoveDir : Action::Trash;
        op.mtime = isDir ? 0 : (*it)->data(2, Qt::UserRole).toLongLong();
        bytes += op.size;
        ops.push_back(std::move(op));
    }
    if (ops.empty()) return;
    if (QMessageBox::question(this, tr("Move to Trash"),
                              tr("Move %n item(s) (%1) to the Trash? Empty folders are simply removed.", nullptr,
                                 static_cast<int>(ops.size()))
                                  .arg(qs(humanSize(bytes)))) != QMessageBox::Yes)
        return;
    run(std::move(ops), tr("Clean up in %1").arg(folder_ ? qs(folder_->name) : QString()), tree_ ? tree_->volume : "local", [this] {
        findings_->clear();
        updateFolderSummary();
        emit message(tr("Done. Rescan to refresh this folder's findings."));
    });
}

// --- Shared -----------------------------------------------------------------------

void CleanupView::run(std::vector<JournalOp> ops, const QString& title, const std::string& volume, std::function<void()> after) {
    if (busy()) return;
    int64_t batch = 0;
    FileSystem* fs = filesystemFor(volume);
    try {
        if (!fs) throw std::runtime_error("This scan belongs to a NAS that is no longer saved.");
        journal_ = std::make_unique<Journal>(Journal::defaultPath());
        batch = journal_->createBatch("clean", title.toStdString(), fs->volumeName(), ops);
    } catch (const std::exception& e) {
        QMessageBox::critical(this, tr("Clean Up"), qs(e.what()));
        return;
    }
    executor_ = std::make_unique<Executor>(*fs, *journal_);
    setWorking(true);
    poll_->start();
    Executor* ex = executor_.get();
    task_.start<ExecResult>(this, [ex, batch] { return ex->run(batch); }, [this, after](ExecResult r) {
        poll_->stop();
        executor_.reset();
        journal_.reset();
        setWorking(false);
        QString msg = tr("Freed %1").arg(qs(humanSize(r.bytes)));
        if (r.skipped) msg += tr(" · %n skipped", nullptr, r.skipped);
        if (r.failed) msg += tr(" · %n could not be removed (in use?) — see History", nullptr, r.failed);
        emit message(msg);
        if (r.done) emit filesChanged();
        after();
    });
}
