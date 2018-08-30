#define _GNU_SOURCE
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
  char *A;
  struct stat sbuffer;
  char *B;
  if (argc < 2)
    {
      printf ("usage: mapper <file>\n");
      return -1;
    }
  fd = open (argv[1], O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
  if (fd < 0)
    {
      printf ("open error %d\n", fd);
      return fd;
    }
  ftruncate(fd, 150000);
  fstat (fd, &sbuffer);
  n = sbuffer.st_size / 4096;
  printf("reading %d pages from %d\n",n,fd);
  A = (char *) mmap (NULL, sbuffer.st_size, PROT_WRITE, MAP_SHARED, fd, 0);
  B = (char *) malloc(4096 * n);

/* read */
  memcpy((void *)B, A, 4096 * n);

  printf("reverse msync\n");

  /* 'reverse' msync -- force update of the specified page cache range from the
   * backing file. In this example we force update of pages 1, 2, 3 and 4.
   */
  lseek(fd, 4097, SEEK_SET);
  read(fd, NULL, 9184);

  printf("write\n");
/* write */
  sprintf(A, "Homer is a chicken\n", NULL);
  printf(A);
  printf("%d\n", syncfs(fd));
  printf("%d\n", fsync(fd));
  printf("%d\n", fdatasync(fd));
  munmap((void *)A, sbuffer.st_size);

  write(fd, "silly", 5);
  lseek(fd, 0, SEEK_SET);
  read(fd, NULL, 4096);
  close(fd);
  free(B);
  return 0;
}
