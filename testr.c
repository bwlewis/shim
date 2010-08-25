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
// Read the first page repeatedly to investigate cache behavior (especially
// useful with a pvfs2 backing):
  for (k = 0; k < 10; k++)
    {
// Force a cache update with lseek/read. This is really only meaningful
// for parallel or remote backing file systems.
      lseek (fd, 0, SEEK_SET);
      read (fd, NULL, p);
      memcpy (&j, A, sizeof (int));
      printf ("mmaped int = %d\n", j);
// Force an actual read of the backing file into a buffer
      lseek (fd, 0, SEEK_SET);
      read (fd, &j, sizeof(int));
      printf ("read int = %d (press any key to invalidate and repeat)\n", j);
      getchar ();
    }
  munmap (A, p);
  close (fd);
  return 0;
}
