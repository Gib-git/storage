#include "MainWindow.h"

#include "NasDialog.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyle>
#include <QToolBar>
#include <QToolButton>
#include <QTreeView>
#include <QUrl>
#include <QVBoxLayout>

#include "BreadcrumbBar.h"
#include "CategoryBar.h"
#include "CleanupView.h"
#include "DuplicatesView.h"
#include "HistoryView.h"
#include "OrganizeView.h"
#include "ScanController.h"
#include "ScanModel.h"
#include "ShareDelegate.h"
#include "Sidebar.h"
#include "Theme.h"
#include "TreemapWidget.h"
#include "storage/core/executor.hpp"
#include "storage/core/format.hpp"
#include "storage/core/index.hpp"
#include "storage/core/paths.hpp"
#include "storage/core/ssh_fs.hpp"
#include "storage/core/volumes.hpp"
#include "storage/core/version.hpp"

using namespace storage;

namespace {
enum Page { WelcomePage = 0, ProgressPage = 1, BrowserPage = 2 };

QString qs(const std::string& s) { return QString::fromStdString(s); }
}  // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(kAppName);
    setWindowIcon(theme::appIcon());

    scanner_ = new ScanController(this);
    connect(scanner_, &ScanController::started, this, &MainWindow::onScanStarted);
    connect(scanner_, &ScanController::progress, this, &MainWindow::onScanProgress);
    connect(scanner_, &ScanController::finished, this, &MainWindow::onScanFinished);

    buildActions();
    buildUi();

    QSettings settings;
    if (!restoreGeometry(settings.value("window/geometry").toByteArray())) resize(1280, 820);
    updateActions();
    QMetaObject::invokeMethod(this, &MainWindow::recoverInterruptedWork, Qt::QueuedConnection);
}

void MainWindow::recoverInterruptedWork() {
    // Anything a crash or power cut left half-done is checked against the disk now.
    std::vector<int64_t> recovered;
    try {
        static LocalFileSystem fs;
        Journal journal(Journal::defaultPath());
        recovered = Executor::recover(fs, journal);
    } catch (const std::exception& e) {
        statusLabel_->setText(tr("Could not open the operation journal: %1").arg(qs(e.what())));
        return;
    }
    if (recovered.empty()) return;
    history_->reload();
    const auto answer = QMessageBox::question(
        this, tr("Interrupted Work"),
        tr("Storage was closed while changing files (%n batch(es)). Every file has been checked and nothing was lost.\n\n"
           "Open History to review and finish the remaining steps?",
           nullptr, static_cast<int>(recovered.size())));
    if (answer == QMessageBox::Yes) setMode(HistoryMode);
}

void MainWindow::findDuplicatesNow() {
    setMode(DuplicatesMode);
    dupes_->startSearch();
}

void MainWindow::previewOrganizeNow() {
    setMode(OrganizeMode);
    organize_->previewNow();
}

void MainWindow::showCleanupNow() {
    setMode(CleanupMode);
    cleanup_->ensureAnalyzed();
}

void MainWindow::setMode(Mode m) {
    modes_->setCurrentIndex(m);
    for (int i = 0; i < ModeCount; ++i) modeActions_[i]->setChecked(i == m);
    if (m == DuplicatesMode) dupes_->setScope(tree_, current_);
    if (m == OrganizeMode) organize_->setScope(tree_, current_);
    if (m == CleanupMode) cleanup_->setScope(tree_, current_);
    if (m == HistoryMode) history_->reload();
    updateActions();
}

