package fixtures.swing;

import javax.swing.JButton;
import javax.swing.JLabel;
import javax.swing.JPanel;

public class Main {
    public static void main(String[] args) {
        JPanel panel = new JPanel();
        JLabel label = new JLabel("before");
        JButton button = new JButton("run");
        button.addActionListener(event -> label.setText("after"));
        panel.add(label);
        panel.add(button);
        button.doClick();
        if (panel.getComponentCount() != 2 || !"after".equals(label.getText())) {
            throw new IllegalStateException("Swing integrity check failed");
        }
        System.out.println("SWING_OK:" + label.getText());
    }
}
