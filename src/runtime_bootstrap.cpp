#include "runtime_bootstrap.h"

#include "app_logger.h"
#include "archive.h"
#include "download_dialog.h"
#include "downloader.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDialog>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QPushButton>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTimer>

namespace {

/// 便携版 JDK 主版本号(与 native-obfuscator 的运行要求一致)
constexpr int kJavaMajorVersion = 25;

const char *kReleaseApiUrl =
    "https://api.github.com/repos/xiaofanforfabric/native-obfuscator/releases/latest";
const char *kReleasePageUrl =
    "https://github.com/xiaofanforfabric/native-obfuscator/releases";
/// GitHub API 不可达(限流/断网)时的兜底版本,该标签已确认释放过资产。
///
/// ⚠️ 必须随新版一起更新:依赖包与 AntiHackerX 是分开发版的,停在旧标签
/// 会让断网用户拿到没有"加载器/隐藏类名随机化"的旧版 —— 两个加壳插件
/// 装在同一台服务器上就会 LinkageError。
const char *kFallbackReleaseTag = "v1.4.8";

/// 由标签推出分发包的下载地址(资产命名规律:native-obfuscator-<tag>.zip)
QString releaseZipUrlForTag(const QString &tag) {
    return QString("%1/download/%2/native-obfuscator-%2.zip")
        .arg(QString::fromLatin1(kReleasePageUrl), tag);
}

/// 标签 `v1.4.8` → `1.4.8`,先归一化才能和 jar 自己报的版本号比较
QString versionFromTag(const QString &tag) {
    QString version = tag.trimmed();
    if (version.startsWith(QLatin1Char('v'), Qt::CaseInsensitive)) {
        version.remove(0, 1);
    }
    return version;
}

/// 便携版 Java 的下载地址(Adoptium Temurin,开源且允许再分发)
///
/// 这个 API 会 307 跳 GitHub,再 302 跳到 release-assets.githubusercontent.com,
/// 所以下载器必须允许跟随重定向。
QString adoptiumDownloadUrl() {
#if defined(Q_OS_WIN)
    const QString osName = QStringLiteral("windows");
#elif defined(Q_OS_MACOS)
    const QString osName = QStringLiteral("mac");
#else
    const QString osName = QStringLiteral("linux");
#endif

    const QString arch = QSysInfo::currentCpuArchitecture();
    const QString archName =
        arch.contains(QStringLiteral("arm"), Qt::CaseInsensitive) ||
                arch.contains(QStringLiteral("aarch64"), Qt::CaseInsensitive)
            ? QStringLiteral("aarch64")
            : QStringLiteral("x64");

    return QString("https://api.adoptium.net/v3/binary/latest/%1/ga/%2/%3/jdk/hotspot/normal/eclipse?project=jdk")
        .arg(kJavaMajorVersion)
        .arg(osName, archName);
}

/// 便携版 Java 压缩包的扩展名(Windows 是 zip,其余是 tar.gz)
QString archiveSuffixForCurrentPlatform() {
#if defined(Q_OS_WIN)
    return QStringLiteral(".zip");
#else
    return QStringLiteral(".tar.gz");
#endif
}

// ─────────────────────────── 便携版 C++ 工具链(zig) ───────────────────────────

/// 索引不可达(限流 / 断网)时的兜底版本,该版本已确认可用
const char *kZigFallbackVersion = "0.16.0";

// 可用的镜像站列表(按响应速度排序)
struct ZigMirror {
    const char *name;        // 显示名称
    const char *indexUrl;    // index.json 地址
    const char *baseUrl;     // 下载基础 URL
};

static const ZigMirror kZigMirrors[] = {
    {"官方源 (ziglang.org)", "https://ziglang.org/download/index.json", "https://ziglang.org/download"},
    {"Linus 镜像 (最快)", "https://zig.linus.dev/zig/index.json", "https://zig.linus.dev/zig"},
    {"BCR 镜像", "https://zig.bcr.ist/index.json", "https://zig.bcr.ist"},
    {"Vortan 镜像", "https://zig.vortan.dev/zig/index.json", "https://zig.vortan.dev/zig"},
};

/// 当前平台在 zig 索引里的键名
QString zigPlatformKey() {
    const QString arch = QSysInfo::currentCpuArchitecture();
    const bool arm =
        arch.contains(QStringLiteral("arm"), Qt::CaseInsensitive) ||
        arch.contains(QStringLiteral("aarch64"), Qt::CaseInsensitive);
#if defined(Q_OS_WIN)
    return arm ? QStringLiteral("aarch64-windows") : QStringLiteral("x86_64-windows");
#elif defined(Q_OS_MACOS)
    return arm ? QStringLiteral("aarch64-macos") : QStringLiteral("x86_64-macos");
#else
    return arm ? QStringLiteral("aarch64-linux") : QStringLiteral("x86_64-linux");
#endif
}

/// 由版本号推出 zig 分发包的下载地址(支持镜像源)
QString zigTarballUrlForVersion(const QString &version, int mirrorIndex = 0) {
    const QString key = zigPlatformKey();
    const QString suffix =
#if defined(Q_OS_WIN)
        QStringLiteral("zip");
#else
        QStringLiteral("tar.xz");
#endif
    
    // 根据镜像索引选择下载源
    const ZigMirror &mirror = kZigMirrors[mirrorIndex];
    
    // 注意:zig 文件名格式是 zig-{arch}-{os}-{version}.{suffix}
    // 例如: zig-x86_64-linux-0.16.0.tar.xz (架构在前,不是 zig-linux-x86_64)
    return QString("%1/%2/zig-%3-%4.%5")
        .arg(QString::fromLatin1(mirror.baseUrl), version, key, version, suffix);
}

/// 便携版 C++ 工具链压缩包的扩展名
QString zigArchiveSuffix() {
#if defined(Q_OS_WIN)
    return QStringLiteral(".zip");
#else
    return QStringLiteral(".tar.xz");
#endif
}

/// Windows 交叉编译所需的 jni_md.h。
///
/// Linux/macOS 的 JDK 只带本机平台的 jni_md.h(`include/linux/`),
/// 交叉编译 Windows 目标时必须另外提供 `include/win32/` 那一份。
/// 这里内嵌现代 OpenJDK 的 win32 版本(内容极少且 ABI 长期稳定)。
const char *kWin32JniMdContent =
    "#ifndef _JAVASOFT_JNI_MD_H_\n"
    "#define _JAVASOFT_JNI_MD_H_\n"
    "/* 由 AntiHackerX 生成:现代 OpenJDK 的 win32 版本。\n"
    " * 本机 JDK 只带所在平台的 jni_md.h,交叉编译 Windows 目标时需要这一份。 */\n"
    "#define JNIEXPORT __declspec(dllexport)\n"
    "#define JNIIMPORT __declspec(dllimport)\n"
    "#define JNICALL __stdcall\n"
    "typedef long jint;\n"
    "typedef __int64 jlong;\n"
    "typedef signed char jbyte;\n"
    "#endif\n";

/// 同步 GET 一个小体积文本/JSON 资源(GitHub API 用)。
///
/// timeoutMs 可调:单纯做更新检查时用短超时,避免离线启动干等半分钟。
bool blockingHttpGet(const QUrl &url, const QByteArray &accept, QByteArray *bodyOut,
                     QString *errorOut, int timeoutMs = 30000) {
    QNetworkAccessManager manager;
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QVariant::fromValue(QNetworkRequest::NoLessSafeRedirectPolicy));
    request.setMaximumRedirectsAllowed(20);
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QString("%1/%2 (Qt)")
                          .arg(QApplication::applicationName(),
                               QApplication::applicationVersion()));
    request.setRawHeader("Accept", accept);
    request.setTransferTimeout(timeoutMs);

    QNetworkReply *reply = manager.get(request);
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    const QNetworkReply::NetworkError netError = reply->error();
    const QString netErrorText = reply->errorString();
    const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    // 超时被 abort 时设备已经关闭,直接 readAll() 会刷警告
    QByteArray data;
    if (reply->isOpen()) {
        data = reply->readAll();
    }
    reply->deleteLater();

    if (netError != QNetworkReply::NoError) {
        if (errorOut) {
            *errorOut = QString("网络错误: %1").arg(netErrorText);
        }
        return false;
    }
    if (httpStatus >= 400) {
        if (errorOut) {
            *errorOut = QString("服务器返回 HTTP %1").arg(httpStatus);
        }
        return false;
    }

    *bodyOut = data;
    return true;
}

