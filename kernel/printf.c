//
// formatted console output -- printf, panic.
//

#include <stdarg.h>

#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"
#include "proc.h"

volatile int panicking = 0; // printing a panic message
volatile int panicked = 0; // spinning forever at end of a panic

// lock to avoid interleaving concurrent printf's.
static struct {
  struct spinlock lock;
} pr;

static char digits[] = "0123456789abcdef";

struct printflags {
  int width;
  int left;
  int zero;
  int plus;
  int space;
  int uppercase;
};

static void
printint(long long xx, int base, int sign, struct printflags *pf)
{
  char buf[32];
  int i, neg;
  unsigned long long x;

  neg = 0;
  if(sign && xx < 0){
    neg = 1;
    x = -xx;
  } else if(pf->plus && xx >= 0) {
    neg = 2;
    x = xx;
  } else if(pf->space && xx >= 0) {
    neg = 3;
    x = xx;
  } else {
    x = xx;
  }

  i = 0;
  do {
    int d = x % base;
    buf[i++] = pf->uppercase ? "0123456789ABCDEF"[d] : digits[d];
  } while((x /= base) != 0);

  int len = i + (neg ? 1 : 0);
  int pad = pf->width - len;
  char padchar = pf->zero ? '0' : ' ';

  if(!pf->left && pad > 0){
    while(pad-- > 0)
      consputc(padchar);
  }
  if(neg == 1)
    consputc('-');
  else if(neg == 2)
    consputc('+');
  else if(neg == 3)
    consputc(' ');

  while(--i >= 0)
    consputc(buf[i]);

  if(pf->left && pad > 0){
    while(pad-- > 0)
      consputc(' ');
  }
}

static void
printptr(uint64 x)
{
  int i;
  consputc('0');
  consputc('x');
  for (i = 0; i < (sizeof(uint64) * 2); i++, x <<= 4)
    consputc(digits[x >> (sizeof(uint64) * 8 - 4)]);
}

// Print to the console.
int
printf(char *fmt, ...)
{
  va_list ap;
  int i, cx, c0;
  struct printflags pf;

  if(panicking == 0)
    acquire(&pr.lock);

  va_start(ap, fmt);
  for(i = 0; (cx = fmt[i] & 0xff) != 0; i++){
    if(cx != '%'){
      consputc(cx);
      continue;
    }
    i++;
    pf.width = pf.left = pf.zero = pf.plus = pf.space = pf.uppercase = 0;

    // Parse flags
    while((c0 = fmt[i] & 0xff) == '-' || c0 == '0' || c0 == '+' || c0 == ' '){
      if(c0 == '-') pf.left = 1;
      if(c0 == '0') pf.zero = 1;
      if(c0 == '+') pf.plus = 1;
      if(c0 == ' ') pf.space = 1;
      i++;
    }

    // Parse width
    while((c0 = fmt[i] & 0xff) >= '0' && c0 <= '9'){
      pf.width = pf.width * 10 + (c0 - '0');
      i++;
    }

    c0 = fmt[i] & 0xff;

    // Length modifiers
    if(c0 == 'h'){
      int hh = (fmt[i+1] & 0xff) == 'h';
      if(c0 == 'd'){
        if(hh)
          printint(va_arg(ap, int), 10, 1, &pf);
        else
          printint(va_arg(ap, int), 10, 1, &pf);
        i += hh ? 2 : 1;
      } else if(c0 == 'u'){
        if(hh)
          printint(va_arg(ap, uint32), 10, 0, &pf);
        else
          printint(va_arg(ap, uint32), 10, 0, &pf);
        i += hh ? 2 : 1;
      }
    } else if(c0 == 'l'){
      int ll = (fmt[i+1] & 0xff) == 'l';
      if(fmt[i+1] == 'd' || (ll && fmt[i+2] == 'd')){
        printint(va_arg(ap, uint64), 10, 1, &pf);
        i += ll ? 2 : 1;
      } else if(fmt[i+1] == 'u' || (ll && fmt[i+2] == 'u')){
        printint(va_arg(ap, uint64), 10, 0, &pf);
        i += ll ? 2 : 1;
      } else if(fmt[i+1] == 'x' || (ll && fmt[i+2] == 'x')){
        printint(va_arg(ap, uint64), 16, 0, &pf);
        i += ll ? 2 : 1;
      } else if(fmt[i+1] == 'o' || (ll && fmt[i+2] == 'o')){
        printint(va_arg(ap, uint64), 8, 0, &pf);
        i += ll ? 2 : 1;
      } else if(fmt[i+1] == 'b' || (ll && fmt[i+2] == 'b')){
        printint(va_arg(ap, uint64), 2, 0, &pf);
        i += ll ? 2 : 1;
      }
    } else if(c0 == 'z'){
      if(fmt[i+1] == 'd'){
        printint(va_arg(ap, uint64), 10, 1, &pf);
        i += 1;
      } else if(fmt[i+1] == 'u'){
        printint(va_arg(ap, uint64), 10, 0, &pf);
        i += 1;
      }
    } else if(c0 == 't'){
      if(fmt[i+1] == 'd'){
        printint(va_arg(ap, uint64), 10, 1, &pf);
        i += 1;
      }
    } else if(c0 == 'd'){
      printint(va_arg(ap, int), 10, 1, &pf);
    } else if(c0 == 'u'){
      printint(va_arg(ap, uint32), 10, 0, &pf);
    } else if(c0 == 'x'){
      printint(va_arg(ap, uint32), 16, 0, &pf);
    } else if(c0 == 'X'){
      pf.uppercase = 1;
      printint(va_arg(ap, uint32), 16, 0, &pf);
    } else if(c0 == 'p'){
      printptr(va_arg(ap, uint64));
    } else if(c0 == 'c'){
      consputc(va_arg(ap, uint));
    } else if(c0 == 's'){
      int len = 0;
      char *ss = va_arg(ap, char*);
      if(ss == 0)
        ss = "(null)";
      char *t;
      for(t = ss; *t; t++)
        len++;
      int pad = pf.width - len;
      if(!pf.left && pad > 0){
        while(pad-- > 0)
          consputc(' ');
      }
      for(; *ss; ss++)
        consputc(*ss);
      if(pf.left && pad > 0){
        while(pad-- > 0)
          consputc(' ');
      }
    } else if(c0 == '%'){
      consputc('%');
    } else if(c0 == 'o'){
      printint(va_arg(ap, uint32), 8, 0, &pf);
    } else if(c0 == 'b'){
      printint(va_arg(ap, uint32), 2, 0, &pf);
    } else if(c0 == 0){
      break;
    } else {
      consputc('%');
      consputc(c0);
    }
  }
  va_end(ap);

  if(panicking == 0)
    release(&pr.lock);

  return 0;
}

void
panic(char *s)
{
  panicking = 1;
  printf("panic: ");
  printf("%s\n", s);
  panicked = 1; // freeze uart output from other CPUs
  for(;;)
    ;
}

void
printfinit(void)
{
  initlock(&pr.lock, "pr");
}
