#include "DuplicatesView.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QSortFilterProxyModel>
#include <QStackedWidget>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QUrl>
#include <QVBoxLayout>

#include "DuplicatesModel.h"
#include "Theme.h"
#include "storage/core/filesystem.hpp"
#include "storage/core/format.hpp"
#include "storage/core/index.hpp"
#include "storage/core/journal.hpp"
#include "storage/core/paths.hpp"
#include "storage/core/volumes.hpp"

using namespace storage;

namespace {
enum Page { EmptyPage = 0, ProgressPage = 1, ResultsPage = 2 };
QString qs(const std::string& s) { return QString::fromStdString(s); }

FileSystem& fsOf(const std::shared_ptr<ScanTree>& tree) {
    FileSystem* fs = filesystemFor(tree ? tree->volume : "local");
    if (!fs) throw std::runtime_error("This scan belongs to a NAS that is no longer saved.");
    return *fs;
}

QString shownPath(const std::shared_ptr<ScanTree>& tree, const Node* n) {
    const bool local = !tree || isLocalVolume(tree->volume);
    const QString p = qs(n->path(local ? nativeSeparator() : '/'));
    return local ? QDir::toNativeSeparators(p) : p;
}

void reveal(const QString& path) {
#if defined(Q_OS_MACOS)
    QProcess::startDetached("open", {"-R", path});
#elif defined(Q_OS_WIN)
    QProcess::startDetached("explorer.exe", {"/select,", QDir::toNativeSeparators(path)});
#else
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
#endif
}
}  // namespace