/// 目录是否可写(用于决定把便携运行时放哪)
bool isWritableDirectory(const QString &path) {
    QDir dir(path);
    if (!dir.exists()) {
        return false;
    }
    const QString probe = dir.filePath(QStringLiteral(".write-probe-%1").arg(QCoreApplication::applicationPid()));
    QFile file(probe);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    file.close();
    QFile::remove(probe);
    return true;
}

} // namespace

RuntimeBootstrap::RuntimeBootstrap(QWidget *parent)
    : QObject(parent),
      m_parent(parent) {
}

QString RuntimeBootstrap::javaExecutableName() {
#if defined(Q_OS_WIN)
    return QStringLiteral("java.exe");
#else
    return QStringLiteral("java");
#endif
}

QString RuntimeBootstrap::bundledJavaExecutable(const QString &root) {
    return QDir(root).filePath(QString("java/bin/") + javaExecutableName());
}

QString RuntimeBootstrap::jarPathForRoot(const QString &root) {
    return QDir(root).filePath(QStringLiteral("libs/native-obfuscator.jar"));
}

QString RuntimeBootstrap::zigRootForRoot(const QString &root) {
    return QDir(root).filePath(QStringLiteral("libs/zig"));
}

QString RuntimeBootstrap::zigExecutable(const QString &root) {
#if defined(Q_OS_WIN)
    return QDir(zigRootForRoot(root)).filePath(QStringLiteral("zig.exe"));
#else
    return QDir(zigRootForRoot(root)).filePath(QStringLiteral("zig"));
#endif
}

QString RuntimeBootstrap::zigCompilerWrapper(const QString &root, bool cxx) {
#if defined(Q_OS_WIN)
    const QString name = cxx ? QStringLiteral("cxx.bat") : QStringLiteral("cc.bat");
#else
    const QString name = cxx ? QStringLiteral("cxx") : QStringLiteral("cc");
#endif
    return QDir(zigRootForRoot(root)).filePath(name);
}

QString RuntimeBootstrap::zigWin32JniMdPath(const QString &root) {
    return QDir(zigRootForRoot(root)).filePath(QStringLiteral("jni/win32/jni_md.h"));
}

bool RuntimeBootstrap::probeJava(const QString &javaExecutable, QString *versionOut, QString *errorOut) {
    if (javaExecutable.isEmpty()) {
        if (errorOut) {
            *errorOut = QStringLiteral("未指定 java 路径");
        }
        return false;
    }
    const QFileInfo info(javaExecutable);
    if (!info.exists() || !info.isFile()) {
        if (errorOut) {
            *errorOut = QString("文件不存在: %1").arg(javaExecutable);
        }
        return false;
    }

    QProcess process;
    // java -version 把版本信息写到 stderr,所以合并两个通道
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(javaExecutable, QStringList{QStringLiteral("-version")});
    if (!process.waitForStarted(10000)) {
        if (errorOut) {
            *errorOut = QString("无法启动: %1").arg(javaExecutable);
        }
        return false;
    }
    if (!process.waitForFinished(20000)) {
        process.kill();
        process.waitForFinished(3000);
        if (errorOut) {
            *errorOut = QString("执行 java -version 超时: %1").arg(javaExecutable);
        }
        return false;
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        if (errorOut) {
            *errorOut = QString("java -version 返回非 0(%1)").arg(process.exitCode());
        }
        return false;
    }

    if (versionOut) {
        const QString output = QString::fromUtf8(process.readAll()).trimmed();
        *versionOut = output.section('\n', 0, 0).trimmed();
    }
    return true;
}

