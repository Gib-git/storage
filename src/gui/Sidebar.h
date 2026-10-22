#pragma once

#include <QStyledItemDelegate>
#include <QTreeWidget>

// Left-hand navigation: common folders, mounted volumes and saved scans.
class Sidebar : public QTreeWidget {
    Q_OBJECT
public:
    enum Role { PathRole = Qt::UserRole + 1, ScanIdRole, SubtitleRole, UsageRole, NasRole, ActionRole };

    explicit Sidebar(QWidget* parent = nullptr);
    void reload();

signals:
    void scanRequested(const QString& path);
    void loadRequested(qint64 scanId);
    void removeRequested(qint64 scanId);
    void nasRequested(const QString& name);       // open/scan a saved NAS
    void addNasRequested();
    void editNasRequested(const QString& name);
    void removeNasRequested(const QString& name);

protected:
    void contextMenuEvent(QContextMenuEvent* e) override;

private:
    QTreeWidgetItem* section(const QString& title);
    void activate(QTreeWidgetItem* item);
};

class SidebarDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter* p, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;
};
