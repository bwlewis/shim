#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

int
main (int argc, char **argv)
{
  int fd, j;
  char *A;
  struct stat sbuffer;
  if (argc < 2)
    {
      printf ("usage: echo data | testmmap <file>\n");
      return -1;
    }
  fd = open (argv[1], O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
  fstat (fd, &sbuffer);
  printf ("Max data size is %ul\n", (unsigned int) sbuffer.st_size);
  A =
    (char *) mmap (NULL, sbuffer.st_size, PROT_WRITE, MAP_SHARED, fd, 0);
  printf ("Mapping active\n");
  j = read (0, A, sbuffer.st_size);
  printf ("Copied %d bytes\n", j);
  j = msync (A, sbuffer.st_size, MS_SYNC);
  printf ("msync %d\n", j);
  munmap (A, sbuffer.st_size);
  close (fd);
  return 0;
}

/*
vim: ts=4
*/