DuplicatesView::DuplicatesView(QWidget* parent) : QWidget(parent) {
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);

    // Scope + options -------------------------------------------------------------
    auto* header = new QWidget(this);
    header->setObjectName("Header");
    auto* h = new QHBoxLayout(header);
    h->setContentsMargins(12, 8, 12, 8);
    scopeLabel_ = new QLabel(tr("Scan a folder first"), header);
    scopeLabel_->setTextFormat(Qt::RichText);
    auto* change = new QToolButton(header);
    change->setText(tr("Change…"));
    change->setAutoRaise(true);
    connect(change, &QToolButton::clicked, this, &DuplicatesView::chooseFolderRequested);

    minSize_ = new QComboBox(header);
    minSize_->addItem(tr("Any size"), QVariant::fromValue<qulonglong>(1));
    minSize_->addItem(tr("≥ 100 KB"), QVariant::fromValue<qulonglong>(100ULL * 1024));
    minSize_->addItem(tr("≥ 1 MB"), QVariant::fromValue<qulonglong>(1024ULL * 1024));
    minSize_->addItem(tr("≥ 10 MB"), QVariant::fromValue<qulonglong>(10ULL * 1024 * 1024));
    minSize_->addItem(tr("≥ 100 MB"), QVariant::fromValue<qulonglong>(100ULL * 1024 * 1024));
    minSize_->setCurrentIndex(2);
    minSize_->setToolTip(tr("Ignore files smaller than this"));
    skipHidden_ = new QCheckBox(tr("Skip hidden"), header);
    skipHidden_->setChecked(true);
    paranoid_ = new QCheckBox(tr("Byte-by-byte check"), header);
    paranoid_->setToolTip(tr("After matching SHA-256 hashes, also compare every byte (slower)"));
    findButton_ = new QPushButton(tr("Find Duplicates"), header);
    findButton_->setDefault(true);
    connect(findButton_, &QPushButton::clicked, this, &DuplicatesView::find);

    h->addWidget(scopeLabel_, 1);
    h->addWidget(change);
    h->addSpacing(12);
    h->addWidget(minSize_);
    h->addWidget(skipHidden_);
    h->addWidget(paranoid_);
    h->addWidget(findButton_);
    v->addWidget(header);

    stack_ = new QStackedWidget(this);
    v->addWidget(stack_, 1);

    // Empty -----------------------------------------------------------------------
    emptyLabel_ = new QLabel(this);
    emptyLabel_->setAlignment(Qt::AlignCenter);
    emptyLabel_->setWordWrap(true);
    emptyLabel_->setStyleSheet(QString("color:%1; font-size:14px;").arg(theme::mutedText().name()));
    emptyLabel_->setText(tr("Find files with identical contents.\n\nFiles are compared by size, then by a SHA-256 hash "
                            "of their contents — names don't matter. Nothing is removed until you choose."));
    stack_->addWidget(emptyLabel_);

    // Progress ----------------------------------------------------------------------
    auto* progress = new QWidget(this);
    auto* pv = new QVBoxLayout(progress);
    pv->setContentsMargins(80, 0, 80, 0);
    pv->addStretch(2);
    progressTitle_ = new QLabel(progress);
    progressTitle_->setObjectName("WelcomeTitle");
    progressTitle_->setAlignment(Qt::AlignCenter);
    pv->addWidget(progressTitle_);
    progressDetail_ = new QLabel(progress);
    progressDetail_->setAlignment(Qt::AlignCenter);
    pv->addWidget(progressDetail_);
    pv->addSpacing(10);
    progressBar_ = new QProgressBar(progress);
    progressBar_->setTextVisible(false);
    progressBar_->setMaximumHeight(6);
    pv->addWidget(progressBar_);
    pv->addSpacing(10);
    auto* stopRow = new QHBoxLayout;
    stopRow->addStretch(1);
    auto* stop = new QPushButton(tr("Stop"), progress);
    connect(stop, &QPushButton::clicked, this, &DuplicatesView::cancel);
    stopRow->addWidget(stop);
    stopRow->addStretch(1);
    pv->addLayout(stopRow);
    pv->addStretch(3);
    stack_->addWidget(progress);

    // Results -----------------------------------------------------------------------
    auto* results = new QWidget(this);
    auto* rv = new QVBoxLayout(results);
    rv->setContentsMargins(0, 0, 0, 0);
    rv->setSpacing(0);

    auto* bar = new QWidget(results);
    auto* bh = new QHBoxLayout(bar);
    bh->setContentsMargins(12, 6, 12, 6);
    bh->addWidget(new QLabel(tr("Keep:"), bar));
    keepRule_ = new QComboBox(bar);
    keepRule_->addItem(tr("Oldest copy"), static_cast<int>(KeepRule::Oldest));
    keepRule_->addItem(tr("Newest copy"), static_cast<int>(KeepRule::Newest));
    keepRule_->addItem(tr("Shortest path"), static_cast<int>(KeepRule::ShortestPath));
    keepRule_->addItem(tr("Longest path"), static_cast<int>(KeepRule::LongestPath));
    keepRule_->addItem(tr("First alphabetically"), static_cast<int>(KeepRule::Alphabetical));
    bh->addWidget(keepRule_);
    auto* autoSelect = new QPushButton(tr("Select Extra Copies"), bar);
    autoSelect->setToolTip(tr("Tick every copy except the one chosen by the rule"));
    connect(autoSelect, &QPushButton::clicked, this, [this] {
        model_->applyRule(static_cast<KeepRule>(keepRule_->currentData().toInt()));
    });
    bh->addWidget(autoSelect);
    auto* clear = new QPushButton(tr("Clear"), bar);
    connect(clear, &QPushButton::clicked, this, [this] { model_->clearSelection(); });
    bh->addWidget(clear);
    bh->addSpacing(12);
    summary_ = new QLabel(bar);
    bh->addWidget(summary_, 1);

    trashButton_ = new QPushButton(tr("Move to Trash"), bar);
    trashButton_->setToolTip(tr("Recoverable: files go to the Trash / Recycle Bin and can be restored from History"));
    connect(trashButton_, &QPushButton::clicked, this, [this] { apply(Action::Trash); });
    bh->addWidget(trashButton_);

    moreButton_ = new QToolButton(bar);
    moreButton_->setText(tr("More"));
    moreButton_->setPopupMode(QToolButton::InstantPopup);
    auto* more = new QMenu(moreButton_);
    more->addAction(tr("Replace with Hard Links"), this, [this] { apply(Action::Hardlink); })
        ->setToolTip(tr("Every path keeps working; the data is stored once. Same drive only."));
#if !defined(Q_OS_WIN)
    more->addAction(tr("Replace with Space-Saving Clones"), this, [this] { apply(Action::Clone); })
        ->setToolTip(tr("Copy-on-write clones (APFS, Btrfs, XFS): independent files that share storage"));