QString RuntimeBootstrap::probeNativeObfuscatorVersion(const QString &javaExecutable,
                                                       const QString &jarPath) {
    if (javaExecutable.isEmpty() || !QFileInfo::exists(jarPath)) {
        return QString();
    }

    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(javaExecutable, QStringList{QStringLiteral("-jar"),
                                              jarPath,
                                              QStringLiteral("--version")});
    if (!process.waitForStarted(10000)) {
        return QString();
    }
    if (!process.waitForFinished(20000)) {
        process.kill();
        process.waitForFinished(3000);
        return QString();
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        return QString();
    }

    // 输出形如 "native-obfuscator 1.4.8"。逐行找,免得 JVM 的告警行挡在前面;
    // 取该行最后一个空白分隔的字段,并顺便吃掉可能存在的 v 前缀。
    const QString output = QString::fromUtf8(process.readAll());
    const QStringList lines = output.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &raw : lines) {
        const QString line = raw.simplified();
        if (!line.contains(QStringLiteral("native-obfuscator"), Qt::CaseInsensitive)) {
            continue;
        }
        const QStringList parts = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (parts.isEmpty()) {
            continue;
        }
        QString version = parts.last();
        if (version.startsWith(QLatin1Char('v'), Qt::CaseInsensitive)) {
            version.remove(0, 1);
        }
        if (!version.isEmpty()) {
            return version;
        }
    }
    return QString();
}

QString RuntimeBootstrap::detectJavaExecutable(QString *versionOut, QString *homeOut) {
    QString version;
    QString dummy;

    // 1) 程序自带的便携版 —— 最优先,保证绿色版自洽
    const QString appRoot = QCoreApplication::applicationDirPath();
    const QString bundled = bundledJavaExecutable(appRoot);
    if (probeJava(bundled, &version, &dummy)) {
        if (versionOut) {
            *versionOut = version;
        }
        if (homeOut) {
            *homeOut = QDir(appRoot).filePath(QStringLiteral("java"));
        }
        return QFileInfo(bundled).absoluteFilePath();
    }

    // 2) JAVA_HOME 环境变量
    const QByteArray javaHomeRaw = qgetenv("JAVA_HOME");
    if (!javaHomeRaw.isEmpty()) {
        const QString home = QString::fromLocal8Bit(javaHomeRaw).trimmed();
        const QString candidate = QDir(home).filePath(QString("bin/") + javaExecutableName());
        if (probeJava(candidate, &version, &dummy)) {
            if (versionOut) {
                *versionOut = version;
            }
            if (homeOut) {
                *homeOut = QDir(home).absolutePath();
            }
            return QFileInfo(candidate).absoluteFilePath();
        }
    }

    // 3) PATH 上的 java
    const QString onPath = QStandardPaths::findExecutable(QStringLiteral("java"));
    if (!onPath.isEmpty()) {
        const QString candidate =
            QDir(onPath).filePath(QStringLiteral("bin/") + javaExecutableName());
        const QString resolved = QFileInfo(candidate).exists() ? candidate : onPath;
        if (probeJava(resolved, &version, &dummy)) {
            if (versionOut) {
                *versionOut = version;
            }
            if (homeOut) {
                // <home>/bin/java -> <home>
                *homeOut = QFileInfo(QFileInfo(resolved).absolutePath()).absolutePath();
            }
            return QFileInfo(resolved).absoluteFilePath();
        }
    }

    return QString();
}

QString RuntimeBootstrap::preferredInstallRoot() const {
    const QString appRoot = QCoreApplication::applicationDirPath();
    if (isWritableDirectory(appRoot)) {
        return appRoot;
    }
    // 装在 Program Files 之类只读位置时,退到用户数据目录
    const QString fallback = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(fallback);
    return fallback;
}

QString RuntimeBootstrap::stagingRoot() const {
    return QDir(m_root).filePath(QStringLiteral(".bootstrap-tmp"));
}

void RuntimeBootstrap::appendLog(const QString &message) {
    m_log << message;
    emit logMessage(message);
    // 同时写入全局日志文件
    AppLogger::instance().log(message);
}

void RuntimeBootstrap::warn(const QString &title, const QString &text) {
    QMessageBox::warning(m_parent, title, text);
}

bool RuntimeBootstrap::refreshJavaDetection() {
    QString version;
    QString home;
    const QString exe = detectJavaExecutable(&version, &home);
    if (exe.isEmpty()) {
        return false;
    }
    m_javaExe = exe;
    m_javaHome = home;
    qputenv("JAVA_HOME", home.toLocal8Bit());
    appendLog(QString("[环境] 使用 Java: %1(%2)").arg(m_javaExe, version));
    return true;
}

bool RuntimeBootstrap::ensureReady() {
    ensureRoot();

    if (!ensureJava()) {
        return false;
    }
    if (!ensureNativeObfuscator()) {
        return false;
    }
    if (!ensureCppToolchain()) {
        return false;
    }

    // 让之后所有子进程都继承 JAVA_HOME
    qputenv("JAVA_HOME", m_javaHome.toLocal8Bit());
    appendLog(QString("[环境] JAVA_HOME = %1").arg(m_javaHome));
    return true;
}

void RuntimeBootstrap::ensureRoot() {
    if (m_root.isEmpty()) {
        m_root = preferredInstallRoot();
        appendLog(QString("[环境] 安装根目录: %1").arg(m_root));
    }
}

