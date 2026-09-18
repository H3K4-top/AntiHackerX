#include "archive.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QStringList>

namespace {

struct Extractor {
    QString program;
    QStringList arguments;
    QString label;
};

/// 拼接可直接执行的命令行文本,用于日志/报错
QString commandLineOf(const Extractor &extractor) {
    QStringList parts;
    parts << extractor.program;
    for (const QString &arg : extractor.arguments) {
        if (arg.contains(' ')) {
            parts << ('"' + arg + '"');
        } else {
            parts << arg;
        }
    }
    return parts.join(' ');
}

/// 按平台生成候选解压命令
QList<Extractor> buildCandidates(const QString &archivePath, const QString &destDir) {
    QList<Extractor> candidates;

    // 1) tar:Windows 10 1803+ 与 macOS 自带的是 bsdtar,zip / tar.gz 通吃;
    //    Linux 上的 GNU tar 读不了 zip,但它会以非 0 退出且不产出文件,
    //    会被后面的结果校验拦下,自动换下一个候选。
    if (!QStandardPaths::findExecutable("tar").isEmpty()) {
        candidates.append({"tar", {"-xf", archivePath, "-C", destDir}, "tar"});
    }

    // 2) unzip:Linux / macOS 的标准工具
    if (!QStandardPaths::findExecutable("unzip").isEmpty()) {
        candidates.append({"unzip", {"-o", "-q", archivePath, "-d", destDir}, "unzip"});
    }

    // 3) 7-Zip:放在 tar/unzip 之后。它解 .tar.gz 时只剥一层壳,
    //    会让"解压后目录非空"的校验误判成功,所以优先级压低。
    if (!QStandardPaths::findExecutable("7z").isEmpty()) {
        candidates.append({"7z", {"x", "-y", "-bso0", "-bsp0", "-o" + destDir, archivePath}, "7z"});
    }
    if (!QStandardPaths::findExecutable("7za").isEmpty()) {
        candidates.append({"7za", {"x", "-y", "-bso0", "-bsp0", "-o" + destDir, archivePath}, "7za"});
    }

#if defined(Q_OS_WIN)
    // 4) PowerShell 兜底(慢,但 Win7/8 之外的机器一定有)
    const QString psCommand =
        QString("$ErrorActionPreference='Stop'; Expand-Archive -LiteralPath '%1' -DestinationPath '%2' -Force")
            .arg(QString(archivePath).replace("'", "''"), QString(destDir).replace("'", "''"));
    if (!QStandardPaths::findExecutable("powershell").isEmpty()) {
        candidates.append({"powershell",
                           {"-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
                            "-Command", psCommand},
                           "powershell"});
    }
#endif

    return candidates;
}

} // namespace

namespace Archive {

bool extract(const QString &archivePath, const QString &destDir, QString *errorOut) {
    const QFileInfo archiveInfo(archivePath);
    if (!archiveInfo.exists() || archiveInfo.size() <= 0) {
        if (errorOut) {
            *errorOut = QString("归档文件不存在或为空: %1").arg(archivePath);
        }
        return false;
    }

    QDir dest(destDir);
    if (!dest.exists() && !dest.mkpath(".")) {
        if (errorOut) {
            *errorOut = QString("无法创建解压目标目录: %1").arg(destDir);
        }
        return false;
    }

    const QList<Extractor> candidates = buildCandidates(archivePath, destDir);
    if (candidates.isEmpty()) {
        if (errorOut) {
            *errorOut = QStringLiteral("系统里找不到任何可用的解压程序(unzip / 7z / tar)");
        }
        return false;
    }

    QStringList failureLog;

    for (const Extractor &candidate : candidates) {
        const int filesBefore = countFilesRecursively(destDir);

        QProcess process;
        process.setWorkingDirectory(QFileInfo(archivePath).absolutePath());
        process.setProcessChannelMode(QProcess::MergedChannels);
        process.start(candidate.program, candidate.arguments);

        const bool started = process.waitForStarted(10000);
        if (!started) {
            failureLog << QString("[%1] 无法启动").arg(candidate.label);
            continue;
        }

        const bool done = process.waitForFinished(30 * 60 * 1000); // 大包(约 200 MB)给足时间
        const QString output = QString::fromUtf8(process.readAll()).trimmed();

        if (!done) {
            process.kill();
            process.waitForFinished(5000);
            failureLog << QString("[%1] 超时未结束").arg(candidate.label);
            continue;
        }

        const int exitCode = process.exitCode();
        const int filesAfter = countFilesRecursively(destDir);

        // 关键校验:不看退出码,只看是否真的产出了文件。
        // bsdtar 解 zip、或者解压时夹带警告,退出码都不一定是 0。
        if (filesAfter > filesBefore) {
            return true;
        }

        failureLog << QString("[%1] 退出码 %2(命令: %3)\n     输出: %4")
                          .arg(candidate.label)
                          .arg(exitCode)
                          .arg(commandLineOf(candidate), 
                               output.isEmpty() ? QStringLiteral("(无)") : output.left(300));
    }

    if (errorOut) {
        *errorOut = QString("所有解压方式均失败:\n") + failureLog.join('\n');
    }
    return false;
}

QStringList topLevelEntries(const QString &dir) {
    QDir d(dir);
    QStringList names;
    const QFileInfoList entries =
        d.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo &info : entries) {
        names << info.fileName();
    }
    return names;
}

int countFilesRecursively(const QString &dir) {
    int count = 0;
    QDirIterator it(dir, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        ++count;
    }
    return count;
}

bool copyDirectoryRecursively(const QString &sourceDir, const QString &destDir, QString *errorOut) {
    QDir src(sourceDir);
    if (!src.exists()) {
        if (errorOut) {
            *errorOut = QString("源目录不存在: %1").arg(sourceDir);
        }
        return false;
    }

    QDir dst(destDir);
    if (!dst.exists() && !dst.mkpath(".")) {
        if (errorOut) {
            *errorOut = QString("无法创建目标目录: %1").arg(destDir);
        }
        return false;
    }

    const QFileInfoList entries =
        src.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo &info : entries) {
        const QString target = dst.filePath(info.fileName());
        if (info.isDir()) {
            if (!copyDirectoryRecursively(info.absoluteFilePath(), target, errorOut)) {
                return false;
            }
        } else {
            // 目标已存在时先删掉,否则 QFile::copy 会失败
            if (QFile::exists(target) && !QFile::remove(target)) {
                if (errorOut) {
                    *errorOut = QString("无法覆盖已存在的文件: %1").arg(target);
                }
                return false;
            }
            if (!QFile::copy(info.absoluteFilePath(), target)) {
                if (errorOut) {
                    *errorOut = QString("复制失败: %1 -> %2").arg(info.absoluteFilePath(), target);
                }
                return false;
            }
        }
    }
    return true;
}

bool removeDirectoryRecursively(const QString &dir) {
    QDir d(dir);
    if (!d.exists()) {
        return true;
    }
    return d.removeRecursively();
}

} // namespace Archive
