/*
 * AntiHackerX 运行时 —— 以 MIT 许可证提供,便于嵌入你自己的程序。
 *
 * SPDX-License-Identifier: MIT
 * 许可证全文见仓库中的 LICENSE。
 */
package top.h3k4;

import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

/**
 * 运行时入口工具。
 *
 * <p>由打包器生成的入口类调用:先 {@link #install} 初始化,再 {@link #load} 取出真实类,
 * 最后按程序形态拉起:</p>
 * <ul>
 *   <li>普通 JAR / Spring Boot:用 {@link #invokeMain} 反射调用原 {@code main};</li>
 *   <li>Paper 插件:由生成的入口类继承 {@code JavaPlugin},自行完成委托
 *       (见 PluginBootstrap 模板),不走 {@code main}。</li>
 * </ul>
 *
 * <h3>为什么不自己写一个 ClassLoader</h3>
 *
 * <p>原先的做法是解密到一个自定义 {@code ClassLoader} 里。这在 Bukkit/Paper 上是<b>行不通</b>的,
 * 因为 {@code JavaPlugin} 的构造器有一条硬检查:</p>
 *
 * <pre>
 *   ClassLoader cl = this.getClass().getClassLoader();
 *   if (!(cl instanceof PluginClassLoader)) {
 *       throw new IllegalStateException("JavaPlugin requires to be created by a valid classloader.");
 *   }
 * </pre>
 *
 * <p>{@code instanceof} 看的是<b>具体类</b>,子类化/包装/代理都绕不过去。所以被加壳的插件主类
 * 必须由<b>服务端自己的 PluginClassLoader</b> 定义。</p>
 *
 * <h3>怎么把类"送进"别人的类加载器</h3>
 *
 * <p>{@code ClassLoader#defineClass} 是 protected 而且在 java.base 里,Java 17 的模块系统
 * 不允许 {@code setAccessible}。但 Java 9 起的
 * {@code MethodHandles.Lookup#defineClass(byte[])} 可以 —— 它把类定义到
 * <b>lookup 所属类的加载器与包里</b>。</p>
 *
 * <p>于是打包器会为载荷里出现的<b>每一个包</b>生成一个极小的"定义器"类
 * {@code <包名>.<随机名>},放在明文区(因此由宿主加载器加载)。运行时按包找到定义器,
 * 取出它的 {@code Lookup},再 {@code defineClass} —— 解密出来的类就落在宿主加载器里了。</p>
 *
 * <p>{@code Lookup#defineClass} 是 Java 9+ 的 API,而本模块用 {@code --release 8} 编译,
 * 所以这里只能反射调用(与 {@code sun.misc.Unsafe} 那个坑同理)。</p>
 */
public final class AhxRuntime {

    /** 宿主类加载器:被加壳程序自己的加载器(Paper 下就是 PluginClassLoader)。 */
    private static volatile ClassLoader host;

    /** 已定义过的类。重复 defineClass 会抛 LinkageError,必须自己记账。 */
    private static final Map<String, Class<?>> DEFINED = new HashMap<String, Class<?>>();

    /** 包名 -> 该包定义器的 Lookup。 */
    private static final Map<String, Object> LOOKUPS = new HashMap<String, Object>();

    /** 缓存的 {@code MethodHandles.Lookup#defineClass(byte[])} */
    private static volatile Method defineClassMethod;

    /** 宿主加载器能不能提供某个类型(结果缓存)。 */
    private static final Map<String, Boolean> EXTERNAL = new HashMap<String, Boolean>();

    /** 解析不出父类/接口时的空结果。 */
    private static final String[] NO_SUPERTYPES = new String[0];

    private AhxRuntime() {
    }

    /**
     * 初始化(幂等)。
     *
     * <p>密钥不直接出现在产物里，而是以三个分片的形式存放在生成的入口类中。
     * 任意单个分片单独看都是随机数据，{@code strings} 扫不到密钥；
     * 必须实际执行 {@link #deriveKey} 才能还原。中间的随机量只存在于寄存器，
     * 异或后互相抵消，从不出现在任何地方。</p>
     *
     * @param shard0 第 1 层分片
     * @param shard1 第 2 层分片(存的时候字节序被反转)
     * @param shard2 第 3 层分片(存的时候逐字节取反)
     * @param link   定义器类的全限定名数组(打包时生成,每个载荷包一个)
     * @param loader 宿主加载器,传入口类自己的 ClassLoader
     */
    public static synchronized void install(int[] shard0, int[] shard1, int[] shard2,
                                            String[] link, ClassLoader loader) {
        if (host != null) {
            return;
        }
        AhxPayload.init(deriveKey(shard0, shard1, shard2));
        host = loader;
        loadLinkLookups(link);
        defineAll();
    }

