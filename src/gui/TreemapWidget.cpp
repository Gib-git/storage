#include "TreemapWidget.h"

#include <QContextMenuEvent>
#include <QHelpEvent>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QToolTip>

#include <algorithm>
#include <cmath>

#include "Theme.h"
#include "storage/core/category.hpp"
#include "storage/core/format.hpp"

using namespace storage;

namespace {

constexpr int kMaxDepth = 4;          // levels drawn below the root
constexpr qreal kHeader = 16.0;       // folder title strip
constexpr qreal kMinFolderW = 48.0;   // smaller folders are drawn as solid blocks
constexpr qreal kMinFolderH = 34.0;

struct Item {
    Node* node;
    qreal area;
};

qreal worst(const std::vector<Item>& items, size_t from, size_t to, qreal side) {
    qreal sum = 0, mx = 0, mn = std::numeric_limits<qreal>::max();
    for (size_t i = from; i < to; ++i) {
        sum += items[i].area;
        mx = std::max(mx, items[i].area);
        mn = std::min(mn, items[i].area);
    }
    if (sum <= 0 || mn <= 0) return std::numeric_limits<qreal>::max();
    const qreal s2 = sum * sum, w2 = side * side;
    return std::max(w2 * mx / s2, s2 / (w2 * mn));
}

// Bruls, Huizing & van Wijk squarified layout. `items` sorted by area, descending.
std::vector<std::pair<Node*, QRectF>> squarify(const std::vector<Item>& items, QRectF rect) {
    std::vector<std::pair<Node*, QRectF>> out;
    out.reserve(items.size());
    size_t i = 0;
    while (i < items.size() && rect.width() > 0.5 && rect.height() > 0.5) {
        const qreal side = std::min(rect.width(), rect.height());
        size_t end = i + 1;
        while (end < items.size() && worst(items, i, end + 1, side) <= worst(items, i, end, side)) ++end;

        qreal rowArea = 0;
        for (size_t k = i; k < end; ++k) rowArea += items[k].area;

        if (rect.width() >= rect.height()) {  // column on the left
            const qreal w = rowArea / rect.height();
            qreal y = rect.top();
            for (size_t k = i; k < end; ++k) {
                const qreal h = items[k].area / w;
                out.emplace_back(items[k].node, QRectF(rect.left(), y, w, h));
                y += h;
            }
            rect.setLeft(rect.left() + w);
        } else {  // row along the top
            const qreal h = rowArea / rect.width();
            qreal x = rect.left();
            for (size_t k = i; k < end; ++k) {
                const qreal w = items[k].area / h;
                out.emplace_back(items[k].node, QRectF(x, rect.top(), w, h));
                x += w;
            }
            rect.setTop(rect.top() + h);
        }
        i = end;
    }
    return out;
}

QColor fileColor(const Node* n) {
    return theme::categoryColor(categorize(n->name));
}

}  // namespace

TreemapWidget::TreemapWidget(QWidget* parent) : QWidget(parent) {
    setMouseTracking(true);
    setMinimumSize(200, 120);
    setAttribute(Qt::WA_OpaquePaintEvent);
}

void TreemapWidget::setRoot(Node* root) {
    root_ = root;
    selected_ = nullptr;
    hover_ = nullptr;
    dirty_ = true;
    update();
}

void TreemapWidget::setSelected(Node* node) {
    if (selected_ == node) return;
    selected_ = node;
    update();
}

void TreemapWidget::refreshTheme() {
    dirty_ = true;
    update();
}

void TreemapWidget::resizeEvent(QResizeEvent*) {
    dirty_ = true;
}

void TreemapWidget::rebuild() {
    const qreal dpr = devicePixelRatioF();
    cache_ = QImage((size() * dpr), QImage::Format_ARGB32_Premultiplied);
    cache_.setDevicePixelRatio(dpr);
    cache_.fill(palette().color(QPalette::Base));
    cells_.clear();
    dirty_ = false;
    if (!root_) return;

    QPainter p(&cache_);
    p.setRenderHint(QPainter::Antialiasing, false);
    QFont f = font();
    f.setPointSizeF(std::max(7.0, f.pointSizeF() - 1.5));
    p.setFont(f);

    const QRectF area = QRectF(rect()).adjusted(2, 2, -2, -2);
    if (!root_->isDir()) {
        p.fillRect(area, fileColor(root_));
        cells_.push_back({area, root_, 0});
        return;
    }
    layoutLevel(root_, area, 0, p);
}

