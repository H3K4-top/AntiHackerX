#include "jar_analyzer.h"
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QRegularExpression>
#include <QDebug>

// 使用内嵌的 minizip 源码
extern "C" {
#include <unzip.h>
}

#include <cstring>

JarAnalysisResult JarAnalyzer::analyze(const QString &jarPath) {
    JarAnalysisResult result;
    result.type = JarType::Unknown;
    result.typeName = QStringLiteral("未知");
    result.isValid = false;

    // 检查文件是否存在
    QFileInfo fileInfo(jarPath);
    if (!fileInfo.exists()) {
        result.errorMessage = QStringLiteral("文件不存在");
        return result;
    }

    if (!fileInfo.isFile()) {
        result.errorMessage = QStringLiteral("不是文件");
        return result;
    }

    if (fileInfo.suffix().toLower() != QStringLiteral("jar")) {
        result.errorMessage = QStringLiteral("不是 JAR 文件");
        return result;
    }

    result.isValid = true;

    // 1. 检查是否是 Spring Boot 应用(优先级最高)
    if (hasDirInZip(jarPath, QStringLiteral("BOOT-INF/"))) {
        result.type = JarType::SpringBoot;
        result.typeName = QStringLiteral("Spring Boot 应用");
        
        QString manifest = readFileFromZip(jarPath, QStringLiteral("META-INF/MANIFEST.MF"));
        if (!manifest.isEmpty()) {
            result.mainClass = parseManifestMainClass(manifest);
            if (!result.mainClass.isEmpty()) {
                result.details << QString("启动类: %1").arg(result.mainClass);
            }
        }
        
        result.details << QStringLiteral("包含 BOOT-INF/ 目录");
        result.details << QStringLiteral("这是一个可执行的 Spring Boot Fat JAR");
        return result;
    }

    // 2. 检查是否是 Minecraft Paper 插件
    QString pluginYml = readFileFromZip(jarPath, QStringLiteral("plugin.yml"));
    if (!pluginYml.isEmpty()) {
        result.type = JarType::MinecraftPaperPlugin;
        result.typeName = QStringLiteral("Minecraft Paper 插件");
        parsePaperPluginYml(pluginYml, result);
        return result;
    }

    // 3. 检查是否是 Minecraft Fabric MOD
    QString fabricModJson = readFileFromZip(jarPath, QStringLiteral("fabric.mod.json"));
    if (!fabricModJson.isEmpty()) {
        result.type = JarType::MinecraftFabricMod;
        result.typeName = QStringLiteral("Minecraft Fabric MOD");
        parseFabricModJson(fabricModJson, result);
        return result;
    }

    // 4. 检查是否是 Minecraft Forge MOD
    // 4.1 Forge 1.13+ (mods.toml)
    QString modsToml = readFileFromZip(jarPath, QStringLiteral("META-INF/mods.toml"));
    if (!modsToml.isEmpty()) {
        result.type = JarType::MinecraftForgeMod;
        result.typeName = QStringLiteral("Minecraft Forge MOD (1.13+)");
        parseForgeModsToml(modsToml, result);
        return result;
    }

    // 4.2 Forge 1.12- (mcmod.info)
    QString mcmodInfo = readFileFromZip(jarPath, QStringLiteral("mcmod.info"));
    if (!mcmodInfo.isEmpty()) {
        result.type = JarType::MinecraftForgeMod;
        result.typeName = QStringLiteral("Minecraft Forge MOD (1.12-)");
        parseForgeMcmodInfo(mcmodInfo, result);
        return result;
    }

    // 5. 默认:普通 JAR
    QString manifest = readFileFromZip(jarPath, QStringLiteral("META-INF/MANIFEST.MF"));
    if (!manifest.isEmpty()) {
        result.mainClass = parseManifestMainClass(manifest);
        if (!result.mainClass.isEmpty()) {
            result.type = JarType::PlainJar;
            result.typeName = QStringLiteral("普通 JAR");
            result.details << QString("主类: %1").arg(result.mainClass);
            result.details << QStringLiteral("可执行的标准 JAR 文件");
        } else {
            result.type = JarType::Unknown;
            result.typeName = QStringLiteral("未知 JAR");
            result.details << QStringLiteral("MANIFEST.MF 中未找到 Main-Class");
            result.details << QStringLiteral("可能是库 JAR 或不完整的文件");
        }
    } else {
        result.type = JarType::Unknown;
        result.typeName = QStringLiteral("未知 JAR");
        result.details << QStringLiteral("未找到 META-INF/MANIFEST.MF");
        result.details << QStringLiteral("可能是不完整的 JAR 文件");
    }

    return result;
}