void MainWindow::buildActions() {
    auto* style = QApplication::style();
    actBack_ = new QAction(style->standardIcon(QStyle::SP_ArrowBack), tr("Back"), this);
    actBack_->setShortcut(QKeySequence::Back);
    connect(actBack_, &QAction::triggered, this, &MainWindow::goBack);

    actForward_ = new QAction(style->standardIcon(QStyle::SP_ArrowForward), tr("Forward"), this);
    actForward_->setShortcut(QKeySequence::Forward);
    connect(actForward_, &QAction::triggered, this, &MainWindow::goForward);

    actUp_ = new QAction(style->standardIcon(QStyle::SP_ArrowUp), tr("Enclosing Folder"), this);
    actUp_->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Up));
    connect(actUp_, &QAction::triggered, this, &MainWindow::goUp);

    actScan_ = new QAction(style->standardIcon(QStyle::SP_DirOpenIcon), tr("Scan Folder…"), this);
    actScan_->setShortcut(QKeySequence::Open);
    connect(actScan_, &QAction::triggered, this, &MainWindow::chooseFolder);

    actRescan_ = new QAction(style->standardIcon(QStyle::SP_BrowserReload), tr("Rescan"), this);
    actRescan_->setShortcut(QKeySequence::Refresh);
    connect(actRescan_, &QAction::triggered, this, [this] {
        if (tree_ && tree_->root) startScan(qs(tree_->root->name), tree_->volume);
    });

    actStop_ = new QAction(style->standardIcon(QStyle::SP_BrowserStop), tr("Stop"), this);
    actStop_->setShortcut(QKeySequence(Qt::Key_Escape));
    connect(actStop_, &QAction::triggered, scanner_, &ScanController::cancel);

    actReveal_ = new QAction(style->standardIcon(QStyle::SP_DirLinkIcon),
#if defined(Q_OS_MACOS)
                             tr("Reveal in Finder"),
#elif defined(Q_OS_WIN)
                             tr("Show in Explorer"),
#else
                             tr("Show in File Manager"),
#endif
                             this);
    actReveal_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_R));
    connect(actReveal_, &QAction::triggered, this, [this] {
        if (Node* n = selectedNode() ? selectedNode() : current_) revealInFileManager(nativePath(n));
    });

    // Menus
    QMenu* file = menuBar()->addMenu(tr("&File"));
    file->addAction(actScan_);
    file->addAction(actRescan_);
    file->addAction(actStop_);
    file->addAction(tr("Add NAS…"), this, [this] { editNas({}); });
    file->addSeparator();
    file->addAction(actReveal_);
    file->addSeparator();
    QAction* quit = file->addAction(tr("Quit"), qApp, &QApplication::quit);
    quit->setShortcut(QKeySequence::Quit);
    quit->setMenuRole(QAction::QuitRole);

    QMenu* go = menuBar()->addMenu(tr("&Go"));
    go->addAction(actBack_);
    go->addAction(actForward_);
    go->addAction(actUp_);

    QMenu* view = menuBar()->addMenu(tr("&View"));
    // Mode actions are created with the toolbar; add them to the menu once they exist.
    QMetaObject::invokeMethod(this, [this, view] {
        view->insertActions(view->actions().value(0),
                            {modeActions_[0], modeActions_[1], modeActions_[2], modeActions_[3], modeActions_[4]});
        view->insertSeparator(view->actions().value(ModeCount));
    }, Qt::QueuedConnection);
    QMenu* appearance = view->addMenu(tr("Appearance"));
    auto* group = new QActionGroup(this);
    const theme::Mode currentMode = theme::current();
    const std::pair<theme::Mode, QString> modes[] = {
        {theme::Mode::System, tr("Match System")}, {theme::Mode::Light, tr("Light")}, {theme::Mode::Dark, tr("Dark")}};
    for (const auto& [mode, label] : modes) {
        QAction* a = appearance->addAction(label);
        a->setCheckable(true);
        a->setChecked(mode == currentMode);
        group->addAction(a);
        connect(a, &QAction::triggered, this, [this, mode = mode] { setThemeMode(static_cast<int>(mode)); });
    }

    QMenu* help = menuBar()->addMenu(tr("&Help"));
    QAction* about = help->addAction(tr("About Storage"), this, &MainWindow::showAbout);
    about->setMenuRole(QAction::AboutRole);
    help->addAction(tr("Project on GitHub"), this, [] { QDesktopServices::openUrl(QUrl(kHomepage)); });
}

