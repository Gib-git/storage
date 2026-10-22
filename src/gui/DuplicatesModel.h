#pragma once

#include <QAbstractItemModel>

#include <vector>

#include "storage/core/duplicates.hpp"

// Two-level model: duplicate groups, each with its files. A checked file is
// one the user wants removed; the model refuses to check every copy of a group.
class DuplicatesModel : public QAbstractItemModel {
    Q_OBJECT
public:
    enum Column { Name, Size, Modified, Folder, ColumnCount };
    enum Role { SortRole = Qt::UserRole + 1, PathRole };

    explicit DuplicatesModel(QObject* parent = nullptr);

    // `base` is the searched folder; the Folder column is shown relative to it.
    void setGroups(std::vector<storage::DupGroup> groups, const QString& base = {});
    const std::vector<storage::DupGroup>& groups() const { return groups_; }
    const std::vector<std::vector<bool>>& selection() const { return remove_; }

    void applyRule(storage::KeepRule rule, const std::vector<std::string>& preferred = {});
    void clearSelection();
    // Keep only file `file` of group `group`; mark the others for removal.
    void keepOnly(int group, int file);
    // Drop files that have been dealt with (paths in `gone`) and groups left with < 2 files.
    void removePaths(const std::vector<std::string>& gone);

    size_t selectedCount() const;
    uint64_t selectedBytes() const;
    uint64_t reclaimableBytes() const;

    bool isGroup(const QModelIndex& index) const { return index.isValid() && index.internalId() == kGroupId; }
    int groupOf(const QModelIndex& index) const;
    QString pathAt(const QModelIndex& index) const;

    QModelIndex index(int row, int column, const QModelIndex& parent = {}) const override;
    QModelIndex parent(const QModelIndex& child) const override;
    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    bool setData(const QModelIndex& index, const QVariant& value, int role = Qt::EditRole) override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

signals:
    void selectionChanged();
    void refused(const QString& why);

private:
    static constexpr quintptr kGroupId = ~quintptr(0);
    QString relativeFolder(const std::string& path) const;

    std::vector<storage::DupGroup> groups_;
    std::vector<std::vector<bool>> remove_;
    QString base_;
};
