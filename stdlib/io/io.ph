#link "./io.o";

long print(string s) => io_print;
long print(int val) => io_print_int;
long print(float val) => io_print_float;
long println(string s) => io_println;
long println(int val) => io_println_int;
long println(float val) => io_println_float;
long print_int(int val) => io_print_int;
long println_int(int val) => io_println_int;
long program_exit(long code) => program_exit_c;
long penguin_strlen(string s) => penguin_strlen;
string input() => input;
string str_concat(string a, string b) => penguin_str_concat;
string int_to_string(int val) => int_to_string;
string float_to_string(float val) => float_to_string;
string bool_to_string(bool val) => bool_to_string;
int parse_int(string s) => parse_int;
float parse_float(string s) => parse_float;