void MainWindow::buildUi() {
    statusLabel_ = new QLabel(this);
    statusBar()->addWidget(statusLabel_, 1);
    statusBar()->setSizeGripEnabled(false);

    QToolBar* tb = addToolBar(tr("Main"));
    tb->setObjectName("MainToolbar");
    tb->setMovable(false);
    tb->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    tb->setIconSize(QSize(16, 16));

    auto* modeGroup = new QActionGroup(this);
    const QString modeNames[] = {tr("Disk Usage"), tr("Duplicates"), tr("Organize"), tr("Clean Up"), tr("History")};
    const QKeySequence modeKeys[] = {QKeySequence(Qt::CTRL | Qt::Key_1), QKeySequence(Qt::CTRL | Qt::Key_2),
                                     QKeySequence(Qt::CTRL | Qt::Key_3), QKeySequence(Qt::CTRL | Qt::Key_4),
                                     QKeySequence(Qt::CTRL | Qt::Key_5)};
    for (int i = 0; i < ModeCount; ++i) {
        modeActions_[i] = new QAction(modeNames[i], this);
        modeActions_[i]->setCheckable(true);
        modeActions_[i]->setShortcut(modeKeys[i]);
        modeGroup->addAction(modeActions_[i]);
        connect(modeActions_[i], &QAction::triggered, this, [this, i] { setMode(static_cast<Mode>(i)); });
        tb->addAction(modeActions_[i]);
        if (auto* w = qobject_cast<QToolButton*>(tb->widgetForAction(modeActions_[i]))) {
            w->setObjectName("ModeButton");
            w->setToolButtonStyle(Qt::ToolButtonTextOnly);
        }
    }
    modeActions_[0]->setChecked(true);
    tb->addSeparator();
    tb->addAction(actBack_);
    tb->addAction(actForward_);
    tb->addAction(actUp_);
    tb->addSeparator();
    tb->addAction(actScan_);
    tb->addAction(actRescan_);
    tb->addAction(actStop_);
    tb->addSeparator();
    tb->addAction(actReveal_);
    for (QAction* a : {actBack_, actForward_, actUp_})
        if (auto* w = tb->widgetForAction(a)) static_cast<QToolButton*>(w)->setToolButtonStyle(Qt::ToolButtonIconOnly);

    sidebar_ = new Sidebar(this);
    connect(sidebar_, &Sidebar::scanRequested, this, [this](const QString& path) {
        if (modes_->currentIndex() == HistoryMode) setMode(UsageMode);
        returnToMode_ = modes_->currentIndex() == UsageMode ? -1 : modes_->currentIndex();
        startScan(path);
    });
    connect(sidebar_, &Sidebar::loadRequested, this, [this](qint64 id) {
        if (scanner_->busy() || dupes_->busy() || organize_->busy()) return;
        if (modes_->currentIndex() == HistoryMode) setMode(UsageMode);
        returnToMode_ = modes_->currentIndex() == UsageMode ? -1 : modes_->currentIndex();
        scanner_->load(id);
    });
    connect(sidebar_, &Sidebar::nasRequested, this, &MainWindow::openNas);
    connect(sidebar_, &Sidebar::addNasRequested, this, [this] { editNas({}); });
    connect(sidebar_, &Sidebar::editNasRequested, this, &MainWindow::editNas);
    connect(sidebar_, &Sidebar::removeNasRequested, this, &MainWindow::removeNas);
    connect(sidebar_, &Sidebar::removeRequested, this, [this](qint64 id) {
        try {
            Index(defaultIndexPath()).remove(id);
        } catch (const std::exception& e) {
            QMessageBox::warning(this, kAppName, qs(e.what()));
        }
        sidebar_->reload();
    });

    stack_ = new QStackedWidget(this);
    stack_->addWidget(buildWelcomePage());
    stack_->addWidget(buildProgressPage());
    stack_->addWidget(buildBrowserPage());

    dupes_ = new DuplicatesView(this);
    connect(dupes_, &DuplicatesView::message, statusLabel_, &QLabel::setText);
    connect(dupes_, &DuplicatesView::searchFinished, this, &MainWindow::duplicatesFound);
    connect(dupes_, &DuplicatesView::filesChanged, this, [this] {
        actRescan_->setText(tr("Rescan (sizes changed)"));
    });
    connect(dupes_, &DuplicatesView::chooseFolderRequested, this, [this] {
        returnToMode_ = DuplicatesMode;
        chooseFolder();
    });

    organize_ = new OrganizeView(this);
    connect(organize_, &OrganizeView::message, statusLabel_, &QLabel::setText);
    connect(organize_, &OrganizeView::previewReady, this, &MainWindow::organizePreviewReady);
    connect(organize_, &OrganizeView::filesChanged, this, [this] { actRescan_->setText(tr("Rescan (sizes changed)")); });
    connect(organize_, &OrganizeView::chooseFolderRequested, this, [this] {
        returnToMode_ = OrganizeMode;
        chooseFolder();
    });

    history_ = new HistoryView(this);
    connect(history_, &HistoryView::message, statusLabel_, &QLabel::setText);
    connect(history_, &HistoryView::filesChanged, this, [this] { actRescan_->setText(tr("Rescan (sizes changed)")); });

    modes_ = new QStackedWidget(this);
    modes_->addWidget(stack_);
    modes_->addWidget(dupes_);
    cleanup_ = new CleanupView(this);
    connect(cleanup_, &CleanupView::message, statusLabel_, &QLabel::setText);
    connect(cleanup_, &CleanupView::analyzed, this, &MainWindow::cleanupAnalyzed);
    connect(cleanup_, &CleanupView::filesChanged, this, [this] {
        actRescan_->setText(tr("Rescan (sizes changed)"));
        sidebar_->reload();  // free space on drives changed
    });

    modes_->addWidget(organize_);
    modes_->addWidget(cleanup_);
    modes_->addWidget(history_);

    auto* split = new QSplitter(Qt::Horizontal, this);
    split->setObjectName("MainSplitter");
    split->setHandleWidth(1);
    split->addWidget(sidebar_);
    split->addWidget(modes_);
    split->setStretchFactor(1, 1);
    split->setSizes({230, 1050});
    setCentralWidget(split);

}

