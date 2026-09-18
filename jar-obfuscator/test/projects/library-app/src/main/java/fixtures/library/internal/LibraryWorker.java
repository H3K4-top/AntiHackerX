package fixtures.library.internal;

public class LibraryWorker {
    private final String prefix = "library";

    public String renderInternal(String name) {
        return prefix + "-" + name + "-" + (17 ^ 9);
    }
}
