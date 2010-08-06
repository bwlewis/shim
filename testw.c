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
  int fd, j, k;
  int *A, *a;
  struct stat sb;
  int p;
  if (argc < 3)
    {
      printf ("usage: testw <file> <pages>\n");
      return -1;
    }
  fd = open (argv[1], O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
  fstat (fd, &sb);
  A = (int *) mmap (NULL, sb.st_size, PROT_WRITE, MAP_SHARED, fd, 0);
  p = atoi (argv[2]);
  a = (int *) A;
  j = 0;
  while (j < p)
    {
      j++;
      memcpy (a, &j, sizeof (int));
      fprintf (stderr, "j=%d, address=%p\n", *a, a);
      a = a + 1024;
// Comment out the following msync line to see its effect.
//              k=msync(A,p,MS_SYNC);
//                usleep(1);
    }
  printf ("OK, press ENTER to msync\n");
  getc (stdin);
  k = msync (A, sb.st_size, MS_SYNC);
  printf ("OK, press ENTER to exit\n");
  getc (stdin);
  munmap (A, sb.st_size);
  close (fd);
  return 0;
}