void TreemapWidget::layoutLevel(Node* dir, const QRectF& area, int depth, QPainter& p) {
    if (dir->size == 0 || area.width() < 2 || area.height() < 2) return;

    std::vector<Item> items;
    const qreal scale = (area.width() * area.height()) / static_cast<qreal>(dir->size);
    for (auto& c : dir->children) {
        if (c->size == 0 || (c->flags & NodeFlag::HardlinkRepeat)) continue;
        const qreal a = static_cast<qreal>(c->size) * scale;
        if (a < 1.0) break;  // children are sorted by size: the rest are sub-pixel
        items.push_back({c.get(), a});
    }

    const QColor frame = palette().color(QPalette::Base);
    const QColor text = palette().color(QPalette::Text);
    for (auto& [node, r] : squarify(items, area)) {
        cells_.push_back({r, node, depth});
        const QRectF inner = r.adjusted(0.5, 0.5, -0.5, -0.5);

        if (node->isDir()) {
            const bool expand = depth + 1 < kMaxDepth && r.width() >= kMinFolderW && r.height() >= kMinFolderH;
            p.fillRect(inner, theme::folderColor(depth + 1));
            if (expand) {
                p.setPen(text);
                const QRectF title(inner.left() + 4, inner.top(), inner.width() - 8, kHeader);
                const QString label = QString::fromStdString(node->name) + "  " +
                                      QString::fromStdString(humanSize(node->size));
                p.drawText(title, Qt::AlignVCenter | Qt::AlignLeft,
                           p.fontMetrics().elidedText(label, Qt::ElideRight, static_cast<int>(title.width())));
                layoutLevel(node, inner.adjusted(2, kHeader, -2, -2), depth + 1, p);
            } else {
                // Collapsed folder: tint it with the colour of its largest file.
                const Node* biggest = node;
                while (biggest->isDir() && !biggest->children.empty()) biggest = biggest->children.front().get();
                const QColor c = biggest->isDir() ? theme::folderColor(depth + 2) : fileColor(biggest).darker(125);
                QLinearGradient g(inner.topLeft(), inner.bottomRight());
                g.setColorAt(0, c.lighter(112));
                g.setColorAt(1, c.darker(108));
                p.fillRect(inner, g);
                if (r.width() > 60 && r.height() > 18) {
                    p.setPen(QColor(255, 255, 255, 220));
                    p.drawText(inner.adjusted(4, 2, -4, -2), Qt::AlignLeft | Qt::AlignTop,
                               p.fontMetrics().elidedText(QString::fromStdString(node->name) + "/", Qt::ElideRight,
                                                          static_cast<int>(inner.width() - 8)));
                }
            }
        } else {
            const QColor c = fileColor(node);
            QLinearGradient g(inner.topLeft(), inner.bottomRight());
            g.setColorAt(0, c.lighter(118));
            g.setColorAt(1, c.darker(112));
            p.fillRect(inner, g);
        }
        p.setPen(frame);
        p.drawRect(r);

        if (!node->isDir() && r.width() > 60 && r.height() > 18) {
            p.setPen(QColor(255, 255, 255, 230));
            p.drawText(inner.adjusted(4, 2, -4, -2), Qt::AlignLeft | Qt::AlignTop,
                       p.fontMetrics().elidedText(QString::fromStdString(node->name), Qt::ElideRight,
                                                  static_cast<int>(inner.width() - 8)));
        }
    }
}

void TreemapWidget::paintEvent(QPaintEvent*) {
    if (dirty_ || cache_.size() != size() * devicePixelRatioF()) rebuild();
    QPainter p(this);
    p.drawImage(0, 0, cache_);

    if (!root_) {
        p.setPen(theme::mutedText());
        p.drawText(rect(), Qt::AlignCenter, tr("Nothing to show"));
        return;
    }
    p.setRenderHint(QPainter::Antialiasing);
    if (hover_ && hover_ != selected_) {
        const QRectF r = rectOf(hover_);
        if (!r.isNull()) {
            p.setPen(QPen(QColor(255, 255, 255, 200), 1.5));
            p.setBrush(QColor(255, 255, 255, 40));
            p.drawRect(r.adjusted(1, 1, -1, -1));
        }
    }
    if (selected_) {
        const QRectF r = rectOf(selected_);
        if (!r.isNull()) {
            p.setPen(QPen(theme::accent(), 2.5));
            p.setBrush(Qt::NoBrush);
            p.drawRect(r.adjusted(1, 1, -1, -1));
        }
    }
}

Node* TreemapWidget::hit(const QPointF& pos) const {
    // Cells are stored parents-before-children, so the last match is the deepest.
    for (auto it = cells_.rbegin(); it != cells_.rend(); ++it)
        if (it->rect.contains(pos)) return it->node;
    return nullptr;
}

QRectF TreemapWidget::rectOf(Node* node) const {
    // The node itself, or its closest drawn ancestor.
    for (Node* n = node; n; n = n->parent)
        for (const auto& c : cells_)
            if (c.node == n) return c.rect;
    return {};
}

void TreemapWidget::mouseMoveEvent(QMouseEvent* e) {
    Node* n = hit(e->position());
    if (n != hover_) {
        hover_ = n;
        update();
    }
}

void TreemapWidget::leaveEvent(QEvent*) {
    hover_ = nullptr;
    update();
}

void TreemapWidget::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;
    if (Node* n = hit(e->position())) emit nodeClicked(n);
}

void TreemapWidget::mouseDoubleClickEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;
    Node* n = hit(e->position());
    // Double-clicking a file navigates into the folder that contains it.
    while (n && !n->isDir()) n = n->parent;
    // Prefer the top-level child of the current root so navigation goes one level at a time.
    while (n && n->parent && n->parent != root_) n = n->parent;
    if (n && n != root_) emit nodeActivated(n);
}

void TreemapWidget::contextMenuEvent(QContextMenuEvent* e) {
    if (Node* n = hit(e->pos())) emit contextMenuRequested(n, e->globalPos());
}

bool TreemapWidget::event(QEvent* e) {
    if (e->type() == QEvent::ToolTip) {
        auto* he = static_cast<QHelpEvent*>(e);
        if (Node* n = hit(he->pos())) {
            QString tip = "<b>" + QString::fromStdString(n->name).toHtmlEscaped() + "</b><br>" +
                          QString::fromStdString(humanSize(n->size));
            if (root_ && root_->size)
                tip += QString(" · %1% of this folder")
                           .arg(100.0 * static_cast<double>(n->size) / static_cast<double>(root_->size), 0, 'f', 1);
            if (n->isDir()) tip += "<br>" + tr("%1 files").arg(QString::fromStdString(withThousands(n->files)));
            else tip += "<br>" + QString::fromUtf8(categoryName(categorize(n->name)));
            QToolTip::showText(he->globalPos(), tip, this);
        } else {
            QToolTip::hideText();
        }
        return true;
    }
    return QWidget::event(e);
}
