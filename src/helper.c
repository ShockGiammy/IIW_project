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
	tcp send_segm[7]; // keeps the segment that we send, so that we can perform resending and/or acknoledg.
	tcp recv_segm; // used to unpack the ack and see if everything was good
	//int n_acked; // this variable shows us how many segments have been acked within the same ack
	int n_read = 0;
	slid_win sender_wind; //sliding window for the sender
	memset(&sender_wind, 0, sizeof(sender_wind)); // we initialize the struct to all 0s
	sender_wind.max_size = MAX_WIN; //we accept at most BUFSIZ bytes on the fly at the same time
	
	struct timeval time_out;
	time_out.tv_sec = 3; // we set 6 sec of timeout, we will estimate it in another moment
	time_out.tv_usec = 0;
	if(setsockopt(socket_desc, SOL_SOCKET, SO_RCVTIMEO, (char *)&time_out, sizeof(time_out)) == -1) {
		printf("Sender error setting opt\n");
	}

	//while((n = read(file_desc, response, MSS)) > 0 || sender_wind.tot_acked < file_size) {
	while(sender_wind.tot_acked < file_size ) {
		if(sender_wind.on_the_fly < sender_wind.max_size) {
			n = read(file_desc, response, MSS);
			n_read += n;
			printf("Letti in totale : %d\n", n_read);

			if(n > 0) {
				// we check if we can send data without exceeding the max number of bytes on the fly
				prepare_segment(send_segm, &sender_wind, response, i, n);
				make_seg(send_segm[i], buffer); // we put our segment in a buffer that will be sent over the socket
				printf("Tento invio di %d, con lunghezza dati %ld\n", send_segm[i].sequence_number, strlen(send_segm[i].data));
				//write(socket_desc, buffer, strlen(buffer));

				send_unreliable(buffer, socket_desc);
				memset(buffer, 0, sizeof(char)*(strlen(buffer)+1)); //we reset the buffer to send the next segment
				memset(response, 0, sizeof(char)*(strlen(response)+1)); // we reset the buffer so that we can reuse it
				i = (i+1)%6;
			}
		}

		// we have read the max number of data, we proceed with the sending in pipelining
		if(sender_wind.on_the_fly == sender_wind.max_size || n_read == file_size) {
			if(recv(socket_desc, buffer, 37, 0) > 0) { //we expect a buffer with only header and no data
				extract_segment(&recv_segm, buffer);
				memset(buffer, 0, sizeof(char)*(strlen(buffer) + 1));

				if(recv_segm.ack_number == sender_wind.next_to_ack) {//sender_wind.last_correctly_acked) {
					printf("Ricevuto un riscontro duplicato\n");
					sender_wind.dupl_ack++; // we increment the number of duplicate acks received
						
					//fast retransmission
					if(sender_wind.dupl_ack == 3) {
						printf("Fast retx\n");
						retx(send_segm, sender_wind, buffer, socket_desc);
					}
				}

				// we received an ack out of order, which is a duplicate ack
				else if((sender_wind.next_to_ack < recv_segm.ack_number) && (recv_segm.ack_number <= sender_wind.last_to_ack)) {
					slide_window(&sender_wind, &recv_segm, send_segm);
					
					//resets the segments that have been acked
					for(k = 0; k < sender_wind.n_seg; k++) {
						memset(&send_segm[(i+k)%6], 0, sizeof(send_segm[(i+k)%6]));
						memset(send_segm[(i+k)%6].data, 0, sizeof(char)*(strlen(send_segm[(i+k)%6].data) + 1));
					}
					printf("\n");
				}
			}

			// we have to retx the last segment not acked due to TO
			else {
				printf("TO expired\n");
				retx(send_segm, sender_wind, buffer, socket_desc);
			}
			memset(recv_segm.data, 0, sizeof(char)*(strlen(recv_segm.data) + 1));
			memset(response, 0, sizeof(char)*(strlen(response)+1));
		}
	}
	fill_struct(&send_segm[7], 0, 0, 0, false, false, false, "END");
	make_seg(send_segm[7], response);
	send(socket_desc, response, strlen(response), 0);
	close(file_desc);

	// resets the time-out
	time_out.tv_sec = 0; // we set 6 sec of timeout, we will estimate it in another moment
	time_out.tv_usec = 0;
	if(setsockopt(socket_desc, SOL_SOCKET, SO_RCVTIMEO, (char *)&time_out, sizeof(time_out)) == -1) {
		printf("Sender error setting opt\n");
	}
	memset(response, 0, sizeof(char)*(strlen(response)+1));
	return 0;
}

