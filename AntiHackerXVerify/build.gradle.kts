plugins {
    id("java")
}

group = "org.example"
version = "1.0-SNAPSHOT"

repositories {
    mavenCentral()
}

dependencies {
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