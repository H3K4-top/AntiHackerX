/*
 * AntiHackerX 示例模块 —— 以 MIT 许可证提供,便于嵌入你自己的程序。
 *
 * SPDX-License-Identifier: MIT
 * 许可证全文见仓库中的 AntiHackerXVerify/LICENSE。
 */
package top.h3k4;

import java.lang.management.ManagementFactory;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.List;
import java.util.Locale;

/**
 * 反 Agent 守卫。
 *
 * <p>Java 的 agent 注入只有两条路:</p>
 * <ol>
 *   <li><b>启动期</b>:由 JVM 启动参数挂载 —— {@code -javaagent:xxx.jar}、
 *       {@code -agentlib:xxx}、{@code -agentpath:/abs/path}
 *       (旧式等价写法 {@code -Xrun<lib>:<opts>});</li>
 *   <li><b>运行期</b>:通过 Attach API 动态调用 {@code VirtualMachine.loadAgent(...)}。</li>
 * </ol>
 *
 * <p>本类负责第 1 种:启动参数里一旦出现上述开关就立即崩溃,不给 agent 落地的机会。
 * 启动参数由 {@link java.lang.management.RuntimeMXBean#getInputArguments()} 提供。</p>
 *
 * <p><b>已知局限</b>:运行期通过 Attach API 动态挂载的 agent <b>不会</b>出现在
 * {@code getInputArguments()} 里 —— 那个列表在 JVM 启动时就定死了。要覆盖这一路,
 * 需要额外检测 {@code Attach Listener} 线程或本地 attach 套接字。</p>
 */
public final class AgentGuard {

    private AgentGuard() {
        // 工具类,禁止实例化
    }

    /**
     * 启动时检查一次启动参数,发现 agent 立即崩溃。
     * 建议放在 main 的最前面,越早越难被绕过。
     */
    public static void check() {
        if (hasAgentArgument()) {
            crashNow();
        }
    }

    /**
     * 判断单个启动参数是否是 agent 挂载开关。
     * 包级可见,便于单测。
     *
     * <p>用 {@code startsWith} 锚定参数开头,所以 {@code -Dfoo=-javaagent:x}
     * 这类系统属性不会误命中。比较前统一转小写,避免大小写变体绕过。</p>
     */
    static boolean isAgentArgument(String arg) {
        if (arg == null) {
            return false;
        }
        String lower = arg.toLowerCase(Locale.ROOT);
        return lower.startsWith("-javaagent:")
                || lower.startsWith("-agentlib:")
                || lower.startsWith("-agentpath:")
                || lower.startsWith("-xrun");
    }

    /**
     * 启动参数里是否存在 agent 挂载开关。
     */
    static boolean hasAgentArgument() {
        List<String> args;
        try {
            args = ManagementFactory.getRuntimeMXBean().getInputArguments();
        } catch (Throwable ignored) {
            // 非标准 JVM 上可能抛 UnsupportedOperationException。此时无法判断,
            // 宁可漏报也不误伤正常用户。
            return false;
        }

        for (String arg : args) {
            if (isAgentArgument(arg)) {
                return true;
            }
        }
        return false;
    }

    /**
     * 制造崩溃:读取内存地址 0,触发原生访问违例
     * (Linux SIGSEGV / Windows 0xC0000005),整个 JVM 立即崩溃。
     *
     * <p>这里刻意与 {@link DebugTimerGuard} 里的崩溃各写一份、不共用:
     * 共用的话逆向者只要定位到那一处并 patch 掉,就能一次性废掉所有防护。</p>
     *
     * <p>全程走反射是因为 {@code sun.misc} 不在 {@code --release 8} 的 API 签名里,
     * 直接引用会编译失败。</p>
     */
    private static void crashNow() {
        try {
            Class<?> unsafeClass = Class.forName("sun.misc.Unsafe");
            Field theUnsafe = unsafeClass.getDeclaredField("theUnsafe");
            theUnsafe.setAccessible(true);
            Object unsafe = theUnsafe.get(null);

            // 读地址 0:此处不返回
            Method getInt = unsafeClass.getMethod("getInt", long.class);
            getInt.invoke(unsafe, 0L);
        } catch (Throwable t) {
            // 只有「没能崩掉」时才会走到这里,兜底抛 Error
            throw new InternalError(t);
        }
    }
}
