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
  int fd, k;
  char *A;
  struct stat sbuffer;
  int p = 4096;
  if (argc < 2)
    {
      printf ("usage: msynch <file>\n");
      return -1;
    }
  fd = open (argv[1], O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
  fstat (fd, &sbuffer);
  printf ("Mapping size is %ld\n", (long)sbuffer.st_size);
  A = (char *) mmap (NULL, sbuffer.st_size, PROT_WRITE, MAP_SHARED, fd, 0);
  k = msync (A, p, MS_SYNC);
  if(k<0) perror ("msync error");
  else printf("msync complete.\n");
  munmap (A, sbuffer.st_size);
  close (fd);
  return 0;
}
