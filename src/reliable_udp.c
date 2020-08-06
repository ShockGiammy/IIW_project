/*
  REALIABLE_UDP.C
  ========
  
*/

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <sys/socket.h>       /*  socket definitions        */
#include <sys/types.h>        /*  socket types              */
#include <sys/stat.h>   	  /*  stat for files transfer   */
#include <arpa/inet.h>        /*  inet (3) funtions         */
#include <unistd.h>           /*  misc. UNIX functions      */
#include "helper.h"
#include "reliable_udp.h"
#include <pthread.h>

#define MAX_LINE  4096
#define MAX_LINE_DECOR 30
#define CHECK_BIT(var,pos) ((var) & (1<<(pos)))


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

int set_last_correctly_acked(tcp *recv_segm, tcp *segm) {
	for(int i = 0; i < 6; i++) {
		if((segm[i].sequence_number + strlen(segm[i].data)) == recv_segm->ack_number) {
			//printf("Segmento in pos %d, ultimo acked in pos %d \n", i, (i+5)%6);
			return segm[i].sequence_number;
		}
	}
}

// function used to move the sliding window properly
void slide_window(slid_win *wind, tcp *recv_segm, tcp *segments) {
	wind->n_seg = count_acked(wind->next_to_ack, wind->last_to_ack, recv_segm->ack_number);
	wind->on_the_fly -= wind->n_seg*MSS; // we scale the number of byte acked from the max
	wind->tot_acked = recv_segm->ack_number;
	wind->next_to_ack = recv_segm->ack_number;
	wind->last_correctly_acked = set_last_correctly_acked(recv_segm, segments);//recv_segm->ack_number;
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

// this function will act a situation in which it is possible to lost segments or acks
void send_unreliable(char *segm_to_go, int sockd) {
	int p = rand() % 10;

	// 30% of possibility to lost the segment
	if(p != 0 && p != 1 && p != 2) {
		send_tcp(sockd, segm_to_go, strlen(segm_to_go), 0);
	}
}

int send_tcp(int sockd, void* buf, size_t size, int flags){
	bool ack, syn, fin;
	char send_buf[BUFSIZ];
	memset(send_buf, 0, BUFSIZ);
	tcp temp = (const tcp) { 0 };

	ack = CHECK_BIT(flags, 0);
	syn = CHECK_BIT(flags, 1);
	fin = CHECK_BIT(flags, 2);

	fill_struct(&temp, 0, 0, 0, ack, fin, syn, buf);
	make_seg(temp, send_buf);

	//printf("Sending: %s\n\n%s\n", buf, send_buf);

	int ret = send(sockd, send_buf, strlen(send_buf)+1, 0);
	return ret;
}

void recv_tcp_segm(int sockd, tcp* dest_segm){
	char recv_buf[BUFSIZ];
	
	int n = recv(sockd, recv_buf, BUFSIZ, 0);

	if(n == -1){
		perror(strerror(errno));
	}
	
	extract_segment(dest_segm, recv_buf);
}

int recv_tcp(int sockd, char* buf, size_t size){
	tcp recv_segment = (const tcp) { 0 };
	char recv_buf[BUFSIZ];

	read(sockd, recv_buf, BUFSIZ);

	extract_segment(&recv_segment, recv_buf);
	int n = strlen(recv_segment.data) + 1;
	n = (n<size) * n + (n>size) * size;
	memcpy(buf, recv_segment.data, n);
	
	//printf("Received (%d bytes): %s\n\n%s", n, recv_segment.data, buf);

	if(recv_segment.fin && !recv_segment.ack){
		close_server_tcp(sockd);
	}

	return n;
}

int connect_tcp(int socket_descriptor, struct sockaddr* addr, socklen_t addr_len){
	tcp temp;
	temp = (const tcp) { 0 };

	if((connect(socket_descriptor, addr, INET_ADDRSTRLEN)) < 0){
		char str_addr[INET_ADDRSTRLEN];
		inet_ntop(addr->sa_family, addr->sa_data, str_addr, addr_len);
		fprintf(stderr, "Could not connect to: %s\n", str_addr);
		return -1;
	}

	for(int i=0; i< MAX_LINE_DECOR; i++)
		printf("-");
	printf("\nEnstablishing connection...\n");

	printf("Opened socket, sending Syn...\n");

	if( send_tcp(socket_descriptor, NULL, 0, Syn) < 0 ){
		perror("Error while sending syn...\n");
		return -1;
	}

	printf("Waiting server response...\n");
	recv_tcp_segm(socket_descriptor, &temp);

	if((temp.syn & temp.ack) == 0){
		perror("No SYN-ACK from the other end\n");
		return -1;
	}

	printf("Received Syn-Ack, sending Ack...\n");

	if( send_tcp(socket_descriptor, NULL, 0, Ack) < 0 ){
		perror("Error while sending ack...\n");
		return -1;
	}

	for(int i=0; i < MAX_LINE_DECOR; i++)
		printf("-");
	printf("\n");

	return 0;

}

int accept_tcp(int socket_descriptor, struct sockaddr* addr, socklen_t* addr_len){
	
	tcp temp = (const tcp) { 0 };
	int conn_sd = accept(socket_descriptor, addr, addr_len);

	if(conn_sd < 0){
		perror("error during accept\n");
		return -1;
	}

	for(int i=0; i < MAX_LINE_DECOR; i++)
		printf("-");
	printf("\n");

	recv_tcp_segm(conn_sd, &temp);

	if(temp.syn){
		printf("Received Syn, sending Syn-Ack...\n");
		send_tcp(conn_sd, NULL, 0, Syn | Ack);
	}
	else {
		perror("Missing SYN\n");
		return -1;
	}
	
	temp = (const tcp) { 0 };
	recv_tcp_segm(conn_sd, &temp);
	
	if(!temp.ack){
		printf("Received Ack...\n");
		perror("Missing ACK, terminating...\n");
		return -1;
	}

	printf("Connection established\n");
	for(int i=0; i < MAX_LINE_DECOR; i++)
		printf("-");
	printf("\n");
	
	return conn_sd;
}

int close_client_tcp(int sockd){
	tcp temp = (const tcp) { 0 };

	for(int i=0; i < MAX_LINE_DECOR; i++)
		printf("-");
	printf("\nConnection termination\n");
	
	printf("Sending Fin...\n");
	send_tcp(sockd, NULL, 0, Fin);

	recv_tcp_segm(sockd, &temp);

	if((temp.fin & temp.ack) == 0){
		perror("Missing fin-ack, could not close connection...\n");
		return -1;
	}

	printf("Received Fin-Ack, sending Ack...\n");

	if ( send_tcp(sockd, NULL, 0, Ack) < 0 ){
		perror("Failed to send ack, could not close connection...\n");
		return -1;
	}

	int res = close(sockd);
	printf("Connection closed\n");
	for(int i=0; i < MAX_LINE_DECOR; i++)
		printf("-");
	printf("\n");
	
	return res;
}

void close_server_tcp(int sockd){
	tcp temp = (const tcp) { 0 };
	int res = -1;

	char response[MAX_LINE];

	for(int i=0; i < MAX_LINE_DECOR; i++)
		printf("-");
	printf("\n");

	printf("Received Fin, closing connection...\nSending Fin-Ack...\n");

	if( send_tcp(sockd, NULL, 0, Fin | Ack) < 0){
		perror("Failed to send fin-ack, could not close connection\n");
		pthread_exit(&res);

	}

	recv_tcp_segm(sockd, &temp);
	if( !temp.ack ){
		perror("Missing ack, could not close connection...\n");
		pthread_exit(&res);
	}

	printf("Received Ack\n");

	res = close(sockd);
	
	printf("Connection closed\n");
	for(int i=0; i < MAX_LINE_DECOR; i++)
		printf("-");
	printf("\n");

	pthread_exit(&res);
}