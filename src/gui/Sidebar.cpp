#include "Sidebar.h"

#include <QContextMenuEvent>
#include <QDir>
#include <QFileIconProvider>
#include <QHeaderView>
#include <QMenu>
#include <QPainter>
#include <QStandardPaths>
#include <QStorageInfo>

#include "Theme.h"
#include "storage/core/format.hpp"
#include "storage/core/index.hpp"
#include "storage/core/paths.hpp"
#include "storage/core/ssh_fs.hpp"

namespace {

bool interestingVolume(const QStorageInfo& v) {
    if (!v.isValid() || !v.isReady() || v.bytesTotal() <= 0) return false;
    const QString root = v.rootPath();
    const QByteArray fs = v.fileSystemType();
    static const QList<QByteArray> pseudo = {"tmpfs", "devtmpfs", "squashfs", "overlay", "proc", "sysfs",
                                             "devfs", "autofs", "efivarfs", "ramfs", "nullfs", "fuse.snapfuse"};
    if (pseudo.contains(fs)) return false;
#if defined(Q_OS_MACOS)
    if (root.startsWith("/System/Volumes") || root.startsWith("/private/var/vm") || root == "/dev") return false;
#elif defined(Q_OS_LINUX)
    if (root.startsWith("/snap/") || root.startsWith("/boot") || root.startsWith("/run") || root.startsWith("/sys") ||
        root.startsWith("/proc") || root.startsWith("/dev"))
        return false;
#endif
    return true;
}

}  // namespace

Sidebar::Sidebar(QWidget* parent) : QTreeWidget(parent) {
    setObjectName("Sidebar");
    setHeaderHidden(true);
    setRootIsDecorated(false);
    setIndentation(8);
    setItemDelegate(new SidebarDelegate(this));
    setFocusPolicy(Qt::NoFocus);
    setMinimumWidth(200);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    connect(this, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item) { activate(item); });
    reload();
}

QTreeWidgetItem* Sidebar::section(const QString& title) {
    auto* s = new QTreeWidgetItem(this, {title.toUpper()});
    s->setFlags(Qt::ItemIsEnabled);
    QFont f = font();
    f.setPointSizeF(f.pointSizeF() - 2);
    f.setBold(true);
    s->setFont(0, f);
    s->setForeground(0, theme::mutedText());
    s->setExpanded(true);
    return s;
}