QWidget* MainWindow::buildWelcomePage() {
    auto* page = new QWidget(this);
    auto* outer = new QVBoxLayout(page);
    outer->addStretch(2);

    auto* icon = new QLabel(page);
    icon->setPixmap(theme::appIcon().pixmap(72, 72));
    icon->setAlignment(Qt::AlignCenter);
    outer->addWidget(icon);

    auto* title = new QLabel(tr("See what's using your space"), page);
    title->setObjectName("WelcomeTitle");
    title->setAlignment(Qt::AlignCenter);
    outer->addWidget(title);

    auto* sub = new QLabel(tr("Pick a location in the sidebar, or scan any folder or drive."), page);
    sub->setAlignment(Qt::AlignCenter);
    sub->setStyleSheet(QString("color:%1").arg(theme::mutedText().name()));
    outer->addWidget(sub);
    outer->addSpacing(18);

    auto* row = new QHBoxLayout;
    row->addStretch(1);
    auto addButton = [&](const QString& text, const QString& path) {
        auto* b = new QPushButton(text, page);
        b->setObjectName("BigButton");
        b->setCursor(Qt::PointingHandCursor);
        if (path.isEmpty()) connect(b, &QPushButton::clicked, this, &MainWindow::chooseFolder);
        else connect(b, &QPushButton::clicked, this, [this, path] { startScan(path); });
        row->addWidget(b);
    };
    addButton(tr("Scan Home Folder"), QDir::toNativeSeparators(QDir::homePath()));
    addButton(tr("Choose Folder or Drive…"), {});
    row->addStretch(1);
    outer->addLayout(row);
    outer->addStretch(3);
    return page;
}

QWidget* MainWindow::buildProgressPage() {
    auto* page = new QWidget(this);
    auto* outer = new QVBoxLayout(page);
    outer->setContentsMargins(60, 0, 60, 0);
    outer->addStretch(2);

    progressTitle_ = new QLabel(page);
    progressTitle_->setObjectName("WelcomeTitle");
    progressTitle_->setAlignment(Qt::AlignCenter);
    outer->addWidget(progressTitle_);

    progressStats_ = new QLabel(page);
    progressStats_->setAlignment(Qt::AlignCenter);
    outer->addWidget(progressStats_);
    outer->addSpacing(12);

    progressBar_ = new QProgressBar(page);
    progressBar_->setRange(0, 0);
    progressBar_->setTextVisible(false);
    progressBar_->setMaximumHeight(6);
    outer->addWidget(progressBar_);

    progressPath_ = new QLabel(page);
    progressPath_->setAlignment(Qt::AlignCenter);
    progressPath_->setStyleSheet(QString("color:%1").arg(theme::mutedText().name()));
    outer->addWidget(progressPath_);
    outer->addSpacing(12);

    auto* row = new QHBoxLayout;
    row->addStretch(1);
    progressCancel_ = new QPushButton(tr("Stop Scan"), page);
    connect(progressCancel_, &QPushButton::clicked, scanner_, &ScanController::cancel);
    row->addWidget(progressCancel_);
    row->addStretch(1);
    outer->addLayout(row);
    outer->addStretch(3);
    return page;
}

