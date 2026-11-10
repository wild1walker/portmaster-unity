// native_main.c - runs a test image's test_main on macOS itself: what the loader's run of it
// is compared with.
long test_main(long argc, char **argv);
int main(int argc, char **argv) { return (int)test_main(argc, argv); }