    /**
     * 把每个定义器类的 Lookup 登记到「包名 -> Lookup」表里。
     *
     * <p>包名不从类名反推,而是直接问 Lookup 要 {@code lookupClass()} ——
     * 少一层猜测,出错时也能把真实的包名报出来。</p>
     */
    private static void loadLinkLookups(String[] definers) {
        if (definers == null) {
            return;
        }
        final Method lookupClassMethod;
        try {
            lookupClassMethod = Class.forName("java.lang.invoke.MethodHandles$Lookup")
                                     .getMethod("lookupClass");
        } catch (Exception newApiMissing) {
            throw new IllegalStateException("当前 JVM 不支持 MethodHandles.Lookup(需要 Java 9+)",
                                            newApiMissing);
        }
        final List<String> missing = new ArrayList<String>();
        for (String name : definers) {
            if (name == null || name.isEmpty()) {
                continue;
            }
            try {
                final Class<?> definer = Class.forName(name, true, host);
                final Field field = definer.getField("l");
                field.setAccessible(true);
                final Object lookup = field.get(null);
                final Class<?> lookupClass = (Class<?>) lookupClassMethod.invoke(lookup);
                final Package pkg = lookupClass.getPackage();
                LOOKUPS.put(pkg == null ? "" : pkg.getName(), lookup);
            } catch (Throwable problem) {
                missing.add(name);
            }
        }
        if (!missing.isEmpty()) {
            // 这个错误基本上只有一个成因:打包器生成的 LINK 里记的是定义器的原名,
            // 而产物里的定义器被混淆器改了包名(类名黑名单挡不住包路径重命名),
            // 于是宿主加载器按原名 Class.forName 不到。
            throw new IllegalStateException(
                    "有 " + missing.size() + " / " + definers.length
                    + " 个定义器类在宿主类加载器里找不到(产物里的包名被改过?),例如 "
                    + missing.get(0));
        }
    }

    /**
     * 把载荷里的所有类定义进宿主加载器。
     *
     * <p>为什么必须“一次性全部”,而不能用哪个解哪个:</p>
     *
     * <p>类一旦被定义进宿主加载器,它引用别的类时就由<b>宿主加载器</b>去解析。
     * 而载荷里的类都不在 JAR 里,宿主加载器根本不认得 —— 除非它们<b>已经被定义过</b>
     * (定义过的类会进加载器自己的表,下一次 {@code findLoadedClass} 就能命中)。
     * 所以要么全定义,要么碰到第一个跨类引用就 NoClassDefFoundError。</p>
     *
     * <h3>为什么要先算依赖</h3>
     *
     * <p>{@code defineClass} 会<b>立即解析父类与接口</b>,所以子类很容易比父类先被轮到。
     * 实测失败可以安全重试(不会留下残留),但“先问一问再定义”能让每轮都推进得更稳、
     * 也少一堆无意义的异常。</p>
     */
    private static void defineAll() {
        // 注意不能声明成 final:每轮要把它换成"还没定义成的那批"
        List<String> pending = new ArrayList<String>(Arrays.asList(AhxPayload.names()));
        while (!pending.isEmpty()) {
            int progress = 0;
            final List<String> still = new ArrayList<String>();
            for (String name : pending) {
                if (!superTypesReady(name)) {
                    // 父类/接口还没轮到 —— 只是时机未到。
                    // 先别急着 define:失败了虽然可以安全重试(实测不会留下残留),
                    // 但先问一问能让每轮推进得更稳,也少一堆无意义的异常。
                    still.add(name);
                    continue;
                }
                try {
                    defineOne(name);
                    ++progress;
                } catch (IllegalStateException packerBug) {
                    // 定义器没登记上 —— 这是打包器的问题,别当成“稍后再试”掩盖掉
                    throw packerBug;
                } catch (Throwable canNot) {
                    // 依赖都齐了还是定义不成:多半真的缺运行时的库。
                    // 留着下一轮 —— 万一它的依赖后来被别的类定义带出来了。
                    still.add(name);
                }
            }
            if (progress == 0) {
                // 按依赖顺序已经推不动了。但这不代表真的没救:
                // depReady() 的问法很保守(比如"宿主现在还不认得"),会误判。
                // defineClass 失败不会留残留,所以这里索性强行挨个试一遍。
                pending = forceDefine(still);
                break;
            }
            pending = still;
        }
    }

