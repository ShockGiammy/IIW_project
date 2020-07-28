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

	int first_byte = 0;
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
	while ( (n = read(file_desc, server_response, BUFSIZ-1)) > 0) {
		tcp segm;
		char *buffer;
		buffer = malloc(8192*sizeof(char));

		memset(&segm, 0, sizeof(&segm)); //initialize the struct
		segm.sequence_number = first_byte; // sets the correct sequence number
		first_byte = first_byte + n + 1; // sequence number for the next segment

		//set the struct 
		segm.sequence_number = first_byte;
		segm.ack = false;
		segm.fin = false;
		segm.syn = false;
		strcpy(segm.data,server_response);
		server_response[n] = '\0';
		serialize_struct(buffer, segm);
		//printf("%s\n", buffer);
		//server_response[n] = '\0';
		//write(socket_desc, server_response, n);
		write(socket_desc, buffer, strlen(buffer));
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
			tcp segment;
			memset(&segment,0,sizeof(segment));
			deserialize_struct(bufferFile, segment);
			/*
			bufferFile[n] = '\0';
			write(fd, bufferFile, n);
			retrieveBuffer[n] = '\0';
			write(fd, retrieveBuffer, n);
			if( n < BUFSIZ-2) {
				printf("file receiving completed \n");
				fflush(stdout);
				break;
			}*/
			strcat(segment.data, "\0");
			write(fd, bufferFile, strlen(segment.data));
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

// serializes the big-endian int value 
void serialize_struct(char *buffer, tcp segment) {

	if(segment.ack) {
		strcat(buffer, "1");
	}
	else {
		strcat(buffer, "0");
	}
	if(segment.fin) {
		strcat(buffer, "1");
	}
	else {
		strcat(buffer, "0");
	}
	if(segment.syn) {
		strcat(buffer, "1");
	}
	else {
		strcat(buffer, "0");
	}
	strcat(buffer, "\n");

	sprintf(buffer, "%d", htonl(segment.sequence_number));
	/*char *buff = malloc(strlen(segment.data)*sizeof(char));
	strncpy(buff, segment.data, strlen(segment.data));*/
	strcat(buffer, "\n");

	strcat(buffer, segment.data);
}

void deserialize_struct(char *buffer, tcp segment) {
	int n = 0;
	char seq_num[4];
	int temp;
	int i = 0;
	char flags[3];
	while(buffer[n] != '\n') {
		flags[i] = buffer[i];
		n++;
	}

	// there is an ack in the segmment
	if(flags[0] == '1') {
		char ack_num[4];
		i = 0;
		while(buffer[n] != '\n') {
			ack_num[i] = buffer[n];
			n++; 
		}
		int temp2;
		sscanf(ack_num, "%d", &temp2);
		segment.ack_number = ack_num;
		printf("Ack recived\n");
	}

	// deserialize segment number
	while(buffer[n] != '\n') {
		seq_num[n] = buffer[n];
		n++; 
	}
	sscanf(seq_num, "%d", &temp);
	segment.sequence_number = ntohl(temp);
	printf("Segment number : %d\n", segment.sequence_number);


	i = 0;
	while(buffer[n] != '\0') {
		segment.data[i] = buffer[n];
		n++;
		i++;
	}
}
