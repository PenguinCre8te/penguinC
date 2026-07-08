#link "./files.o"

# Module-level functions
file open(string path, long mode) => files_open

# File class with resource management
class file {
    enter() -> long => files_enter
    exit() => files_close
    read(long buf, long count) -> long => files_read
    write(long buf, long count) -> long => files_write
    writeStr(string str) -> long => files_write_str
    readAll() -> string => files_read_all
    seek(long offset, long whence) -> long => files_seek
    tell() -> long => files_tell
    eof() -> long => files_eof
    flush() -> long => files_flush
}
