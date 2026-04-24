#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#include <stdarg.h>

static char digits[] = "0123456789ABCDEF";

struct printflags {
  int width;
  int left;
  int zero;
  int plus;
  int space;
  int uppercase;
};

static void
putc(int fd, char c)
{
  write(fd, &c, 1);
}

static void
printint(int fd, long long xx, int base, int sgn, struct printflags *pf)
{
  char buf[32];
  int i, neg;
  unsigned long long x;

  neg = 0;
  if(sgn && xx < 0){
    neg = 1;
    x = -xx;
  } else if(pf->plus && xx >= 0){
    neg = 2;
    x = xx;
  } else if(pf->space && xx >= 0){
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
      putc(fd, padchar);
  }
  if(neg == 1)
    putc(fd, '-');
  else if(neg == 2)
    putc(fd, '+');
  else if(neg == 3)
    putc(fd, ' ');

  while(--i >= 0)
    putc(fd, buf[i]);

  if(pf->left && pad > 0){
    while(pad-- > 0)
      putc(fd, ' ');
  }
}

static void
printptr(int fd, uint64 x)
{
  int i;
  putc(fd, '0');
  putc(fd, 'x');
  for (i = 0; i < (sizeof(uint64) * 2); i++, x <<= 4)
    putc(fd, digits[x >> (sizeof(uint64) * 8 - 4)]);
}

// Print to the given fd.
void
vprintf(int fd, const char *fmt, va_list ap)
{
  int c0, i;
  struct printflags pf;

  for(i = 0; (c0 = fmt[i] & 0xff) != 0; i++){
    if(c0 != '%'){
      putc(fd, c0);
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
    if(c0 == 'l'){
      int ll = (fmt[i+1] & 0xff) == 'l';
      if(fmt[i+1] == 'd' || (ll && fmt[i+2] == 'd')){
        printint(fd, va_arg(ap, uint64), 10, 1, &pf);
        i += ll ? 2 : 1;
      } else if(fmt[i+1] == 'u' || (ll && fmt[i+2] == 'u')){
        printint(fd, va_arg(ap, uint64), 10, 0, &pf);
        i += ll ? 2 : 1;
      } else if(fmt[i+1] == 'x' || (ll && fmt[i+2] == 'x')){
        printint(fd, va_arg(ap, uint64), 16, 0, &pf);
        i += ll ? 2 : 1;
      } else if(fmt[i+1] == 'o' || (ll && fmt[i+2] == 'o')){
        printint(fd, va_arg(ap, uint64), 8, 0, &pf);
        i += ll ? 2 : 1;
      } else if(fmt[i+1] == 'b' || (ll && fmt[i+2] == 'b')){
        printint(fd, va_arg(ap, uint64), 2, 0, &pf);
        i += ll ? 2 : 1;
      }
    } else if(c0 == 'z'){
      if(fmt[i+1] == 'd'){
        printint(fd, va_arg(ap, uint64), 10, 1, &pf);
        i += 1;
      } else if(fmt[i+1] == 'u'){
        printint(fd, va_arg(ap, uint64), 10, 0, &pf);
        i += 1;
      }
    } else if(c0 == 't'){
      if(fmt[i+1] == 'd'){
        printint(fd, va_arg(ap, uint64), 10, 1, &pf);
        i += 1;
      }
    } else if(c0 == 'd'){
      printint(fd, va_arg(ap, int), 10, 1, &pf);
    } else if(c0 == 'u'){
      printint(fd, va_arg(ap, uint32), 10, 0, &pf);
    } else if(c0 == 'x'){
      printint(fd, va_arg(ap, uint32), 16, 0, &pf);
    } else if(c0 == 'X'){
      pf.uppercase = 1;
      printint(fd, va_arg(ap, uint32), 16, 0, &pf);
    } else if(c0 == 'p'){
      printptr(fd, va_arg(ap, uint64));
    } else if(c0 == 'c'){
      putc(fd, va_arg(ap, uint32));
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
          putc(fd, ' ');
      }
      for(; *ss; ss++)
        putc(fd, *ss);
      if(pf.left && pad > 0){
        while(pad-- > 0)
          putc(fd, ' ');
      }
    } else if(c0 == '%'){
      putc(fd, '%');
    } else if(c0 == 'o'){
      printint(fd, va_arg(ap, uint32), 8, 0, &pf);
    } else if(c0 == 'b'){
      printint(fd, va_arg(ap, uint32), 2, 0, &pf);
    } else if(c0 == 0){
      break;
    } else {
      putc(fd, '%');
      putc(fd, c0);
    }
  }
}

void
fprintf(int fd, const char *fmt, ...)
{
  va_list ap;

  va_start(ap, fmt);
  vprintf(fd, fmt, ap);
}

void
printf(const char *fmt, ...)
{
  va_list ap;

  va_start(ap, fmt);
  vprintf(1, fmt, ap);
}
