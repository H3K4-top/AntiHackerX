/*
 * AntiHackerX 示例模块 —— 以 MIT 许可证提供,便于嵌入你自己的程序。
 *
 * SPDX-License-Identifier: MIT
 * 许可证全文见仓库中的 AntiHackerXVerify/LICENSE。
 */
package top.h3k4;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.util.Locale;

import javax.swing.JOptionPane;

/**
 * 反虚拟机守卫(零依赖实现)。
 *
 * <p>思路:虚拟机的固件会"如实"把自己写进 SMBIOS/DMI —— 主板厂商、主板型号、
 * 系统厂商、系统型号、BIOS 厂商这几个字段在 VMware / VirtualBox / QEMU / Hyper-V
 * 上都有固定特征串。把它们读出来做子串匹配即可判定。</p>
 *
 * <p>各平台取数方式(均不依赖第三方库):</p>
 * <ul>
 *   <li><b>Linux</b>:{@code /sys/class/dmi/id/} 下的 sys_vendor、product_name、
 *       board_vendor、board_name、bios_vendor(普通用户可读);</li>
 *   <li><b>Windows</b>:{@code HKLM\HARDWARE\DESCRIPTION\System\BIOS} 注册表项,
 *       用 reg query 读出后解析(与 OSHI 在 Windows 上取的是同一份数据);</li>
 *   <li><b>macOS</b>:{@code sysctl -n hw.model}(VMware Fusion 会报 "VMware7,1")。</li>
 * </ul>
 *
 * <p><b>行为与其它守卫不同</b>:检测到虚拟机时<b>不</b>触发访问违例,而是弹出提示框,
 * 用户点确定后调用 {@code System.exit(-1)},让 JVM 正常退出(shutdown hook 会执行)。</p>
 */
public final class VirtualMachineGuard {

    /** 提示语(按要求固定) */
    static final String BLOCK_MESSAGE =
            "YOU APP ARE RUN IN A VM,YOU CAN'T USE VM RUN THIS APP!";

    /**
     * 命中即判定为虚拟机的特征串(统一转小写后做子串匹配)。
     *
     * <p>注意这里匹配的是<b>全部</b> SMBIOS 字段的拼接结果,而不只是厂商/型号 ——
     * 2026-09-17 实测发现:有 VM 会把 SystemManufacturer/SystemProductName 伪装成
     * "ASRock"/"B450M" 冒充真机,但 BIOSVendor 仍是 "SeaBIOS"、BIOSVersion 里仍带着
     * "qemu.org"。所以固件厂商与版本字段同样必须参与匹配。</p>
     */
    private static final String[] VM_SIGNATURES = {
            "vmware",           // VMware Workstation / ESXi / Fusion
            "virtualbox",       // VirtualBox
            "vbox",             // VirtualBox 的短名
            "innotek",          // VirtualBox 早期厂商名(innotek GmbH)
            "qemu",             // QEMU / KVM
            "kvm",              // KVM
            "bochs",            // QEMU 有时报 Bochs
            "xen",              // Xen
            "parallels",        // Parallels Desktop
            "bhyve",            // FreeBSD bhyve
            "hyper-v",          // Hyper-V
            "virtual machine",  // Hyper-V / 部分 VMware 的系统型号字段
            "seabios",          // QEMU/KVM 的开源固件,真机不会使用
            "ovmf",             // QEMU 的 UEFI 固件(Open Virtual Machine Firmware)
            "virtio"            // 半虚拟化设备前缀,常出现在 VM 的固件串里
    };

    private VirtualMachineGuard() {
        // 工具类,禁止实例化
    }

    /**
     * 检测到虚拟机就弹窗并退出,否则什么都不做。
     * 建议放在 main 的最前面。
     */
    public static void check() {
        if (isVirtualMachine()) {
            blockAndExit();
        }
    }

    /**
     * 当前是否运行在虚拟机内。
     * 包级可见,便于单测 —— 直接调用不会弹窗、不会退出。
     */
    static boolean isVirtualMachine() {
        String fingerprint = collectFingerprint();
        if (fingerprint.isEmpty()) {
            // 取不到硬件信息(权限不足 / 非主流平台 / 命令不可用),
            // 宁可漏报也不误杀真实机器。
            return false;
        }
        return matchesVmSignature(fingerprint);
    }

    /**
     * 特征串匹配。包级可见,便于单测。
     */
    static boolean matchesVmSignature(String fingerprint) {
        if (fingerprint == null || fingerprint.isEmpty()) {
            return false;
        }
        String lower = fingerprint.toLowerCase(Locale.ROOT);
        for (String signature : VM_SIGNATURES) {
            if (lower.contains(signature)) {
                return true;
            }
        }
        return false;
    }