    /**
     * 不再问"依赖齐了没",直接把剩下的挨个定义一遍。
     *
     * <p>之所以敢这么做:{@code defineClass} 失败不会有任何残留(已实测),
     * 重试永远安全。</p>
     *
     * <p>还有定义不成的分两种，必须分开处理:</p>
     *
     * <p><b>A. 超类型本身就是宿主没有的可选库</b>(比如 PlaceholderAPI 的
     * {@code PlaceholderExpansion}、gson 的 {@code TypeAdapter}、hikari 的
     * {@code CodahaleHealthChecker})。这种插件的常规做法就是把它隔到单独一个类里,
     * 靠“依赖装了才会走到”来保证不被加载 —— 也就是<b>未加壳时的懒加载语义</b>。
     * 载荷必须一次性全定义,这种类当然定不了;此时直接跳过,行为跟没加壳时等价
     * (真被用到才会 NoClassDefFoundError),不能因此让整个插件加载失败。</p>
     *
     * <p><b>B. 超类型都能解析,却还是定义不成</b> —— 那是真的出问题了(打包器 bug、
     * 载荷被改坏、JVM 版本不支持),必须抛异常。这里抛而不是打印日志,是因为
     * NOBF 转换过的代码里只有异常能被看到;而且少定义了几个类,运行期会以
     * NoClassDefFoundError 在毫不相干的调用点炸掉,不如现在报清楚。</p>
     */
    private static List<String> forceDefine(List<String> remaining) {
        final List<String> failed = new ArrayList<String>();
        for (String name : remaining) {
            try {
                defineOne(name);
            } catch (IllegalStateException packerBug) {
                throw packerBug;
            } catch (Throwable canNot) {
                failed.add(name);
            }
        }
        if (failed.isEmpty()) {
            return failed;
        }

        // 先做不动点:某个类只要有一个超类型“宿主没有、载荷里也没有”,
        // 或者它的超类型本身已经被判定为跳过,那它也跟着跳过。
        final Map<String, Boolean> skipped = new HashMap<String, Boolean>();
        boolean changed = true;
        while (changed) {
            changed = false;
            for (String name : failed) {
                if (skipped.containsKey(name)) {
                    continue;
                }
                if (hasAbsentSupertype(name, skipped)) {
                    skipped.put(name, Boolean.TRUE);
                    changed = true;
                }
            }
        }

        final List<String> real = new ArrayList<String>();
        for (String name : failed) {
            if (!skipped.containsKey(name)) {
                real.add(name);
            }
        }
        if (skipped.isEmpty()) {
            return real;
        }

        // 跳过的那批要留个声 —— System.out 在 NOBF 转换过的代码里是可用的
        // (System.err 才是打不出来的那个)。
        System.out.println("[AntiHackerX] " + skipped.size()
                + " 个载荷类未定义:它们的超类型依赖宿主没有装的可选库。"
                + "这与未加壳时的懒加载行为一致,真被用到才会报 NoClassDefFoundError。");
        final int shown = Math.min(skipped.size(), 6);
        int i = 0;
        for (String name : skipped.keySet()) {
            if (++i > shown) {
                System.out.println("    ...另有 " + (skipped.size() - shown) + " 个");
                break;
            }
            System.out.println("    " + name.replace('/', '.') + "  <-  "
                    + missingSupertypesOf(name));
        }
        if (real.isEmpty()) {
            return real;
        }
        // 剩下的属于 B 类:超类型都解析得到却定义不成。这是真出问题了,报清楚。
        final StringBuilder sb = new StringBuilder();
        sb.append(real.size()).append(" / ").append(AhxPayload.names().length)
          .append(" 个载荷类定义失败(超类型都能解析,仍失败)。逐条列出它们缺的超类型:");
        final int limit = Math.min(real.size(), 8);
        for (int k = 0; k < limit; k++) {
            sb.append("\n  ").append(real.get(k)).append("  <-  ")
              .append(missingSupertypesOf(real.get(k)));
        }
        if (real.size() > limit) {
            sb.append("\n  ...另有 ").append(real.size() - limit).append(" 个");
        }
        throw new IllegalStateException(sb.toString());
    }

