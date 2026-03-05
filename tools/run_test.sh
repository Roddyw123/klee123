#!/bin/bash
set -e

echo "========================================"
echo "  TEST 1.1: klee_make_symbolic_bytes"
echo "========================================"

cat > /tmp/test_msb.c << 'EOF'
#include "klee/klee.h"
#include <assert.h>
#include <stdio.h>
int main() {
  char buf[5] = { 'H', 'E', 'L', 'L', 'O' };
  unsigned char mask[5] = { 0, 1, 0, 1, 0 };
  klee_make_symbolic_bytes(buf, 5, "mixed_buf", mask, 5);
  assert(buf[0] == 'H');
  assert(buf[2] == 'L');
  assert(buf[4] == 'O');
  if (buf[1] == 'X' && buf[3] == 'Y') printf("found: HXLYO\n");
  if (buf[1] == 'A' && buf[3] == 'B') printf("found: HALBO\n");
  return 0;
}
EOF

cd /tmp
clang -emit-llvm -c -g -I /tmp/klee_src/include test_msb.c -o test_msb.bc
klee test_msb.bc
echo "--- Sample test case ---"
ktest-tool /tmp/klee-out-0/test000001.ktest

echo ""
echo "========================================"
echo "  TEST 1.2: --sym-template-stdin"
echo "========================================"

echo -n 'SELECT * FROM ?????' > /tmp/query.tpl

cat > /tmp/test_tpl.c << 'EOF'
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
int main() {
  char buf[64];
  int n = read(0, buf, sizeof(buf));
  assert(n == 19);
  assert(memcmp(buf, "SELECT * FROM ", 14) == 0);
  if (buf[14]=='u' && buf[15]=='s' && buf[16]=='e' && buf[17]=='r' && buf[18]=='s')
    printf("found: users\n");
  if (buf[14]=='i' && buf[15]=='t' && buf[16]=='e' && buf[17]=='m' && buf[18]=='s')
    printf("found: items\n");
  return 0;
}
EOF

clang -emit-llvm -c -g test_tpl.c -o test_tpl.bc
klee --posix-runtime --libc=uclibc test_tpl.bc --sym-template-stdin /tmp/query.tpl
echo "--- Sample test case ---"
ktest-tool /tmp/klee-out-1/test000001.ktest

echo ""
echo "========================================"
echo "  TEST 3.1: ?{digit} constraint"
echo "========================================"

echo -n 'id=?{digit}?{digit}?{digit}' > /tmp/constraint_digit.tpl

cat > /tmp/test_digit.c << 'EOF'
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
int main() {
  char buf[16];
  int n = read(0, buf, sizeof(buf));
  assert(n == 6);
  assert(buf[0] == 'i' && buf[1] == 'd' && buf[2] == '=');
  /* klee_assume guarantees these are digits */
  assert(buf[3] >= '0' && buf[3] <= '9');
  assert(buf[4] >= '0' && buf[4] <= '9');
  assert(buf[5] >= '0' && buf[5] <= '9');
  if (buf[3] == '4' && buf[4] == '2' && buf[5] == '0')
    printf("found: 420\n");
  return 0;
}
EOF

clang -emit-llvm -c -g test_digit.c -o test_digit.bc
klee --posix-runtime --libc=uclibc test_digit.bc --sym-template-stdin /tmp/constraint_digit.tpl
echo "--- Sample test case ---"
ktest-tool /tmp/klee-out-2/test000001.ktest

echo ""
echo "========================================"
echo "  TEST 3.2: ?{range:65-90} (uppercase)"
echo "========================================"

echo -n '?{range:65-90}?{range:65-90}?{range:65-90}' > /tmp/constraint_range.tpl

cat > /tmp/test_range.c << 'EOF'
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
int main() {
  char buf[8];
  int n = read(0, buf, sizeof(buf));
  assert(n == 3);
  /* klee_assume guarantees uppercase */
  assert(buf[0] >= 'A' && buf[0] <= 'Z');
  assert(buf[1] >= 'A' && buf[1] <= 'Z');
  assert(buf[2] >= 'A' && buf[2] <= 'Z');
  if (buf[0] == 'A' && buf[1] == 'B' && buf[2] == 'C')
    printf("found: ABC\n");
  return 0;
}
EOF

clang -emit-llvm -c -g test_range.c -o test_range.bc
klee --posix-runtime --libc=uclibc test_range.bc --sym-template-stdin /tmp/constraint_range.tpl
echo "--- Sample test case ---"
ktest-tool /tmp/klee-out-3/test000001.ktest

echo ""
echo "========================================"
echo "  TEST 3.3: ?{set:} constraint"
echo "========================================"

printf 'op=?{set:+-*/}' > /tmp/constraint_set.tpl

cat > /tmp/test_set.c << 'EOF'
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
int main() {
  char buf[8];
  int n = read(0, buf, sizeof(buf));
  assert(n == 4);
  assert(buf[0] == 'o' && buf[1] == 'p' && buf[2] == '=');
  /* klee_assume guarantees one of +, -, *, / */
  assert(buf[3] == '+' || buf[3] == '-' || buf[3] == '*' || buf[3] == '/');
  if (buf[3] == '+') printf("found: +\n");
  if (buf[3] == '-') printf("found: -\n");
  if (buf[3] == '*') printf("found: *\n");
  if (buf[3] == '/') printf("found: /\n");
  return 0;
}
EOF

clang -emit-llvm -c -g test_set.c -o test_set.bc
klee --posix-runtime --libc=uclibc test_set.bc --sym-template-stdin /tmp/constraint_set.tpl
echo "--- Sample test case ---"
ktest-tool /tmp/klee-out-4/test000001.ktest