void Sidebar::reload() {
    clear();
    QFileIconProvider icons;

    auto* places = section(tr("Locations"));
    auto addPlace = [&](QStandardPaths::StandardLocation loc, const QString& fallbackName) {
        const QString path = QStandardPaths::writableLocation(loc);
        if (path.isEmpty() || !QDir(path).exists()) return;
        auto* it = new QTreeWidgetItem(places, {QDir(path).dirName().isEmpty() ? fallbackName : QDir(path).dirName()});
        it->setIcon(0, icons.icon(QFileIconProvider::Folder));
        it->setData(0, PathRole, QDir::toNativeSeparators(path));
        it->setToolTip(0, QDir::toNativeSeparators(path));
    };
    addPlace(QStandardPaths::HomeLocation, tr("Home"));
    addPlace(QStandardPaths::DesktopLocation, tr("Desktop"));
    addPlace(QStandardPaths::DocumentsLocation, tr("Documents"));
    addPlace(QStandardPaths::DownloadLocation, tr("Downloads"));
    addPlace(QStandardPaths::PicturesLocation, tr("Pictures"));
    addPlace(QStandardPaths::MoviesLocation, tr("Videos"));

    auto* volumes = section(tr("Drives"));
    for (const QStorageInfo& v : QStorageInfo::mountedVolumes()) {
        if (!interestingVolume(v)) continue;
        QString name = v.displayName();
        if (name.isEmpty() || name == v.rootPath()) name = v.isRoot() ? tr("System Drive") : v.rootPath();
        auto* it = new QTreeWidgetItem(volumes, {name});
        it->setIcon(0, icons.icon(QFileIconProvider::Drive));
        it->setData(0, PathRole, QDir::toNativeSeparators(v.rootPath()));
        const qint64 used = v.bytesTotal() - v.bytesAvailable();
        it->setData(0, UsageRole, static_cast<double>(used) / static_cast<double>(v.bytesTotal()));
        it->setData(0, SubtitleRole, tr("%1 free of %2")
                                         .arg(QString::fromStdString(storage::humanSize(static_cast<uint64_t>(v.bytesAvailable()))),
                                              QString::fromStdString(storage::humanSize(static_cast<uint64_t>(v.bytesTotal())))));
        it->setToolTip(0, QDir::toNativeSeparators(v.rootPath()) + " · " + QString::fromUtf8(v.fileSystemType()));
    }

    auto* network = section(tr("Network"));
    for (const auto& nas : storage::loadNasConfigs()) {
        auto* it = new QTreeWidgetItem(network, {QString::fromStdString(nas.name)});
        it->setIcon(0, icons.icon(QFileIconProvider::Network));
        it->setData(0, NasRole, QString::fromStdString(nas.name));
        it->setData(0, SubtitleRole, QString::fromStdString(nas.volume().substr(6) + " · " + nas.root));
        it->setToolTip(0, QString::fromStdString(nas.volume() + nas.root));
    }
    auto* add = new QTreeWidgetItem(network, {tr("Add NAS…")});
    add->setIcon(0, icons.icon(QFileIconProvider::Network));
    add->setData(0, ActionRole, QStringLiteral("add-nas"));
    add->setForeground(0, theme::mutedText());

    try {
        storage::Index index(storage::defaultIndexPath());
        const auto scans = index.scans();
        if (!scans.empty()) {
            auto* recent = section(tr("Saved Scans"));
            for (const auto& s : scans) {
                const QString root = QString::fromStdString(s.root);
                QString name = QDir(root).dirName();
                if (name.isEmpty()) name = root;
                auto* it = new QTreeWidgetItem(recent, {name});
                it->setIcon(0, icons.icon(QFileIconProvider::Folder));
                it->setData(0, ScanIdRole, static_cast<qlonglong>(s.id));
                it->setData(0, PathRole, root);
                const std::string where = s.volume == "local" ? std::string() : s.volume.substr(6) + " · ";
                it->setData(0, SubtitleRole, QString::fromStdString(where + storage::humanSize(s.bytes) + " · " +
                                                                    storage::formatTime(s.finished)));
                if (s.volume != "local") it->setIcon(0, icons.icon(QFileIconProvider::Network));
                it->setToolTip(0, root);
            }
        }
    } catch (const std::exception&) {
        // A broken index should never stop the app from starting.
    }
}

void Sidebar::activate(QTreeWidgetItem* item) {
    if (!item || !item->parent()) return;
    if (item->data(0, ActionRole).toString() == "add-nas") {
        emit addNasRequested();
        return;
    }
    if (const QString nas = item->data(0, NasRole).toString(); !nas.isEmpty()) {
        emit nasRequested(nas);
        return;
    }
    const qint64 id = item->data(0, ScanIdRole).toLongLong();
    if (id > 0) {
        emit loadRequested(id);
        return;
    }
    const QString path = item->data(0, PathRole).toString();
    if (path.isEmpty()) return;
    // Open the saved scan if there is one; otherwise scan now.
    try {
        storage::Index index(storage::defaultIndexPath());
        if (const int64_t saved = index.findScan(storage::normalizeLocalPath(path.toStdString()))) {
            emit loadRequested(saved);
            return;
        }
    } catch (const std::exception&) {
    }
    emit scanRequested(path);
}

void Sidebar::contextMenuEvent(QContextMenuEvent* e) {
    QTreeWidgetItem* item = itemAt(e->pos());
    if (!item || !item->parent()) return;
    const QString path = item->data(0, PathRole).toString();
    const qint64 id = item->data(0, ScanIdRole).toLongLong();
    QMenu menu(this);
    if (const QString nas = item->data(0, NasRole).toString(); !nas.isEmpty()) {
        menu.addAction(tr("Scan"), this, [this, nas] { emit nasRequested(nas); });
        menu.addAction(tr("Edit…"), this, [this, nas] { emit editNasRequested(nas); });
        menu.addSeparator();
        menu.addAction(tr("Remove"), this, [this, nas] { emit removeNasRequested(nas); });
        menu.exec(e->globalPos());
        return;
    }
    if (!path.isEmpty() && item->data(0, ScanIdRole).toLongLong() == 0)
        menu.addAction(tr("Scan Again"), this, [this, path] { emit scanRequested(path); });
    if (id > 0) menu.addAction(tr("Remove from Index"), this, [this, id] { emit removeRequested(id); });
    if (!menu.isEmpty()) menu.exec(e->globalPos());
}

