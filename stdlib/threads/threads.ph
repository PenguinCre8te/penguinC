#link "./threads.o";

class thread {
    void join() => thread_join;
    long get() => thread_get;
}

long run(long fn) => thread_run1;
long run(long fn, long arg) => thread_run;
