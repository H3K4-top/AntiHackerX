/*
 * AntiHackerX 示例模块 —— 以 MIT 许可证提供,便于嵌入你自己的程序。
 *
 * SPDX-License-Identifier: MIT
 * 许可证全文见仓库中的 AntiHackerXVerify/LICENSE。
 */
package top.h3k4;

import java.awt.GraphicsEnvironment;
import java.util.Locale;

import javax.swing.JOptionPane;

/**
 * AntiHackerX 加壳版权提示。
 *
 * <p>被 AntiHackerX 加壳保护的程序在启动时展示这段信息:说明本程序由哪个组织的
 * AntiHackerX 加壳,并给出**加壳程序自身**的版本、版权与许可证。</p>
 *
 * <p>注意区分两类版权:这里声明的是<b>加壳程序</b>的版权,不是被加壳程序自身的
 * 版权 —— 后者归它各自的作者所有。</p>
 *
 * <p><b>为什么不在这里声明 native-obfuscator:</b>本模块是被加壳的目标程序,与加壳
 * 工具之间只是「工具处理输入、产出新产物」的关系,不存在代码级依赖。工具自身的
 * GPL-3.0 对输出代码的约束,已由上游的 Output Exception 免除,且工具产出的 runtime
 * 源文件里本身带有许可证声明。所以本程序不需要、也不应在界面上公开这一依赖关系 ——
 * 客户拿到的应该只是一个由 AntiHackerX 加壳的普通程序。</p>
 *
 * <p>有桌面环境时弹出图形对话框;服务器等没有桌面环境时改为把版权信息打印到日志,
 * 不弹窗、不阻塞、不抛异常。</p>
 *
 * <p>强制只打日志(自动化脚本、CI、无头容器):</p>
 * <pre>
 *   java -Dantihackerx.splash=false ...
 *   ANTIHACKERX_NO_SPLASH=1 java ...
 * </pre>
 *
 * <p>白标 / 定制(每个客户不一样时用启动参数覆盖,不必改代码):</p>
 * <pre>
 *   -Dantihackerx.product=产品名    默认 AntiHackerX
 *   -Dantihackerx.org=组织名        默认 H3K4
 *   -Dantihackerx.holder=版权人     默认 H3K4
 *   -Dantihackerx.years=年份区间    默认 2024-2026
 * </pre>
 *
 * <p><b>为什么日志只用 ASCII:</b>stdout 经常被上层程序用管道读取(例如 AntiHackerX
 * 主程序用 QProcess 抓取子进程输出),管道编码无法保证,中文很可能变成乱码。
 * 图形对话框是直接给人看的,用中文没有问题。</p>
 */
public final class CopyrightNotice {

    /** 覆盖加壳程序名字的系统属性 */
    public static final String PROP_PRODUCT = "antihackerx.product";

    /** 覆盖加壳程序所属组织的系统属性 */
    public static final String PROP_ORG = "antihackerx.org";

    /** 覆盖加壳程序版权人的系统属性 */
    public static final String PROP_HOLDER = "antihackerx.holder";

    /** 覆盖版权年份区间的系统属性 */
    public static final String PROP_YEARS = "antihackerx.years";

    /** 关掉图形弹窗、只打日志的系统属性 */
    public static final String OPT_OUT_PROPERTY = "antihackerx.splash";

    /** 关掉图形弹窗、只打日志的环境变量 */
    public static final String OPT_OUT_ENV = "ANTIHACKERX_NO_SPLASH";

    /** 加壳程序(本工具)的名字 */
    public static final String APP_NAME = setting(PROP_PRODUCT, "AntiHackerX");

    /** 加壳程序所属的组织 */
    public static final String APP_ORG = setting(PROP_ORG, "H3K4");

    /*
     * 这里刻意**不放版本号**,也不要再加回来。
     *
     * 被加壳程序的界面是逆向者第一个会看的地方:只要弹出「AntiHackerX 1.0.0」,
     * 对方就能直接去搜对应版本的一键脱壳脚本。不报版本号,对方连该用哪个版本的
     * 工具链都确定不了。
     *
     * ⚠️ 注意:静态最终的字面量即使“没被用到”,javac 也会把它写进 class 文件的
     * ConstantValue 属性 —— `strings` 一跑就能读到。所以想藏版本号必须把
     * **整个常量删掉**,只删引用是没用的。
     */

    /** 加壳程序的版权所有者 */
    public static final String COPYRIGHT_HOLDER = setting(PROP_HOLDER, "H3K4");

    /** 版权年份区间 */
    public static final String COPYRIGHT_YEARS = setting(PROP_YEARS, "2024-2026");

    /** 加壳程序的版权声明(对话框用,© 写成 \\u00a9 以保证源码纯 ASCII 安全) */
    public static final String COPYRIGHT =
            "\u00a9 " + COPYRIGHT_YEARS + " " + COPYRIGHT_HOLDER;

