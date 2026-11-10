// darwin_cxx.cpp - a C++ test of the loader and the shims, built on a Mac against the system's
// libc++ (as a game's code is) and run on Linux with the libc++ and libc++abi built as Mach-O
// images (build-cxx-macho.sh) loaded before it. Its output is the same on both.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <exception>
#include <fstream>
#include <functional>
#include <future>
#include <iomanip>
#include <iostream>
#include <locale>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <typeinfo>
#include <unordered_map>
#include <vector>
#include <unistd.h>

static int failed;
static void check(const char *name, bool ok)
{
    failed += !ok;
    std::cout << (ok ? "ok   " : "FAIL ") << name << "\n";
}

struct Base { virtual ~Base() {} virtual int id() const { return 1; } };
struct Left : virtual Base { int id() const override { return 2; } };
struct Right : virtual Base { int id() const override { return 3; } };
struct Both : Left, Right { int id() const override { return 4; } };

struct Noisy {
    static std::atomic<int> alive;
    std::string tag;
    explicit Noisy(std::string t) : tag(std::move(t)) { alive++; }
    ~Noisy() { alive--; }
};
std::atomic<int> Noisy::alive;

struct MyError : std::runtime_error {
    int code;
    MyError(const std::string &what, int c) : std::runtime_error(what), code(c) {}
};

__attribute__((noinline)) static int thrower(int depth, int kind)
{
    Noisy n("frame");
    if (depth > 0) return thrower(depth - 1, kind) + 1;
    switch (kind) {
    case 0: throw 42;
    case 1: throw MyError("mine", 7);
    case 2: throw std::out_of_range("range");
    case 3: throw std::string("text");
    case 4: return std::stoi("not a number");
    case 5: { std::vector<int> v(2); return v.at(9); }
    case 6: throw std::bad_alloc();
    }
    return 0;
}

static std::string catcher(int kind)
{
    std::ostringstream o;
    try {
        try {
            thrower(12, kind);
            o << "nothing";
        } catch (const MyError &e) {
            o << "MyError " << e.what() << " " << e.code << ", again: ";
            throw;
        }
    } catch (int v) {
        o << "int " << v;
    } catch (const std::out_of_range &e) {
        o << "out_of_range";
    } catch (const std::logic_error &e) {
        o << "logic_error";
    } catch (const std::runtime_error &e) {
        o << "runtime_error " << e.what();
    } catch (const std::exception &e) {
        o << "exception";
    } catch (...) {
        o << "other";
    }
    o << " (alive " << Noisy::alive << ")";
    return o.str();
}

static std::atomic<int> tls_made, tls_gone;
struct Tls {
    int v = 5;
    Tls() { tls_made++; }
    ~Tls() { tls_gone++; }
};
static thread_local Tls tls;
static thread_local int tls_plain = 11;
static thread_local std::string tls_text = "init";

static void test_exceptions()
{
    for (int k = 0; k < 7; k++) std::cout << "throw " << k << ": " << catcher(k) << "\n";
    std::exception_ptr p;
    try { throw MyError("kept", 3); } catch (...) { p = std::current_exception(); }
    try { std::rethrow_exception(p); } catch (const MyError &e) { std::cout << "exception_ptr " << e.what() << " " << e.code << "\n"; }
    try {
        try { throw std::runtime_error("inner"); }
        catch (...) { std::throw_with_nested(std::logic_error("outer")); }
    } catch (const std::logic_error &e) {
        std::cout << "nested " << e.what();
        try { std::rethrow_if_nested(e); } catch (const std::exception &in) { std::cout << " <- " << in.what(); }
        std::cout << "\n";
    }
    std::cout << "uncaught " << std::uncaught_exceptions() << "\n";

    std::unique_ptr<Base> b(new Both);
    Left *l = dynamic_cast<Left *>(b.get());
    Right *r = dynamic_cast<Right *>(b.get());
    check("dynamic_cast", l && r && dynamic_cast<Both *>(l) == dynamic_cast<Both *>(r) && b->id() == 4);
    Base plain;
    check("dynamic_cast fails", dynamic_cast<Left *>(&plain) == nullptr);
    try { (void)dynamic_cast<Both &>(plain); check("bad_cast", false); } catch (const std::bad_cast &) { check("bad_cast", true); }
    std::cout << "typeid " << typeid(*l).name() << " " << (typeid(*l) == typeid(Both)) << "\n";
    volatile size_t huge = size_t(1) << 60;
    char *volatile none = new (std::nothrow) char[huge];
    check("new nothrow huge", none == nullptr);
    try { none = new char[huge]; check("bad_alloc", false); } catch (const std::bad_alloc &) { check("bad_alloc", true); }
    try { std::function<void()> f; f(); check("bad_function_call", false); } catch (const std::bad_function_call &) { check("bad_function_call", true); }
}

