// Reader-writer locks for processes
struct rwlock {
  struct spinlock lk;
  char *name;
  int readers;
  int writer;
  int waiting_readers;
  int waiting_writers;
  int writer_pid;
};