QWidget* MainWindow::buildBrowserPage() {
    auto* page = new QWidget(this);
    auto* v = new QVBoxLayout(page);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);

    auto* header = new QWidget(page);
    header->setObjectName("Header");
    auto* h = new QHBoxLayout(header);
    h->setContentsMargins(10, 6, 12, 6);
    crumbs_ = new BreadcrumbBar(header);
    connect(crumbs_, &BreadcrumbBar::navigate, this, [this](Node* n) { navigate(n); });
    summary_ = new QLabel(header);
    summary_->setObjectName("Summary");
    h->addWidget(crumbs_, 1);
    h->addWidget(summary_);
    v->addWidget(header);

    model_ = new ScanModel(this);
    proxy_ = new QSortFilterProxyModel(this);
    proxy_->setSourceModel(model_);
    proxy_->setSortRole(ScanModel::SortRole);

    view_ = new QTreeView(page);
    view_->setModel(proxy_);
    view_->setSortingEnabled(true);
    view_->sortByColumn(ScanModel::Size, Qt::DescendingOrder);
    view_->setUniformRowHeights(true);
    view_->setAlternatingRowColors(true);
    view_->setSelectionMode(QAbstractItemView::SingleSelection);
    view_->setContextMenuPolicy(Qt::CustomContextMenu);
    view_->setItemDelegateForColumn(ScanModel::Share, new ShareDelegate(view_));
    view_->header()->setStretchLastSection(false);
    view_->header()->setSectionResizeMode(ScanModel::Name, QHeaderView::Stretch);
    view_->header()->resizeSection(ScanModel::Size, 90);
    view_->header()->resizeSection(ScanModel::Share, 150);
    view_->header()->resizeSection(ScanModel::Items, 90);
    view_->header()->resizeSection(ScanModel::Modified, 140);
    view_->header()->setSortIndicatorShown(true);

    connect(view_, &QTreeView::activated, this, [this](const QModelIndex& idx) {
        Node* n = model_->nodeAt(proxy_->mapToSource(idx));
        if (n && n->isDir() && !n->children.empty()) navigate(n);
    });
    connect(view_->selectionModel(), &QItemSelectionModel::currentChanged, this, [this](const QModelIndex& idx) {
        Node* n = model_->nodeAt(proxy_->mapToSource(idx));
        treemap_->setSelected(n);
        if (n) statusLabel_->setText(nativePath(n) + "  —  " + qs(humanSize(n->size)));
        updateActions();
    });
    connect(view_, &QTreeView::customContextMenuRequested, this, [this](const QPoint& pos) {
        Node* n = model_->nodeAt(proxy_->mapToSource(view_->indexAt(pos)));
        if (n) showContextMenu(n, view_->viewport()->mapToGlobal(pos));
    });

    treemap_ = new TreemapWidget(page);
    connect(treemap_, &TreemapWidget::nodeClicked, this, &MainWindow::selectNode);
    connect(treemap_, &TreemapWidget::nodeActivated, this, [this](Node* n) { navigate(n); });
    connect(treemap_, &TreemapWidget::contextMenuRequested, this, &MainWindow::showContextMenu);

    categories_ = new CategoryBar(page);

    auto* mapBox = new QWidget(page);
    auto* mapLayout = new QVBoxLayout(mapBox);
    mapLayout->setContentsMargins(0, 0, 0, 0);
    mapLayout->setSpacing(0);
    mapLayout->addWidget(treemap_, 1);
    mapLayout->addWidget(categories_);

    auto* split = new QSplitter(Qt::Vertical, page);
    split->setObjectName("BrowserSplitter");
    split->setHandleWidth(1);
    split->addWidget(view_);
    split->addWidget(mapBox);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 1);
    split->setSizes({380, 360});
    v->addWidget(split, 1);
    return page;
}

