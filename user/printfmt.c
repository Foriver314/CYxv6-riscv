// Test program for enhanced printf format specifiers

#include "kernel/types.h"
#include "user/user.h"

int main(void)
{
  int i = 42;
  int neg = -123;
  unsigned int u = 255;
  long l = 1234567890L;
  long long ll = 887766554433LL;
  unsigned long ul = 0xDEADBEEFUL;
  unsigned long long ull = 0x123456789ABCDEF0ULL;
  char *s = "hello";
  char c = 'X';
  int zero = 0;
  int pos = 42;

  printf("=== Basic format specifiers ===\n");
  printf("%%d: %d\n", i);
  printf("%%ld: %ld\n", l);
  printf("%%lld: %lld\n", ll);
  printf("%%u: %u\n", u);
  printf("%%lu: %lu\n", ul);
  printf("%%llu: %llu\n", ull);

  printf("\n=== Hex and octal ===\n");
  printf("%%x: %x\n", u);
  printf("%%lx: %lx\n", ul);
  printf("%%llx: %llx\n", ull);
  printf("%%X: %X\n", u);
  printf("%%o (octal): %o\n", u);
  printf("%%b (binary): %b\n", u);

  printf("\n=== New: width and alignment ===\n");
  printf("%%5d: [%5d]\n", i);
  printf("%%-5d: [%-5d]\n", i);
  printf("%%05d: [%05d]\n", i);
  printf("%%10s: [%10s]\n", s);
  printf("%%-10s: [%-10s]\n", s);

  printf("\n=== New: sign flags ===\n");
  printf("%%+d: %+d\n", pos);
  printf("%%+d: %+d\n", neg);
  printf("%% d: % d\n", pos);
  printf("%% d: % d\n", neg);

  printf("\n=== Pointer ===\n");
  printf("%%p: %p\n", (void*)0x1000);

  printf("\n=== Character and string ===\n");
  printf("%%c: %c\n", c);
  printf("%%s: %s\n", s);
  printf("null: (null)\n");

  printf("\n=== Zero and negative ===\n");
  printf("zero: %d\n", zero);
  printf("neg:  %d\n", neg);
  printf("%%x of 0: %x\n", zero);

  printf("\n=== All done ===\n");
  exit(0);
}
