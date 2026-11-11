#undef assert

#ifdef NDEBUG
#define assert(condition) ((void)0)
#else
[[noreturn]] void __assert_fail(const char *condition, const char *file, int line, const char *function);
#define assert(condition) \
    ((condition) ? (void)0 : __assert_fail(#condition, __FILE__, __LINE__, __func__))
#endif
