#include <QApplication>
#include <QCommandLineParser>
#include <QSettings>
#include <QTimer>

#include "MainWindow.h"
#include "Theme.h"
#include "storage/core/version.hpp"

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(storage::kAppName);
    QApplication::setApplicationDisplayName(storage::kAppName);
    QApplication::setApplicationVersion(storage::kVersion);
    QApplication::setOrganizationName("Storage");
    QApplication::setOrganizationDomain("github.com/Gib-git/storage");
    QApplication::setWindowIcon(theme::appIcon());

    theme::apply(theme::modeFromName(QSettings().value("ui/theme", "system").toString()));

    QCommandLineParser cli;
    cli.setApplicationDescription("Index, deduplicate, organize and clean your disks and NAS");
    cli.addHelpOption();
    cli.addVersionOption();
    cli.addPositionalArgument("path", "Folder or drive to open on start");
    QCommandLineOption rescan("rescan", "Always rescan instead of opening the saved scan");
    cli.addOption(rescan);
    QCommandLineOption screenshot("screenshot", "Save a PNG of the window once the path is loaded, then quit", "file");
    cli.addOption(screenshot);
    QCommandLineOption writeIcons("write-icons", "Render the app icon as PNGs into a directory (build tooling)", "dir");
    writeIcons.setFlags(QCommandLineOption::HiddenFromHelp);
    cli.addOption(writeIcons);
    cli.process(app);

    if (cli.isSet(writeIcons)) {
        const QString dir = cli.value(writeIcons);
        bool ok = true;
        for (int size : {16, 32, 48, 64, 128, 256, 512, 1024})
            ok &= theme::appIcon().pixmap(size, size).save(QString("%1/icon_%2.png").arg(dir).arg(size));
        return ok ? 0 : 1;
    }

    MainWindow w;
    w.show();
    if (cli.isSet(screenshot)) {
        // Used for docs and as a headless smoke test in CI (QT_QPA_PLATFORM=offscreen).
        const QString file = cli.value(screenshot);
        QObject::connect(&w, &MainWindow::scanFinished, &w, [&w, file](bool ok) {
            QTimer::singleShot(400, &w, [&w, file, ok] {
                const bool saved = w.grab().save(file);
                QCoreApplication::exit(ok && saved ? 0 : 1);
            });
        });
    }
    if (!cli.positionalArguments().isEmpty()) w.openPath(cli.positionalArguments().first(), cli.isSet(rescan));
    return QApplication::exec();
}
