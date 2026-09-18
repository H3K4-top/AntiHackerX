package fixtures.cli;

public class DefaultGreeter implements Greeter {
    @Override
    public String greet(String name) {
        return "hello-" + name;
    }
}
