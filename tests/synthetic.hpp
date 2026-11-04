#pragma once
void parent();  // self ~50us + 2 x child() ~200us each
void edge();    // exception unwinding + 3-deep recursion
void child();   // ~200us
int recurse(int n);