QString JarAnalyzer::readFileFromZip(const QString &jarPath, const QString &filePath) {
    unzFile zipFile = unzOpen(jarPath.toUtf8().constData());
    if (!zipFile) {
        return QString();
    }

    // 查找文件
    if (unzLocateFile(zipFile, filePath.toUtf8().constData(), 1) != UNZ_OK) {
        unzClose(zipFile);
        return QString();
    }

    // 打开当前文件
    if (unzOpenCurrentFile(zipFile) != UNZ_OK) {
        unzClose(zipFile);
        return QString();
    }

    // 获取文件信息
    unz_file_info fileInfo;
    if (unzGetCurrentFileInfo(zipFile, &fileInfo, nullptr, 0, nullptr, 0, nullptr, 0) != UNZ_OK) {
        unzCloseCurrentFile(zipFile);
        unzClose(zipFile);
        return QString();
    }

    // 读取文件内容
    QByteArray data;
    data.resize(fileInfo.uncompressed_size);
    int bytesRead = unzReadCurrentFile(zipFile, data.data(), fileInfo.uncompressed_size);
    
    unzCloseCurrentFile(zipFile);
    unzClose(zipFile);

    if (bytesRead < 0) {
        return QString();
    }

    return QString::fromUtf8(data);
}

bool JarAnalyzer::hasFileInZip(const QString &jarPath, const QString &filePath) {
    unzFile zipFile = unzOpen(jarPath.toUtf8().constData());
    if (!zipFile) {
        return false;
    }

    bool exists = (unzLocateFile(zipFile, filePath.toUtf8().constData(), 1) == UNZ_OK);
    unzClose(zipFile);
    return exists;
}

bool JarAnalyzer::hasDirInZip(const QString &jarPath, const QString &dirPath) {
    unzFile zipFile = unzOpen(jarPath.toUtf8().constData());
    if (!zipFile) {
        return false;
    }

    // 遍历所有文件
    if (unzGoToFirstFile(zipFile) != UNZ_OK) {
        unzClose(zipFile);
        return false;
    }

    char fileName[256];
    do {
        if (unzGetCurrentFileInfo(zipFile, nullptr, fileName, sizeof(fileName), nullptr, 0, nullptr, 0) == UNZ_OK) {
            QString name = QString::fromUtf8(fileName);
            if (name.startsWith(dirPath, Qt::CaseInsensitive)) {
                unzClose(zipFile);
                return true;
            }
        }
    } while (unzGoToNextFile(zipFile) == UNZ_OK);

    unzClose(zipFile);
    return false;
}

QString JarAnalyzer::parseManifestMainClass(const QString &manifestContent) {
    QStringList lines = manifestContent.split('\n');
    for (const QString &line : lines) {
        QString trimmed = line.trimmed();
        if (trimmed.startsWith(QStringLiteral("Main-Class:"), Qt::CaseInsensitive)) {
            QString mainClass = trimmed.mid(11).trimmed();
            return mainClass;
        }
    }
    return QString();
}

void JarAnalyzer::parsePaperPluginYml(const QString &content, JarAnalysisResult &result) {
    // 简单的 YAML 解析(仅提取关键字段)
    QStringList lines = content.split('\n');
    
    for (const QString &line : lines) {
        QString trimmed = line.trimmed();
        
        // name: PluginName
        if (trimmed.startsWith(QStringLiteral("name:"), Qt::CaseInsensitive)) {
            result.pluginName = trimmed.mid(5).trimmed();
            if (result.pluginName.startsWith('"') && result.pluginName.endsWith('"')) {
                result.pluginName = result.pluginName.mid(1, result.pluginName.length() - 2);
            }
        }
        
        // version: 1.0.0
        else if (trimmed.startsWith(QStringLiteral("version:"), Qt::CaseInsensitive)) {
            result.version = trimmed.mid(8).trimmed();
            if (result.version.startsWith('"') && result.version.endsWith('"')) {
                result.version = result.version.mid(1, result.version.length() - 2);
            }
        }
        
        // main: com.example.Main
        else if (trimmed.startsWith(QStringLiteral("main:"), Qt::CaseInsensitive)) {
            result.mainClass = trimmed.mid(5).trimmed();
            if (result.mainClass.startsWith('"') && result.mainClass.endsWith('"')) {
                result.mainClass = result.mainClass.mid(1, result.mainClass.length() - 2);
            }
        }
    }
    
    if (!result.pluginName.isEmpty()) {
        result.details << QString("插件名: %1").arg(result.pluginName);
    }
    if (!result.version.isEmpty()) {
        result.details << QString("版本: %1").arg(result.version);
    }
    if (!result.mainClass.isEmpty()) {
        result.details << QString("主类: %1").arg(result.mainClass);
    }
}