// --- Scanning -----------------------------------------------------------------

void MainWindow::openPath(const QString& path, bool forceRescan) {
    const QString native = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
    if (!forceRescan) {
        try {
            if (const int64_t id = Index(defaultIndexPath()).findScan(normalizeLocalPath(native.toStdString()))) {
                scanner_->load(id);
                return;
            }
        } catch (const std::exception&) {
        }
    }
    startScan(native);
}

void MainWindow::chooseFolder() {
    const QString start = tree_ && tree_->root ? qs(tree_->root->name) : QDir::homePath();
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Choose a folder or drive to scan"), start);
    if (!dir.isEmpty()) startScan(QDir::toNativeSeparators(dir));
}

void MainWindow::startScan(const QString& path, const std::string& volume) {
    if (scanner_->busy()) return;
    pendingScanRoot_ = path;
    scanner_->scan(path, volume);
}

bool MainWindow::remote() const { return tree_ && !isLocalVolume(tree_->volume); }

void MainWindow::openNas(const QString& name) {
    for (const auto& cfg : loadNasConfigs()) {
        if (QString::fromStdString(cfg.name) != name) continue;
        if (modes_->currentIndex() == HistoryMode) setMode(UsageMode);
        returnToMode_ = modes_->currentIndex() == UsageMode ? -1 : modes_->currentIndex();
        try {
            if (const int64_t id = Index(defaultIndexPath()).findScan(cfg.root, cfg.volume())) {
                scanner_->load(id);  // instant; use Rescan for fresh numbers
                return;
            }
        } catch (const std::exception&) {
        }
        startScan(qs(cfg.root), cfg.volume());
        return;
    }
}

void MainWindow::editNas(const QString& name) {
    auto list = loadNasConfigs();
    NasConfig current;
    for (const auto& c : list)
        if (qs(c.name) == name) current = c;
    NasDialog dlg(current, this);
    if (dlg.exec() != QDialog::Accepted) return;
    const NasConfig updated = dlg.config();
    list.erase(std::remove_if(list.begin(), list.end(),
                              [&](const NasConfig& c) { return qs(c.name) == name || c.name == updated.name; }),
               list.end());
    list.push_back(updated);
    saveNasConfigs(list);
    sidebar_->reload();
    if (name.isEmpty() &&
        QMessageBox::question(this, kAppName, tr("Scan %1 now?").arg(qs(updated.name))) == QMessageBox::Yes)
        openNas(qs(updated.name));
}

void MainWindow::removeNas(const QString& name) {
    if (QMessageBox::question(this, kAppName, tr("Remove the NAS “%1”? Saved scans and history are kept.").arg(name)) !=
        QMessageBox::Yes)
        return;
    auto list = loadNasConfigs();
    list.erase(std::remove_if(list.begin(), list.end(), [&](const NasConfig& c) { return qs(c.name) == name; }), list.end());
    saveNasConfigs(list);
    sidebar_->reload();
}

void MainWindow::onScanStarted(const QString& what) {
    progressTitle_->setText(scanner_->busy() && !pendingScanRoot_.isEmpty() ? tr("Scanning %1").arg(QFileInfo(what).fileName().isEmpty() ? what : QFileInfo(what).fileName())
                                                                              : what);
    progressStats_->setText(tr("Starting…"));
    progressPath_->clear();
    progressCancel_->setVisible(!pendingScanRoot_.isEmpty());
    stack_->setCurrentIndex(ProgressPage);
    modes_->setCurrentIndex(UsageMode);
    for (int i = 0; i < ModeCount; ++i) modeActions_[i]->setChecked(i == UsageMode);
    updateActions();
}

void MainWindow::onScanProgress(const ScanProgress& p) {
    progressStats_->setText(tr("%1 files · %2 folders · %3")
                                .arg(qs(withThousands(p.files)), qs(withThousands(p.dirs)), qs(humanSize(p.bytes))));
    progressPath_->setText(progressPath_->fontMetrics().elidedText(qs(p.current), Qt::ElideMiddle,
                                                                   std::max(200, progressPath_->width())));
}

