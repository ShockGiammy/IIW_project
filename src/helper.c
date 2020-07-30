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

int SendFile(int socket_desc, char* file_name, char* response, bool is_ack) {

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
	if(is_ack) {
		recv(socket_desc, response, 6, 0); // waits for the receiver to start transmitting the file
		if(strcmp(response, "READY") != 0) {
			printf("Error \n");
			exit(-1);
		}
	}
	memset(response, 0, sizeof(char)*(strlen(response) +1 ));
	int n;
	while ( (n = read(file_desc, response, BUFSIZ-1)) > 0) {
		tcp segm;
		char *buffer;
		buffer = malloc(8192*sizeof(unsigned char));

		segm.sequence_number = first_byte + 1; // sets the correct sequence number
		first_byte = first_byte + n + 1; // sequence number for the next segment
		segm.ack_number = 1;
		segm.reciver_window = 0;

		//set the struct
		segm.ack = false;
		segm.fin = false;
		segm.syn = false;
		strcpy(segm.data,response);
		segm.data[n] = '\0';
		make_seg(segm, buffer);
		write(socket_desc, buffer, strlen(buffer));
		memset(buffer, 0, sizeof(char)*(strlen(buffer)+1));
		memset(response, 0, sizeof(char)*(strlen(response)+1));
	}

	close(file_desc);
	//close(socket_desc);
	return 0;
}

int RetrieveFile(int socket_desc, char* fname, bool is_ack) {
	char retrieveBuffer[BUFSIZ];

	int fd = open(fname, O_WRONLY|O_CREAT, S_IRWXU);
	if (fd == -1) {
		printf("error to create file");
		//recv(conn_s, retrieveBuffer, BUFSIZ-1, 0);   //only to consume the socket buffer;
		return 1;
	}
	if(is_ack)
		send(socket_desc, "READY", 5, 0); // kind of ack to tell the sender that the receiver is ready
	
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
			extract_segment(&segment, retrieveBuffer);
			strcat(segment.data, "\0");
			write(fd, segment.data, strlen(segment.data));
			retrieveBuffer[n] = '\0';
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

void make_seg(tcp segment, char *send_segm) {
	
	int x;
	// verify for the sequence number
	if(segment.sequence_number != 0) {
		sprintf(send_segm, "%d", htonl(segment.sequence_number));
		strcat(send_segm, "\n");
	}
	else {
		strcpy(send_segm, "0000\0"); // we send a send_num = 0, so that the recv knows taht there are no data
	}

	// verify for the ack
	if(segment.ack_number != 0) {
		char ack[10];
		sprintf(ack, "%d", htonl(segment.ack_number));
		strcat(send_segm, ack);
		strcat(send_segm, "\n");
	}
	else {
		strcat(send_segm, "0000\0");
	}

	// verify if there is any flag to send
	if(segment.ack) {
		strcat(send_segm, "1\n");
	}
	else {
		strcat(send_segm, "0\n");
	}
	if(segment.syn) {
		strcat(send_segm, "1\n");
	}
	else {
		strcat(send_segm, "0\n");
	}
	if(segment.fin) {
		strcat(send_segm, "1\n");
	}
	else {
		strcat(send_segm, "0\n");
	}

	// verify for the receiver window
	if(segment.reciver_window != 0) {
		char recv[10];
		sprintf(recv, "%d", htonl(segment.reciver_window));
		strcat(send_segm, recv);
		strcat(send_segm, "\n");
	}
	else {
		strcat(send_segm, "00\n");
	}
	strcat(send_segm, segment.data);
}

void extract_segment(tcp *segment, char *recv_segm) {
	int i = 0;
	int j = 0; // usefull for indexing the struct

	//deserialize seg number
	char seg[20];
	while(recv_segm[i] != '\n') {
		seg[j] = recv_segm[i];
		i++;
		j++;
	}
	i++;
	if(strcmp(seg, "0000") != 0) {
		int seg_num;
		sscanf(seg, "%d", &seg_num);
		segment->sequence_number = ntohl(seg_num);
		printf("Segment number received : %d\n", segment->sequence_number);
	}
	else {
		segment->sequence_number = 0;
	}
	j = 0;

	//deserialize ack number
	char ack[20];
	while(recv_segm[i] != '\n') {
		ack[j] = recv_segm[i];
		i++;
		j++;
	}
	i++;
	if(strcmp(ack, "0000") != 0) {
		int ack_num;
		sscanf(seg, "%d", &ack_num);
		segment->ack_number = ntohl(ack_num);
		printf("Ack number : %d\n", segment->ack_number);
	}
	else {
		segment->ack_number = 0;
	}
	j = 0;

	// deserialize flags
	if(recv_segm[i] == '1') {
		segment->ack = true;
	}
	else {
		segment->ack = false;
	}
	i+=2;
	if(recv_segm[i] == '1') {
		segment->syn = true;
	}
	else {
		segment->syn = false;
	}
	i+=2;
	if(recv_segm[i] ==  '1') {
		segment->fin = true;
	}
	else {
		segment->fin = false;
	}
	i+=2;

	int recv;
	char rv[20];
	while(recv_segm[i] != '\n') {
		rv[j] = recv_segm[i];
		i++;
		j++;
	}
	i++;
	if(strcmp(rv, "00") != 0) {
		sscanf(rv, "%d", &recv);
		segment->reciver_window = ntohl(recv);
	}
	else {
		segment->reciver_window = 0;
	}
	j = 0;
	//deserialize data
	while(recv_segm[i] != '\0') {
		segment->data[j] = recv_segm[i];
		i++;
		j++;		
	}
}