static void test_streams()
{
    std::ostringstream o;
    o << 42 << ' ' << -7L << ' ' << 3.5 << ' ' << 1e100 << ' ' << std::hex << 255 << std::dec << ' ' << std::setw(6) << std::setfill('*') << 12
      << ' ' << std::fixed << std::setprecision(3) << 2.0 / 3 << ' ' << true << std::boolalpha << false << ' ' << (void *)0x10 << ' ' << 1.5f
      << ' ' << (long double)2.25 << ' ' << 18446744073709551615ull;
    std::cout << "ostream " << o.str() << "\n";
    std::istringstream in("17 2.5 word 0x1f -3 1e3");
    int i = 0, neg = 0; double d = 0, e = 0; std::string w; unsigned h = 0;
    in >> i >> d >> w >> std::hex >> h >> std::dec >> neg >> e;
    std::cout << "istream " << i << ' ' << d << ' ' << w << ' ' << h << ' ' << neg << ' ' << e << ' ' << in.eof() << in.fail() << "\n";
    std::istringstream bad("x");
    bad >> i;
    std::cout << "istream bad " << bad.fail() << "\n";
    std::wostringstream wo;
    wo << L"wide " << 12 << L' ' << 1.25;
    std::cout << "wostream " << wo.str().size() << "\n";
    std::cout << "to_string " << std::to_string(123) << ' ' << std::to_string(1.5) << ' ' << std::stod("2.75") << ' ' << std::stoul("ff", nullptr, 16)
              << ' ' << std::stoll("-9000000000") << "\n";
    std::cout << "locale " << std::locale().name() << ' ' << std::use_facet<std::numpunct<char>>(std::locale()).decimal_point() << ' '
              << std::isalpha('x', std::locale()) << std::toupper('x', std::locale()) << "\n";
    try { std::locale named("en_US.UTF-8"); std::cout << "named locale " << named.name() << "\n"; }
    catch (const std::runtime_error &) { std::cout << "named locale refused\n"; }

    std::string path = "/tmp/ua-darwin-cxx-" + std::to_string(getpid());
    { std::ofstream f(path); f << "line one\n" << 99 << "\n"; }
    { std::ifstream f(path); std::string line; int n = 0; std::getline(f, line); f >> n;
      std::cout << "fstream " << line << ' ' << n << ' ' << f.good() << "\n"; }
    { std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary); f.seekg(0, std::ios::end); auto size = (long)f.tellg();
      f.seekp(5); f.write("ONE", 3); f.seekg(0); std::string all((std::istreambuf_iterator<char>(f)), {}); 
      std::cout << "fstream size " << size << ' ' << all.substr(0, 8) << "\n"; }
    std::remove(path.c_str());
    std::ifstream none(path);
    std::cout << "fstream missing " << none.is_open() << "\n";
    std::cerr.flush();
    std::printf("printf with cout\n");
    std::cout << std::flush;
}

static void test_containers()
{
    std::map<std::string, int> m;
    std::unordered_map<int, std::string> u;
    std::vector<int> v;
    std::mt19937 rng(12345);
    for (int i = 0; i < 2000; i++) { int r = (int)(rng() % 1000); v.push_back(r); m["k" + std::to_string(r)]++; u[r] = std::to_string(i); }
    std::sort(v.begin(), v.end());
    long sum = 0;
    for (auto &kv : m) sum = sum * 31 + kv.second + (long)kv.first.size();
    std::cout << "containers " << m.size() << ' ' << u.size() << ' ' << v[1000] << ' ' << sum << ' ' << rng() << "\n";
    std::uniform_real_distribution<double> dist(0, 1);
    std::cout << "distribution " << std::setprecision(10) << dist(rng) << "\n";
    std::shared_ptr<Noisy> a = std::make_shared<Noisy>("shared"), b = a;
    std::weak_ptr<Noisy> w = a;
    a.reset();
    std::cout << "shared_ptr " << b.use_count() << ' ' << w.expired();
    b.reset();
    std::cout << ' ' << w.expired() << ' ' << Noisy::alive << "\n";
    std::string big(100000, 'x');
    big += std::string(50, 'y');
    std::cout << "string " << big.size() << ' ' << big.find('y') << ' ' << std::hash<std::string>()("abc") << "\n";
}

