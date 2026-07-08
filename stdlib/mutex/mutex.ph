#link "./mutex.o";
#link "pthread";

class mutex {
    void enter() => mutex_enter;
    void exit() => mutex_exit;
    long tryEnter() => mutex_try_enter;
    long getLockCount() => mutex_get_lock_count;
}

long create() => mutex_create;
void destroy(long handle) => mutex_destroy;
