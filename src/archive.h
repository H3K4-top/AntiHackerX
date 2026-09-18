#ifndef ARCHIVE_H
#define ARCHIVE_H

#include <QString>
#include <QStringList>

/**
 * 归档解压工具。
 *
 * 为什么不用 Qt 自带的解压?
 *   Qt6 的 QZipReader 位于私有头文件(qzipreader_p.h),官方发行版不带
 *   Qt6GuiPrivate 的 CMake 配置,直接引用会在用户机器上编译失败。
 *
 * 这里改成"调用系统自带解压程序 + 事后校验"的方式,覆盖三大平台:
 *   tar -xf       (macOS 与 Windows 10 1803+ 自带 bsdtar,zip / tar.gz 都能解)
 *   unzip -o -q   (Linux / macOS / 装了 unzip 的 Windows)
 *   7z x -y -o    (装了 7-Zip 的机器;优先级最低,见 .cpp 里的说明)
 *   PowerShell Expand-Archive (Windows 兜底)
 *
 * 每个候选都跑一遍,只要"解压后目标目录里的文件数变多了"就算成功,否则换下一个。
 * 全部失败时返回 false,并把各候选的失败原因拼进 errorOut,便于用户排查。
 */
namespace Archive {

/// 把 archivePath 解压到 destDir(会自动创建)。成功返回 true。
bool extract(const QString &archivePath, const QString &destDir, QString *errorOut);

/// 列出 dir 下的顶层项名(不含 . 与 ..);目录名不带结尾斜杠
QStringList topLevelEntries(const QString &dir);

/// 递归统计 dir 下的文件数量,用于校验解压是否真的产出了内容
int countFilesRecursively(const QString &dir);

/// 递归复制目录,dir 本身会被复制成 destPath
bool copyDirectoryRecursively(const QString &sourceDir, const QString &destDir, QString *errorOut);

/// 递归删除目录;不存在时也返回 true
bool removeDirectoryRecursively(const QString &dir);

} // namespace Archive

#endif // ARCHIVE_H
