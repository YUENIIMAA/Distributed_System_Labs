package server.storage.instance;

import server.storage.kvStorage;

public class kvStorage02 {
    public static void main(String[] argv) {
        kvStorage instance = new kvStorage("Storage-02", 10002, true, "Storage-00", 1);
        instance.run();
    }
}