static void test_threads()
{
    std::mutex mu;
    std::condition_variable cv;
    long total = 0;
    int turn = 0;
    std::vector<std::thread> ts;
    for (int i = 0; i < 6; i++)
        ts.emplace_back([&, i] {
            tls.v += i;
            tls_plain += i;
            tls_text += std::to_string(i);
            for (int k = 0; k < 5000; k++) { std::lock_guard<std::mutex> g(mu); total += i; }
            std::unique_lock<std::mutex> g(mu);
            cv.wait(g, [&] { return turn == i; });
            total += tls.v * 1000000L + tls_plain + (long)tls_text.size();
            turn++;
            cv.notify_all();
        });
    for (auto &t : ts) t.join();
    std::cout << "threads " << total << ' ' << turn << ' ' << tls.v << ' ' << tls_plain << ' ' << tls_text << "\n";
    std::cout << "thread_local objects " << tls_made.load() << ' ' << tls_gone.load() << "\n";

    std::unique_lock<std::mutex> g(mu);
    auto t0 = std::chrono::steady_clock::now();
    bool hit = cv.wait_for(g, std::chrono::milliseconds(40), [] { return false; });
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    check("wait_for times out", !hit && ms >= 38 && ms < 1000);
    g.unlock();
    t0 = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    check("sleep_for", ms >= 30 && ms < 1000);
    check("system_clock", std::chrono::system_clock::now().time_since_epoch() > std::chrono::hours(24 * 365 * 50));

    std::atomic<int> flag{0};
    std::atomic<int> seen{0};
    std::vector<std::thread> waiters;
    for (int i = 0; i < 3; i++) waiters.emplace_back([&] { flag.wait(0); seen += flag.load(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    flag.store(5);
    flag.notify_all();
    for (auto &t : waiters) t.join();
    std::cout << "atomic wait " << seen.load() << "\n";

    std::promise<int> pr;
    std::future<int> fu = pr.get_future();
    std::thread setter([&] { std::this_thread::sleep_for(std::chrono::milliseconds(10)); pr.set_value(77); });
    std::cout << "future " << fu.get();
    setter.join();
    auto as = std::async(std::launch::async, [] { thrower(3, 1); return 1; });
    try { as.get(); } catch (const MyError &e) { std::cout << " async threw " << e.what(); }
    std::once_flag once;
    int calls = 0;
    std::vector<std::thread> oncers;
    for (int i = 0; i < 4; i++) oncers.emplace_back([&] { std::call_once(once, [&] { calls++; }); });
    for (auto &t : oncers) t.join();
    std::cout << " once " << calls << " cores " << (std::thread::hardware_concurrency() > 0) << "\n";

    // an exception thrown and caught in many threads at once
    std::atomic<long> caught{0};
    std::vector<std::thread> throwers;
    for (int i = 0; i < 4; i++)
        throwers.emplace_back([&] {
            for (int k = 0; k < 2000; k++) try { thrower(k % 8, k % 4); } catch (const std::exception &) { caught++; } catch (...) { caught += 1000; }
        });
    for (auto &t : throwers) t.join();
    std::cout << "threads throwing " << caught.load() << " alive " << Noisy::alive << "\n";
}

struct Global {
    std::string s;
    Global() : s("constructed") {}
};
static Global global;
static std::vector<int> global_v = {1, 2, 3};

extern "C" long test_main(long argc, char **argv)
{
    std::cout << "statics " << global.s << ' ' << global_v.size() << "\n";
    test_exceptions();
    test_streams();
    test_containers();
    test_threads();
    std::cout << "failed " << failed << std::endl;
    return failed;
}
