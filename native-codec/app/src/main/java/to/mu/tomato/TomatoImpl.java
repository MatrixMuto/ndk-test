package to.mu.tomato;

/**
 * Created by wq1950 on 16-8-4.
 */

public class TomatoImpl implements Tomato {

    @Override
    public void prepare(String uri) {
        test();
    }

    @Override
    public void release() {

    }

    private native void test();
}