    /**
     * 按当前平台收集硬件特征串,返回拼接结果。
     */
    static String collectFingerprint() {
        String os = System.getProperty("os.name", "").toLowerCase(Locale.ROOT);

        if (os.contains("win")) {
            // Windows 没有 sysfs,改从注册表读(reg query 是系统自带命令)
            return parseWindowsBiosRegistry(runCommand(
                    "reg", "query", "HKLM\\HARDWARE\\DESCRIPTION\\System\\BIOS"));
        }
        if (os.contains("linux")) {
            return readLinuxDmi(new File("/sys/class/dmi/id"));
        }
        if (os.contains("mac")) {
            return runCommand("sysctl", "-n", "hw.model");
        }
        return "";
    }

    /**
     * 读 Linux 的 DMI 字段并拼接。
     * 包级可见,便于单测传一个假目录进来。
     */
    static String readLinuxDmi(File dmiDir) {
        String[] names = {
                "sys_vendor",    // 系统厂商  "VMware, Inc." / "QEMU" / "innotek GmbH"
                "product_name",  // 系统型号  "Virtual Machine" / "Standard PC (i440FX...)"
                "board_vendor",  // 主板厂商
                "board_name",    // 主板型号  "440BX Desktop Reference Platform"
                "bios_vendor"    // BIOS 厂商
        };

        StringBuilder sb = new StringBuilder();
        for (String name : names) {
            String value = readFirstLine(new File(dmiDir, name));
            if (!value.isEmpty()) {
                sb.append(value).append('\n');
            }
        }
        return sb.toString();
    }

    /**
     * 解析 {@code reg query ...} 的输出,取出所有 REG_SZ 值并拼接。
     * 包级可见,便于单测。
     *
     * <p>典型输出:</p>
     * <pre>
     * HKEY_LOCAL_MACHINE\HARDWARE\DESCRIPTION\System\BIOS
     *     BaseBoardManufacturer    REG_SZ    Intel Corporation
     *     SystemManufacturer       REG_SZ    VMware, Inc.
     *     SystemProductName        REG_SZ    VMware Virtual Platform
     * </pre>
     * 非 REG_SZ 的项(如 REG_DWORD)会被跳过。
     */
    static String parseWindowsBiosRegistry(String output) {
        if (output == null || output.isEmpty()) {
            return "";
        }

        String[] lines = output.split("\n");
        StringBuilder sb = new StringBuilder();
        for (String line : lines) {
            int marker = line.indexOf("REG_SZ");
            if (marker < 0) {
                continue;
            }
            String value = line.substring(marker + "REG_SZ".length()).trim();
            if (!value.isEmpty()) {
                sb.append(value).append('\n');
            }
        }
        return sb.toString();
    }

    /**
     * 弹窗提示,用户点确定后退出 JVM(正常退出,shutdown hook 会执行)。
     * 包级可见,便于单测。
     */
    static void blockAndExit() {
        // 有桌面环境才弹窗;无头环境(服务器/容器)退化为只打日志,避免抛 HeadlessException
        if (!java.awt.GraphicsEnvironment.isHeadless()) {
            try {
                JOptionPane.showMessageDialog(null, BLOCK_MESSAGE, "AntiHackerX",
                        JOptionPane.ERROR_MESSAGE);
            } catch (Throwable ignored) {
                // 弹窗失败(无显示设备等)就继续走退出流程
            }
        }

        // 控制台只用 ASCII:stdout/stderr 常被上层程序用管道读取,中文可能乱码
        System.err.println("[AntiHackerX] " + BLOCK_MESSAGE);
        System.err.println("[AntiHackerX] virtual machine detected, exiting.");

        System.exit(-1);
    }

    /** 读文件第一行,失败返回空串。 */
    private static String readFirstLine(File file) {
        InputStream in = null;
        try {
            in = new FileInputStream(file);
            BufferedReader reader = new BufferedReader(
                    new InputStreamReader(in, StandardCharsets.UTF_8));
            String line = reader.readLine();
            return line == null ? "" : line.trim();
        } catch (Throwable ignored) {
            return "";
        } finally {
            closeQuietly(in);
        }
    }

    /** 执行命令并返回合并后的输出(含 stderr),失败返回空串。 */
    private static String runCommand(String... command) {
        Process process = null;
        try {
            ProcessBuilder builder = new ProcessBuilder(command);
            builder.redirectErrorStream(true);
            process = builder.start();

            StringBuilder sb = new StringBuilder();
            BufferedReader reader = new BufferedReader(
                    new InputStreamReader(process.getInputStream(), StandardCharsets.UTF_8));
            String line;
            while ((line = reader.readLine()) != null) {
                sb.append(line).append('\n');
            }
            process.waitFor();
            return sb.toString();
        } catch (Throwable ignored) {
            return "";
        } finally {
            if (process != null) {
                process.destroy();
            }
        }
    }

    private static void closeQuietly(InputStream in) {
        if (in == null) {
            return;
        }
        try {
            in.close();
        } catch (Throwable ignored) {
            // 忽略
        }
    }
}
