#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <string.h>
#include <errno.h>

int
main (int argc, char **argv)
{
  int fd, j, k;
  int *A;
  struct stat sbuffer;
  int p = 1 * 4096;
  if (argc < 2)
    {
      printf ("usage: testr <file>\n");
      return -1;
    }
  j = 0;
  fd = open (argv[1], O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
  fstat (fd, &sbuffer);
  A = (int *) mmap (NULL, p, PROT_WRITE, MAP_SHARED, fd, 0);
  memcpy (&j, A + 1024, sizeof (int));	//force read of 2nd page
  for (k = 0; k < 10; k++)
    {
      lseek (fd, 0, SEEK_SET);
      read (fd, (void *) A, p);
      memcpy (&j, A, sizeof (int));
      fprintf (stderr, "j=%d press any key to invalidate...\n", j);
      getchar ();
    }
  munmap (A, p);
  close (fd);
  return 0;
}

/*
vim: ts=4
*/
