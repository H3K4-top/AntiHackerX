package fixtures.cli;

public class Worker {
    private int secretValue = 41;

    public int calculateInternal(int calculateInput) {
        int localResult = calculateInput * 2 + secretValue + 314159;
        return localResult;
    }
}
