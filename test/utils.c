#include "utils.h"

char *i2b(uint32_t value, char *str)
{
    char *s = str;
    for (int i = 7; i >= 0; --i) {
        if ((value >> i) & 1) {
            *s++ = '1';
        } else {
            *s++ = '0';
        }
        *s = '\0';
    }
    return str;
}
