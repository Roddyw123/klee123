// RUN: %clang %s -emit-llvm %O0opt -c -o %t.bc
// RUN: rm -rf %t.klee-out
// RUN: %klee --output-dir=%t.klee-out --exit-on-error %t.bc 2>&1 | FileCheck %s

// Test klee_make_symbolic_bytes: only masked bytes become symbolic,
// unmasked bytes keep their concrete values.

#include "klee/klee.h"
#include <assert.h>
#include <stdio.h>

int main() {
  // Initialize buffer with known concrete values
  char buf[6] = "HELLO";  // 'H','E','L','L','O','\0'

  // Mask: only bytes 3 and 4 ('L' and 'O') become symbolic
  unsigned char mask[6] = {0, 0, 0, 1, 1, 0};

  klee_make_symbolic_bytes(buf, 6, "buf", mask, 6);

  // Concrete bytes must keep their original values
  assert(buf[0] == 'H');  // concrete — always true
  assert(buf[1] == 'E');  // concrete — always true
  assert(buf[2] == 'L');  // concrete — always true
  assert(buf[5] == '\0'); // concrete — always true

  // Symbolic bytes: KLEE should fork here
  if (buf[3] == 'X') {
    printf("path_X\n");
    // CHECK-DAG: path_X
  }
  if (buf[4] == 'Y') {
    printf("path_Y\n");
    // CHECK-DAG: path_Y
  }

  return 0;
}
