#include <QApplication>
#include <QDebug>
#include "runtime_bootstrap.h"

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("AntiHackerX");
    QApplication::setApplicationVersion("2.0.0");
    QApplication::setOrganizationName("H3K4");
    
    RuntimeBootstrap bootstrap(nullptr);
    
    // 打印引导日志(原版会传给 MainWindow,这里直接打印)
    QObject::connect(&bootstrap, &RuntimeBootstrap::logMessage,
                     [](const QString &msg) { qDebug().noquote() << msg; });
    
    if (!bootstrap.ensureReady()) {
        qDebug() << "ensureReady() 返回 false,错误:" << bootstrap.errorMessage();
        return 1;
    }
    
    qDebug() << "========== 引导成功 ==========";
    qDebug() << "Java:" << bootstrap.javaExecutable();
    qDebug() << "NOBF:" << bootstrap.obfuscatorJarPath();
    qDebug() << "C++ CC:" << bootstrap.cppCompilerPath();
    qDebug() << "C++ CXX:" << bootstrap.cppCompilerXXPath();
    return 0;
}
