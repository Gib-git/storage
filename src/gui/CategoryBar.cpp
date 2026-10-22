#include "CategoryBar.h"

#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <vector>

#include "Theme.h"
#include "storage/core/format.hpp"

using namespace storage;

CategoryBar::CategoryBar(QWidget* parent) : QWidget(parent) {
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

QSize CategoryBar::sizeHint() const {
    return QSize(400, fontMetrics().height() + 26);
}

void CategoryBar::setNode(const Node* node) {
    bytes_.fill(0);
    total_ = 0;
    if (node) {
        std::vector<const Node*> stack{node};
        while (!stack.empty()) {
            const Node* n = stack.back();
            stack.pop_back();
            if (n->isDir()) {
                for (const auto& c : n->children) stack.push_back(c.get());
            } else if (n->kind == NodeKind::File && !(n->flags & NodeFlag::HardlinkRepeat)) {
                bytes_[static_cast<size_t>(categorize(n->name))] += n->size;
                total_ += n->size;
            }
        }
    }
    update();
}

void CategoryBar::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRect r = rect().adjusted(10, 6, -10, -4);
    const QRect bar(r.left(), r.top(), r.width(), 8);

    std::vector<int> order;
    for (int i = 0; i < kCount; ++i)
        if (bytes_[static_cast<size_t>(i)]) order.push_back(i);
    std::sort(order.begin(), order.end(), [&](int a, int b) { return bytes_[static_cast<size_t>(a)] > bytes_[static_cast<size_t>(b)]; });

    p.setPen(Qt::NoPen);
    QColor track = palette().color(QPalette::Mid);
    p.setBrush(track);
    p.drawRoundedRect(bar, 4, 4);
    if (total_ == 0) return;

    // Stacked segments, clipped to the rounded track.
    QPainterPath clip;
    clip.addRoundedRect(bar, 4, 4);
    p.save();
    p.setClipPath(clip);
    qreal x = bar.left();
    for (int i : order) {
        const qreal w = bar.width() * static_cast<qreal>(bytes_[static_cast<size_t>(i)]) / static_cast<qreal>(total_);
        p.setBrush(theme::categoryColor(static_cast<Category>(i)));
        p.drawRect(QRectF(x, bar.top(), w + 0.5, bar.height()));
        x += w;
    }
    p.restore();

    // Legend
    QFont f = font();
    f.setPointSizeF(f.pointSizeF() - 1);
    p.setFont(f);
    const QFontMetrics fm(f);
    int lx = r.left();
    const int ly = bar.bottom() + 6;
    for (int i : order) {
        const QString label = QString::fromUtf8(categoryName(static_cast<Category>(i))) + " " +
                              QString::fromStdString(humanSize(bytes_[static_cast<size_t>(i)]));
        const int w = 14 + fm.horizontalAdvance(label) + 14;
        if (lx + w > r.right()) break;
        p.setPen(Qt::NoPen);
        p.setBrush(theme::categoryColor(static_cast<Category>(i)));
        p.drawEllipse(QRectF(lx, ly + (fm.height() - 8) / 2.0, 8, 8));
        p.setPen(theme::mutedText());
        p.drawText(QRect(lx + 12, ly, w, fm.height()), Qt::AlignLeft | Qt::AlignVCenter, label);
        lx += w;
    }
}
