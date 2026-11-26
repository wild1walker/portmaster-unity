// test_arenascan.c: glarena_scan.h's wide scan against its plain one: random index arrays of 1, 2 and 4 bytes,
// every length 0 - 300 at every start offset 0 - 7 (unaligned starts), extremes at the ends, in the tail and in the
// wide part. Exit 0: every answer the same.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../glarena_scan.h"

int main(void)
{
    static unsigned char buf[300 * 4 + 64];
    unsigned long cases = 0, bad = 0, wide = 0;
    srand(12345);
    for (int rep = 0; rep < 20; rep++)
        for (unsigned sz = 1; sz <= 4; sz *= 2)
            for (int n = 0; n <= 300; n++)
                for (int off = 0; off < 8; off++) {
                    for (size_t i = 0; i < sizeof buf; i++) buf[i] = (unsigned char)rand();
                    unsigned char *q = buf + off;
                    if (n && rep % 4 == 1) memset(q, 0xff, sz);                       // the highest first
                    if (n && rep % 4 == 2) memset(q + (size_t)(n - 1) * sz, 0, sz);   // the lowest last
                    if (n && rep % 4 == 3) { memset(q, 0x7f, (size_t)n * sz); q[(size_t)(rand() % n) * sz] = 0x80; }
                    uint32_t l1, h1, l2, h2;
                    scan_plain(q, n, sz, &l1, &h1);
                    scan_wide(q, n, sz, &l2, &h2);
                    cases++; wide += (size_t)n * sz >= 64;
                    if (l1 != l2 || h1 != h2) {
                        if (bad++ < 5) fprintf(stderr, "FAIL sz %u n %d off %d: plain %u-%u wide %u-%u\n", sz, n, off, l1, h1, l2, h2);
                    }
                }
    printf("test_arenascan: %lu cases (%lu through the wide loop), %lu differ\n", cases, wide, bad);
    return bad != 0;
}