#endif
    more->addSeparator();
    more->addAction(tr("Delete Permanently…"), this, [this] { apply(Action::Delete); });
    more->setToolTipsVisible(true);
    moreButton_->setMenu(more);
    bh->addWidget(moreButton_);
    rv->addWidget(bar);

    model_ = new DuplicatesModel(this);
    connect(model_, &DuplicatesModel::selectionChanged, this, &DuplicatesView::updateSummary);
    connect(model_, &DuplicatesModel::refused, this, &DuplicatesView::message);
    proxy_ = new QSortFilterProxyModel(this);
    proxy_->setSourceModel(model_);
    proxy_->setSortRole(DuplicatesModel::SortRole);

    view_ = new QTreeView(results);
    view_->setModel(proxy_);
    view_->setUniformRowHeights(true);
    view_->setAlternatingRowColors(true);
    view_->setSortingEnabled(true);
    view_->sortByColumn(DuplicatesModel::Size, Qt::DescendingOrder);
    view_->setContextMenuPolicy(Qt::CustomContextMenu);
    view_->header()->setStretchLastSection(true);
    view_->header()->resizeSection(DuplicatesModel::Name, 360);
    view_->header()->resizeSection(DuplicatesModel::Size, 110);
    view_->header()->resizeSection(DuplicatesModel::Modified, 150);
    connect(view_, &QTreeView::customContextMenuRequested, this, &DuplicatesView::showContextMenu);
    connect(view_, &QTreeView::doubleClicked, this, [this](const QModelIndex& idx) {
        const QString path = model_->pathAt(proxy_->mapToSource(idx));
        if (!path.isEmpty()) reveal(path);
    });
    rv->addWidget(view_, 1);
    stack_->addWidget(results);

    poll_ = new QTimer(this);
    poll_->setInterval(120);
    connect(poll_, &QTimer::timeout, this, [this] {
        if (finder_) {
            const auto p = finder_->progress();
            progressTitle_->setText(QString::fromUtf8(stageName(p.stage)) + "…");
            if (p.stage == DupStage::FullHash || p.stage == DupStage::Verifying) {
                progressBar_->setRange(0, 1000);
                progressBar_->setValue(p.bytesTotal ? static_cast<int>(1000 * p.bytesDone / p.bytesTotal) : 0);
                progressDetail_->setText(tr("%1 of %2 · %3 of %4 files")
                                             .arg(qs(humanSize(p.bytesDone)), qs(humanSize(p.bytesTotal)),
                                                  qs(withThousands(p.filesDone)), qs(withThousands(p.filesTotal))));
            } else {
                progressBar_->setRange(0, p.filesTotal ? 1000 : 0);
                progressBar_->setValue(p.filesTotal ? static_cast<int>(1000 * p.filesDone / p.filesTotal) : 0);
                progressDetail_->setText(p.filesTotal ? tr("%1 of %2 files").arg(qs(withThousands(p.filesDone)),
                                                                                 qs(withThousands(p.filesTotal)))
                                                      : QString());
            }
        } else if (executor_) {
            const auto p = executor_->progress();
            progressBar_->setRange(0, std::max(1, p.total));
            progressBar_->setValue(p.done);
            progressDetail_->setText(tr("%1 of %2 · %3").arg(p.done).arg(p.total).arg(QFileInfo(qs(p.current)).fileName()));
        }
    });

    updateSummary();
}

DuplicatesView::~DuplicatesView() {
    cancel();
    task_.join();
}

bool DuplicatesView::busy() const { return task_.running(); }

void DuplicatesView::setScope(std::shared_ptr<ScanTree> tree, Node* folder) {
    if (busy()) return;
    tree_ = std::move(tree);
    folder_ = folder;
    if (folder_) {
        QString name = qs(folder_->name);
        if (!folder_->parent && !QFileInfo(name).fileName().isEmpty()) name = QFileInfo(name).fileName();
        scopeLabel_->setText(tr("Search in <b>%1</b> <span style='color:%3'>%2</span>")
                                 .arg(name.toHtmlEscaped(),
                                      qs(humanSize(folder_->size) + " · " + withThousands(folder_->files) + " files"),
                                      theme::mutedText().name()));
        scopeLabel_->setToolTip(shownPath(tree_, folder_));
    } else {
        scopeLabel_->setText(tr("Scan a folder first"));
    }
    findButton_->setEnabled(folder_ != nullptr);
}

void DuplicatesView::setBusy(bool b, const QString& title) {
    findButton_->setEnabled(!b && folder_);
    minSize_->setEnabled(!b);
    if (b) {
        progressTitle_->setText(title);
        progressDetail_->clear();
        progressBar_->setRange(0, 0);
        stack_->setCurrentIndex(ProgressPage);
        poll_->start();
    } else {
        poll_->stop();
    }
}

