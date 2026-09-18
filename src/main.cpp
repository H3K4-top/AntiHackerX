#include <QApplication>
#include <QTimer>
#include "mainwindow.h"
#include "runtime_bootstrap.h"

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    
    // 设置应用信息
    QApplication::setApplicationName("AntiHackerX");
    QApplication::setApplicationVersion("1.0.0");
    // 组织名。注意:Qt 用它决定 QSettings 的存储位置
    // (Linux: ~/.config/<组织名>/,Windows 注册表: HKCU\Software\<组织名>),
    // 改动会让旧位置的设置读不到。
    QApplication::setOrganizationName("H3K4");
    
    // 主窗口显示之前先保证运行环境可用:
    //   1) 有 Java —— 没有就询问用户,同意则下载便携版 JDK 25 并设置 JAVA_HOME
    //   2) 有 native-obfuscator 依赖 —— 取最新 Release,解到 <EXE 同级>/libs/
    // 用户拒绝安装 Java、或依赖获取失败时,引导流程自己弹窗说明,这里直接退出。
    RuntimeBootstrap bootstrap(nullptr);
    if (!bootstrap.ensureReady()) {
        return 0;
    }
    
    MainWindow window;
    window.applyRuntime(bootstrap.javaExecutable(),
                        bootstrap.javaHome(),
                        bootstrap.obfuscatorJarPath(),
                        bootstrap.collectedLog());
    window.show();

    // 命令行给了一个 JAR 就直接打开它 —— 便于做文件关联,也方便自动化验证。
    // 注意要在 show() 之后再开:analyzeAndDisplayJar 会往日志面板写东西。
    QString cliJar;
    if (argc > 1 && !QString::fromLocal8Bit(argv[1]).startsWith(QLatin1Char('-'))) {
        cliJar = QString::fromLocal8Bit(argv[1]);
        window.openJarFile(cliJar);
    }

    // 自测钩子:AHX_AUTOSTART 非空时,打开 JAR 后立刻开始加壳,跑完自动退出。
    // 用来在无桌面环境(qoffscreen)下端到端验证 GUI 链路 ——
    // 光靠编译通过是发现不了「按钮忘了 connect」这类问题的。
    if (!cliJar.isEmpty() && !qEnvironmentVariable("AHX_AUTOSTART").isEmpty()) {
        window.setAutoCloseProgress(true);
        QTimer::singleShot(0, &window, [&window] {
            QMetaObject::invokeMethod(&window, "onStartObfuscation");
        });
    }

    return app.exec();
}