void JarAnalyzer::parseFabricModJson(const QString &content, JarAnalysisResult &result) {
    QJsonDocument doc = QJsonDocument::fromJson(content.toUtf8());
    if (!doc.isObject()) {
        return;
    }
    
    QJsonObject obj = doc.object();
    
    // id
    if (obj.contains("id")) {
        result.pluginName = obj["id"].toString();
    }
    
    // version
    if (obj.contains("version")) {
        result.version = obj["version"].toString();
    }
    
    // entrypoints.main (主入口)
    if (obj.contains("entrypoints")) {
        QJsonObject entrypoints = obj["entrypoints"].toObject();
        if (entrypoints.contains("main")) {
            QJsonArray mainArray = entrypoints["main"].toArray();
            if (!mainArray.isEmpty()) {
                result.mainClass = mainArray[0].toString();
            }
        }
    }
    
    if (!result.pluginName.isEmpty()) {
        result.details << QString("MOD ID: %1").arg(result.pluginName);
    }
    if (!result.version.isEmpty()) {
        result.details << QString("版本: %1").arg(result.version);
    }
    if (!result.mainClass.isEmpty()) {
        result.details << QString("入口点: %1").arg(result.mainClass);
    }
}

void JarAnalyzer::parseForgeModsToml(const QString &content, JarAnalysisResult &result) {
    // 简单的 TOML 解析(仅提取关键字段)
    QStringList lines = content.split('\n');
    
    bool inModsSection = false;
    for (const QString &line : lines) {
        QString trimmed = line.trimmed();
        
        // [[mods]]
        if (trimmed == QStringLiteral("[[mods]]")) {
            inModsSection = true;
            continue;
        }
        
        if (!inModsSection) {
            continue;
        }
        
        // modId="example"
        if (trimmed.startsWith(QStringLiteral("modId"), Qt::CaseInsensitive)) {
            QRegularExpression re(R"(modId\s*=\s*["\']([^"\']+)["\'])");
            QRegularExpressionMatch match = re.match(trimmed);
            if (match.hasMatch()) {
                result.pluginName = match.captured(1);
            }
        }
        
        // version="1.0"
        else if (trimmed.startsWith(QStringLiteral("version"), Qt::CaseInsensitive)) {
            QRegularExpression re(R"(version\s*=\s*["\']([^"\']+)["\'])");
            QRegularExpressionMatch match = re.match(trimmed);
            if (match.hasMatch()) {
                result.version = match.captured(1);
            }
        }
    }
    
    if (!result.pluginName.isEmpty()) {
        result.details << QString("MOD ID: %1").arg(result.pluginName);
    }
    if (!result.version.isEmpty()) {
        result.details << QString("版本: %1").arg(result.version);
    }
}

void JarAnalyzer::parseForgeMcmodInfo(const QString &content, JarAnalysisResult &result) {
    QJsonDocument doc = QJsonDocument::fromJson(content.toUtf8());
    if (!doc.isArray()) {
        return;
    }
    
    QJsonArray arr = doc.array();
    if (arr.isEmpty()) {
        return;
    }
    
    QJsonObject obj = arr[0].toObject();
    
    // modid
    if (obj.contains("modid")) {
        result.pluginName = obj["modid"].toString();
    }
    
    // version
    if (obj.contains("version")) {
        result.version = obj["version"].toString();
    }
    
    if (!result.pluginName.isEmpty()) {
        result.details << QString("MOD ID: %1").arg(result.pluginName);
    }
    if (!result.version.isEmpty()) {
        result.details << QString("版本: %1").arg(result.version);
    }
}
