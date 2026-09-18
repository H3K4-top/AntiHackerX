/*
 * AntiHackerX 示例模块 —— 以 MIT 许可证提供,便于嵌入你自己的程序。
 *
 * SPDX-License-Identifier: MIT
 * 许可证全文见仓库中的 AntiHackerXVerify/LICENSE。
 */
package top.h3k4;

public class unpassMain
{
    public static void main(String[] args)
    {
        // 反虚拟机:检测到运行在虚拟机内则弹窗提示,点确定后 exit(-1) 正常退出
        VirtualMachineGuard.check();

        // 反 agent:启动参数里带 -javaagent / -agentlib / -agentpath 直接崩
        AgentGuard.check();

        // 反调试:第一时间启动计时器守卫(越早越难被绕过)
        DebugTimerGuard.start();

        // 有桌面环境时弹出版权对话框;服务器等无桌面环境自动改为打印日志。
        // 内部已做检测与降级,不会抛异常、不会阻塞无头环境。
        CopyrightNotice.show();

        System.out.println("AntiHackerX is loaded");

    }
}
