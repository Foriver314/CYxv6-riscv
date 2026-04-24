#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"
#include "kernel/fcntl.h"

char*
fmtname(char *path)
{
  static char buf[DIRSIZ+1];
  char *p;

  // Find first character after last slash.
  for(p=path+strlen(path); p >= path && *p != '/'; p--)
    ;
  p++;

  // Return blank-padded name.
  if(strlen(p) >= DIRSIZ)
    return p;
  memmove(buf, p, strlen(p));
  memset(buf+strlen(p), ' ', DIRSIZ-strlen(p));
  buf[sizeof(buf)-1] = '\0';
  return buf;
}

char*
filetype(int type)
{
  switch(type){
  case T_DIR:    return "dir";
  case T_FILE:   return "file";
  case T_DEVICE: return "dev";
  default:       return "???";
  }
}

void
printsize(uint64 size)
{
  if(size >= 1048576)
    printf("%dM", (int)(size / 1048576));
  else if(size >= 1024)
    printf("%dK", (int)(size / 1024));
  else
    printf("%d", (int)size);
}

void
ls(char *path)
{
  char buf[512], *p;
  int fd;
  struct dirent de;
  struct stat st;
  int total = 0;

  if((fd = open(path, O_RDONLY)) < 0){
    fprintf(2, "ls: cannot open %s\n", path);
    return;
  }

  if(fstat(fd, &st) < 0){
    fprintf(2, "ls: cannot stat %s\n", path);
    close(fd);
    return;
  }

  switch(st.type){
  case T_DEVICE:
  case T_FILE:
    printf("%-14s  %-4s  %2d  ", fmtname(path), filetype(st.type), st.nlink);
    printsize(st.size);
    printf("\n");
    break;

  case T_DIR:
    if(strlen(path) + 1 + DIRSIZ + 1 > sizeof buf){
      printf("ls: path too long\n");
      break;
    }
    strcpy(buf, path);
    p = buf+strlen(buf);
    *p++ = '/';
    while(read(fd, &de, sizeof(de)) == sizeof(de)){
      if(de.inum == 0)
        continue;
      memmove(p, de.name, DIRSIZ);
      p[DIRSIZ] = 0;
      if(stat(buf, &st) < 0){
        printf("ls: cannot stat %s\n", buf);
        continue;
      }
      total += st.size;
    }
    close(fd);

    // Reopen to print details
    fd = open(path, O_RDONLY);
    if(fd < 0){
      fprintf(2, "ls: cannot open %s\n", path);
      return;
    }

    // Print header
    printf("\n");
    printf("%-14s  %-4s  %2s  %s\n", "Name", "Type", "Ln", "Size");
    printf("%-14s  %-4s  %2s  %s\n", "----", "----", "--", "----");

    while(read(fd, &de, sizeof(de)) == sizeof(de)){
      if(de.inum == 0)
        continue;
      memmove(p, de.name, DIRSIZ);
      p[DIRSIZ] = 0;
      if(stat(buf, &st) < 0){
        printf("ls: cannot stat %s\n", buf);
        continue;
      }
      printf("%-14s  %-4s  %2d  ", fmtname(buf), filetype(st.type), st.nlink);
      printsize(st.size);
      printf("\n");
    }
    printf("%-14s  %-4s  %2s  ", "", "=", "");
    printsize(total);
    printf(" (total)\n");
    break;
  }
  close(fd);
}

int
main(int argc, char *argv[])
{
  int i;

  if(argc < 2){
    ls(".");
    exit(0);
  }
  for(i=1; i<argc; i++)
    ls(argv[i]);
  exit(0);
}
