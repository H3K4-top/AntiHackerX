package fixtures.cli;

import java.io.IOException;
import java.io.InputStream;
import java.util.Properties;
import java.util.ServiceLoader;

public class Main {
    public static void main(String[] args) throws Exception {
        Worker worker = new Worker();
        int result = worker.calculateInternal(7);
        String greeting = ServiceLoader.load(Greeter.class).iterator().next().greet("codex");
        String dynamic = loadDynamicMessage();
        if (result != 314214 || !"hello-codex".equals(greeting) || !"dynamic-resource-ok".equals(dynamic)) {
            throw new IllegalStateException("CLI integrity check failed");
        }
        System.out.println("CLI_OK:" + result + ":" + greeting + ":" + dynamic + ":SUPER_SECRET_MESSAGE");
    }

    private static String loadDynamicMessage() throws Exception {
        Properties properties = new Properties();
        try (InputStream input = Main.class.getResourceAsStream("/META-INF/dynamic.properties")) {
            if (input == null) {
                throw new IOException("missing dynamic.properties");
            }
            properties.load(input);
        }
        Class<?> type = Class.forName(properties.getProperty("dynamic.class"));
        Object instance = type.newInstance();
        return (String) type.getMethod("value").invoke(instance);
    }
}
