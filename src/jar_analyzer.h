#ifndef JAR_ANALYZER_H
#define JAR_ANALYZER_H

#include <QString>
#include <QStringList>

/**
 * JAR 文件类型
 */
enum class JarType {
    Unknown,              // 未知类型
    PlainJar,            // 普通 JAR(主类写在 MANIFEST.MF)
    MinecraftPaperPlugin,  // Minecraft Paper 插件(plugin.yml)
    MinecraftFabricMod,    // Minecraft Fabric MOD(fabric.mod.json)
    MinecraftForgeMod,     // Minecraft Forge MOD(mods.toml 或 mcmod.info)
    SpringBoot            // Spring Boot 应用(BOOT-INF/)
};

/**
 * JAR 文件分析结果
 */
struct JarAnalysisResult {
    JarType type;
    QString typeName;         // 类型名称(中文)
    QString mainClass;        // 主类(如果有)
    QString pluginName;       // 插件名(如果有)
    QString version;          // 版本号(如果有)
    QStringList details;      // 详细信息
    bool isValid;             // 是否是有效的 JAR
    QString errorMessage;     // 错误信息(如果有)
};

/**
 * JAR 文件分析器
 * 
 * 支持识别:
 * - 普通 JAR(通过 MANIFEST.MF)
 * - Minecraft Paper 插件(plugin.yml)
 * - Minecraft Fabric MOD(fabric.mod.json)
 * - Minecraft Forge MOD(mods.toml 或 mcmod.info)
 * - Spring Boot 应用(BOOT-INF/ 目录)
 */
class JarAnalyzer {
public:
    /**
     * 分析 JAR 文件
     * @param jarPath JAR 文件路径
     * @return 分析结果
     */
    static JarAnalysisResult analyze(const QString &jarPath);

private:
    // 读取 ZIP 内文件内容
    static QString readFileFromZip(const QString &jarPath, const QString &filePath);
    
    // 检查 ZIP 内是否存在文件
    static bool hasFileInZip(const QString &jarPath, const QString &filePath);
    
    // 检查 ZIP 内是否存在目录
    static bool hasDirInZip(const QString &jarPath, const QString &dirPath);
    
    // 解析 MANIFEST.MF
    static QString parseManifestMainClass(const QString &manifestContent);
    
    // 解析 plugin.yml
    static void parsePaperPluginYml(const QString &content, JarAnalysisResult &result);
    
    // 解析 fabric.mod.json
    static void parseFabricModJson(const QString &content, JarAnalysisResult &result);
    
    // 解析 mods.toml (Forge 1.13+)
    static void parseForgeModsToml(const QString &content, JarAnalysisResult &result);
    
    // 解析 mcmod.info (Forge 1.12-)
    static void parseForgeMcmodInfo(const QString &content, JarAnalysisResult &result);
};

#endif // JAR_ANALYZER_H