bool RuntimeBootstrap::ensureJava() {
    // 已经装好了(自带 / JAVA_HOME / PATH 任一处)就直接用
    if (refreshJavaDetection()) {
        return true;
    }

    appendLog(QStringLiteral("[环境] 未检测到任何可用的 Java 运行时,询问用户是否安装"));

    const QString question = QString(
        "该 APP 需要 Java 才能运行,但您的电脑上没有安装 Java。\n\n"
        "是否现在下载并安装 Java %1 便携版?\n\n"
        "· 来源:Adoptium Temurin(开源,可自由分发)\n"
        "· 位置:%2\n"
        "· 体积:约 180~200 MB,下载耗时取决于网速")
        .arg(kJavaMajorVersion)
        .arg(QDir::toNativeSeparators(QDir(m_root).filePath(QStringLiteral("java"))));

    QMessageBox box(m_parent);
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle(QStringLiteral("缺少 Java 运行环境"));
    box.setText(question);
    QPushButton *yesButton = box.addButton(QStringLiteral("是,现在安装"), QMessageBox::AcceptRole);
    box.addButton(QStringLiteral("否,退出"), QMessageBox::RejectRole);
    box.setDefaultButton(yesButton);
    box.exec();

    if (box.clickedButton() != yesButton) {
        appendLog(QStringLiteral("[环境] 用户拒绝安装 Java,程序退出"));
        QMessageBox::critical(m_parent,
                              QStringLiteral("无法继续"),
                              QStringLiteral("您必须安装 Java 才能使用该软件。\n\n程序即将退出。"));
        m_error = QStringLiteral("用户拒绝安装 Java 运行时");
        return false;
    }

    if (!installPortableJava()) {
        QMessageBox::critical(m_parent,
                              QStringLiteral("安装 Java 失败"),
                              QString("Java 便携版安装失败,程序无法继续运行。\n\n%1\n\n"
                                      "您也可以手动安装 Java %2 后重新启动本程序。")
                                  .arg(m_error)
                                  .arg(kJavaMajorVersion));
        return false;
    }

    // 安装完立刻切到新装的运行时(对应"进入设置 JAVA_HOME")
    if (!refreshJavaDetection()) {
        m_error = QStringLiteral("便携版 Java 安装后校验失败");
        return false;
    }
    appendLog(QStringLiteral("[环境] Java 便携版安装完成,已设置 JAVA_HOME"));
    return true;
}

bool RuntimeBootstrap::installPortableJava() {
    ensureRoot();
    m_error.clear();

    const QString staging = stagingRoot();
    Archive::removeDirectoryRecursively(staging);
    if (!QDir().mkpath(staging)) {
        m_error = QString("无法创建临时目录: %1").arg(staging);
        return false;
    }

    const QString archivePath = QDir(staging).filePath(QStringLiteral("java-runtime") + archiveSuffixForCurrentPlatform());
    const QUrl url(adoptiumDownloadUrl());
    appendLog(QString("[环境] 开始下载 Java %1:%2").arg(kJavaMajorVersion).arg(url.toString()));

    QString error;
    if (!runDownloadWithDialog(QStringLiteral("下载 Java 运行环境"),
                               QString("该 APP 需要 Java 才能运行,正在为您安装 Java %1 便携版。")
                                   .arg(kJavaMajorVersion),
                               QString("正在下载 Java %1 便携版:").arg(kJavaMajorVersion),
                               url,
                               archivePath,
                               &error)) {
        Archive::removeDirectoryRecursively(staging);
        m_error = error;
        return false;
    }

    const QString unpackDir = QDir(staging).filePath(QStringLiteral("unpack"));
    appendLog(QStringLiteral("[环境] 下载完成,正在解压 Java 运行时…"));
    if (!Archive::extract(archivePath, unpackDir, &error)) {
        Archive::removeDirectoryRecursively(staging);
        m_error = QString("解压 Java 压缩包失败:%1").arg(error);
        return false;
    }

    // Adoptium 的包里带一层顶层目录(Windows/Linux 是 jdk-25.x,
    // macOS 是 jdk-25.jdk 且真正的 home 在 Contents/Home)
    QString javaHomeSource = unpackDir;
    const QStringList tops = Archive::topLevelEntries(unpackDir);
    if (tops.size() == 1 && QFileInfo(QDir(unpackDir).filePath(tops.first())).isDir()) {
        javaHomeSource = QDir(unpackDir).filePath(tops.first());
        if (QDir(javaHomeSource + QStringLiteral("/Contents/Home")).exists()) {
            javaHomeSource += QStringLiteral("/Contents/Home");
        }
    }

    if (!QFileInfo(javaHomeSource).isDir()) {
        Archive::removeDirectoryRecursively(staging);
        m_error = QStringLiteral("解压结果结构异常,未找到 Java 目录");
        return false;
    }

    const QString targetHome = QDir(m_root).filePath(QStringLiteral("java"));
    Archive::removeDirectoryRecursively(targetHome);

    // staging 建在安装根目录下,所以这里通常是同一个文件系统,rename 是原子的
    if (QDir().rename(javaHomeSource, targetHome)) {
        Archive::removeDirectoryRecursively(staging);
    } else {
        appendLog(QStringLiteral("[环境] 跨设备移动,改用复制方式"));
        if (!Archive::copyDirectoryRecursively(javaHomeSource, targetHome, &error)) {
            Archive::removeDirectoryRecursively(staging);
            m_error = QString("移动 Java 运行时失败:%1").arg(error);
            return false;
        }
        Archive::removeDirectoryRecursively(staging);
    }

    const QString installed = bundledJavaExecutable(m_root);
    if (!QFileInfo(installed).exists()) {
        m_error = QString("安装后未找到 java 主程序: %1").arg(installed);
        return false;
    }

    appendLog(QString("[环境] Java 已安装到: %1").arg(targetHome));
    return true;
}