void DuplicatesView::cancel() {
    if (finder_) finder_->cancel();
    if (executor_) executor_->cancel();
}

void DuplicatesView::find() {
    if (!folder_ || busy()) return;
    DupOptions opt;
    opt.minSize = minSize_->currentData().toULongLong();
    opt.skipHidden = skipHidden_->isChecked();
    opt.verifyBytes = paranoid_->isChecked();
    opt.excludeNames = {".git", ".svn", ".hg", "node_modules"};

    // SQLite is built thread-safe; the connection is only used by the worker from here on.
    try {
        cache_ = std::make_unique<Index>(defaultIndexPath());
    } catch (const std::exception&) {
        cache_.reset();  // hashing still works without the cache
    }
    try {
        finder_ = std::make_unique<DuplicateFinder>(fsOf(tree_), opt, cache_.get());
    } catch (const std::exception& e) {
        emit message(qs(e.what()));
        return;
    }
    setBusy(true, tr("Looking for duplicates…"));

    auto keepAlive = tree_;  // the scan tree must outlive the worker
    Node* folder = folder_;
    DuplicateFinder* finder = finder_.get();
    task_.start<std::vector<DupGroup>>(
        this, [keepAlive, folder, finder] { return finder->run(*folder); },
        [this](std::vector<DupGroup> groups) { onFound(std::move(groups)); });
}

void DuplicatesView::onFound(std::vector<DupGroup> groups) {
    const bool cancelled = finder_ && finder_->cancelled();
    const uint64_t errors = finder_ ? finder_->progress().errors : 0;
    finder_.reset();
    cache_.reset();
    setBusy(false);
    if (cancelled) {
        stack_->setCurrentIndex(model_->rowCount() ? ResultsPage : EmptyPage);
        emit message(tr("Search stopped."));
        return;
    }
    model_->setGroups(std::move(groups), folder_ ? qs(folder_->path(fsOf(tree_).separator())) : QString());
    model_->applyRule(static_cast<KeepRule>(keepRule_->currentData().toInt()));
    if (model_->rowCount() == 0) {
        emptyLabel_->setText(tr("No duplicates found in %1.").arg(folder_ ? qs(folder_->name) : QString()));
        stack_->setCurrentIndex(EmptyPage);
    } else {
        stack_->setCurrentIndex(ResultsPage);
        view_->collapseAll();
        for (int r = 0; r < std::min(proxy_->rowCount(), 30); ++r) view_->expand(proxy_->index(r, 0));
    }
    QString msg = tr("Found %n duplicate group(s)", nullptr, model_->rowCount()) +
                  tr(" · %1 reclaimable").arg(qs(humanSize(model_->reclaimableBytes())));
    if (errors) msg += tr(" · %n file(s) could not be read", nullptr, static_cast<int>(errors));
    emit message(msg);
    emit searchFinished(model_->rowCount());
}

void DuplicatesView::updateSummary() {
    const size_t n = model_->selectedCount();
    summary_->setText(n ? tr("<b>%n file(s)</b> selected · %1", nullptr, static_cast<int>(n))
                              .arg(qs(humanSize(model_->selectedBytes())))
                        : tr("Nothing selected"));
    trashButton_->setEnabled(n > 0 && !busy());
    moreButton_->setEnabled(n > 0 && !busy());
}

