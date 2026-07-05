#link "libc";

// ============================================================================
// 1. POSIX CORE CONSTANTS & MACROS
// ============================================================================

// Standard File Descriptors
const int STDIN_FILENO  = 0; // Standard input stream
const int STDOUT_FILENO = 1; // Standard output stream
const int STDERR_FILENO = 2; // Standard error stream

// File offset positioning (lseek)
const int SEEK_SET = 0; // Seek from the beginning of the file
const int SEEK_CUR = 1; // Seek from the current position indicator
const int SEEK_END = 2; // Seek from the end of the file

// Accessibility check modes (access)
const int F_OK = 0; // Test for existence of file
const int X_OK = 1; // Test for execute permission
const int W_OK = 2; // Test for write permission
const int R_OK = 4; // Test for read permission

// Section locking management (lockf)
const int F_ULOCK = 0; // Unlock a previously locked region
const int F_LOCK  = 1; // Lock a region (blocking if already locked)
const int F_TLOCK = 2; // Test and lock a region (non-blocking)
const int F_TEST  = 3; // Test a region for external locks

// Runtime system configurable variables (sysconf / pathconf)
const int _PC_LINK_MAX           = 1;  // Maximum number of links to a file
const int _PC_MAX_CANON          = 2;  // Maximum capacity of a formatted input line
const int _PC_NAME_MAX           = 3;  // Maximum number of bytes in a filename
const int _PC_PATH_MAX           = 4;  // Maximum number of bytes in a relative pathname
const int _SC_ARG_MAX            = 0;  // Maximum length of arguments to exec functions
const int _SC_CHILD_MAX          = 1;  // Maximum number of simultaneous processes per user ID
const int _SC_CLK_TCK            = 2;  // Number of clock ticks per second
const int _SC_PAGESIZE           = 30; // System memory page size in bytes

// ============================================================================
// 2. FILE & ASYNCHRONOUS DESCRIPTOR I/O
// ============================================================================

// Reads up to 'count' bytes from file descriptor 'fd' into the buffer 'buf_ptr'
long read(long fd, long buf_ptr, long count) => read;

// Writes up to 'count' bytes from the buffer 'buf_ptr' to file descriptor 'fd'
long write(long fd, long buf_ptr, long count) => write;

// Reads from 'fd' at a specific 'offset' without changing the file pointer position
long pread(long fd, long buf_ptr, long count, long offset) => pread;

// Writes to 'fd' at a specific 'offset' without changing the file pointer position
long pwrite(long fd, long buf_ptr, long count, long offset) => pwrite;

// Closes a file descriptor so that it no longer refers to any file
long close(long fd) => close;

// Creates a unidirectional data channel (pipe); allocates two descriptors into 'pipedes_array_ptr'
long pipe(long pipedes_array_ptr) => pipe;

// Repositions the file read/write offset of the file descriptor 'fd'
long lseek(long fd, long offset, long whence) => lseek;

// Allocates a new file descriptor that refers to the same open file description as 'oldfd'
long dup(long oldfd) => dup;

// Allocates a specific 'newfd' descriptor to refer to the same file description as 'oldfd'
long dup2(long oldfd, long newfd) => dup2;

// Similar to dup2, but allows specifying flag bits like O_CLOEXEC
long dup3(long oldfd, long newfd, long flags) => dup3;

// Flushes all modified core data and metadata of a file descriptor to the disk storage
long fsync(long fd) => fsync;

// Flushes modified data of a file descriptor to storage, ignoring metadata changes unless necessary
long fdatasync(long fd) => fdatasync;

// ============================================================================
// 3. FILE SYSTEM & LINK MANIPULATION
// ============================================================================

// Changes the owner user ID and group ID of the file specified by 'pathname_ptr'
long chown(long pathname_ptr, long owner, long group) => chown;

// Changes the owner user ID and group ID of an open file referred to by descriptor 'fd'
long fchown(long fd, long owner, long group) => fchown;

// Changes the owner of a file, but does not follow symbolic links if the path is a symlink
long lchown(long pathname_ptr, long owner, long group) => lchown;

// Changes ownership of a file relative to a directory file descriptor 'fd'
long fchownat(long fd, long pathname_ptr, long owner, long group, long flag) => fchownat;

// Creates a new hard link (also known as a directory entry) to an existing file
long link(long oldpath_ptr, long newpath_ptr) => link;

// Creates a new hard link relative to directory file descriptors
long linkat(long olddirfd, long oldpath_ptr, long newdirfd, long newpath_ptr, long flags) => linkat;

