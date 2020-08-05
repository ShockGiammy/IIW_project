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

#define MAX_LINE  4096


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
}

// function called when it's necessary to retx a segment with TCP fast retx
void retx(tcp *segments, slid_win win, char *buffer, int socket_desc) {
	for(int i = 0; i < 6; i++) {
		if(win.next_to_ack == segments[i].sequence_number) {
			make_seg(segments[i], buffer);
			write(socket_desc, buffer, strlen(buffer));
			printf("Ritrasmetto segmento con numero di sequenza %d\n", segments[i].sequence_number);
			memset(buffer, 0, sizeof(char)*(strlen(buffer)+1)); //we reset the buffer to send the next segment
			break;
		}
	}
}


void buffer_in_order(int list_size, tcp *segment_head, tcp to_buf, slid_win *win) {
	if(list_size == 0){
		segment_head[0] = to_buf;
	}
	for(int i = 0; i <= list_size-1; i++) {
		if(segment_head[i].sequence_number < to_buf.sequence_number) {
			//tcp temp;
			//temp = segment_head[i];
			segment_head[i+1] = to_buf;
			//segment_head[i+1] = temp;
		}
	}
}

// we call this function to write eventually out of order segments
void write_all(int fd, int list_size, tcp *segm_buff, slid_win *win) {
	for(int i = 0; i < list_size; i++) {
		write(fd, segm_buff[i].data, strlen(segm_buff[i].data));
		win->last_to_ack += strlen(segm_buff[i].data);
		win->last_correctly_acked = segm_buff[i].sequence_number;
		win->tot_acked += strlen(segm_buff[i].data);
		//free the segments
		memset(&segm_buff[i], 0, sizeof(segm_buff));
	}
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
	//make_seg(segment[index], buffer); // we put our segment in a buffer that will be sent over the socket
}

// function used to move the sliding window properly
void slide_window(slid_win *wind, tcp *recv_segm) {
	wind->n_seg = count_acked(wind->next_to_ack, wind->last_to_ack, recv_segm->ack_number);
	wind->on_the_fly -= wind->n_seg*MSS; // we scale the number of byte acked from the max
	wind->tot_acked = recv_segm->ack_number;
	wind->next_to_ack = recv_segm->ack_number;
	wind->last_correctly_acked = recv_segm->ack_number -wind->n_seg*MSS;//abs((sender_wind.last_correctly_acked-recv_segm.ack_number))
	+wind->last_correctly_acked;
}

// function that writes all the segments in order received and reset the buffered segments list length
void ack_segments(int fd, int *list_length, tcp *buf_segm, tcp *ack,  slid_win *recv_win, char *retrieveBuffer) {
	write_all(fd, *list_length,  buf_segm, recv_win);
	*list_length = 0;
	fill_struct(ack, 0, recv_win->tot_acked, 0, true, false, false, NULL);
	make_seg(*ack, retrieveBuffer);
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