    /**
     * {@code internal} 是不是“因为宿主缺可选库而定义不了”。
     *
     * <p>{@code skipped} 是已判定为跳过的一批,用来做级联判定:</p>
     * <pre>Y extends X,而 X extends 宿主没有的库方法 ⇒ Y 也定义不了</pre>
     */
    private static boolean hasAbsentSupertype(String internal, Map<String, Boolean> skipped) {
        final byte[] bytes = AhxPayload.readClass(internal);
        if (bytes == null) {
            return false;
        }
        for (String dep : directSupertypes(bytes)) {
            if (dep == null || dep.isEmpty()
                    || dep.charAt(0) == '[' || dep.length() == 1) {
                continue;
            }
            synchronized (DEFINED) {
                if (DEFINED.containsKey(dep)) {
                    continue;
                }
            }
            if (AhxPayload.contains(dep)) {
                if (skipped.containsKey(dep)) {
                    return true;    // 级联
                }
                continue;           // 它自己的问题留给自己那一轮
            }
            if (!hostKnows(dep)) {
                return true;        // 宿主没有 ⇒ 可选依赖没装
            }
        }
        return false;
    }

    /** 宿主加载器认不认识这个内部名(不走 EXTERNAL 缓存,要的是此刻的真相)。 */
    private static boolean hostKnows(String internal) {
        try {
            Class.forName(internal.replace('/', '.'), false, host);
            return true;
        } catch (Throwable absent) {
            return false;
        }
    }

    /**
     * 列出 {@code internal} 那些"现在解析不到"的直接超类型,并带上失败原因。
     *
     * <p>这里故意不走 {@link #depReady} 的缓存 —— 缓存会把"某一刻加载不到"
     * 永久记成 false,而那往往正是问题被掩盖的地方。</p>
     */
    private static String missingSupertypesOf(String internal) {
        final byte[] bytes = AhxPayload.readClass(internal);
        if (bytes == null) {
            return "(载荷里读不到它的字节)";
        }
        final StringBuilder sb = new StringBuilder();
        for (String dep : directSupertypes(bytes)) {
            if (dep == null || dep.isEmpty()
                    || dep.charAt(0) == '[' || dep.length() == 1) {
                continue;
            }
            synchronized (DEFINED) {
                if (DEFINED.containsKey(dep)) {
                    continue;
                }
            }
            if (sb.length() > 0) {
                sb.append(", ");
            }
            sb.append(dep).append('[');
            if (AhxPayload.contains(dep)) {
                sb.append("载荷里也有,但它自己没定义成功");
            } else {
                try {
                    Class.forName(dep.replace('/', '.'), false, host);
                    sb.append("宿主居然认得(那不该失败)");
                } catch (Throwable absent) {
                    sb.append(absent.getClass().getName());
                    if (absent.getMessage() != null) {
                        sb.append(": ").append(absent.getMessage());
                    }
                }
            }
            sb.append(']');
        }
        return sb.length() == 0
                ? "(超类型都解析得到,但 defineClass 仍然失败 —— 见 IllegalStateException 的 cause)"
                : sb.toString();
    }

    /**
     * {@code name} 的父类与直接接口是不是都已经能解析了。
     *
     * <p>只有当一个类确实“到时候了”才去定义它,从而少踩
     * {@code defineClass} 因解析失败而报错的弯路。</p>
     */
    private static boolean superTypesReady(String internal) {
        final byte[] bytes = AhxPayload.readClass(internal);
        if (bytes == null) {
            return true;    // 读不到就交给 defineOne 去报真正的原因
        }
        for (String dep : directSupertypes(bytes)) {
            if (!depReady(dep)) {
                return false;
            }
        }
        return true;
    }