// Deletes a name from the filesystem; if it's the last link, the file is removed
long unlink(long pathname_ptr) => unlink;

// Deletes a directory entry relative to a directory file descriptor 'fd'
long unlinkat(long fd, long pathname_ptr, long flag) => unlinkat;

// Creates a symbolic link named 'linkpath_ptr' which contains the string 'target_ptr'
long symlink(long target_ptr, long linkpath_ptr) => symlink;

// Creates a symbolic link relative to a directory file descriptor 'newdirfd'
long symlinkat(long target_ptr, long newdirfd, long linkpath_ptr) => symlinkat;

// Places the contents of a symbolic link 'pathname_ptr' into the buffer 'buf_ptr'
long readlink(long pathname_ptr, long buf_ptr, long bufsiz) => readlink;

// Places the contents of a symbolic link into a buffer relative to directory descriptor 'fd'
long readlinkat(long fd, long pathname_ptr, long buf_ptr, long bufsiz) => readlinkat;

// Causes the regular file named by 'pathname_ptr' to be truncated to exactly 'length' bytes
long truncate(long pathname_ptr, long length) => truncate;

// Causes the regular file referenced by descriptor 'fd' to be truncated to exactly 'length' bytes
long ftruncate(long fd, long length) => ftruncate;

// Checks whether the calling process can access the file 'pathname_ptr' using given 'mode' bitmasks
long access(long pathname_ptr, long mode) => access;

// Checks file accessibility relative to a directory file descriptor 'fd'
long faccessat(long fd, long pathname_ptr, long mode, long flag) => faccessat;

// ============================================================================
// 4. PROCESS LIFECYCLE & EXECUTION
// ============================================================================

// Creates a new child process by duplicating the calling process
long fork() => fork;

// Optimization fork variant; parent process is suspended until child calls _exit or execve
long vfork() => vfork;

// Executes the binary or script pointed to by 'pathname_ptr', passing 'argv' and environment 'envp'
long execve(long pathname_ptr, long argv_ptr, long envp_ptr) => execve;

// Executes the binary pointed to by 'pathname_ptr', passing null-terminated arguments
long execv(long pathname_ptr, long argv_ptr) => execv;

// Executes a file, searching through the system's PATH environment variable if no slash is in name
long execvp(long file_ptr, long argv_ptr) => execvp;

// Executes a file, combining the PATH directory search behavior with an explicit environment array
long execvpe(long file_ptr, long argv_ptr, long envp_ptr) => execvpe;

// Executes a program loaded into the file descriptor 'fd', passing arguments and environment
long fexecve(long fd, long argv_ptr, long envp_ptr) => fexecve;

// Terminates the calling process immediately, skipping standard C cleanup operations
void _exit(long status) => _exit;

// ============================================================================
// 5. RECONNAISSANCE & SYSTEM METRICS
// ============================================================================

// Returns the process ID (PID) of the calling process
long getpid() => getpid;

// Returns the process ID of the parent of the calling process
long getppid() => getppid;

// Returns the real user ID of the calling process
long getuid() => getuid;

// Returns the effective user ID of the calling process
long geteuid() => geteuid;

// Returns the real group ID of the calling process
long getgid() => getgid;

// Returns the effective group ID of the calling process
long getegid() => getegid;

// Gets the list of supplementary group IDs of the calling process into 'list_ptr'
long getgroups(long size, long list_ptr) => getgroups;

// Returns the process group ID of the calling process
long getpgrp() => getpgrp;

// Returns the process group ID of the process specified by 'pid'
long getpgid(long pid) => getpgid;

// Returns the session ID of the process specified by 'pid'
long getsid(long pid) => getsid;

// Sets the real and effective user ID of the calling process
long setuid(long uid) => setuid;

// Sets the effective user ID of the calling process
long seteuid(long euid) => seteuid;

// Sets the real and effective group ID of the calling process
long setgid(long gid) => setgid;

// Sets the effective group ID of the calling process
long setegid(long egid) => setegid;

// Sets real and effective user IDs cleanly for unprivileged processes
long setreuid(long ruid, long euid) => setreuid;

// Sets real and effective group IDs cleanly for unprivileged processes
long setregid(long rgid, long egid) => setregid;

// Sets real, effective, and saved set-user-ID of the calling process
long setresuid(long ruid, long euid, long suid) => setresuid;

// Sets real, effective, and saved set-group-ID of the calling process
long setresgid(long rgid, long egid, long sgid) => setresgid;