bool RuntimeBootstrap::ensureCppToolchain() {
    ensureRoot();

    const QString zigExe = zigExecutable(m_root);
    const QString ccWrapper = zigCompilerWrapper(m_root, false);
    const QString cxxWrapper = zigCompilerWrapper(m_root, true);

    if (QFileInfo::exists(zigExe) && QFileInfo::exists(ccWrapper) &&
        QFileInfo::exists(cxxWrapper)) {
        m_cppCompiler = ccWrapper;
        m_cppCompilerXX = cxxWrapper;
        appendLog(QString("[工具链] C++ 工具链已就绪: %1").arg(zigRootForRoot(m_root)));
        appendLog(QString("[工具链]   CC  = %1").arg(m_cppCompiler));
        appendLog(QString("[工具链]   CXX = %1").arg(m_cppCompilerXX));
        return true;
    }

    appendLog(QStringLiteral("[工具链] 未检测到便携版 C++ 编译器,询问用户是否安装"));

#if defined(Q_OS_WIN)
    const QString sizeHint = QStringLiteral("约 95 MB");
#else
    const QString sizeHint = QStringLiteral("约 55 MB");
#endif

    const QString question = QString(
        "打包时需要把生成的 C++ 代码编译成原生库,但还没有可用的 C++ 工具链。\n\n"
        "是否现在下载并安装便携版 C++ 工具链(zig)?\n\n"
        "· 来源:ziglang.org(开源,可自由分发)\n"
        "· 位置:%1\n"
        "· 体积:%2,下载耗时取决于网速\n"
        "· 特点:自带 libc 与 libc++,不依赖系统里的任何编译环境;\n"
        "  同一份代码可同时产出 Windows DLL 与 Linux SO")
        .arg(QDir::toNativeSeparators(zigRootForRoot(m_root)), sizeHint);

    QMessageBox box(m_parent);
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle(QStringLiteral("缺少 C++ 工具链"));
    box.setText(question);
    QPushButton *yesButton = box.addButton(QStringLiteral("是,现在安装"), QMessageBox::AcceptRole);
    box.addButton(QStringLiteral("稍后再说"), QMessageBox::RejectRole);
    box.setDefaultButton(yesButton);
    box.exec();

    // C++ 工具链是"打包时才需要"的依赖,允许用户推迟;
    // 不阻断启动,真正开始打包时会再次提示。
    if (box.clickedButton() != yesButton) {
        appendLog(QStringLiteral("[工具链] 用户选择稍后安装(打包功能暂不可用)"));
        warn(QStringLiteral("C++ 工具链未安装"),
             QStringLiteral("未安装 C++ 工具链,打包功能暂时不可用。\n\n"
                            "可以在菜单「工具 → 重新安装 C++ 工具链」里随时补装。"));
        return true;
    }

    appendLog(QStringLiteral("[工具链] 用户确认安装,开始下载…"));
    if (!installPortableCppToolchain()) {
        appendLog(QString("[工具链] 安装失败: %1").arg(m_error));
        warn(QStringLiteral("安装 C++ 工具链失败"),
             QString("便携版 C++ 工具链安装失败,打包功能暂时不可用。\n\n%1")
                 .arg(m_error));
        return true;
    }
    appendLog(QStringLiteral("[工具链] 安装成功"));
    return true;
}