    /**
     * 某个类型现在能不能被解析到 —— 要么已经定义过,要么宿主加载器认得。
     *
     * <p>“宿主认不认得”需要实际问一次加载器,结果会缓存。
     * 载荷里的类一律不算就绪(它们只能靠 {@code defineClass} 一个个落位)。</p>
     */
    private static boolean depReady(String internalName) {
        if (internalName == null || internalName.isEmpty()) {
            return true;
        }
        if (internalName.charAt(0) == '[' || internalName.length() == 1) {
            return true;    // 数组 / 基本类型:JVM 自己会造
        }
        synchronized (DEFINED) {
            if (DEFINED.containsKey(internalName)) {
                return true;
            }
        }
        if (AhxPayload.contains(internalName)) {
            return false;   // 在载荷里,但还没轮到
        }
        synchronized (EXTERNAL) {
            final Boolean cached = EXTERNAL.get(internalName);
            if (cached != null) {
                return cached.booleanValue();
            }
            boolean ok;
            try {
                Class.forName(internalName.replace('/', '.'), false, host);
                ok = true;
            } catch (Throwable absent) {
                ok = false;
            }
            EXTERNAL.put(internalName, Boolean.valueOf(ok));
            return ok;
        }
    }

    /**
     * 从 class 字节里读出<b>直接父类与直接接口</b>的内部名。
     *
     * <p>只解析到 interfaces 表为止 —— 常量池以外的字段、方法、属性一概不碰。</p>
     */
    private static String[] directSupertypes(byte[] b) {
        try {
            int p = 8;                                      // magic(4) minor(2) major(2)
            final int cpCount = u2(b, p);
            p += 2;
            final String[] utf8 = new String[cpCount];
            final int[] classNameIdx = new int[cpCount];
            for (int i = 1; i < cpCount; i++) {
                final int tag = b[p++] & 0xFF;
                switch (tag) {
                    case 1: {                               // Utf8
                        final int len = u2(b, p);
                        p += 2;
                        utf8[i] = new String(b, p, len, java.nio.charset.StandardCharsets.UTF_8);
                        p += len;
                        break;
                    }
                    case 7:                                 // Class
                        classNameIdx[i] = u2(b, p);
                        p += 2;
                        break;
                    case 8: case 16: case 19: case 20:      // String / MethodType / Module / Package
                        p += 2;
                        break;
                    case 15:                                // MethodHandle
                        p += 3;
                        break;
                    case 3: case 4:                         // Integer / Float
                    case 9: case 10: case 11: case 12:      // Fieldref / Methodref / IfaceMethodref / NameAndType
                    case 17: case 18:                       // Dynamic / InvokeDynamic
                        p += 4;
                        break;
                    case 5: case 6:                         // Long / Double 各占两个常量槽
                        p += 8;
                        i++;
                        break;
                    default:
                        return NO_SUPERTYPES;               // 不认识的 tag,不猜
                }
            }
            p += 4;                                         // access_flags(2) + this_class(2)
            final int superIdx = u2(b, p);
            p += 2;
            final int ifCount = u2(b, p);
            p += 2;

            final String[] out = new String[1 + ifCount];
            int n = 0;
            if (superIdx != 0) {                            // 0 = 只有 java/lang/Object 没有父类
                final String s = utf8[classNameIdx[superIdx]];
                if (s != null) {
                    out[n++] = s;
                }
            }
            for (int i = 0; i < ifCount; i++) {
                final String s = utf8[classNameIdx[u2(b, p)]];
                p += 2;
                if (s != null) {
                    out[n++] = s;
                }
            }
            return n == out.length ? out : Arrays.copyOf(out, n);
        } catch (Throwable malformed) {
            return NO_SUPERTYPES;   // 畸形就当作“没有依赖”,交给 defineClass 自己去报
        }
    }

    private static int u2(byte[] b, int p) {
        return ((b[p] & 0xFF) << 8) | (b[p + 1] & 0xFF);
    }

