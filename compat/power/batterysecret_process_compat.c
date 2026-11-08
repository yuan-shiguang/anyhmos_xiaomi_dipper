extern void *dlsym(void *, const char *);
extern int strcmp(const char *, const char *);

static int copy_property(char *value, const char *source)
{
    int n = 0;
    while (source && source[n] && n < 91) {
        value[n] = source[n];
        n++;
    }
    value[n] = 0;
    return n;
}

/* 仅向充电进程提供原机型属性 */
int property_get(const char *key, char *value, const char *fallback)
{
    const char *source = 0;
    if (key && !strcmp(key, "ro.product.device")) {
        source = "dipper";
    } else if (key && !strcmp(key, "ro.product.name")) {
        source = "dipper_global";
    }
    if (source) {
        return copy_property(value, source);
    }

    int (*real)(const char *, char *, const char *) =
        dlsym((void *)-1L, "property_get");
    if (real) {
        return real(key, value, fallback);
    }
    return copy_property(value, fallback);
}

/* 禁止认证过程日志写入内核缓冲区 */
void klog_write(int level, const char *format, ...)
{
    (void)level;
    (void)format;
}