void MainWindow::onScanFinished(std::shared_ptr<ScanTree> tree, const QString& error) {
    pendingScanRoot_.clear();
    if (!tree || !tree->root) {
        stack_->setCurrentIndex(tree_ ? BrowserPage : WelcomePage);
        if (returnToMode_ >= 0) {
            setMode(static_cast<Mode>(returnToMode_));
            returnToMode_ = -1;
        }
        if (!error.isEmpty()) statusLabel_->setText(error);
        updateActions();
        emit scanFinished(false);
        return;
    }

    // Detach views before the old tree is freed.
    treemap_->setRoot(nullptr);
    model_->setTree(nullptr);
    back_.clear();
    forward_.clear();
    current_ = nullptr;

    tree_ = std::move(tree);
    model_->setTree(tree_.get());
    navigate(tree_->root.get(), false);
    stack_->setCurrentIndex(BrowserPage);
    sidebar_->reload();

    const Node& r = *tree_->root;
    QString msg = tree_->complete ? tr("Scanned %1 in %2 files").arg(qs(humanSize(r.size)), qs(withThousands(r.files)))
                                  : tr("Scan stopped early — results are partial and were not saved");
    if (r.errors) msg += tr(" · %1 folders could not be read").arg(qs(withThousands(r.errors)));
    statusLabel_->setText(msg);
    if (!error.isEmpty()) statusLabel_->setText(tr("The scan finished but could not be saved: %1").arg(error));
    actRescan_->setText(tr("Rescan"));
    if (returnToMode_ >= 0) {
        setMode(static_cast<Mode>(returnToMode_));
        returnToMode_ = -1;
    }
    updateActions();
    emit scanFinished(true);
}

// --- Navigation ----------------------------------------------------------------

void MainWindow::navigate(Node* node, bool record) {
    if (!node || node == current_) return;
    if (!node->isDir()) node = node->parent;
    if (!node) return;
    if (record && current_) {
        back_.push_back(current_);
        forward_.clear();
    }
    current_ = node;

    const QModelIndex src = model_->indexFor(node);
    view_->setRootIndex(proxy_->mapFromSource(src));
    view_->scrollToTop();
    treemap_->setRoot(node);
    categories_->setNode(node);
    crumbs_->setNode(node);
    dupes_->setScope(tree_, node);
    organize_->setScope(tree_, node);
    cleanup_->setScope(tree_, node);
    updateSummary();
    updateActions();
}

void MainWindow::goBack() {
    if (back_.empty()) return;
    forward_.push_back(current_);
    Node* n = back_.back();
    back_.pop_back();
    navigate(n, false);
}

void MainWindow::goForward() {
    if (forward_.empty()) return;
    back_.push_back(current_);
    Node* n = forward_.back();
    forward_.pop_back();
    navigate(n, false);
}

void MainWindow::goUp() {
    if (current_ && current_->parent) navigate(current_->parent);
}

void MainWindow::selectNode(Node* node) {
    if (!node || !current_) return;
    // Expand ancestors between the current folder and the node so it is visible.
    std::vector<Node*> chain;
    for (Node* n = node->parent; n && n != current_; n = n->parent) chain.push_back(n);
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) view_->expand(proxy_->mapFromSource(model_->indexFor(*it)));
    const QModelIndex idx = proxy_->mapFromSource(model_->indexFor(node));
    view_->setCurrentIndex(idx);
    view_->scrollTo(idx);
}

Node* MainWindow::selectedNode() const {
    return model_ ? model_->nodeAt(proxy_->mapToSource(view_->currentIndex())) : nullptr;
}

void MainWindow::updateActions() {
    const bool busy = scanner_->busy();
    actBack_->setEnabled(!busy && !back_.empty());
    actForward_->setEnabled(!busy && !forward_.empty());
    actUp_->setEnabled(!busy && current_ && current_->parent);
    actScan_->setEnabled(!busy);
    actRescan_->setEnabled(!busy && tree_);
    actStop_->setEnabled(busy && !pendingScanRoot_.isEmpty());
    actReveal_->setEnabled(!busy && current_ && !remote());
}

void MainWindow::updateSummary() {
    if (!current_) {
        summary_->clear();
        return;
    }
    QString s = tr("%1 · %2 files · %3 folders")
                    .arg(qs(humanSize(current_->size)), qs(withThousands(current_->files)), qs(withThousands(current_->dirs)));
    if (tree_) s += tr(" · scanned %1").arg(qs(formatTime(tree_->finished)));
    if (tree_ && !tree_->complete) s += tr(" (partial)");
    summary_->setText(s);
}

