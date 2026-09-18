package fixtures.war;

public class WarEntry {
    public static void main(String[] args) {
        String value = "war-" + (21 * 2);
        if (!"war-42".equals(value)) {
            throw new IllegalStateException(value);
        }
        System.out.println("WAR_OK:" + value);
    }
}
