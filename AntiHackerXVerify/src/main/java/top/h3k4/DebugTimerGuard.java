/*
 * AntiHackerX 示例模块 —— 以 MIT 许可证提供,便于嵌入你自己的程序。
 *
 * SPDX-License-Identifier: MIT
 * 许可证全文见仓库中的 AntiHackerXVerify/LICENSE。
 */
package top.h3k4;

import java.lang.reflect.Field;
import java.lang.reflect.Method;

/**
 * 反调试计时器守卫(最简单版)。
 *
 * <p>原理:守护线程每 {@link #CHECK_INTERVAL_MS} 醒来一次,用单调时钟
 * {@link System#nanoTime()} 测量「距离上一次醒来实际过了多久」。
 * 正常运行时该间隔约等于检查周期;一旦调试器下断点把 JVM 挂起
 * (JDWP 断点默认挂起全部线程),被冻结的这段时间里系统时间仍在走,
 * 而本线程被挂起无法醒来。恢复后测得的间隔会明显超过阈值,判定
 * 「被下过断点」,立即触发崩溃。</p>
 *
 * <p>为什么用 {@code nanoTime()} 而不是 {@code currentTimeMillis()}:后者是墙钟,
 * 会因 NTP 校时跳变造成误报;前者是单调时钟,只增不减,不受校时影响。</p>
 *
 * <p>关于「崩溃」:这里<b>不能</b>用 {@code null.hashCode()} —— native-obfuscator 转
 * C++ 时会自动插入 null 检查,把它变成规范的 {@link NullPointerException},并不会
 * 真正崩溃。因此改为用 {@code sun.misc.Unsafe} 读取内存地址 0,直接触发原生访问
 * 违例(Linux SIGSEGV / Windows 0xC0000005),整个 JVM 立即崩溃。该手法在纯 Java
 * 阶段即已生效,转成 C++ 后同样生效。</p>
 */
public final class DebugTimerGuard {

    /** 检查周期:每 5 秒检查一次 */
    static final long CHECK_INTERVAL_MS = 5_000L;

    /** 冻结判定阈值:实际间隔超过 10 秒即认为 JVM 被挂起过 */
    static final long FREEZE_THRESHOLD_MS = 10_000L;

    private static volatile boolean started = false;

    private DebugTimerGuard() {
        // 工具类,禁止实例化
    }

    /**
     * 启动反调试计时器(幂等,重复调用无副作用)。
     * 建议放在 main 的第一行,越早启动越难被绕过。
     */
    public static synchronized void start() {
        if (started) {
            return;
        }
        started = true;

        Thread watcher = new Thread(new Runnable() {
            @Override
            public void run() {
                long lastTick = System.nanoTime();
                for (;;) {
                    try {
                        Thread.sleep(CHECK_INTERVAL_MS);
                    } catch (InterruptedException e) {
                        return; // 线程被中断,安静退出
                    }

                    long now = System.nanoTime();
                    long gapMs = (now - lastTick) / 1_000_000L;

                    // 正常 gap ≈ 5000ms;被断点冻结后 gap 会超过 10 秒阈值
                    if (isFrozen(gapMs)) {
                        crashNow();
                    }
                    lastTick = now;
                }
            }
        }, "AntiHackerX-Timer");

        watcher.setDaemon(true);
        watcher.start();
    }

    /**
     * 判断一次间隔是否说明 JVM 曾被冻结。
     * 包级可见,便于单测。
     */
    static boolean isFrozen(long gapMs) {
        return gapMs > FREEZE_THRESHOLD_MS;
    }

    /**
     * 制造崩溃:读取内存地址 0,触发原生访问违例,整个 JVM 立即崩溃。
     *
     * <p>为什么不用 {@code null.hashCode()}:native-obfuscator 在转 C++ 时会为引用
     * 调用插入 null 检查并抛出规范的 NullPointerException,不会崩溃。</p>
     *
     * <p>为什么全程走反射:{@code sun.misc} 不在 {@code --release 8} 的 API 签名里,
     * 直接引用会编译失败。</p>
     */
    private static void crashNow() {
        try {
            Class<?> unsafeClass = Class.forName("sun.misc.Unsafe");
            Field theUnsafe = unsafeClass.getDeclaredField("theUnsafe");
            theUnsafe.setAccessible(true);
            Object unsafe = theUnsafe.get(null);

            // 读地址 0:Linux SIGSEGV / Windows 0xC0000005,此处不返回
            Method getInt = unsafeClass.getMethod("getInt", long.class);
            getInt.invoke(unsafe, 0L);
        } catch (Throwable t) {
            // 只有「没能崩掉」时才会走到这里,兜底抛 Error
            throw new InternalError(t);
        }
    }
}