bool RuntimeBootstrap::installPortableCppToolchain() {
    ensureRoot();
    m_error.clear();

    appendLog(QStringLiteral("[工具链] ──────── 开始安装便携版 C++ 工具链 ────────"));

    const QString staging = stagingRoot();
    Archive::removeDirectoryRecursively(staging);
    if (!QDir().mkpath(staging)) {
        m_error = QString("无法创建临时目录: %1").arg(staging);
        appendLog(QString("[工具链] 错误: %1").arg(m_error));
        return false;
    }
    appendLog(QString("[工具链] 临时目录: %1").arg(staging));

    // 让用户选择下载镜像源
    QMessageBox mirrorBox(m_parent);
    mirrorBox.setIcon(QMessageBox::Question);
    mirrorBox.setWindowTitle(QStringLiteral("选择下载源"));
    mirrorBox.setText(QStringLiteral("请选择 zig 工具链下载源(约 50-90 MB):"));
    
    QList<QPushButton*> mirrorButtons;
    for (size_t i = 0; i < sizeof(kZigMirrors) / sizeof(kZigMirrors[0]); ++i) {
        QPushButton *btn = mirrorBox.addButton(
            QString::fromUtf8(kZigMirrors[i].name),
            QMessageBox::ActionRole);
        mirrorButtons.append(btn);
    }
    QPushButton *cancelBtn = mirrorBox.addButton(QStringLiteral("取消"), QMessageBox::RejectRole);
    mirrorBox.setDefaultButton(mirrorButtons[1]); // 默认选择最快的镜像
    mirrorBox.setInformativeText(QStringLiteral("提示:已启用系统代理设置"));
    mirrorBox.exec();
    
    int selectedMirror = -1;
    for (int i = 0; i < mirrorButtons.size(); ++i) {
        if (mirrorBox.clickedButton() == mirrorButtons[i]) {
            selectedMirror = i;
            break;
        }
    }
    
    if (selectedMirror == -1 || mirrorBox.clickedButton() == cancelBtn) {
        m_error = QStringLiteral("用户取消下载");
        appendLog(QStringLiteral("[工具链] 用户取消下载"));
        return false;
    }
    
    const ZigMirror &mirror = kZigMirrors[selectedMirror];
    appendLog(QString("[工具链] 选择下载源: %1").arg(QString::fromUtf8(mirror.name)));

    // 查询最新版本号;索引不可达就退回固定版本
    QString version = QString::fromLatin1(kZigFallbackVersion);
    QByteArray body;
    QString indexError;
    
    appendLog(QString("[工具链] 查询最新版本: %1").arg(QString::fromLatin1(mirror.indexUrl)));
    if (blockingHttpGet(QUrl(QString::fromLatin1(mirror.indexUrl)),
                        QByteArrayLiteral("application/json"),
                        &body,
                        &indexError)) {
        const QJsonObject root = QJsonDocument::fromJson(body).object();
        const QString latest = root.value(QStringLiteral("version")).toString();
        if (!latest.isEmpty()) {
            version = latest;
            appendLog(QString("[工具链] 最新版本: %1").arg(version));
        } else {
            appendLog(QString("[工具链] 索引无版本号,使用内置版本 %1").arg(version));
        }
    } else {
        appendLog(QString("[工具链] 读取版本索引失败: %1").arg(indexError));
        appendLog(QString("[工具链] 改用内置版本: %1").arg(version));
    }

    const QUrl url(zigTarballUrlForVersion(version, selectedMirror));
    const QString archivePath =
        QDir(staging).filePath(QStringLiteral("zig-toolchain") + zigArchiveSuffix());
    appendLog(QString("[工具链] 下载地址: %1").arg(url.toString()));
    appendLog(QString("[工具链] 保存路径: %1").arg(archivePath));

    QString error;
    if (!runDownloadWithDialog(QStringLiteral("下载 C++ 工具链"),
                               QString("正在为您安装便携版 C++ 工具链(zig %1)。\n\n"
                                      "提示:已启用系统代理设置。").arg(version),
                               QString("正在从 %1 下载 zig %2:")
                                   .arg(QString::fromUtf8(mirror.name), version),
                               url,
                               archivePath,
                               &error)) {
        Archive::removeDirectoryRecursively(staging);
        m_error = error;
        appendLog(QString("[工具链] 下载失败: %1").arg(error));
        return false;
    }
    appendLog(QStringLiteral("[工具链] 下载完成"));

    const QString unpackDir = QDir(staging).filePath(QStringLiteral("unpack"));
    appendLog(QString("[工具链] 开始解压到: %1").arg(unpackDir));
    if (!Archive::extract(archivePath, unpackDir, &error)) {
        Archive::removeDirectoryRecursively(staging);
        m_error = QString("解压 zig 压缩包失败:%1").arg(error);
        appendLog(QString("[工具链] 解压失败: %1").arg(error));
        return false;
    }
    appendLog(QStringLiteral("[工具链] 解压完成"));

    // zig 的包带一层顶层目录(zig-<平台>-<版本>),剥掉它
    QString toolchainSource = unpackDir;
    const QStringList tops = Archive::topLevelEntries(unpackDir);
    appendLog(QString("[工具链] 顶层目录数: %1").arg(tops.size()));
    if (tops.size() == 1 && QFileInfo(QDir(unpackDir).filePath(tops.first())).isDir()) {
        toolchainSource = QDir(unpackDir).filePath(tops.first());
        appendLog(QString("[工具链] 剥离顶层目录: %1").arg(tops.first()));
    }

    // 检查解压后的 zig 可执行文件(直接在 toolchainSource 目录下)
    const QString unpackedZigExe = QDir(toolchainSource).filePath(QStringLiteral("zig"));
    if (!QFileInfo::exists(unpackedZigExe)) {
        Archive::removeDirectoryRecursively(staging);
        m_error = QString("解压结果结构异常,未找到 zig 主程序: %1").arg(unpackedZigExe);
        appendLog(QString("[工具链] 错误: %1").arg(m_error));
        appendLog(QString("[工具链] 解压目录内容: %1").arg(
            QDir(toolchainSource).entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot).join(", ")));
        return false;
    }
    appendLog(QString("[工具链] 验证通过: %1").arg(unpackedZigExe));

    const QString targetDir = zigRootForRoot(m_root);
    const QString targetParent = QFileInfo(targetDir).absolutePath();
    appendLog(QString("[工具链] 目标目录: %1").arg(targetDir));
    if (!QDir().mkpath(targetParent)) {
        Archive::removeDirectoryRecursively(staging);
        m_error = QString("无法创建目录: %1").arg(targetParent);
        appendLog(QString("[工具链] 错误: %1").arg(m_error));
        return false;
    }
    Archive::removeDirectoryRecursively(targetDir);

    // staging 建在安装根目录下,所以这里通常是同一个文件系统,rename 是原子的
    appendLog(QStringLiteral("[工具链] 开始移动文件…"));
    if (QDir().rename(toolchainSource, targetDir)) {
        Archive::removeDirectoryRecursively(staging);
        appendLog(QStringLiteral("[工具链] 移动完成(原子重命名)"));
    } else {
        appendLog(QStringLiteral("[工具链] 跨设备移动,改用复制方式"));
        if (!Archive::copyDirectoryRecursively(toolchainSource, targetDir, &error)) {
            Archive::removeDirectoryRecursively(staging);
            m_error = QString("移动工具链失败:%1").arg(error);
            appendLog(QString("[工具链] 复制失败: %1").arg(error));
            return false;
        }
        Archive::removeDirectoryRecursively(staging);
        appendLog(QStringLiteral("[工具链] 复制完成"));
    }

    const QString zigExe = zigExecutable(m_root);
    if (!QFileInfo::exists(zigExe)) {
        m_error = QString("安装后未找到 zig 主程序: %1").arg(zigExe);
        appendLog(QString("[工具链] 错误: %1").arg(m_error));
        return false;
    }
    appendLog(QString("[工具链] zig 主程序就绪: %1").arg(zigExe));

#ifdef Q_OS_UNIX
    // 部分解压程序不保留可执行位,这里补回来
    appendLog(QStringLiteral("[工具链] 设置可执行权限…"));
    QFile::setPermissions(zigExe,
                          QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner |
                              QFile::ReadGroup | QFile::ExeGroup |
                              QFile::ReadOther | QFile::ExeOther);
#endif

    // 写出把 zig 当编译器用的包装脚本(CMake / Makefile / 自写脚本都会用到)
    appendLog(QStringLiteral("[工具链] 生成编译器包装脚本…"));
    const auto writeWrapper = [this](const QString &path, const QString &content) -> bool {
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            m_error = QString("无法写入编译器包装脚本: %1").arg(path);
            appendLog(QString("[工具链] 错误: %1").arg(m_error));
            return false;
        }
        file.write(content.toUtf8());
        file.close();
#ifdef Q_OS_UNIX
        QFile::setPermissions(path,
                              QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner |
                                  QFile::ReadGroup | QFile::ExeGroup |
                                  QFile::ReadOther | QFile::ExeOther);
#endif
        appendLog(QString("[工具链] 生成包装脚本: %1").arg(path));
        return true;
    };

#if defined(Q_OS_WIN)
    if (!writeWrapper(zigCompilerWrapper(m_root, false),
                      QStringLiteral("@echo off\r\n\"%~dp0zig.exe\" cc %*\r\n")) ||
        !writeWrapper(zigCompilerWrapper(m_root, true),
                      QStringLiteral("@echo off\r\n\"%~dp0zig.exe\" c++ %*\r\n"))) {
        return false;
    }
#else
    // readlink -f 解析自身真实路径,这样通过符号链接调用也能正常工作
    const QString shTemplate =
        QStringLiteral("#!/bin/sh\n"
                       "# 由 AntiHackerX 生成:把 zig 当作编译器使用\n"
                       "exec \"$(dirname \"$(readlink -f \"$0\")\")/zig\" %1 \"$@\"\n");
    if (!writeWrapper(zigCompilerWrapper(m_root, false),
                      shTemplate.arg(QStringLiteral("cc"))) ||
        !writeWrapper(zigCompilerWrapper(m_root, true),
                      shTemplate.arg(QStringLiteral("c++")))) {
        return false;
    }
