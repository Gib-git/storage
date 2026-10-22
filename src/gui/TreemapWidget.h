#pragma once

#include <QImage>
#include <QWidget>

#include <vector>

#include "storage/core/node.hpp"

// Squarified treemap of a directory, nested a few levels deep. Files are
// coloured by category; folders are framed with their name.
class TreemapWidget : public QWidget {
    Q_OBJECT
public:
    explicit TreemapWidget(QWidget* parent = nullptr);

    void setRoot(storage::Node* root);
    void setSelected(storage::Node* node);
    storage::Node* root() const { return root_; }
    void refreshTheme();

signals:
    void nodeClicked(storage::Node* node);
    void nodeActivated(storage::Node* node);  // double-click
    void contextMenuRequested(storage::Node* node, const QPoint& globalPos);

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void leaveEvent(QEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;
    bool event(QEvent*) override;

private:
    struct Cell {
        QRectF rect;
        storage::Node* node;
        int depth;
    };

    void rebuild();
    void layoutLevel(storage::Node* dir, const QRectF& area, int depth, class QPainter& p);
    storage::Node* hit(const QPointF& pos) const;
    QRectF rectOf(storage::Node* node) const;

    storage::Node* root_ = nullptr;
    storage::Node* selected_ = nullptr;
    storage::Node* hover_ = nullptr;
    std::vector<Cell> cells_;
    QImage cache_;
    bool dirty_ = true;
};
