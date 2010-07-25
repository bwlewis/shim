#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <string.h>
#include <errno.h>

int main(int argc, char **argv)
{
	int fd,j,k;
	int *A;
	struct stat sbuffer;
	int p=4096;
	if (argc<2) {
		printf ("usage: testw <file>\n");
		return -1;
	}
	j=0;
	fd = open (argv[1], O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
	fstat (fd, &sbuffer);
	A=(int *)mmap(NULL,p,PROT_WRITE, MAP_SHARED,fd,0);
	while(j<100) {
		j++;
		memcpy (A, &j, sizeof(int));
// Comment out the following msync line to see its effect.
		k=msync(A,p,MS_SYNC);
		fprintf(stderr, "j=%d\n",*A);
		usleep (900000);
	}
	munmap(A,p);
	close (fd);
	return 0;
}

/*
vim: ts=4
*/