    private static void defineOne(String internal) throws Exception {
        synchronized (DEFINED) {
            if (DEFINED.containsKey(internal)) {
                return;
            }
        }
        final byte[] bytes = AhxPayload.readClass(internal);
        if (bytes == null) {
            throw new ClassNotFoundException(internal);
        }
        final Object lookup = lookupFor(packageOf(internal));
        final Class<?> defined;
        try {
            defined = (Class<?>) defineClassMethod().invoke(lookup, (Object) bytes);
        } catch (java.lang.reflect.InvocationTargetException wrapped) {
            final Throwable cause = wrapped.getCause();
            // 父类/接口还没定义好 —— 这是收敛过程中的正常中间状态,不是错误。
            // 类之间的父子关系构成 DAG,子类完全可能比父类先被轮到(比如
            // ShortArrays$ArrayHashStrategy 的父类 Hash$Strategy 在**另一个包**里)。
            // 抛成 ClassNotFoundException,让 defineAll 下一轮再来。
            //
            // 千万不能在这里包成 IllegalStateException:defineAll 一见它就判定
            // “定义器没登记上 = 打包器 bug”,立刻终止整个加载。
            if (cause instanceof NoClassDefFoundError
                    || cause instanceof ClassNotFoundException) {
                throw new ClassNotFoundException(internal, cause);
            }
            if (cause instanceof LinkageError) {
                // 典型是“attempted duplicate class definition” —— 这个名字在加载器里
                // 已经有了。能把它取回来就当作成功,取不回来才是真的没救。
                final Class<?> existing =
                        Class.forName(internal.replace('/', '.'), false, host);
                synchronized (DEFINED) {
                    DEFINED.put(internal, existing);
                }
                return;
            }
            // 其余才是真问题(不同包、字节码格式错……),把上下文一起报出来
            final Class<?> lookupClass =
                    (Class<?>) Class.forName("java.lang.invoke.MethodHandles$Lookup")
                                    .getMethod("lookupClass").invoke(lookup);
            final Package lookupPkg = lookupClass.getPackage();
            throw new IllegalStateException(
                    "向宿主类加载器定义 " + internal + " 失败:载荷包=" + packageOf(internal)
                    + ",定义器包=" + (lookupPkg == null ? "(默认包)" : lookupPkg.getName()),
                    cause);
        }
        synchronized (DEFINED) {
            DEFINED.put(internal, defined);
        }
    }

    /**
     * 取类。正常情况下 {@link #install} 已经把载荷全部定义进宿主加载器了,直接问它要;
     * 开机时因缺依赖没定义成的,这里再试一次。
     *
     * @param className 内部名或全限定名都可
     */
    public static Class<?> load(String className) throws Exception {
        final String internal = className.replace('.', '/');

        synchronized (DEFINED) {
            final Class<?> cached = DEFINED.get(internal);
            if (cached != null) {
                return cached;
            }
        }
        if (host == null) {
            throw new IllegalStateException("AhxRuntime 尚未初始化,请先调用 install()");
        }
        if (AhxPayload.contains(internal)) {
            defineOne(internal);
            synchronized (DEFINED) {
                final Class<?> just = DEFINED.get(internal);
                if (just != null) {
                    return just;
                }
            }
        }
        // 不在载荷里的(例如被原生化过的类,其明文 stub 就在 JAR 里)交给宿主
        return Class.forName(internal.replace('/', '.'), false, host);
    }

    /** 取某个包的定义器里的 Lookup(install 时已经登记好了)。 */
    private static Object lookupFor(String pkg) {
        final Object lookup = LOOKUPS.get(pkg);
        if (lookup == null) {
            final StringBuilder sample = new StringBuilder();
            int n = 0;
            for (String key : LOOKUPS.keySet()) {
                if (n > 0) {
                    sample.append(", ");
                }
                sample.append('\'').append(key.isEmpty() ? "(默认包)" : key).append('\'');
                if (++n >= 8) {
                    sample.append(", ...");
                    break;
                }
            }
            throw new IllegalStateException("找不到包「" + pkg + "」的定义器,已登记 "
                    + LOOKUPS.size() + " 个包"
                    + (LOOKUPS.isEmpty() ? "" : ":" + sample));
        }
        return lookup;
    }