// --- Context actions -------------------------------------------------------------

QString MainWindow::nativePath(const Node* node) const {
    if (!node) return {};
    if (remote()) return qs(node->path('/'));  // NAS paths are shown exactly as the NAS knows them
    return QDir::toNativeSeparators(qs(node->path(nativeSeparator())));
}

void MainWindow::showContextMenu(Node* node, const QPoint& globalPos) {
    const QString path = nativePath(node);
    QMenu menu(this);
    if (node->isDir() && node != current_)
        menu.addAction(tr("Open “%1”").arg(qs(node->name)), this, [this, node] { navigate(node); });
    if (!remote()) {
        menu.addAction(actReveal_->text(), this, [this, path] { revealInFileManager(path); });
        menu.addAction(tr("Open with Default App"), this, [path] { QDesktopServices::openUrl(QUrl::fromLocalFile(path)); });
    }
    menu.addSeparator();
    menu.addAction(tr("Copy Path"), this, [path] { QApplication::clipboard()->setText(path); });
    if (node->isDir()) {
        menu.addSeparator();
        menu.addAction(tr("Find Duplicates Here"), this, [this, node] {
            setMode(DuplicatesMode);
            dupes_->setScope(tree_, node);
        });
        menu.addAction(tr("Organize This Folder…"), this, [this, node] {
            setMode(OrganizeMode);
            organize_->setScope(tree_, node);
        });
        const std::string volume = tree_ ? tree_->volume : std::string("local");
        menu.addAction(tr("Scan This Folder Separately"), this, [this, path, volume] { startScan(path, volume); });
    }
    menu.exec(globalPos);
}

void MainWindow::revealInFileManager(const QString& path) {
#if defined(Q_OS_MACOS)
    QProcess::startDetached("open", {"-R", path});
#elif defined(Q_OS_WIN)
    QProcess::startDetached("explorer.exe", {"/select,", QDir::toNativeSeparators(path)});
#else
    const QFileInfo fi(path);
    QDesktopServices::openUrl(QUrl::fromLocalFile(fi.isDir() ? path : fi.absolutePath()));
#endif
}

// --- Misc ---------------------------------------------------------------------

void MainWindow::setThemeMode(int mode) {
    theme::apply(static_cast<theme::Mode>(mode));
    QSettings().setValue("ui/theme", theme::modeName(static_cast<theme::Mode>(mode)));
    treemap_->refreshTheme();
    if (current_) crumbs_->setNode(current_);
}

void MainWindow::showAbout() {
    QMessageBox box(this);
    box.setWindowTitle(tr("About Storage"));
    box.setIconPixmap(theme::appIcon().pixmap(64, 64));
    box.setText(QString("<h3>Storage %1</h3>").arg(kVersion));
    box.setInformativeText(tr("Index, deduplicate, organize and clean your disks and NAS — safely.<br><br>"
                              "<a href=\"%1\">%1</a><br>MIT License · Built with Qt %2")
                               .arg(kHomepage, qVersion()));
    box.exec();
}

void MainWindow::changeEvent(QEvent* e) {
    if (e->type() == QEvent::ThemeChange && theme::current() == theme::Mode::System) {
        // Re-evaluate the system colour scheme when the OS switches light/dark.
        QMetaObject::invokeMethod(this, [this] { setThemeMode(static_cast<int>(theme::Mode::System)); },
                                  Qt::QueuedConnection);
    }
    QMainWindow::changeEvent(e);
}

void MainWindow::closeEvent(QCloseEvent* e) {
    if (dupes_->busy() || organize_->busy() || cleanup_->busy() || history_->busy()) {
        const auto answer = QMessageBox::question(
            this, kAppName,
            tr("Storage is still working. Quit anyway?\n\nIt is safe: the current file finishes and the rest can be "
               "resumed from History next time."));
        if (answer != QMessageBox::Yes) {
            e->ignore();
            return;
        }
    }
    scanner_->cancel();
    QSettings().setValue("window/geometry", saveGeometry());
    QMainWindow::closeEvent(e);
}
