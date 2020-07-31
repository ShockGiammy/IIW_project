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
	char buffer[8192]; //we use this buffer for now, we'll try to use only response buffer
	//send_wind = malloc(6*sizeof(char*)); //we allocate 6 char *buffers to keep our segments

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
	int i;
	tcp send_segm[7];
	tcp recv_segm;
	int byte_read = 0; // to count how many byte are left
	int seg_div; //usefull to break the data into chunks
	//while ( (n = read(file_desc, response, BUFSIZ-1)) > 0) {
	while(byte_read < file_size) {
		memset(&send_segm, 0, sizeof(send_segm));
		n = read(file_desc, response, BUFSIZ-1);

		byte_read += n;
		// at the end of the cycle we will have our segments on the fly without any response
		while (seg_div <= n) {
			strncpy(send_segm[i].data, response, MSS);
			send_segm[i].data[MSS+1] = '\0';
			fill_struct(&send_segm[i], first_byte, 0, 0, false, false, false, NULL);
			make_seg(send_segm[i], buffer);
			first_byte = first_byte + MSS; // sequence number for the next segment
			printf("%ld\n", strlen(buffer));
			write(socket_desc, buffer, strlen(buffer));
			memset(buffer, 0, sizeof(char)*(strlen(buffer)+1));
			i++;
			seg_div += MSS;
			response += MSS;
		}
		i = 0;
		printf("N : %d\n", n);
		while(i < n) {
			recv(socket_desc, buffer, MSS+13, 0);
			extract_segment(&recv_segm, buffer);
			i += recv_segm.ack_number;
			printf("I : %d\n", i);
			if(recv_segm.ack_number == i) { // we received an in order segment
				continue; //we will add the sliding window
			}
			memset(recv_segm.data, 0, sizeof(char)*(strlen(recv_segm.data) + 1));
		}
		memset(response, 0, sizeof(char)*(strlen(response)+1));
	}
	fill_struct(&send_segm[7], 0, 0, 0, false, false, false, "END");
	make_seg(send_segm[7], response);
	send(socket_desc, response, strlen(response), 0);

	close(file_desc);
	memset(response, 0, sizeof(char)*(strlen(response)+1));
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
	int next = 0;
	tcp segment;
	tcp ack;
	//while ((n = recv(socket_desc, retrieveBuffer, BUFSIZ-1, 0)) > 0) {
	while ((n = recv(socket_desc, retrieveBuffer, MSS+13, 0)) > 0) {
		if (strcmp(retrieveBuffer, "ERROR") == 0) {
			printf("file transfer error \n");
			if (remove(fname) != 0) {
      			printf("Unable to delete the file \n");
				fflush(stdout);
				return 1;
			}
		}
		else {
			extract_segment(&segment, retrieveBuffer);
			if(strcmp(segment.data, "END") == 0) {
				printf("file receiving completed \n");
				fflush(stdout);
				break;
			}
			memset(retrieveBuffer, 0, sizeof(char)*(strlen(retrieveBuffer)+1));
			strcat(segment.data, "\0");
			write(fd, segment.data, strlen(segment.data));
			next += strlen(segment.data);
			fill_struct(&ack, 0, next, 0, true, false, false, NULL);
			make_seg(ack, retrieveBuffer);
			send(socket_desc, retrieveBuffer, strlen(retrieveBuffer), 0);
			memset(retrieveBuffer, 0, sizeof(char)*(strlen(retrieveBuffer)+1));
		}
		memset(&segment, 0, sizeof(segment));
		memset(&ack, 0, sizeof(ack));
		memset(retrieveBuffer, 0, sizeof(char)*(strlen(retrieveBuffer)+1));
	}
	//close(conn_s);
	close(fd);
	memset(retrieveBuffer, 0, sizeof(char)*(strlen(retrieveBuffer)+1));
	return 0;		
}

void make_seg(tcp segment, char *send_segm) {
	
	// verify for the sequence number
	if(segment.sequence_number != 0) {
		sprintf(send_segm, "%0x", htonl(segment.sequence_number));
		strcat(send_segm, "\n");
	}
	else {
		strcpy(send_segm, "0000\n"); // we send a send_num = 0, so that the recv knows taht there are no data
	}

	// verify for the ack
	if(segment.ack_number != 0) {
		char ack[4];
		sprintf(ack, "%0x", htonl(segment.ack_number));
		strcat(send_segm, ack);
		strcat(send_segm, "\n");
	}
	else {
		strcat(send_segm, "0000\n");
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
		char recv[20];
		sprintf(recv, "%0x", htonl(segment.reciver_window));
		strcat(send_segm, recv);
		strcat(send_segm, "\n");
	}
	else {
		strcat(send_segm, "00\n");
	}
	if(strcmp(segment.data, "") != 0) {
		strcat(send_segm, segment.data);
	}
	else{
		strcat(send_segm, "\0");
	}
}

void extract_segment(tcp *segment, char *recv_segm) {
	int i = 0;
	int j = 0; // usefull for indexing the struct

	//deserialize seg number
	char seg[4];
	while(recv_segm[i] != '\n') {
		seg[j] = recv_segm[i];
		i++;
		j++;
	}
	i++;
	if(strcmp(seg, "0000") != 0) {
		int seg_num;
		//seg_num = (int)strtol(seg, NULL, 16);
		sscanf(seg, "%x", &seg_num);
		segment->sequence_number = ntohl(seg_num);
		printf("Segment number received : %d\n", segment->sequence_number);
	}
	else {
		segment->sequence_number = 0;
	}
	j = 0;

	//deserialize ack number
	char ack[4];
	memset(ack, 0, sizeof(char)*(strlen(ack)+1));
	while(recv_segm[i] != '\n') {
		ack[j] = recv_segm[i];
		i++;
		j++;
	}
	i++;
	if(strcmp(ack, "0000") != 0) {
		int ack_num;
		sscanf(ack, "%x", &ack_num);
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
		sscanf(rv, "%x", &recv);
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

void fill_struct(tcp *segment, int seq_num, int ack_num, int recv, bool is_ack, bool is_fin, bool is_syn, char *data) {
	segment->sequence_number = seq_num;
	segment->ack_number = ack_num;
	segment->reciver_window = recv;
	segment->ack = is_ack;
	segment->fin = is_fin;
	segment->syn = is_syn;
	if(data != NULL) {
		strcpy(segment->data,data);
	}
}

void copy_str(char *strdest, char *strsrc, int max) {
	printf("Entrato\n");
	for(int i = 0; i < max; i++) {
		strdest[i] = strsrc[i];
		printf("Sono qui\n");
	}
}