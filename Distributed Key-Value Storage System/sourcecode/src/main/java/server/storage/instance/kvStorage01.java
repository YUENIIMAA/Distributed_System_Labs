package server.storage.instance;

import server.storage.kvStorage;

public class kvStorage01 {
    public static void main(String[] argv) {
        kvStorage instance = new kvStorage("Storage-01", 10001, false, 1);
        instance.run();
    }
}