    private static Method defineClassMethod() throws Exception {
        Method cached = defineClassMethod;
        if (cached == null) {
            cached = Class.forName("java.lang.invoke.MethodHandles$Lookup")
                             .getMethod("defineClass", byte[].class);
            defineClassMethod = cached;
        }
        return cached;
    }

    /** {@code a/b/C} -> {@code a.b};默认包返回空串。 */
    private static String packageOf(String internal) {
        final int slash = internal.lastIndexOf('/');
        return slash < 0 ? "" : internal.substring(0, slash).replace('/', '.');
    }

    /**
     * 由三个分片还原 AES-256 密钥。
     *
     * <p>三层各自做过不同变换，运行时逐层反向还原后异或合并：</p>
     * <pre>
     *   a = 还原(S0)          // 第 1 层：直接展开
     *   b = 反转(还原(S1))     // 第 2 层：字节序反转
     *   c = 取反(还原(S2))     // 第 3 层：逐字节按位取反
     *   key = a XOR b XOR c
     * </pre>
     *
     * <p>打包端构造时：{@code S0 = K^r0}、{@code S1 = r0^r1}、{@code S2 = r1}，
     * 其中 {@code r0}、{@code r1} 是随机量且<b>不落盘</b>，异或时自然抵消。</p>
     */
    private static byte[] deriveKey(int[] shard0, int[] shard1, int[] shard2) {
        final byte[] a = toBytes(shard0);
        final byte[] b = reverse(toBytes(shard1));
        final byte[] c = notEach(toBytes(shard2));

        final byte[] key = new byte[a.length];
        for (int i = 0; i < key.length; i++) {
            key[i] = (byte) (a[i] ^ b[i] ^ c[i]);
        }
        return key;
    }

    /** int[] 展开成字节(每 int 4 字节，高位在前) */
    private static byte[] toBytes(int[] words) {
        final byte[] out = new byte[words.length * 4];
        for (int i = 0; i < words.length; i++) {
            final int w = words[i];
            out[i * 4] = (byte) (w >>> 24);
            out[i * 4 + 1] = (byte) (w >>> 16);
            out[i * 4 + 2] = (byte) (w >>> 8);
            out[i * 4 + 3] = (byte) w;
        }
        return out;
    }

    private static byte[] reverse(byte[] in) {
        final byte[] out = new byte[in.length];
        for (int i = 0; i < in.length; i++) {
            out[i] = in[in.length - 1 - i];
        }
        return out;
    }

    private static byte[] notEach(byte[] in) {
        final byte[] out = new byte[in.length];
        for (int i = 0; i < in.length; i++) {
            out[i] = (byte) ~in[i];
        }
        return out;
    }

    /** 取宿主加载器;未初始化时返回 null */
    public static ClassLoader host() {
        return host;
    }

    /**
     * 加载真实入口类并调用其 {@code main(String[])}。
     * 用于普通 JAR / Spring Boot 这类有 main 入口的程序。
     */
    public static void invokeMain(String className, String[] args) throws Exception {
        final Class<?> target = load(className);
        final Method main = target.getMethod("main", String[].class);
        // 反射调用静态方法时第一个参数传 null
        main.invoke(null, (Object) args);
    }

    // =======================================================================
    //  Fabric:解密完成后拉起真实入口
    // =======================================================================

    /**
     * Fabric 的三个入口接口与各自的方法名。
     *
     * <p>顺序就是 Fabric 自己的调用顺序:先所有 {@code main},再 client / server。
     * 有些 mod 依赖这个顺序(主入口先注册内容,client 入口才去注册渲染),
     * 所以外层循环必须是接口、内层才是类。</p>
     */
    private static final String[][] FABRIC_ENTRYPOINTS = {
            {"net/fabricmc/api/ModInitializer", "onInitialize"},
            {"net/fabricmc/api/ClientModInitializer", "onInitializeClient"},
            {"net/fabricmc/api/DedicatedServerModInitializer", "onInitializeServer"},
    };

