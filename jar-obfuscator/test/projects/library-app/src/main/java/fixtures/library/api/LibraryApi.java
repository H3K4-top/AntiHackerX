package fixtures.library.api;

import fixtures.library.internal.LibraryWorker;

public class LibraryApi {
    public String render(String name) {
        return new LibraryWorker().renderInternal(name);
    }
}
