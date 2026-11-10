// sh_fmt.inc - a printf or scanf format as glibc is to read it, for char and for wchar_t:
// sh_stdio.c includes this twice, with CH, FMT_PASS and FMT_FN defined. What macOS's libc
// reads differently:
//   - long double is double there. printf: an L is dropped (the argument is a double; before
//     an integer conversion macOS ignores the L, glibc would read a long long). scanf: "%Lf"
//     stores a double, so it becomes "%lf"; before another conversion the L is dropped;
//   - printf("%p") of NULL prints 0x0 there, (nil) here: a %p with no flags, width or
//     precision becomes 0x%lx (one with them is left to glibc: only NULL's text differs);
//   - %D %O %U are %ld %lo %lu (an h before them loses to the long, as it does there).
// The rest (positional arguments, %C and %S, q, the scan sets) glibc reads the same way.
// Not covered: a NaN with its sign bit set prints "-nan" here, "nan" there.

// Writes the format to out (NULL: counts only); the length without the closing 0.
static size_t FMT_PASS(const CH *f, int scan, CH *out, int *changed)
{
    size_t n = 0;
#define EMIT(c) do { if (out) out[n] = (CH)(c); n++; } while (0)
    while (*f) {
        if (*f != '%') { EMIT(*f); f++; continue; }
        const CH *start = f++;
        if (*f == '%') { EMIT('%'); EMIT('%'); f++; continue; }
        const CH *p = f;
        while (*p == ' ' || *p == '#' || *p == '+' || *p == '-' || *p == '\'' || *p == '*' || *p == '$' || *p == '.' ||
               (*p >= '0' && *p <= '9')) p++;
        const CH *len = p;
        int sized = 0;                      // a length that says more than int: D, O and U add no l to it
        while (*p == 'h' || *p == 'l' || *p == 'j' || *p == 'z' || *p == 't' || *p == 'L' || *p == 'q') {
            if (*p != 'h' && *p != 'L') sized = 1;
            p++;
        }
        CH conv = *p;
        if (!conv) {                        // a format that ends inside a conversion: as it is
            for (; start < p; start++) EMIT(*start);
            f = p;
            break;
        }
        if (!scan && conv == 'p' && p == f) {
            EMIT('0'); EMIT('x'); EMIT('%'); EMIT('l'); EMIT('x');
            *changed = 1;
            f = p + 1;
            continue;
        }
        int real = conv == 'a' || conv == 'A' || conv == 'e' || conv == 'E' || conv == 'f' || conv == 'F' ||
                   conv == 'g' || conv == 'G';
        int compat = conv == 'D' || conv == 'O' || conv == 'U';
        for (const CH *q = start; q < len; q++) EMIT(*q);
        for (const CH *q = len; q < p; q++) {
            if (*q == 'L') {
                *changed = 1;
                if (scan && real) EMIT('l');
                continue;
            }
            if (*q == 'h' && compat) continue;
            EMIT(*q);
        }
        if (compat) {
            if (!sized) EMIT('l');
            EMIT(conv == 'D' ? 'd' : conv == 'O' ? 'o' : 'u');
            *changed = 1;
        } else {
            EMIT(conv);
        }
        f = p + 1;
        if (scan && conv == '[') {          // the set: a ] right after [ or [^ is a member of it
            if (*f == '^') { EMIT(*f); f++; }
            if (*f == ']') { EMIT(*f); f++; }
            while (*f && *f != ']') { EMIT(*f); f++; }
            if (*f == ']') { EMIT(*f); f++; }
        }
    }
#undef EMIT
    return n;
}

const CH *FMT_FN(const CH *fmt, int scan, CH *buf, size_t buflen)
{
    int changed = 0;
    size_t n = FMT_PASS(fmt, scan, NULL, &changed);
    if (!changed) return fmt;
    CH *out = n + 1 <= buflen ? buf : malloc((n + 1) * sizeof(CH));
    if (!out) return NULL;
    FMT_PASS(fmt, scan, out, &changed);
    out[n] = 0;
    return out;
}
