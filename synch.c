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
  if (argc < 3)
    {
      printf ("usage: synch <read | write> <file> [char]\n");
      printf ("Test manual cache synchornization between two processes.\n");
      printf("Example (reader process):\n");
      printf("synch read /path/to/pvshm/file\n\n");
      printf("Example (writer process):\n");
      printf("synch write /path/to/pvshm/file X\n\n");
      return -1;
    }
  fd = open (argv[2], O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
  fstat (fd, &sbuffer);
  printf ("Mapping size is %ld\n", (long)sbuffer.st_size);
  A = (char *) mmap (NULL, sbuffer.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  printf ("Mapped %p; press any key to continue...\n",(void *)A);
  getchar ();
  if(strncmp(argv[1],"write",3)==0) {
    memset (A, (int) *argv[3], p);
    printf ("Wrote to %p; press any key to msync...\n",(void *)A);
    getchar ();
    k = msync (A, p, MS_SYNC);
    if(k<0) perror ("msync error");
    else printf("msync complete.\n");
  }
  else {
    printf("Current mapping char = %c; press any key to invalidate cache...\n", A[0]);
    getchar();
    read(fd, NULL, p);
    printf("Cache synch'd: current mapping char = %c.\n", A[0]);
  }
  munmap (A, sbuffer.st_size);
  close (fd);
  return 0;
}