#endif

    // Windows 交叉编译所需的 jni_md.h(本机 JDK 不带这一份)
    appendLog(QStringLiteral("[工具链] 生成 Windows 交叉编译头文件…"));
    const QString jniMdPath = zigWin32JniMdPath(m_root);
    if (!QDir().mkpath(QFileInfo(jniMdPath).absolutePath())) {
        m_error = QString("无法创建目录: %1").arg(QFileInfo(jniMdPath).absolutePath());
        appendLog(QString("[工具链] 错误: %1").arg(m_error));
        return false;
    }
    QFile jniMdFile(jniMdPath);
    if (!jniMdFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        m_error = QString("无法写入 %1").arg(jniMdPath);
        appendLog(QString("[工具链] 错误: %1").arg(m_error));
        return false;
    }
    jniMdFile.write(kWin32JniMdContent);
    jniMdFile.close();
    appendLog(QString("[工具链] 生成头文件: %1").arg(jniMdPath));

    m_cppCompiler = zigCompilerWrapper(m_root, false);
    m_cppCompilerXX = zigCompilerWrapper(m_root, true);
    appendLog(QString("[工具链] zig %1 已安装到: %2").arg(version, targetDir));
    appendLog(QString("[工具链] CC  = %1").arg(m_cppCompiler));
    appendLog(QString("[工具链] CXX = %1").arg(m_cppCompilerXX));
    appendLog(QStringLiteral("[工具链] ──────── 安装完成 ────────"));
    return true;
}

bool RuntimeBootstrap::ensureNativeObfuscator() {
    m_jarPath = jarPathForRoot(m_root);
    const bool haveJar = QFileInfo(m_jarPath).size() > 0;

    appendLog(QString("[依赖] 查询最新版 native-obfuscator:%1").arg(kReleaseApiUrl));

    // 查 GitHub API 拿到最新 Release 的标签与压缩包地址。
    // 本地已有依赖时这次查询只为比对版本,超时收紧到 10 秒 —— 离线启动
    // 不该为了更新检查干等半分钟。
    QString tag;
    QString zipUrl;
    QString apiError;
    QByteArray body;
    if (blockingHttpGet(QUrl(QString::fromLatin1(kReleaseApiUrl)),
                        QByteArrayLiteral("application/vnd.github+json"),
                        &body,
                        &apiError,
                        haveJar ? 10000 : 30000)) {
        const QJsonObject root = QJsonDocument::fromJson(body).object();
        tag = root.value(QStringLiteral("tag_name")).toString();
        const QJsonArray assets = root.value(QStringLiteral("assets")).toArray();
        for (const QJsonValue &value : assets) {
            const QJsonObject asset = value.toObject();
            const QString name = asset.value(QStringLiteral("name")).toString();
            // 只要分发包,排除 -src.zip 源码包
            if (name.endsWith(QStringLiteral(".zip"), Qt::CaseInsensitive) &&
                !name.contains(QStringLiteral("-src"))) {
                zipUrl = asset.value(QStringLiteral("browser_download_url")).toString();
                break;
            }
        }
        if (zipUrl.isEmpty() && !tag.isEmpty()) {
            zipUrl = releaseZipUrlForTag(tag);
        }
    }

    if (haveJar) {
        // 本地已有依赖。这里的关键是"宁可不动":只有确认远端版本确实不同
        // 才重下,否则断网时会把好好的依赖降级成 kFallbackReleaseTag 那个旧版。
        if (tag.isEmpty()) {
            appendLog(QString("[依赖] 已存在但查不到远端版本(%1),保留现状: %2")
                          .arg(apiError, m_jarPath));
            return true;
        }

        const QString localVersion = probeNativeObfuscatorVersion(m_javaExe, m_jarPath);
        const QString remoteVersion = versionFromTag(tag);
        if (localVersion.isEmpty()) {
            // 读不出本地版本(jar 损坏,或用户手动塞了别的产物)——同样保留,
            // 免得每次启动都重下一遍
            appendLog(QString("[依赖] 已存在但读不出本地版本,保留现状: %1").arg(m_jarPath));
            return true;
        }
        if (localVersion == remoteVersion) {
            appendLog(QString("[依赖] 已是最新(%1),跳过下载: %2").arg(localVersion, m_jarPath));
            return true;
        }
        appendLog(QString("[依赖] 本地 %1 → 最新 %2,开始更新")
                      .arg(localVersion, remoteVersion.isEmpty() ? tag : remoteVersion));
    } else if (zipUrl.isEmpty()) {
        appendLog(QString("[依赖] 查询 Release 失败(%1),回退到固定版本 %2")
                      .arg(apiError, QString::fromLatin1(kFallbackReleaseTag)));
        zipUrl = releaseZipUrlForTag(QString::fromLatin1(kFallbackReleaseTag));
    } else if (!tag.isEmpty()) {
        appendLog(QString("[依赖] 最新版本: %1").arg(tag));
    }

    if (zipUrl.isEmpty()) {
        m_error = QString("无法确定 native-obfuscator 下载地址:%1").arg(apiError);
        warn(QStringLiteral("获取依赖失败"),
             QString("无法确定 native-obfuscator 的下载地址。\n\n%1\n\n请检查网络后重试。").arg(apiError));
        return false;
    }

    QString error;
    if (!downloadNativeObfuscatorArchive(zipUrl, &error)) {
        m_error = error;
        warn(QStringLiteral("下载依赖失败"),
             QString("%1\n\n您可以手动下载后把 native-obfuscator.jar 放到:\n%2")
                 .arg(error, QDir::toNativeSeparators(jarPathForRoot(m_root))));
        return false;
    }

    // 把装上的是哪个版本写进日志 —— 用户排查"我到底有没有拿到新版"时
    // 第一眼就该看到它
    const QString installedVersion = probeNativeObfuscatorVersion(m_javaExe, m_jarPath);
    appendLog(installedVersion.isEmpty()
                  ? QString("[依赖] 就绪: %1").arg(m_jarPath)
                  : QString("[依赖] 就绪: %1 (版本 %2)").arg(m_jarPath, installedVersion));
    return true;
}

