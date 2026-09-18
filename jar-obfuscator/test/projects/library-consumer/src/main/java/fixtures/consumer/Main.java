package fixtures.consumer;

import fixtures.library.api.LibraryApi;

public class Main {
    public static void main(String[] args) {
        String value = new LibraryApi().render("consumer");
        if (!"library-consumer-24".equals(value)) {
            throw new IllegalStateException(value);
        }
        System.out.println("LIBRARY_OK:" + value);
    }
}