int RetrieveFile(int socket_desc, char* fname) {
	char retrieveBuffer[BUFSIZ];

	int fd = open(fname, O_WRONLY|O_CREAT, S_IRWXU);
	if (fd == -1) {
		printf("error to create file");
		return 1;
	}

	int n;
	// the head for our segment linked list
	tcp *buf_segm;
	buf_segm = malloc(sizeof(tcp));
	buf_segm = NULL;

	tcp ack;
	slid_win recv_win; // the sliding window for the receiver
	int list_length = 0;
	/*
	recv_win.tot_acked = 0; 
	recv_win.next_to_ack = 0;
	recv_win.next_seq_num = 0;
	*/
	memset(&recv_win, 0, sizeof(recv_win));
	recv_win.last_to_ack = 7500;
	
	struct timeval recv_timeout;

	while ((n = recv(socket_desc, retrieveBuffer, MSS+37, 0)) > 0) {
		printf("Totale riscontrati %d\n", recv_win.tot_acked);
		if (strcmp(retrieveBuffer, "ERROR") == 0) {
			printf("file transfer error \n");
			if (remove(fname) != 0) {
      			printf("Unable to delete the file \n");
				fflush(stdout);
				return 1;
			}
		}
		else {
			tcp *segment = malloc(sizeof(tcp));
			extract_segment(segment, retrieveBuffer);
			memset(retrieveBuffer, 0, sizeof(char)*(strlen(retrieveBuffer)+1));
			if(strcmp(segment->data, "END") == 0) {
				printf("file receiving completed \n");
				free(segment);
				fflush(stdout);
				break;
			}
			else {
				// we can still buffer segments
				if(list_length < MAX_BUF_SIZE && (recv_win.next_to_ack <= segment->sequence_number) && ( segment->sequence_number <= recv_win.last_to_ack)) {
					strcat(segment->data, "\0");
					buffer_in_order(&buf_segm, segment, &recv_win);
					list_length++;
				}
				recv_timeout.tv_sec = 0;
				recv_timeout.tv_usec = 500000;
				if(setsockopt(socket_desc, SOL_SOCKET, SO_RCVTIMEO, (char *)&recv_timeout, sizeof(recv_timeout)) == -1) {
					printf("Error while setting options");
				}

				// we are in delayed ack and check if we get a new segment 
				if(recv(socket_desc, retrieveBuffer, MSS+37, 0) > 0) {
					
					tcp *second_segm = malloc(sizeof(tcp));
					//we got the new segment
					extract_segment(second_segm, retrieveBuffer);
					memset(retrieveBuffer, 0, sizeof(char)*(strlen(retrieveBuffer)+1));
					
					//check if this is an EOF message 
					if(strcmp(second_segm->data, "END") == 0) {
						if(list_length != 0) {

							// writes all the data still in buffer
							ack_segments(fd,socket_desc, &list_length, &buf_segm, &ack,  &recv_win, retrieveBuffer);
						}
						else {
							printf("file receiving completed \n");
							fflush(stdout);
							break;
						}
						free(second_segm);
					}

					else if(list_length < MAX_BUF_SIZE && (recv_win.next_to_ack <= second_segm->sequence_number) && (recv_win.next_to_ack <= recv_win.last_to_ack)) {
						strcat(second_segm->data, "\0");
						buffer_in_order(&buf_segm, second_segm, &recv_win);
						list_length++;
					}
				}

				ack_segments(fd,socket_desc, &list_length, &buf_segm, &ack, &recv_win, retrieveBuffer);
				memset(retrieveBuffer, 0, sizeof(char)*(strlen(retrieveBuffer)+1));
				recv_timeout.tv_sec = 0;
				recv_timeout.tv_usec = 0;
				setsockopt(socket_desc, SOL_SOCKET, SO_RCVTIMEO, (char *)&recv_timeout, sizeof(recv_timeout));
			}
		}
		memset(&ack, 0, sizeof(ack));
		memset(retrieveBuffer, 0, sizeof(char)*(strlen(retrieveBuffer)+1));
	}
	close(fd);

	//resets the time-out
	recv_timeout.tv_sec = 0;
	recv_timeout.tv_usec = 0;
	setsockopt(socket_desc, SOL_SOCKET, SO_RCVTIMEO, (char *)&recv_timeout, sizeof(recv_timeout));
	memset(retrieveBuffer, 0, sizeof(char)*(strlen(retrieveBuffer)+1));
	return 0;		
}

