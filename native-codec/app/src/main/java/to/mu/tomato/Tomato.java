package to.mu.tomato;

/**
 * Created by wq1950 on 16-8-4.
 */

public interface Tomato {

    class Factory {
        public static Tomato newInstance() {
            return new TomatoImpl();
        }
    }

    void prepare(String uri);

    void release();
}