void DuplicatesView::apply(const char* action) {
    const size_t n = model_->selectedCount();
    if (!n || busy()) return;
    const QString size = qs(humanSize(model_->selectedBytes()));
    const std::string act = action;

    QString title, text, button;
    QMessageBox::Icon icon = QMessageBox::Question;
    if (act == Action::Trash) {
        title = tr("Move to Trash");
        text = tr("Move %n file(s) (%1) to the Trash?", nullptr, static_cast<int>(n)).arg(size);
        button = tr("Move to Trash");
    } else if (act == Action::Hardlink) {
        title = tr("Replace with Hard Links");
        text = tr("Replace %n file(s) with hard links to the kept copy? This frees %1.\n\n"
                  "Every path keeps working, but editing one copy changes all of them.",
                  nullptr, static_cast<int>(n))
                   .arg(size);
        button = tr("Replace");
    } else if (act == Action::Clone) {
        title = tr("Replace with Clones");
        text = tr("Replace %n file(s) with space-sharing clones of the kept copy? This frees %1.\n\n"
                  "Clones stay independent files; it only works on APFS, Btrfs and XFS.",
                  nullptr, static_cast<int>(n))
                   .arg(size);
        button = tr("Replace");
    } else {
        title = tr("Delete Permanently");
        text = tr("Permanently delete %n file(s) (%1)?\n\nThis cannot be undone. Contents are re-checked right "
                  "before each deletion and nothing is deleted unless an identical copy remains.",
                  nullptr, static_cast<int>(n))
                   .arg(size);
        button = tr("Delete Permanently");
        icon = QMessageBox::Warning;
    }
    QMessageBox box(icon, title, text, QMessageBox::Cancel, this);
    QPushButton* ok = box.addButton(button, act == Action::Delete ? QMessageBox::DestructiveRole : QMessageBox::AcceptRole);
    box.setDefaultButton(QMessageBox::Cancel);
    box.exec();
    if (box.clickedButton() != ok) return;

    int64_t batch = 0;
    try {
        const auto ops = planDedupe(model_->groups(), model_->selection(), act);
        journal_ = std::make_unique<Journal>(Journal::defaultPath());
        const QString where = folder_ ? qs(folder_->name) : QString();
        batch = journal_->createBatch("dedupe", tr("Duplicates in %1 (%2)").arg(where, QString::fromUtf8(action)).toStdString(),
                                      fsOf(tree_).volumeName(), ops);
    } catch (const std::exception& e) {
        QMessageBox::critical(this, title, qs(e.what()));
        return;
    }

    executor_ = std::make_unique<Executor>(fsOf(tree_), *journal_);
    setBusy(true, title + "…");
    updateSummary();
    Executor* ex = executor_.get();
    task_.start<ExecResult>(
        this, [ex, batch] { return ex->run(batch); },
        [this, batch](ExecResult r) { onApplied(batch, r); });
}

void DuplicatesView::onApplied(int64_t batch, ExecResult r) {
    executor_.reset();
    std::vector<std::string> gone;
    try {
        for (const auto& op : journal_->ops(batch))
            if (op.state == OpState::Done) gone.push_back(op.src);
    } catch (const std::exception&) {
    }
    journal_.reset();
    setBusy(false);
    // Hard-linked/cloned files still exist, but they are no longer duplicates worth listing.
    model_->removePaths(gone);
    stack_->setCurrentIndex(model_->rowCount() ? ResultsPage : EmptyPage);
    if (!model_->rowCount()) emptyLabel_->setText(tr("All selected duplicates were handled."));

    QString msg = tr("Done: %n file(s)", nullptr, r.done) + tr(", %1 freed").arg(qs(humanSize(r.bytes)));
    if (r.skipped) msg += tr(" · %n skipped (changed since the search)", nullptr, r.skipped);
    if (r.failed) msg += tr(" · %n failed — see History", nullptr, r.failed);
    emit message(msg);
    if (r.done) emit filesChanged();
}

void DuplicatesView::showContextMenu(const QPoint& pos) {
    const QModelIndex src = proxy_->mapToSource(view_->indexAt(pos));
    if (!src.isValid()) return;
    QMenu menu(this);
    const QString path = model_->pathAt(src);
    if (!path.isEmpty()) {
        menu.addAction(tr("Keep Only This Copy"), this, [this, src] { model_->keepOnly(model_->groupOf(src), src.row()); });
        menu.addSeparator();
        menu.addAction(
#if defined(Q_OS_MACOS)
            tr("Reveal in Finder"),
#elif defined(Q_OS_WIN)
            tr("Show in Explorer"),
#else
            tr("Show in File Manager"),
#endif
            this, [path] { reveal(path); });
        menu.addAction(tr("Open"), this, [path] { QDesktopServices::openUrl(QUrl::fromLocalFile(path)); });
        menu.addAction(tr("Copy Path"), this, [path] { QApplication::clipboard()->setText(path); });
    } else {
        const int g = model_->groupOf(src);
        menu.addAction(tr("Keep Oldest in This Group"), this, [this, g] {
            const auto& files = model_->groups()[static_cast<size_t>(g)];
            model_->keepOnly(g, static_cast<int>(chooseKeeper(files, KeepRule::Oldest)));
        });
    }
    menu.exec(view_->viewport()->mapToGlobal(pos));
}