void make_seg(tcp segment, char *send_segm) {
	
	// check the sequence number
	if(segment.sequence_number != 0) {
		unsigned char seq[13];
		sprintf(seq, "%X", htonl(segment.sequence_number));
		concat_segm(send_segm, seq, 13);
	}
	else {
		strcat(send_segm, "0000000000000");
	}

	// check ack
	if(segment.ack_number != 0) {
		unsigned char ack[13];
		sprintf(ack, "%X", htonl(segment.ack_number));
		concat_segm(send_segm, ack, 13);
	}
	else {
		strcat(send_segm, "0000000000000");
	}
	// verify if there is any flag to send
	if(segment.ack) {
		strcat(send_segm, "1");
	}
	else {
		strcat(send_segm, "0");
	}
	if(segment.syn) {
		strcat(send_segm, "1");
	}
	else {
		strcat(send_segm, "0");
	}
	if(segment.fin) {
		strcat(send_segm, "1");
	}
	else {
		strcat(send_segm, "0");
	}

	// verify for the receiver window
	if(segment.receiver_window != 0) {
		char recv[8];
		sprintf(recv, "%X", htons(segment.receiver_window));
		concat_segm(send_segm, recv, 8);
	}
	else {
		strcat(send_segm, "00000000");
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

int count_acked (int min, int max, int acknum) {
	int n_ack;
	int j = 0;

	for(n_ack = ceil(min/MSS); n_ack <= ceil(max/MSS); n_ack++) {
		if(n_ack == ceil(acknum/MSS))
			return j;
		else
			j++;
	}
	return j;
}

// function called when it's necessary to retx a segment with TCP fast retx
void retx(tcp *segments, slid_win win, char *buffer, int socket_desc) {
	for(int i = 0; i < 6; i++) {
		if(win.next_to_ack == segments[i].sequence_number) {
			make_seg(segments[i], buffer);
			send(socket_desc, buffer, strlen(buffer), 0);
			printf("Ritrasmetto segmento con numero di sequenza %d\n", segments[i].sequence_number);
			memset(buffer, 0, sizeof(char)*(strlen(buffer)+1)); //we reset the buffer to send the next segment
			break;
		}
	}
	printf("\n");
}


void buffer_in_order(tcp **segment_head, tcp *to_buf, slid_win *win) {
	tcp *index;
	index = *segment_head;
	while(index != NULL) {
		if(index->sequence_number == to_buf->sequence_number) {
			return;
		}
		index = index->next;
	}

	if(*segment_head == NULL){
		*segment_head = to_buf;
		to_buf->next = NULL;
	}

	else {
		index = *segment_head;
		tcp *temp;
		if(index->next == NULL && index->sequence_number > to_buf->sequence_number) {
			printf("Inserisco veloce in testa\n");
			temp = index;
			to_buf->next = temp;
			*segment_head = to_buf;
			return;
		}
		while(index->next != NULL) {
			if(index->sequence_number > to_buf->sequence_number) {
				if(index == *segment_head) {
					printf("Inserisco in testa\n");
					temp = index;
					to_buf->next = temp;
					*segment_head = to_buf;
					return;
				}
				else {
					printf("Inserisco nel mezzo\n");
					temp = index;
					to_buf->next = temp;
					index = to_buf;
					return;
				}
			}
			else {
				index = index->next;
			}
		}
		printf("Inserisco nel fondo\n");
		index->next = to_buf;
		to_buf->next = NULL;
	}
}

// we call this function to write eventually out of order segments
int write_all(int fd, int list_size, tcp **segm_buff, slid_win *win) {
	int n_wrote = 0;
	tcp *index;
	index = *segm_buff;
	while(index != NULL) {
		if((index)->sequence_number == win->next_to_ack) {
			printf("Manipolo %d, lunghezza dati %ld\n", index->sequence_number, strlen(index->data));
			write(fd, (index)->data, strlen((index)->data));
			win->last_to_ack += strlen((index)->data);
			win->last_correctly_acked = (index)->sequence_number;
			win->tot_acked += strlen((index)->data);
			win->next_seq_num += strlen((index)->data);
			win->next_to_ack += strlen((index)->data);
			n_wrote++;
		}
		// the segment is out of order, we can't send it now
		else {
			break;
		}
		index = index->next;
	}

	//reorder_list(segm_buff, list_size);
	free_sent_segm(segm_buff, n_wrote);
	return n_wrote;
}

void free_sent_segm(tcp ** head, int n_free) {
	tcp *index;
	index = *head;
	tcp *temp_free;
	for(int i = 0; i < n_free; i++) {
		temp_free = index;
		index = index->next;
		memset(temp_free->data, 0, sizeof(char)*(strlen(temp_free->data)+1));
		free(temp_free);
	}
	*head = index;
}

// function used to prepare a segemnt that will be send and to set the window parameters properly
void prepare_segment(tcp * segment, slid_win *wind, char *data,  int index, int n_byte) {
	memset(&segment[index], 0, sizeof(segment[index]));
	strncpy(segment[index].data, data, MSS);
	segment[index].data[MSS+1] = '\0';
	fill_struct(&segment[index], wind->next_seq_num, 0, 0, false, false, false, NULL);
	wind->next_seq_num += strlen(segment[index].data); // sequence number for the next segment
	wind->on_the_fly += n_byte;
	wind->last_to_ack += n_byte;
}

// function used to move the sliding window properly
void slide_window(slid_win *wind, tcp *recv_segm, tcp *segments) {
	wind->n_seg = count_acked(wind->next_to_ack, wind->last_to_ack, recv_segm->ack_number);
	wind->on_the_fly -= wind->n_seg*MSS; // we scale the number of byte acked from the max
	wind->tot_acked = recv_segm->ack_number;
	wind->next_to_ack = recv_segm->ack_number;
	wind->last_correctly_acked = set_last_correctly_acked(recv_segm, segments);//recv_segm->ack_number;
}

int set_last_correctly_acked(tcp *recv_segm, tcp *segm) {
	for(int i = 0; i < 6; i++) {
		if((segm[i].sequence_number + strlen(segm[i].data)) == recv_segm->ack_number) {
			//printf("Segmento in pos %d, ultimo acked in pos %d \n", i, (i+5)%6);
			return segm[i].sequence_number;
		}
	}
}

// function that writes all the segments in order received and reset the buffered segments list length
void ack_segments(int fd, int recv_sock,  int *list_length, tcp **buf_segm, tcp *ack,  slid_win *recv_win, char *retrieveBuffer) {
	*list_length -= write_all(fd, *list_length,  buf_segm, recv_win);
	fill_struct(ack, 0, recv_win->tot_acked, 0, true, false, false, NULL);
	make_seg(*ack, retrieveBuffer);
	send_unreliable(retrieveBuffer, recv_sock);
	
}

/* this fucntion is usefull to reorder the list in case that some segments are processed 
and some other aren't due to they are out of order*/
void reorder_list(tcp *segment_list, int size) {
	int i, j;
	for(i = 0; i < size; i++) {
		if(segment_list[i].data == 0) {
			j = i+1;
			while(segment_list[j].data != 0 && j < size) {
				segment_list[i] = segment_list[j];
				memset(&segment_list[j], 0, sizeof(segment_list[j]));
				i = j;
				j++;
			}
		}
	}
}

// this function will act a situation in which it is possible to lost segments or acks
void send_unreliable(char *segm_to_go, int sockd) {
	int p = rand() % 10;

	// 30% of possibility to lost the segment
	if(p != 0 && p != 1 && p != 2) {
		send(sockd, segm_to_go, strlen(segm_to_go), 0);
	}
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