bool RuntimeBootstrap::downloadNativeObfuscatorArchive(const QString &zipUrl, QString *errorOut) {
    const QString staging = stagingRoot();
    Archive::removeDirectoryRecursively(staging);
    if (!QDir().mkpath(staging)) {
        *errorOut = QString("无法创建临时目录: %1").arg(staging);
        return false;
    }

    const QString archivePath = QDir(staging).filePath(QStringLiteral("native-obfuscator.zip"));
    if (!runDownloadWithDialog(QStringLiteral("下载运行依赖"),
                               QStringLiteral("正在获取 native-obfuscator 运行依赖(最新版)。"),
                               QStringLiteral("正在下载依赖:"),
                               QUrl(zipUrl),
                               archivePath,
                               errorOut)) {
        Archive::removeDirectoryRecursively(staging);
        return false;
    }

    const QString unpackDir = QDir(staging).filePath(QStringLiteral("unpack"));
    appendLog(QStringLiteral("[依赖] 下载完成,正在解压…"));
    if (!Archive::extract(archivePath, unpackDir, errorOut)) {
        Archive::removeDirectoryRecursively(staging);
        *errorOut = QString("解压依赖压缩包失败:%1").arg(*errorOut);
        return false;
    }

    // 压缩包根目录直接就是产物(native-obfuscator.jar + 许可证/文档)
    QString sourceDir = unpackDir;
    const QStringList tops = Archive::topLevelEntries(unpackDir);
    if (tops.size() == 1 && QFileInfo(QDir(unpackDir).filePath(tops.first())).isDir()) {
        sourceDir = QDir(unpackDir).filePath(tops.first());
    }

    const QString libsDir = QDir(m_root).filePath(QStringLiteral("libs"));

    // 先试改名(staging 就在安装根目录下,同盘时是瞬时完成的原子操作);
    // 目标已存在或跨设备时退回复制。
    bool installed = false;
    if (!QFileInfo::exists(libsDir)) {
        installed = QDir().rename(sourceDir, libsDir);
    }
    if (!installed) {
        if (!Archive::copyDirectoryRecursively(sourceDir, libsDir, errorOut)) {
            Archive::removeDirectoryRecursively(staging);
            *errorOut = QString("安装依赖文件失败:%1").arg(*errorOut);
            return false;
        }
    }

    Archive::removeDirectoryRecursively(staging);

    const QString jar = jarPathForRoot(m_root);
    if (QFileInfo(jar).size() <= 0) {
        *errorOut = QString("下载的依赖包里没有找到 native-obfuscator.jar(%1)").arg(jar);
        return false;
    }
    return true;
}

bool RuntimeBootstrap::refreshNativeObfuscator() {
    ensureRoot();
    m_error.clear();
    // 强制重下:先删掉旧的
    const QString jar = jarPathForRoot(m_root);
    if (QFileInfo::exists(jar)) {
        QFile::remove(jar);
    }
    return ensureNativeObfuscator();
}

bool RuntimeBootstrap::runDownloadWithDialog(const QString &title,
                                             const QString &intro,
                                             const QString &stage,
                                             const QUrl &url,
                                             const QString &destFile,
                                             QString *errorOut) {
    DownloadDialog dialog(title, intro, m_parent);
    dialog.setStage(stage);

    Downloader downloader;
    // 走代理时 190 MB 很容易中途断掉,多给几次机会;
    // 配上限 3 次断点续传,不用从零重下。
    downloader.setMaxRetries(5);
    downloader.setStallTimeoutMs(45000);

    QEventLoop loop;

    bool succeeded = false;
    QString failure;

    connect(&downloader, &Downloader::progress, &dialog,
            [&dialog](qint64 received, qint64 total, double kbps, int eta) {
                dialog.setProgress(received, total);
                if (total <= 0) {
                    dialog.setDetail(QString("已接收 %1 · %2")
                                         .arg(DownloadUtil::humanBytes(received),
                                              DownloadUtil::humanSpeed(kbps)));
                } else {
                    dialog.setDetail(QString("%1 / 秒 · 剩余 %2")
                                         .arg(DownloadUtil::humanSpeed(kbps),
                                              DownloadUtil::humanEta(eta)));
                }
            });
    connect(&downloader, &Downloader::retrying, &dialog,
            [&dialog](int attemptNo, int maxAttempts, const QString &reason) {
                dialog.setBusy(QString("%1\n连接不稳定,正在断点续传重试(%2/%3)…")
                                   .arg(reason)
                                   .arg(attemptNo)
                                   .arg(maxAttempts));
            });
    connect(&downloader, &Downloader::finished, &dialog,
            [&dialog](const QString &) { dialog.setProgress(1, 1); });
    connect(&downloader, &Downloader::failed, &dialog, [&dialog](const QString &message) {
        dialog.setDetail(message);
    });
    connect(&downloader, &Downloader::finished, &loop, [&loop, &succeeded](const QString &) {
        succeeded = true;
        loop.quit();
    });
    connect(&downloader, &Downloader::failed, &loop, [&loop, &failure](const QString &message) {
        failure = message;
        loop.quit();
    });
    connect(&dialog, &DownloadDialog::cancelRequested, &downloader, &Downloader::abort);

    downloader.start(url, destFile);
    dialog.show();
    loop.exec();

    if (succeeded) {
        dialog.settle(true, QString("下载完成 → %1").arg(QDir::toNativeSeparators(destFile)));
        // 让用户看到"完成"状态,但不要卡住用户:200 ms 后自动关
        QTimer::singleShot(200, &dialog, &QDialog::accept);
        dialog.exec();
        return true;
    }

    dialog.settle(false, failure.isEmpty() ? QStringLiteral("下载失败") : failure);
    dialog.exec();
    if (errorOut) {
        *errorOut = failure.isEmpty() ? QStringLiteral("下载失败") : failure;
    }
    return false;
}
