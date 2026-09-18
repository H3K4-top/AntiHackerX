plugins {
    id("java")
}

group = "org.example"
version = "1.0-SNAPSHOT"

repositories {
    mavenCentral()
    // Mixin 的上游(org.spongepowered:mixin)只发在 Sponge 自己的仓库里,Central 上没有。
    // 这里先用 Central 上就有的 Fabric fork(net.fabricmc:sponge-mixin):它与上游
    // 共用 org.spongepowered.asm.mixin.* 命名空间与注解,源码层面通用。
    // 真要出 Forge 版时,打开下面这行、并把依赖换成 org.spongepowered:mixin:0.8.5。
    // maven("https://repo.spongepowered.org/repository/maven-public/")
}

dependencies {
    // ── Mixin:为后续 Fabric / Forge 支持预留 ────────────────────────────────
    // 为什么是 compileOnly:两个平台都**自带** Mixin 运行时(Fabric Loader 与 Forge
    // 都内置),产物里再打包一份必然在启动时撞类。
    //
    // 两点必须记住,否则真开始写 mixin 类时会踩:
    //   1) 本模块最终是**打包时由 javac 现场编译**的(见 packer_pipeline 的
    //      stepCompileModule),它的 -cp = 目标 JAR + 界面上填的「额外依赖」。
    //      所以只在这里声明是不够的 —— 打包机上要**同时**把 mixin jar 放进那个
    //      目录,否则 javac 会以「找不到符号 org.spongepowered.asm.mixin.Mixin」
    //      失败。界面上那一栏支持直接给目录(会递归找 *.jar)。
    //   2) Fabric 生产环境的 @Inject 目标要靠 refmap 重映射,而我们没有 fabric-loom
    //      去生成 refmap。所以后续注入的 mixin 要写成 targets = "..."(字符串目标),
    //      让 Mixin 运行期自己解析,别用类型化 target。
    compileOnly("net.fabricmc:sponge-mixin:0.15.4+mixin.0.8.7")

    testImplementation(platform("org.junit:junit-bom:5.10.0"))
    testImplementation("org.junit.jupiter:junit-jupiter")
    testRuntimeOnly("org.junit.platform:junit-platform-launcher")
}

tasks.test {
    useJUnitPlatform()
}

// 目标固定为 Java 8（字节码版本 52）。
// 用 options.release 而不是单独的 source/target：它会对照 Java 8 的 API 签名做
// 检查，误用 Java 9+ 的 API 会直接编译报错，而不是拖到运行期才炸。
// （注意：--release 需要 JDK 9+ 才能构建，但产物仍是 Java 8 字节码。）
java {
    sourceCompatibility = JavaVersion.VERSION_1_8
    targetCompatibility = JavaVersion.VERSION_1_8
}

tasks.withType<JavaCompile>().configureEach {
    // 源码里有中文和 © 等非 ASCII 字符。不显式指定时 Gradle 用平台编码
    // （Windows 上是 GBK），会让对话框文字和版权信息变成乱码。
    options.encoding = "UTF-8"
    options.release.set(8)
}

// 打出的 JAR 带上 Main-Class，方便用 `java -jar` 直接运行。
// native-obfuscator 会把输入 JAR 的 MANIFEST 原样复制到输出 JAR，所以这里设置的
// 入口在加壳后的产物里依然有效。
tasks.jar {
    manifest {
        attributes["Main-Class"] = "top.h3k4.unpassMain"
    }
}