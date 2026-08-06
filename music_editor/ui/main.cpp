/**
 * ui — RCP 乐谱编辑器 GUI
 *
 * 用法:
 *   ui                    (启动图形界面)
 *
 * 依赖: Qt Widgets, music-core
 */

#include <QApplication>
#include "mainwindow.h"

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName("RCP 乐谱编辑器");
    QApplication::setApplicationVersion("1.0.0");

    MainWindow w;
    w.show();

    return app.exec();
}