echo ""
echo "========================================"
echo "  TEST 3.4: backward compat (plain ?)"
echo "========================================"

echo -n 'Hi ????!' > /tmp/compat.tpl

cat > /tmp/test_compat.c << 'EOF'
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
int main() {
  char buf[16];
  int n = read(0, buf, sizeof(buf));
  assert(n == 8);
  assert(buf[0] == 'H' && buf[1] == 'i' && buf[2] == ' ');
  assert(buf[7] == '!');
  if (buf[3] == 'K' && buf[4] == 'L' && buf[5] == 'E' && buf[6] == 'E')
    printf("found: KLEE\n");
  return 0;
}
EOF

clang -emit-llvm -c -g test_compat.c -o test_compat.bc
klee --posix-runtime --libc=uclibc test_compat.bc --sym-template-stdin /tmp/compat.tpl
echo "--- Sample test case ---"
ktest-tool /tmp/klee-out-5/test000001.ktest

echo ""
echo "========================================"
echo "  TEST 4.1: Hex template basic"
echo "========================================"

cat > /tmp/basic.btpl << 'EOF'
# PNG magic + 4 symbolic bytes
89 50 4E 47 ?? ?? ?? ??
EOF

cat > /tmp/test_hex_basic.c << 'EOF'
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
int main() {
  unsigned char buf[16];
  int n = read(0, buf, sizeof(buf));
  assert(n == 8);
  assert(buf[0] == 0x89);
  assert(buf[1] == 0x50);
  assert(buf[2] == 0x4E);
  assert(buf[3] == 0x47);
  if (buf[4] == 0x00 && buf[5] == 0x00 && buf[6] == 0x00 && buf[7] == 0x0D)
    printf("found: IHDR length 13\n");
  return 0;
}
EOF

clang -emit-llvm -c -g test_hex_basic.c -o test_hex_basic.bc
klee --posix-runtime --libc=uclibc test_hex_basic.bc --sym-hex-template-stdin /tmp/basic.btpl
echo "--- Sample test case ---"
ktest-tool /tmp/klee-out-6/test000001.ktest

echo ""
echo "========================================"
echo "  TEST 4.2: Hex template with constraints"
echo "========================================"

cat > /tmp/constrained.btpl << 'EOF'
CA FE ?{range:1-9} ?{range:0-99}
EOF

cat > /tmp/test_hex_constrained.c << 'EOF'
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
int main() {
  unsigned char buf[8];
  int n = read(0, buf, sizeof(buf));
  assert(n == 4);
  assert(buf[0] == 0xCA);
  assert(buf[1] == 0xFE);
  assert(buf[2] >= 1 && buf[2] <= 9);
  assert(buf[3] >= 0 && buf[3] <= 99);
  if (buf[2] == 1 && buf[3] == 0)
    printf("found: version 1.0\n");
  if (buf[2] == 2 && buf[3] == 42)
    printf("found: version 2.42\n");
  return 0;
}
EOF

clang -emit-llvm -c -g test_hex_constrained.c -o test_hex_constrained.bc
klee --posix-runtime --libc=uclibc test_hex_constrained.bc --sym-hex-template-stdin /tmp/constrained.btpl
echo "--- Sample test case ---"
ktest-tool /tmp/klee-out-7/test000001.ktest

echo ""
echo "========================================"
echo "  TEST 4.3: Hex template comments/ws"
echo "========================================"

cat > /tmp/comments.btpl << 'EOF'
# "Hello " in hex
48 65 6C 6C
6F 20
# Two symbolic bytes
?? ??
# "!" in hex
21
EOF

cat > /tmp/test_hex_comments.c << 'EOF'
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
int main() {
  char buf[16];
  int n = read(0, buf, sizeof(buf));
  assert(n == 9);
  assert(memcmp(buf, "Hello ", 6) == 0);
  assert(buf[8] == '!');
  if (buf[6] == 'O' && buf[7] == 'K')
    printf("found: Hello OK!\n");
  return 0;
}
EOF

clang -emit-llvm -c -g test_hex_comments.c -o test_hex_comments.bc
klee --posix-runtime --libc=uclibc test_hex_comments.bc --sym-hex-template-stdin /tmp/comments.btpl
echo "--- Sample test case ---"
ktest-tool /tmp/klee-out-8/test000001.ktest

echo ""
echo "========================================"
echo "  TEST 4.4: Hex template binary data"
echo "========================================"

cat > /tmp/binary.btpl << 'EOF'
00 FF 7F 80 ?? ?{range:128-255}
EOF

cat > /tmp/test_hex_binary.c << 'EOF'
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
int main() {
  unsigned char buf[8];
  int n = read(0, buf, sizeof(buf));
  assert(n == 6);
  assert(buf[0] == 0x00);
  assert(buf[1] == 0xFF);
  assert(buf[2] == 0x7F);
  assert(buf[3] == 0x80);
  /* buf[4] is unconstrained */
  assert(buf[5] >= 128);  /* klee_assume guarantees high byte */
  if (buf[4] == 0x42 && buf[5] == 0xAA)
    printf("found: 42 AA\n");
  return 0;
}
EOF

clang -emit-llvm -c -g test_hex_binary.c -o test_hex_binary.bc
klee --posix-runtime --libc=uclibc test_hex_binary.bc --sym-hex-template-stdin /tmp/binary.btpl
echo "--- Sample test case ---"
ktest-tool /tmp/klee-out-9/test000001.ktest
