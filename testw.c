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
  struct stat sb;
  char *A;
  char *B;
  char *C;
  size_t p, q, j;
  if (argc < 3)
    {
      printf ("Write characters to a memory-mapped file in chunks.\n");
      printf ("usage: testw <file> <nr chunks>\n");
      return -1;
    }
  k = atoi(argv[2]);
  fd = open (argv[1], O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
  fstat (fd, &sb);
  p = sb.st_size / k;
  q = sb.st_size / p;
  if(q<1) {
      printf ("File not big enough\n");
      return -1;
  }
  B = (char *) malloc(p);
  A = (char *) mmap (NULL, sb.st_size, PROT_WRITE, MAP_SHARED, fd, 0);
  C = A;
  j = 0;
  while (j < q)
    {
      j++;
      memcpy (C, B, p);
      printf("Wrote %ld bytes at address %p\n",(long)p,C);
      C = C + p;
    }
  printf ("OK, press ENTER to msync\n");
  getc (stdin);
  k = msync (A, sb.st_size, MS_SYNC);
  printf ("OK, press ENTER to exit\n");
  getc (stdin);
  munmap (A, sb.st_size);
  free(B);
  close (fd);
  return 0;
}
