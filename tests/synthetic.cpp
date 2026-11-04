#include "synthetic.hpp"
#include <time.h>
// C clock_gettime, not std::chrono: Clang can't exclude std headers, so chrono inlines would show up as extra hops.
__attribute__((no_instrument_function)) static long ns() {
  timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1000000000L + t.tv_nsec;
}
__attribute__((no_instrument_function)) static void spin(int us) {
  long end = ns() + us * 1000L;
  while (ns() < end) {}
}
__attribute__((noinline)) void child() {
  spin(200);
}
void parent() {
  spin(50);
  child();
  child();
}
__attribute__((noinline)) void thrower() {
  throw 1;
}
__attribute__((noinline)) void catcher() {
  try {
    thrower();
  } catch (int) {}
}
__attribute__((noinline)) int recurse(int n) {  // asm barrier: Clang otherwise turns this into a loop
  if (!n) return 0;
  int r = recurse(n - 1);
  asm volatile("" : "+r"(r));
  return r + 1;
}
void edge() {
  catcher();
  recurse(3);
}
