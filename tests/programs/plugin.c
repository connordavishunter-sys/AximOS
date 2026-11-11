// Opened with dlopen by libs.c, and calls back into it.
int program_callback(int);

int plugin_run(int x) {
    return program_callback(x) * 10;
}
