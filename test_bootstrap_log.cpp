#include <QCoreApplication>
#include <QDebug>
#include <iostream>
#include "src/runtime_bootstrap.h"

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("AntiHackerX");
    QCoreApplication::setApplicationVersion("1.0.0");
    QCoreApplication::setOrganizationName("H3K4");
    
    std::cout << "========== AntiHackerX 引导测试 ==========" << std::endl;
    
    RuntimeBootstrap bootstrap(nullptr);
    
    // 打印所有引导日志
    QObject::connect(&bootstrap, &RuntimeBootstrap::logMessage,
                     [](const QString &msg) { 
                         std::cout << msg.toStdString() << std::endl;
                     });
    
    std::cout << "开始引导..." << std::endl;
    if (!bootstrap.ensureReady()) {
        std::cout << "❌ 引导失败: " << bootstrap.errorMessage().toStdString() << std::endl;
        return 1;
    }
    
    std::cout << "\n========== 引导成功 ==========" << std::endl;
    std::cout << "Java:    " << bootstrap.javaExecutable().toStdString() << std::endl;
    std::cout << "NOBF:    " << bootstrap.obfuscatorJarPath().toStdString() << std::endl;
    std::cout << "C++ CC:  " << bootstrap.cppCompilerPath().toStdString() << std::endl;
    std::cout << "C++ CXX: " << bootstrap.cppCompilerXXPath().toStdString() << std::endl;
    
    return 0;
}