    /** 加壳程序的许可证 */
    public static final String LICENSE_NAME = "GNU GPL v3.0";

    private CopyrightNotice() {
    }

    /**
     * 显示版权信息:有桌面环境就弹窗,否则打印到日志。
     *
     * <p>本方法在任何情况下都不抛异常 —— 弹窗失败只会退化成日志输出,
     * 绝不会让程序启动失败。</p>
     */
    public static void show() {
        if (isOptedOut()) {
            log("splash disabled via -D" + OPT_OUT_PROPERTY + "=false or "
                    + OPT_OUT_ENV + "=1");
            return;
        }

        String headlessReason = detectHeadlessReason();
        if (headlessReason != null) {
            log(headlessReason);
            return;
        }

        try {
            JOptionPane.showMessageDialog(
                    null,
                    dialogText(),
                    APP_NAME,
                    JOptionPane.INFORMATION_MESSAGE);
        } catch (Throwable t) {
            // HeadlessException、X server 连不上、java.desktop 模块缺失
            // (NoClassDefFoundError)…… 一律退化成日志,不向上抛
            log("graphical splash failed (" + t.getClass().getName()
                    + "), falling back to log");
        }
    }

    /** 只打印版权信息,不做任何图形调用。日志内容为纯 ASCII。 */
    public static void log() {
        log(null);
    }

    private static void log(String reason) {
        String prefix = "[" + APP_NAME + "] ";
        System.out.println(prefix + "This program is packed with " + APP_NAME
                + " by " + APP_ORG);
        System.out.println(prefix + "Packer license: " + LICENSE_NAME);
        System.out.println(prefix + "Packer copyright (c) " + COPYRIGHT_YEARS
                + " " + COPYRIGHT_HOLDER);
        if (reason != null && !reason.isEmpty()) {
            System.out.println(prefix + reason);
        }
        System.out.flush();
    }

    private static String dialogText() {
        return "本程序由 " + APP_ORG + " 组织的 " + APP_NAME + " 加壳保护。\n"
                + "\n"
                + "加壳程序版权所有:" + COPYRIGHT + "\n"
                + "加壳程序许可证:" + LICENSE_NAME;
    }

    /**
     * 读系统属性,给白标 / 定制留口子。
     *
     * <p>写成方法调用而不是字面量,顺便避开了 javac 把常量内联进调用方 ——
     * 否则其他类里也会被写死默认值,覆盖就失效了。</p>
     *
     * @param key      系统属性名
     * @param fallback 未设置或为空白时返回的默认值
     */
    private static String setting(String key, String fallback) {
        String value = System.getProperty(key);
        if (value == null) {
            return fallback;
        }
        value = value.trim();
        return value.isEmpty() ? fallback : value;
    }

    private static boolean isOptedOut() {
        if (isTruthy(System.getenv(OPT_OUT_ENV))) {
            return true;
        }
        return isFalsy(System.getProperty(OPT_OUT_PROPERTY));
    }

    private static boolean isTruthy(String value) {
        return value != null && ("1".equals(value.trim())
                || "true".equalsIgnoreCase(value.trim())
                || "yes".equalsIgnoreCase(value.trim()));
    }

    private static boolean isFalsy(String value) {
        return value != null && ("0".equals(value.trim())
                || "false".equalsIgnoreCase(value.trim())
                || "no".equalsIgnoreCase(value.trim()));
    }

    /**
     * 判断当前环境有没有桌面。
     *
     * @return 没有桌面时返回可读的原因;有桌面时返回 {@code null}
     */
    private static String detectHeadlessReason() {
        String os = System.getProperty("os.name", "").toLowerCase(Locale.ROOT);
        boolean unixLike = !os.contains("win") && !os.contains("mac");

        // Linux / BSD 等:没有 DISPLAY 也没有 WAYLAND_DISPLAY,就是纯命令行环境。
        // 先查这个,避免在服务器上白跑一遍 AWT 的初始化。
        if (unixLike && isBlank(System.getenv("DISPLAY"))
                && isBlank(System.getenv("WAYLAND_DISPLAY"))) {
            return "no desktop environment (DISPLAY / WAYLAND_DISPLAY unset), "
                    + "copyright notice printed to log";
        }

        if (Boolean.parseBoolean(System.getProperty("java.awt.headless", "false"))) {
            return "java.awt.headless=true, copyright notice printed to log";
        }

        // 交给 AWT 自己判断:Windows 服务会话、DISPLAY 指向不存在的 X server 等
        // 情况由它兜底
        try {
            if (GraphicsEnvironment.isHeadless()) {
                return "GraphicsEnvironment reports headless, "
                        + "copyright notice printed to log";
            }
        } catch (Throwable t) {
            return "headless check failed (" + t.getClass().getName() + "), "
                    + "copyright notice printed to log";
        }

        return null;
    }

    private static boolean isBlank(String value) {
        return value == null || value.trim().isEmpty();
    }
}
