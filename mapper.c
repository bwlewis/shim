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
  int fd, n, j;
  char *A, *C;
  struct stat sbuffer;
  char *B = (char *)malloc(4096);
  if (argc < 2)
    {
      printf ("usage: mapper <file>\n");
      return -1;
    }
  fd = open (argv[1], O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
  fstat (fd, &sbuffer);
  n = sbuffer.st_size / 4096;
printf("reading %d pages\n",n);
  A = (char *) mmap (NULL, sbuffer.st_size, PROT_WRITE, MAP_SHARED, fd, 0);
  C = A;
  for(j=0;j<n;++j) {
    memcpy((void *)B, C, 4096);
    C+= 4096;
  }
  munmap ((void *)A, sbuffer.st_size);
  close (fd);
  free(B);
  return 0;
}
