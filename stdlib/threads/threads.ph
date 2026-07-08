#link "./threads.o";

class thread {
    void join() => thread_join;
    long get() => thread_get;
}

thread run(long fn) => thread_run1;
thread run(long fn, long arg) => thread_run;
