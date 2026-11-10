// dl_cxx.cpp - the C++ plugin darwin_cxx.cpp opens (dl_cxx.dylib): exceptions that start in an
// image loaded late or pass through one, its static objects, its thread-local objects.
#include <stdexcept>
#include <string>
#include <vector>

struct PlugError : std::exception {
    int code;
    explicit PlugError(int c) : code(c) {}
    const char *what() const noexcept override { return "PlugError"; }
};

static int destroyed;
struct Counted { ~Counted() { destroyed++; } };

static std::vector<std::string> words = { "static", "objects", "made" };
static thread_local std::string mine = "thread";

extern "C" {
void cxx_throw(int kind)
{
    Counted c;
    switch (kind) {
    case 0: throw std::runtime_error("from the plugin");
    case 1: throw 7;
    case 2: throw PlugError(9);
    default: throw std::string("a string");
    }
}
// fn's exception passes through this frame (its object is destroyed) and, when again is set,
// is caught and thrown again here
int cxx_call(int (*fn)(int), int v, int again)
{
    Counted c;
    if (!again) return fn(v);
    try { return fn(v); }
    catch (...) { destroyed += 100; throw; }
}
int cxx_destroyed(void) { return destroyed; }
const char *cxx_words(void)
{
    static std::string joined;
    joined.clear();
    for (const std::string &w : words) joined += w + ' ';
    return joined.c_str();
}
const char *cxx_mine(const char *add)
{
    mine += add;
    return mine.c_str();
}
}
