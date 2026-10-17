/* NimbusOS version numbers read like decimals: after the first dot the digits are a fraction,
 * so 3.2 is newer than 3.123 (0.200 > 0.123), 3.121 is newer than 3.12, and 3.10 equals 3.1.
 * More dots are ignored ("1.2.3" is read as 1.23).  Returns <0, 0 or >0 like strcmp. */
#ifndef NIMBUS_VERCMP_H
#define NIMBUS_VERCMP_H
static inline int nimbus_version_cmp(const char *a, const char *b)
{
    int x = 0, y = 0;
    while (*a >= '0' && *a <= '9') x = x * 10 + (*a++ - '0');
    while (*b >= '0' && *b <= '9') y = y * 10 + (*b++ - '0');
    if (x != y) return x < y ? -1 : 1;
    for (;;) {
        while (*a && (*a < '0' || *a > '9')) a++;      /* skip dots and anything else */
        while (*b && (*b < '0' || *b > '9')) b++;
        if (!*a && !*b) return 0;
        int da = *a ? *a++ - '0' : 0, db = *b ? *b++ - '0' : 0;
        if (da != db) return da < db ? -1 : 1;
    }
}
#endif