// Sets the process group ID of a specific process 'pid' to 'pgid'
long setpgid(long pid, long pgid) => setpgid;

// Creates a new session if the calling process is not a process group leader
long setsid() => setsid;

// ============================================================================
// 6. DIRECTORIES & THE WORKING ENVIRONMENT
// ============================================================================

// Changes the current working directory of the calling process to 'pathname_ptr'
long chdir(long pathname_ptr) => chdir;

// Changes the current working directory to the directory referenced by descriptor 'fd'
long fchdir(long fd) => fchdir;

// Changes the root directory of the calling process to that specified in 'pathname_ptr'
long chroot(long pathname_ptr) => chroot;

// Copies the absolute pathname of the current working directory into the buffer 'buf_ptr'
long getcwd(long buf_ptr, long size) => getcwd;

// Deletes an empty directory specified by the string 'pathname_ptr'
long rmdir(long pathname_ptr) => rmdir;

// ============================================================================
// 7. ENVIRONMENT USERS & HARDWARE INTERACTIVE
// ============================================================================

// Returns the null-terminated standard host name of the current machine into 'name_ptr'
long gethostname(long name_ptr, long len) => gethostname;

// Sets the host name of the current machine to the value in 'name_ptr' (Requires root privileges)
long sethostname(long name_ptr, long len) => sethostname;

// Gets the NIS domain name of the current machine into 'name_ptr'
long getdomainname(long name_ptr, long len) => getdomainname;

// Sets the NIS domain name of the current machine to 'name_ptr'
long setdomainname(long name_ptr, long len) => setdomainname;

// Get a unique 32-bit identifier for the current internet host hardware
long gethostid() => gethostid;

// Set a unique 32-bit identifier for the current internet host hardware
long sethostid(long hostid) => sethostid;

// Gets the username of the user logged into the terminal process session into 'buf_ptr'
long getlogin_r(long buf_ptr, long bufsize) => getlogin_r;

// Generates a path string for the controlling terminal device file associated with the process
long ctermid(long s_ptr) => ctermid;

// Returns a pointer to a string containing the null-terminated pathname of the terminal 'fd'
long ttyname(long fd) => ttyname;

// Thread-safe variant; stores the pathname of terminal 'fd' in the provided buffer 'buf_ptr'
long ttyname_r(long fd, long buf_ptr, long buflen) => ttyname_r;

// Tests whether file descriptor 'fd' is an open file descriptor referring to a valid terminal
long isatty(long fd) => isatty;

// ============================================================================
// 8. TIMING, DELAYS & ALARMS
// ============================================================================

// Suspends execution of the calling thread for 'seconds' or until a signal arrives
long sleep(long seconds) => sleep;

// Suspends execution of the calling thread for 'usec' microseconds
long usleep(long usec) => usleep;

// Arranges for a SIGALRM signal to be delivered to the calling process in 'seconds'
long alarm(long seconds) => alarm;

// Arranges for a SIGALRM signal to trigger in microseconds, repeating on an interval
long ualarm(long usecs, long interval) => ualarm;

// Causes the calling process or thread to sleep until a signal is delivered to handle
long pause() => pause;

// ============================================================================
// 9. CONFIGURATION & RUNTIME INSPECTION
// ============================================================================

// Gets system configuration parameters at runtime (e.g., number of online CPUs, page size)
long sysconf(long name) => sysconf;

// Gets configuration values for properties of a specific file or directory path
long pathconf(long pathname_ptr, long name) => pathconf;

// Gets configuration values for properties of a file referenced by descriptor 'fd'
long fpathconf(long fd, long name) => fpathconf;

// Gets structural configuration string variables (like the default execution PATH definition)
long confstr(long name, long buf_ptr, long len) => confstr;

// ============================================================================
// 10. SYSTEM MEMORY TUNING & FILE LOCKING
// ============================================================================

// Changes the location of the program break (end of data segment heap boundary) to 'addr_ptr'
long brk(long addr_ptr) => brk;

// Increments the program data space (heap allocation) by 'increment' bytes
long sbrk(long increment) => sbrk;

// Applies, tests, or removes a POSIX advisory lock on a section of an open file
long lockf(long fd, long cmd, long len) => lockf;

// Causes all buffered modifications to file data and metadata to be flushed to underlying filesystems
long sync() => sync;

// Synchronizes all data and metadata buffered structures specific to file descriptor 'fd's filesystem
long syncfs(long fd) => syncfs;

// Alters the scheduling priority (nice value) of the calling process by 'inc' amount
long nice(long inc) => nice;