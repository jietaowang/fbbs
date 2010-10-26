#ifndef FB_STRING_H
#define FB_STRING_H

#include <stdlib.h>

#define streq(a, b)  (!strcmp(a, b))

extern char *trim(char *str);

extern size_t strlcpy(char *dst, const char *src, size_t siz);

extern size_t mbwidth(const char *s);

#endif // FB_STRING_H

