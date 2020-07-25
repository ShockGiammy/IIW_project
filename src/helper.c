/*

  HELPER.C
  ========
  
*/

#include "helper.h"
#include <sys/socket.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <sys/sendfile.h>
#include <fcntl.h>
#include <sys/socket.h>       /*  socket definitions        */
#include <sys/types.h>        /*  socket types              */
#include <sys/stat.h>   	  /*  stat for files transfer   */
#include <arpa/inet.h>        /*  inet (3) funtions         */
#include <unistd.h>           /*  misc. UNIX functions      */


/*  Read a line from a socket  */

ssize_t Readline(int sockd, void *vptr, size_t maxlen)
{
    ssize_t n, rc;
    char    c, *buffer;

    buffer = vptr;

    for ( n = 1; n < maxlen; n++ ) {
		rc = read(sockd, &c, 1);
		if ( rc == 1 ) {
	    		*buffer++ = c;
	    		if ( c == '\n' ) break;
		}
		else
			if ( rc == 0 ) {
			    if ( n == 1 ) return 0;
			    else break;
			}
			else {
			    if ( errno == EINTR ) continue;
			    return -1;
			}
    }
    *buffer = 0;
    return n;
}


/*  Write a line to a socket  */

ssize_t Writeline(int sockd, const void *vptr, size_t n)
{
    size_t      nleft;
    ssize_t     nwritten;
    const char *buffer;

    buffer = vptr;
    nleft  = n;

    while ( nleft > 0 )
	{
		if ( (nwritten = write(sockd, buffer, nleft)) <= 0 )
		{
	    	if ( errno == EINTR ) nwritten = 0;
		    else return -1;
		}
		nleft  -= nwritten;
		buffer += nwritten;
    }
    return n;
}

int SendFile(int socket_desc, char* file_name, char* response) {

	struct stat	obj;

	char path[100] = "DirectoryFiles/";
	strcat(path, file_name);
	int file_desc = open(path, O_RDONLY);
	if (fstat(file_desc, &obj) == -1) {
		printf("Error: file not found\n");
		fflush(stdout);
		return 1;
	}
	int file_size = obj.st_size;

	printf("opening file\n");

	int n;
	while ( (n = read(file_desc, response, BUFSIZ-1)) > 0) {
		response[n] = '\0';
		write(socket_desc, response, n);
		memset(response, 0, sizeof(char)*(strlen(response)+1));
	}

	close(file_desc);
	//close(socket_desc);
	return 0;
}

int RetrieveFile(int socket_desc, char* fname) {
	char retrieveBuffer[BUFSIZ];

	int fd = open(fname, O_WRONLY|O_CREAT, S_IRWXU);
	if (fd == -1) {
		printf("error to create file");
		//recv(conn_s, retrieveBuffer, BUFSIZ-1, 0);   //only to consume the socket buffer;
		return 1;
	}

	int n;
	while ((n = recv(socket_desc, retrieveBuffer, BUFSIZ-1, 0)) > 0) {
		if (strcmp(retrieveBuffer, "ERROR") == 0) {
			printf("file transfer error \n");
			if (remove(fname) != 0) {
      			printf("Unable to delete the file \n");
				fflush(stdout);
				return 1;
			}
		}
		else {
			retrieveBuffer[n] = '\0';
			write(fd, retrieveBuffer, n);
			if( n < BUFSIZ-2) {
				printf("file receiving completed \n");
				fflush(stdout);
				break;
			}
		}
	}
	//close(conn_s);
	close(fd);
	return 0;		
}