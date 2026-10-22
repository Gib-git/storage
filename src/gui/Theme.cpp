#include "Theme.h"

#include <QApplication>
#include <QGuiApplication>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QStyle>
#include <QStyleFactory>
#include <QStyleHints>

namespace theme {

namespace {
Mode gMode = Mode::System;
bool gDark = false;

bool systemPrefersDark() {
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    const auto scheme = QGuiApplication::styleHints()->colorScheme();
    if (scheme != Qt::ColorScheme::Unknown) return scheme == Qt::ColorScheme::Dark;
#endif
    const QPalette p = QApplication::style()->standardPalette();
    return p.color(QPalette::Window).lightness() < 128;
}

QPalette lightPalette() {
    QPalette p;
    p.setColor(QPalette::Window, QColor(0xF5, 0xF6, 0xF8));
    p.setColor(QPalette::WindowText, QColor(0x1F, 0x23, 0x29));
    p.setColor(QPalette::Base, Qt::white);
    p.setColor(QPalette::AlternateBase, QColor(0xF7, 0xF8, 0xFA));
    p.setColor(QPalette::ToolTipBase, QColor(0x1F, 0x23, 0x29));
    p.setColor(QPalette::ToolTipText, Qt::white);
    p.setColor(QPalette::Text, QColor(0x1F, 0x23, 0x29));
    p.setColor(QPalette::PlaceholderText, QColor(0x8A, 0x91, 0x9C));
    p.setColor(QPalette::Button, QColor(0xFF, 0xFF, 0xFF));
    p.setColor(QPalette::ButtonText, QColor(0x1F, 0x23, 0x29));
    p.setColor(QPalette::Highlight, accent());
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::Link, accent());
    p.setColor(QPalette::Mid, QColor(0xD9, 0xDC, 0xE1));
    p.setColor(QPalette::Midlight, QColor(0xE8, 0xEA, 0xEE));
    p.setColor(QPalette::Dark, QColor(0xB8, 0xBD, 0xC5));
    p.setColor(QPalette::Disabled, QPalette::Text, QColor(0xA0, 0xA6, 0xAE));
    p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(0xA0, 0xA6, 0xAE));
    p.setColor(QPalette::Disabled, QPalette::WindowText, QColor(0xA0, 0xA6, 0xAE));
    return p;
}

QPalette darkPalette() {
    QPalette p;
    p.setColor(QPalette::Window, QColor(0x1E, 0x20, 0x24));
    p.setColor(QPalette::WindowText, QColor(0xE6, 0xE8, 0xEB));
    p.setColor(QPalette::Base, QColor(0x17, 0x19, 0x1C));
    p.setColor(QPalette::AlternateBase, QColor(0x1C, 0x1E, 0x22));
    p.setColor(QPalette::ToolTipBase, QColor(0x2B, 0x2E, 0x34));
    p.setColor(QPalette::ToolTipText, QColor(0xE6, 0xE8, 0xEB));
    p.setColor(QPalette::Text, QColor(0xE6, 0xE8, 0xEB));
    p.setColor(QPalette::PlaceholderText, QColor(0x7C, 0x83, 0x8D));
    p.setColor(QPalette::Button, QColor(0x2A, 0x2D, 0x32));
    p.setColor(QPalette::ButtonText, QColor(0xE6, 0xE8, 0xEB));
    p.setColor(QPalette::Highlight, accent());
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::Link, accent().lighter(130));
    p.setColor(QPalette::Mid, QColor(0x3A, 0x3E, 0x45));
    p.setColor(QPalette::Midlight, QColor(0x33, 0x36, 0x3C));
    p.setColor(QPalette::Dark, QColor(0x12, 0x13, 0x15));
    p.setColor(QPalette::Light, QColor(0x45, 0x49, 0x50));
    p.setColor(QPalette::Disabled, QPalette::Text, QColor(0x6A, 0x70, 0x78));
    p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(0x6A, 0x70, 0x78));
    p.setColor(QPalette::Disabled, QPalette::WindowText, QColor(0x6A, 0x70, 0x78));
    return p;
}