    /**
     * Fabric:把载荷里(以及 JAR 里那些被原生化过的 stub 里)的入口类找出来拉起来。
     *
     * <h3>为什么入口类名不在任何地方出现</h3>
     *
     * <p>打包时已经把 {@code fabric.mod.json} 里的 {@code entrypoints} 抹掉、换成
     * 指向本桥的 {@code preLaunch}。这里也**不读任何类名常量**,而是直接扫
     * 「已经定义进宿主的类」+「本 JAR 里的类」,按<b>实现了哪个接口</b>来判定。</p>
     *
     * <p>于是产物里没有任何一处写着"某某类是入口":混淆后的类名照旧,
     * 而入口特征(接口)只以字节码形式存在,与其它类别无二致。</p>
     *
     * <p>两个来源都要扫的原因:被 native-obfuscator 搬进原生库的类不在载荷里,
     * 它的明文 stub 就在 JAR 内 —— 而 stub 保留了接口声明,所以照样能被认出来。</p>
     *
     * @return 实际拉起的入口个数
     */
    public static int invokeEntrypoints() throws Exception {
        if (host == null) {
            throw new IllegalStateException("AhxRuntime 尚未初始化,请先调用 install()");
        }

        final Set<String> candidates = new LinkedHashSet<String>();
        synchronized (DEFINED) {
            candidates.addAll(DEFINED.keySet());
        }
        candidates.addAll(jarClassNames());

        int invoked = 0;
        for (String[] entry : FABRIC_ENTRYPOINTS) {
            final String interfaceName = entry[0];
            final String methodName = entry[1];
            for (String internal : candidates) {
                Class<?> type;
                try {
                    // 不初始化:只是看一眼接口,别触发对方的静态块
                    type = Class.forName(internal.replace('/', '.'), false, host);
                } catch (Throwable unavailable) {
                    // 缺可选依赖、或者不是这个环境的类(如只有客户端才有的类)
                    continue;
                }
                if (!implementsInterface(type, interfaceName)) {
                    continue;
                }
                invokeEntrypoint(type, methodName);
                invoked++;
            }
        }
        return invoked;
    }

    /** 调一个入口方法。Fabric 的入口是实例方法,自己 new 一个;静态方法就直接调。 */
    private static void invokeEntrypoint(Class<?> type, String methodName) throws Exception {
        final Method method = type.getMethod(methodName);
        if (java.lang.reflect.Modifier.isStatic(method.getModifiers())) {
            method.invoke(null);
            return;
        }
        final Object instance = type.newInstance();
        method.invoke(instance);
    }

    /** 类(含父类)是否实现了指定接口。用接口名比较,避免编译期就要依赖那些接口。 */
    private static boolean implementsInterface(Class<?> type, String interfaceInternalName) {
        final String wanted = interfaceInternalName.replace('/', '.');
        for (Class<?> current = type; current != null; current = current.getSuperclass()) {
            final Class<?>[] interfaces = current.getInterfaces();
            for (Class<?> candidate : interfaces) {
                if (candidate.getName().equals(wanted)
                        || implementsInterface(candidate, interfaceInternalName)) {
                    return true;
                }
            }
        }
        return false;
    }

    /**
     * 本 JAR 里所有类的内部名。
     *
     * <p>拿不到(不是从 JAR 启动、或被包在别的容器里)时返回空表 ——
     * 那就只扫载荷里的类,不报错。</p>
     */
    private static List<String> jarClassNames() {
        final List<String> names = new ArrayList<String>();
        try {
            final java.security.CodeSource source =
                    AhxRuntime.class.getProtectionDomain().getCodeSource();
            if (source == null || source.getLocation() == null) {
                return names;
            }
            final java.io.File file = new java.io.File(source.getLocation().toURI());
            if (!file.isFile()) {
                return names;
            }
            final java.util.zip.ZipFile zip = new java.util.zip.ZipFile(file);
            try {
                final java.util.Enumeration<? extends java.util.zip.ZipEntry> entries =
                        zip.entries();
                while (entries.hasMoreElements()) {
                    final String name = entries.nextElement().getName();
                    if (!name.endsWith(".class")) {
                        continue;
                    }
                    // 跳过 module-info 之类的非类条目
                    if (!name.endsWith("/") && name.indexOf("module-info") >= 0) {
                        continue;
                    }
                    names.add(name.substring(0, name.length() - ".class".length()));
                }
            } finally {
                zip.close();
            }
        } catch (Throwable ignored) {
            // 只影响"能不能认出被原生化过的入口",不该让启动失败
        }
        return names;
    }
}
