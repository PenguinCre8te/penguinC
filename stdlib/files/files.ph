#link "./files.o"

# Module-level functions
long open(string path, long mode) => files_open

# File class with resource management
class file {
    long enter(long self) => files_enter
    void exit(long self) => files_close
    long read(long self, long buf, long count) => files_read
    long write(long self, long buf, long count) => files_write
    long writeStr(long self, string str) => files_write_str
    string readAll(long self) => files_read_all
    long seek(long self, long offset, long whence) => files_seek
    long tell(long self) => files_tell
    long eof(long self) => files_eof
    long flush(long self) => files_flush
}