QString styleSheet(bool dark) {
    const QString sidebar = dark ? "#191B1F" : "#ECEEF1";
    const QString border = dark ? "#2E3137" : "#DCDFE4";
    const QString hover = dark ? "#2A2D33" : "#E2E5EA";
    return QStringLiteral(R"(
        QToolBar { border: none; spacing: 4px; padding: 4px 8px; }
        QToolBar QToolButton { padding: 4px 8px; border-radius: 6px; }
        QToolBar QToolButton:hover { background: %3; }
        QStatusBar { border-top: 1px solid %2; }
        #Sidebar { background: %1; border: none; border-right: 1px solid %2; }
        #Sidebar::item { padding: 2px 4px; border-radius: 6px; }
        #Header { border-bottom: 1px solid %2; }
        #Crumb { border: none; padding: 3px 6px; border-radius: 5px; font-weight: 500; }
        #Crumb:hover { background: %3; }
        #Summary { color: palette(placeholder-text); }
        QTreeView { border: none; }
        QHeaderView::section { padding: 4px 6px; border: none; border-bottom: 1px solid %2;
                               border-right: 1px solid %2; background: palette(window); }
        QSplitter::handle { background: %2; }
        #WelcomeTitle { font-size: 22px; font-weight: 600; }
        #BigButton { padding: 12px 18px; border-radius: 10px; border: 1px solid %2; text-align: left; }
        #BigButton:hover { background: %3; }
    )")
        .arg(sidebar, border, hover);
}

}  // namespace

void apply(Mode mode) {
    gMode = mode;
    QApplication::setStyle(QStyleFactory::create("Fusion"));
    gDark = mode == Mode::Dark || (mode == Mode::System && systemPrefersDark());
    QApplication::setPalette(gDark ? darkPalette() : lightPalette());
    qApp->setStyleSheet(styleSheet(gDark));
}

Mode current() { return gMode; }
bool isDark() { return gDark; }

QString modeName(Mode m) {
    switch (m) {
        case Mode::Light: return "light";
        case Mode::Dark: return "dark";
        default: return "system";
    }
}

Mode modeFromName(const QString& s) {
    if (s == "light") return Mode::Light;
    if (s == "dark") return Mode::Dark;
    return Mode::System;
}

QColor accent() { return QColor(0x3B, 0x82, 0xF6); }

QColor mutedText() { return gDark ? QColor(0x9A, 0xA1, 0xAB) : QColor(0x6B, 0x72, 0x7D); }

QColor categoryColor(storage::Category c) {
    using storage::Category;
    switch (c) {
        case Category::Image: return QColor(0x2B, 0xB3, 0xA3);
        case Category::Video: return QColor(0x8E, 0x6C, 0xEF);
        case Category::Audio: return QColor(0xE8, 0x6A, 0x92);
        case Category::Document: return QColor(0x4C, 0x8D, 0xF6);
        case Category::Archive: return QColor(0xF2, 0xA9, 0x3B);
        case Category::Code: return QColor(0x5C, 0xB8, 0x5C);
        case Category::Application: return QColor(0xEF, 0x6F, 0x4C);
        case Category::System: return QColor(0x8A, 0x94, 0xA6);
        case Category::Folder: return folderColor(0);
        case Category::Other: break;
    }
    return QColor(0xA3, 0xAE, 0xC2);
}

QColor folderColor(int depth) {
    if (gDark) return QColor(0x26, 0x29, 0x2E).lighter(100 + depth * 12);
    return QColor(0xE4, 0xE7, 0xEC).darker(100 + depth * 5);
}

QIcon appIcon() {
    QIcon icon;
    for (int size : {16, 32, 48, 64, 128, 256, 512, 1024}) {
        QPixmap pm(size, size);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        const qreal s = size;
        QPainterPath bg;
        bg.addRoundedRect(QRectF(s * 0.04, s * 0.04, s * 0.92, s * 0.92), s * 0.2, s * 0.2);
        QLinearGradient g(0, 0, 0, s);
        g.setColorAt(0, QColor(0x25, 0x63, 0xEB));
        g.setColorAt(1, QColor(0x1E, 0x3A, 0x8A));
        p.fillPath(bg, g);
        // A tiny treemap: the app's signature view.
        const qreal m = s * 0.18, w = s - 2 * m, gap = qMax<qreal>(1.0, s * 0.03), r = s * 0.04;
        auto block = [&](qreal x, qreal y, qreal bw, qreal bh, QColor c) {
            p.setBrush(c);
            p.setPen(Qt::NoPen);
            p.drawRoundedRect(QRectF(m + x * w + gap / 2, m + y * w + gap / 2, bw * w - gap, bh * w - gap), r, r);
        };
        block(0, 0, 0.58, 1.0, QColor(0x2B, 0xD3, 0xC0));
        block(0.58, 0, 0.42, 0.55, QColor(0xFF, 0xC2, 0x4D));
        block(0.58, 0.55, 0.42, 0.45, QColor(0xF0, 0x7C, 0xA8));
        p.end();
        icon.addPixmap(pm);
    }
    return icon;
}

}  // namespace theme