QSize SidebarDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const {
    QSize s = QStyledItemDelegate::sizeHint(option, index);
    if (!index.parent().isValid()) return QSize(s.width(), s.height() + 10);  // section header
    const int line = option.fontMetrics.height();
    if (index.data(Sidebar::UsageRole).isValid()) return QSize(s.width(), line * 2 + 22);
    if (index.data(Sidebar::SubtitleRole).isValid()) return QSize(s.width(), line * 2 + 12);
    return QSize(s.width(), line + 12);
}

void SidebarDelegate::paint(QPainter* p, const QStyleOptionViewItem& option, const QModelIndex& index) const {
    if (!index.parent().isValid()) {
        QStyleOptionViewItem opt(option);
        opt.state &= ~(QStyle::State_Selected | QStyle::State_MouseOver);
        opt.rect.adjust(4, 8, 0, 0);
        QStyledItemDelegate::paint(p, opt, index);
        return;
    }
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    const QRect r = option.rect.adjusted(4, 1, -6, -1);
    const bool selected = option.state & QStyle::State_Selected;
    const bool hover = option.state & QStyle::State_MouseOver;
    if (selected || hover) {
        QColor bg = selected ? theme::accent() : option.palette.color(QPalette::Midlight);
        if (selected) bg.setAlpha(theme::isDark() ? 110 : 60);
        p->setPen(Qt::NoPen);
        p->setBrush(bg);
        p->drawRoundedRect(r, 6, 6);
    }

    const QIcon icon = index.data(Qt::DecorationRole).value<QIcon>();
    const int iconSize = 18;
    const QRect iconRect(r.left() + 6, r.top() + (r.height() - iconSize) / 2, iconSize, iconSize);
    icon.paint(p, iconRect);

    const QString title = index.data(Qt::DisplayRole).toString();
    const QVariant subtitle = index.data(Sidebar::SubtitleRole);
    const QVariant usage = index.data(Sidebar::UsageRole);
    const QRect text = r.adjusted(iconSize + 14, 4, -6, -4);
    const QFontMetrics fm = option.fontMetrics;

    p->setPen(option.palette.color(QPalette::Text));
    if (!subtitle.isValid()) {
        p->drawText(text, Qt::AlignVCenter | Qt::AlignLeft, fm.elidedText(title, Qt::ElideMiddle, text.width()));
        p->restore();
        return;
    }
    p->drawText(QRect(text.left(), text.top(), text.width(), fm.height()), Qt::AlignLeft | Qt::AlignVCenter,
                fm.elidedText(title, Qt::ElideMiddle, text.width()));

    QFont small = option.font;
    small.setPointSizeF(small.pointSizeF() - 1.5);
    p->setFont(small);
    const int y2 = text.top() + fm.height() + 2;
    if (usage.isValid()) {
        const double u = std::clamp(usage.toDouble(), 0.0, 1.0);
        const QRect bar(text.left(), y2 + 2, text.width(), 4);
        QColor track = option.palette.color(QPalette::Mid);
        p->setPen(Qt::NoPen);
        p->setBrush(track);
        p->drawRoundedRect(bar, 2, 2);
        p->setBrush(u > 0.9 ? QColor(0xEF, 0x44, 0x44) : u > 0.75 ? QColor(0xF5, 0x9E, 0x0B) : theme::accent());
        p->drawRoundedRect(QRect(bar.left(), bar.top(), std::max(2, static_cast<int>(bar.width() * u)), bar.height()), 2, 2);
        p->setPen(theme::mutedText());
        p->drawText(QRect(text.left(), bar.bottom() + 1, text.width(), QFontMetrics(small).height()),
                    Qt::AlignLeft | Qt::AlignVCenter, subtitle.toString());
    } else {
        p->setPen(theme::mutedText());
        p->drawText(QRect(text.left(), y2, text.width(), QFontMetrics(small).height()), Qt::AlignLeft | Qt::AlignVCenter,
                    QFontMetrics(small).elidedText(subtitle.toString(), Qt::ElideRight, text.width()));
    }
    p->restore();
}
