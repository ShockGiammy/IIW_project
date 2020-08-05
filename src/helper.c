/*
  HELPER.C
  ========
  
*/

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
#include "helper.h"

#define MAX_LINE  4096


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
	    		if ( c == '\n' || c == 0) break;
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

	if(strcmp(vptr, "FIN") == 0){
		int res;
		if((res = close_server_tcp(sockd)) < 0){
			fprintf(stderr, "received FIN but could not close connection\n");
			return -2;
		}
		return -1;
	}
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
	char end = 0;
	do{nleft = write(sockd, &end, 1);}
	while(nleft == 0);
    
	return n;
}

int SendFile(int socket_desc, char* file_name, char* response) {

	int first_byte = 0;
	struct stat	obj;
	char buffer[8192]; //we use this buffer for now, we'll try to use only response buffer

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
	int n = 0; // used to keep trace of the number of bytes that we are reading
	int i = 0; // for tcp struct indexing
	int k;
	tcp send_segm[8]; // keeps the segment that we send, so that we can perform resending and/or acknoledg.
	tcp recv_segm; // used to unpack the ack and see if everything was good
	int n_acked;
	int n_read = 0;
	slid_win sender_wind; //sliding window for the sender
	memset(&sender_wind, 0, sizeof(sender_wind)); // we initialize the struct to all 0s
	sender_wind.max_size = MAX_WIN; //we accept at most BUFSIZ bytes on the fly at the same time

	while((n = read(file_desc, response, MSS)) > 0 || sender_wind.acked < file_size) {
		
		n_read += n;
		// we check if we can send data without exceeding the max number of bytes on the fly
		if(sender_wind.on_the_fly <= sender_wind.max_size && n > 0) {
			memset(&send_segm[i], 0, sizeof(send_segm[i]));
			printf("%d\n", i);
			strncpy(send_segm[i].data, response, MSS);
			send_segm[i].data[MSS+1] = '\0';
			fill_struct(&send_segm[i], sender_wind.next_seq_num, 0, 0, false, false, false, NULL);
			sender_wind.next_seq_num += strlen(send_segm[i].data); // sequence number for the next segment
			sender_wind.n_seg++;
			sender_wind.on_the_fly += n;
			sender_wind.last += n;
			make_seg(send_segm[i], buffer); // we put our segment in a buffer that will be sent over the socket
			write(socket_desc, buffer, strlen(buffer));
			memset(buffer, 0, sizeof(char)*(strlen(buffer)+1)); //we reset the buffer to send the next segment
			memset(response, 0, sizeof(char)*(strlen(response)+1)); // we reset the buffer so taht we can reuse it
			i = (i+1)%7;
			printf("Finestra : [%d  %d]\n", sender_wind.first, sender_wind.last);
		}
		// we have read the max number of data, we proceed with the sending in pipelining
		if(sender_wind.on_the_fly == sender_wind.max_size || n_read == file_size) {

			sender_wind.n_seg = 0;

			recv(socket_desc, buffer, 37, 0); //we expect a buffer with only header and no data
			extract_segment(&recv_segm, buffer);
			memset(buffer, 0, sizeof(char)*(strlen(buffer) + 1));
			if(sender_wind.first <= recv_segm.ack_number <= sender_wind.last) { //we check if we received a segment in order
				sender_wind.next_to_ack = recv_segm.ack_number;
				n_acked = count_acked(sender_wind.first, recv_segm.ack_number); 
				sender_wind.on_the_fly -= MSS; // we scale the number of byte acked from the max
				sender_wind.acked = recv_segm.ack_number;
				sender_wind.first = recv_segm.ack_number;
				i = (i+1)%7;
			}
			memset(recv_segm.data, 0, sizeof(char)*(strlen(recv_segm.data) + 1));
			memset(response, 0, sizeof(char)*(strlen(response)+1));
			for(k = 0; k < n_acked; k++) {
				memset(send_segm[k].data, 0, sizeof(char)*(strlen(send_segm[k].data) + 1));
			}
		}
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
	while ((n = recv(socket_desc, retrieveBuffer, MSS+37, 0)) > 0) {
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
			else {
				memset(retrieveBuffer, 0, sizeof(char)*(strlen(retrieveBuffer)+1));
				strcat(segment.data, "\0");
				write(fd, segment.data, strlen(segment.data));
				next += strlen(segment.data);
				fill_struct(&ack, 0, next, 0, true, false, false, NULL);
				make_seg(ack, retrieveBuffer);
				send(socket_desc, retrieveBuffer, strlen(retrieveBuffer), 0);
				printf("Riscontro il seg %d\n", segment.sequence_number);
				memset(retrieveBuffer, 0, sizeof(char)*(strlen(retrieveBuffer)+1));
			}
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
	
	// check the sequence number
	if(segment.sequence_number != 0) {
		unsigned char seq[13];
		sprintf(seq, "%X", htonl(segment.sequence_number));
		concat_segm(send_segm, seq, 13);
		//strcat(send_segm, seq);
	}
	else {
		strcat(send_segm, "0000000000000");
		//strcpy(send_segm, "0000\n"); // we send a send_num = 0, so that the recv knows taht there are no data
	}

	// check ack
	if(segment.ack_number != 0) {
		unsigned char ack[13];
		sprintf(ack, "%X", htonl(segment.ack_number));
		concat_segm(send_segm, ack, 13);
		//strcat(send_segm, ack);
	}
	else {
		strcat(send_segm, "0000000000000");
		//strcat(send_segm, "0000\n");
	}
	// verify if there is any flag to send
	if(segment.ack) {
		strcat(send_segm, "1");
		//strcat(send_segm, "1\n");
	}
	else {
		strcat(send_segm, "0");
		//strcat(send_segm, "0\n");
	}
	if(segment.syn) {
		strcat(send_segm, "1");
		//strcat(send_segm, "1\n");
	}
	else {
		strcat(send_segm, "0");
		//strcat(send_segm, "0\n");
	}
	if(segment.fin) {
		strcat(send_segm, "1");
		//strcat(send_segm, "1\n");
	}
	else {
		strcat(send_segm, "0");
		//strcat(send_segm, "0\n");
	}

	// verify for the receiver window
	if(segment.receiver_window != 0) {
		char recv[8];
		sprintf(recv, "%X", htons(segment.receiver_window));
		concat_segm(send_segm, recv, 8);
		//strcat(send_segm, recv);
		//strcat(send_segm, "\n");
	}
	else {
		strcat(send_segm, "00000000");
		//strcat(send_segm, "00\n");
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
	char seg[15];
	memset(seg, 0, sizeof(char)*(strlen(seg)+1));
	while(i < 13) {
		seg[j] = recv_segm[i];
		i++;
		j++;
	}
	if(strcmp(seg, "0000000000000") != 0) {
		int seg_num;
		//seg_num = (int)strtol(seg, NULL, 16);
		sscanf(seg, "%X", &seg_num);
		segment->sequence_number = ntohl(seg_num);
		printf("Segment number received : %d\n", segment->sequence_number);
	}
	else {
		segment->sequence_number = 0;
	}
	j = 0;

	//deserialize ack number
	char ack[15];
	memset(ack, 0, sizeof(char)*(strlen(ack)+1));
	while(i <= 25) {
		ack[j] = recv_segm[i];
		i++;
		j++;
	}
	if(strcmp(ack, "0000000000000") != 0) {
		int ack_num;
		sscanf(ack, "%X", &ack_num);
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
	i++;
	if(recv_segm[i] == '1') {
		segment->syn = true;
	}
	else {
		segment->syn = false;
	}
	i++;
	if(recv_segm[i] ==  '1') {
		segment->fin = true;
	}
	else {
		segment->fin = false;
	}
	i++;

	int recv;
	char rv[15];
	memset(rv, 0, sizeof(char)*(strlen(rv)+1));
	while(i < 37) {
		rv[j] = recv_segm[i];
		i++;
		j++;
	}
	if(strcmp(rv, "00000000") != 0) {
		sscanf(rv, "%X", &recv);
		segment->receiver_window = ntohl(recv);
	}
	else {
		segment->receiver_window = 0;
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
	segment->receiver_window = recv;
	segment->ack = is_ack;
	segment->fin = is_fin;
	segment->syn = is_syn;
	if(data != NULL) {
		strcpy(segment->data,data);
	}
}

void concat_segm(char *segm, char *to_concat, int max) {
	int i;
	int length = strlen(to_concat);
	for(i = 0; i < max-length; i++) {
		strcat(segm, "0"); //we concat some 0s to make the header always the same size
	}
	strcat(segm, to_concat); //last, we concat the data
}

// this function will return the exact number of bytes that have been copied from src to dest
int copy_with_ret(char * dest, char *src, int max) {
	int i = 0;
	int ncopied = 0;
	while(i < max && src[i] != '\0') {
		dest[i] = src[i];
		i++;
		ncopied++;
	}
	return ncopied;
}

int count_acked (int min, int acked) {
	int n_ack = 0;
	for(int j = min; j < acked; j+=1500) {
		n_ack++;
	}
	return n_ack;
}

int connect_tcp(int socket_descriptor, struct sockaddr* addr, socklen_t addr_len){
	char server_response[BUFSIZ];
	int res;
	if((res = connect(socket_descriptor, addr, INET_ADDRSTRLEN)) < 0){
		char str_addr[INET_ADDRSTRLEN];
		inet_ntop(addr->sa_family, addr->sa_data, str_addr, addr_len);
		fprintf(stderr, "Could not connect to: %s\n", str_addr);
		return res;
	}

	Writeline(socket_descriptor, "SYN", 3);

	printf("Waiting server response...\n");
	Readline(socket_descriptor, server_response, MAX_LINE -1);

	if(strcmp(server_response, "SYN-ACK") != 0){
		fprintf(stderr, "Could not establish connection with server, response: %s\n", server_response);
		res = -1;
		return res;
	}

	Writeline(socket_descriptor, "ACK", 3);

	res = 0;
	return res;

}

int accept_tcp(int socket_descriptor, struct sockaddr* addr, socklen_t* addr_len){
	char client_message[MAX_LINE];

	int conn_sd = accept(socket_descriptor, addr, addr_len);

	if(conn_sd < 0){
		fprintf(stderr, "error during accept\n");
		return -1;
	}

	Readline(conn_sd, client_message, MAX_LINE -1);

	if(strcmp(client_message, "SYN") == 0){
		Writeline(conn_sd, "SYN-ACK", 7);
	}
	else {
		fprintf(stderr, "server: expected SYN but received %s\n", client_message);
		return -1;
	}
	
	Readline(conn_sd, client_message, MAX_LINE -1);

	if(strcmp(client_message, "ACK") != 0){
		printf("server: missing ACK, terminating...\n");
		return -1;
	}
	
	printf("server: connection established\n");
	return conn_sd;
}

int close_client_tcp(int sockd){
	char response[MAX_LINE];

	Writeline(sockd, "FIN", 3);

	Readline(sockd, response, MAX_LINE);
	
	if(strcmp(response, "FIN-ACK") != 0){
		fprintf(stderr, "Could not close connection...\n");
		return -1;
	}

	Writeline(sockd, "ACK", 3);

	int res = close(sockd);
	printf("Connection closed\n");
	return res;
}

int close_server_tcp(int sockd){
	char response[MAX_LINE];

	Writeline(sockd, "FIN-ACK", 7);

	Readline(sockd, response, MAX_LINE);
	if(strcmp(response, "ACK") != 0 ){
		fprintf(stderr, "Could not close connection...\n");
		return -1;
	}

	int res = close(sockd);
	
	printf("Connection closed\n");
	return res;
}