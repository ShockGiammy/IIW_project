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
#include <netinet/in.h>
#include <netdb.h>
#include <net/if.h>

#define MAX_TENTATIVES 3

cong_struct *cong;
static slid_win recv_win; // the sliding window for the receiver
static slid_win sender_wind; //sliding window for the sender

int make_seg(tcp segment, char *send_segm) {

	int bytes_written = 0;
	
	// copy the sequence number
	unsigned int* send_segm_ptr = (unsigned int*) send_segm;
	*send_segm_ptr = htonl(segment.sequence_number);
	send_segm_ptr++;
	bytes_written += sizeof(unsigned int);

	// copy ack
	*send_segm_ptr = htonl(segment.ack_number);
	send_segm_ptr++;
	bytes_written += sizeof(unsigned int);

	// copy flags
	bool* send_segm_ptr_flag = (bool*) send_segm_ptr;
	*send_segm_ptr_flag = segment.ack;
	send_segm_ptr_flag++;
	bytes_written += sizeof(bool);

	*send_segm_ptr_flag = segment.syn;
	send_segm_ptr_flag++;
	bytes_written += sizeof(bool);

	*send_segm_ptr_flag = segment.fin;
	send_segm_ptr_flag++;
	bytes_written += sizeof(bool);
	
	// verify for the receiver window
	send_segm_ptr = (unsigned int*) send_segm_ptr_flag;
	*send_segm_ptr = htonl(segment.receiver_window);
	send_segm_ptr++;
	bytes_written += sizeof(unsigned int);

	// send data field length
	*send_segm_ptr = ntohl(segment.data_length); //htonl(segment.data_length);
	send_segm_ptr++;
	bytes_written += sizeof(unsigned int);

	memcpy(send_segm_ptr, segment.data, segment.data_length);
	bytes_written += segment.data_length;

	// printf("Seq num: %d\n", segment.sequence_number);
	// printf("Ack num: %d\n", segment.ack_number);
	// printf("ASF: %d%d%d\n", segment.ack, segment.syn, segment.fin);
	// printf("Data length: %d\n", segment.data_length);

	// printf("make_segment: written %d bytes to send_buf\n", bytes_written);
	
	return bytes_written;
}

int extract_segment(tcp *segment, char *recv_segm) {

	memset(segment, 0, sizeof(*segment));
	memset(&(segment->data), 0, MSS);

	//deserialize sequence number
	unsigned int* recv_buf_ptr = (unsigned int*) recv_segm;
	segment->sequence_number = ntohl(*recv_buf_ptr);
	//printf("Seq num: %d\n", segment->sequence_number);
	recv_buf_ptr++;
	
	// deserialize ack number
	segment->ack_number = ntohl(*recv_buf_ptr);
	//printf("Ack num: %d\n", segment->ack_number);
	recv_buf_ptr++;
	
	// deserialize flags
	bool* recv_buf_ptr_flag = (bool*) recv_buf_ptr;

	segment->ack = *recv_buf_ptr_flag;
	recv_buf_ptr_flag++;

	segment->syn = *recv_buf_ptr_flag;
	recv_buf_ptr_flag++;

	segment->fin = *recv_buf_ptr_flag;
	recv_buf_ptr_flag++;

	//printf("ASF: %d%d%d\n", segment->ack, segment->syn, segment->fin);

	// deserialize rcvwnd
	recv_buf_ptr = (unsigned int*) recv_buf_ptr_flag;
	segment->receiver_window = ntohl(*recv_buf_ptr);
	recv_buf_ptr++;

	// deserialize data field length
	segment->data_length = ntohl(*recv_buf_ptr);
	//printf("Data length: %d\n", segment->data_length);
	recv_buf_ptr++;

	memcpy(segment->data, (char*)recv_buf_ptr, segment->data_length);

	//printf("Extracted %s, %d bytes of data...\n", segment->data, segment->data_length);

	return segment->data_length;
}


void fill_struct(tcp *segment, unsigned long seq_num, unsigned long ack_num, unsigned long recv, bool is_ack, bool is_fin, bool is_syn) {
	segment->sequence_number = seq_num;
	segment->ack_number = ack_num;
	segment->receiver_window = recv;
	segment->ack = is_ack;
	segment->fin = is_fin;
	segment->syn = is_syn;
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
	bool retransmitted = false;
	for(int i = 0; i < MAX_BUF_SIZE; i++) {
		//printf("%d == %d ?\n", win.next_to_ack, segments[i].sequence_number);
		if(win.next_to_ack == segments[i].sequence_number) {
			make_seg(segments[i], buffer);
			retransmitted = true;
			int n_send = send_unreliable(socket_desc, buffer, segments[i].data_length + HEAD_SIZE);
			printf("Ritrasmesso segmento con numero di sequenza %d (%d bytes)\n", segments[i].sequence_number, n_send);
			memset(buffer, 0, segments[i].data_length + HEAD_SIZE); //we reset the buffer to send the next segment
			break;
		}
	}
	if(!retransmitted){
		printf("retx failed\n");
		exit(EXIT_FAILURE);
	}
	printf("\nFinished retx\n");
}

void buffer_in_order(tcp **segment_head, tcp *to_buf, slid_win *win, int* bytes_recvd) {
	tcp *current = *segment_head;

	// check if to_buf was already received
	//printf("buff_in_order: %d < %d ? \n", to_buf->sequence_number, win->tot_acked);
	if(to_buf->sequence_number < win->tot_acked){
		printf("buff_in_order: won't buffer segment\n");
		return;
	}


	// check if to_buf is already in the list
	while(current != NULL){
		if(current->sequence_number == to_buf->sequence_number) {
			printf("buff_in_order: segment already in list\n");
			return;
		}
		current = current->next;
	};

	// update last_byte_buffered and how much free memory is free in recv buffer
	//printf("%d + %d = %d > %d?\n", to_buf->sequence_number, to_buf->data_length, to_buf->data_length + to_buf->sequence_number, win->rcvwnd);
	if(((to_buf->sequence_number) + (to_buf->data_length))%INT_MAX > win->last_byte_buffered){
		win->last_byte_buffered = (to_buf->sequence_number + to_buf->data_length)%INT_MAX;
	}

	*bytes_recvd += to_buf->data_length;
	win->rcvwnd -= to_buf->data_length;
	if(win->rcvwnd < 0) {
		win->rcvwnd = 0;
		return;
	}
	printf("Updating rcvwnd to %d\n", win->rcvwnd);

	// if the list is empty, to_buf becomes the head
	if(*segment_head == NULL){
		// printf("Inserisco in testa, (%d bytes of data)\n", to_buf->data_length);
		*segment_head = to_buf;
		to_buf->next = NULL;
		return;
	}
	else {
		// there are segments in the list
		// look for the correct position of to_buf in the list based on seq_num

		// if seq_num of to_buf is > than that of the head, to_buf becomes the new head
		if( ( *segment_head )->sequence_number > to_buf->sequence_number){
			// printf("Inserisco in testa\n");
			to_buf->next = *segment_head;
			*segment_head = to_buf;
			return;
		}

		current = *segment_head;
		int pos_in_list = 0;
		tcp* next = current->next;
		
		// the current segment has always seq_num <= than that of to_buf
		do {
			if(next == NULL){
				pos_in_list++;
				// printf("Inserisco in posizione %d\n", pos_in_list);
				current->next = to_buf;
				to_buf->next = NULL;
				return;
			}
			else if(next->sequence_number > to_buf->sequence_number) {
				// to_buf must be inserted in the list before the next segment
				pos_in_list++;
				// printf("Inserisco in posizione %d\n", pos_in_list);
				current->next = to_buf;
				to_buf->next = next;
				return;
			}
			else {
				pos_in_list++;
				current = current->next;
				next = next->next;
			};

		} while(1);
	}
}

// we call this function to write eventually out of order segments
int write_all(char** buf, int list_size, tcp **segm_buff, slid_win *win) {
	int n_wrote = 0;
	tcp *current = *segm_buff;
	while(current != NULL) {
		// printf(" %d == %d ?\n", current->sequence_number, win->next_to_ack);
		if((current)->sequence_number == win->next_to_ack) {
			// printf("write_all: Manipolo %d, lunghezza dati %ld, copio in %p\n", current->sequence_number, current->data_length, *buf);

			int n = current->data_length;
			memcpy(*buf, current->data, n);
			//printf("write_all: \nData: %s\nCopied: %s\n", current->data, *buf);
			
			*buf += n;
			//printf("write_all: last_to_ack %d -> ", win->last_to_ack);
			win->last_to_ack += n;
			win->last_to_ack %= INT_MAX;
			//printf("%d\n", win->last_to_ack);
			win->last_correctly_acked = (current)->sequence_number;
			
			win->tot_acked += n;
			win->tot_acked %= INT_MAX;

			win->next_seq_num += n;
			win->next_seq_num %= INT_MAX;

			win->next_to_ack += n;
			win->next_to_ack %= INT_MAX;

			n_wrote++;
			//printf("write_all: %d bytes acked / next_to_ack: %d\n", win->tot_acked, win->next_to_ack);
		}
		else {
			// the segment is out of order, we can't write it now
			break;
		}
		//printf("current = %p\n", current->next);
		current = current->next;
	}
	// printf("list_length %d, freeing %d\n", list_size, n_wrote);
	free_segms_in_buff(segm_buff, n_wrote);
	return n_wrote;
}

// function used to prepare a segemnt that will be send and to set the window parameters properly
void prepare_segment(tcp * segment, slid_win *wind, char *data,  int index, int n_byte, int flags) {
	bool ack, syn, fin;

	ack = CHECK_BIT(flags, 0);
	syn = CHECK_BIT(flags, 1);
	fin = CHECK_BIT(flags, 2);
	
	memset(&(segment[index]), 0, sizeof(*segment));
	memcpy(segment[index].data, data, n_byte);
	segment[index].data_length = n_byte;
	fill_struct(&segment[index], wind->next_seq_num, 0, wind->rcvwnd, ack, fin, syn);
	// printf("SEQ_NUM: %d\n", wind->next_seq_num);
	//printf("ASF: %d%d%d\n", segment[index].ack, segment[index].syn, segment[index].fin);
	
	wind->next_seq_num += n_byte; // sequence number for the next segment
	wind->next_seq_num %= INT_MAX;

	wind->on_the_fly += n_byte;
	wind->on_the_fly %= INT_MAX;
	//printf("last_to_ack %d -> ", wind->last_to_ack);
	wind->last_to_ack += n_byte;
	wind->last_to_ack %= INT_MAX;
	//printf("%d\n", wind->last_to_ack);
	//printf("prepare_segment: Copying %d to position %d\n", segment[index].sequence_number, index);
}

// function used to move the sliding window properly
void slide_window(slid_win *wind, tcp *recv_segm, tcp *segments) {
	wind->n_seg = count_acked(wind->next_to_ack, wind->last_to_ack, recv_segm->ack_number);
	wind->on_the_fly -= wind->n_seg*MSS; // we scale the number of byte acked from the max
	if(wind->on_the_fly < 0){
		wind->on_the_fly = 0;
	}
	wind->tot_acked = recv_segm->ack_number;
	wind->next_to_ack = recv_segm->ack_number;
}

int send_flags(int sockd, int flags){
	tcp segment;
	char buf[HEAD_SIZE];
	bool ack, syn, fin;
	
	memset(buf, 0, HEAD_SIZE);

	ack = CHECK_BIT(flags, 0);
	syn = CHECK_BIT(flags, 1);
	fin = CHECK_BIT(flags, 2);
	
	memset(&segment, 0, sizeof(segment));
	fill_struct(&segment,0, 0, MAX_WIN, ack, fin, syn);
	printf("ASF: %d%d%d\n", segment.ack, segment.syn, segment.fin);
	make_seg(segment, buf);
	int ret = write(sockd, buf, HEAD_SIZE);
	return ret;
}

// function that writes all the segments in order received and reset the buffered segments list length
void ack_segments(char** buf, int recv_sock,  int *list_length, tcp **buf_segm, tcp *ack,  slid_win *recv_win) {
	char buff[BUFSIZ];
	memset(buff, 0 , BUFSIZ);
	memset(ack->data, 0, MSS);
	*list_length -= write_all(buf, *list_length,  buf_segm, recv_win);
	fill_struct(ack, 0, recv_win->tot_acked, recv_win->rcvwnd, true, false, false);
	ack->data_length = 0;
	//printf("ack: %d,%d %d%d%d %d\n", ack->sequence_number, ack->ack_number, ack->ack, ack->syn, ack->fin, ack->data_length);
	make_seg(*ack, buff);
	int n = send_unreliable(recv_sock, buff, HEAD_SIZE);
	//printf("Sent %d bytes for ack...\n", n);
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
				memset(&segment_list[j], 0, sizeof(tcp));
				i = j;
				j++;
			}
		}
	}
}

void free_segms_in_buff(tcp ** head, int n_free) {
	tcp* temp;
	for(int i = 0; i < n_free; i++) {
		temp = *head;
		*head = (*head)->next;
		free(temp);
	}
}

// this function will act a situation in which it is possible to lost segments or acks
int send_unreliable(int sockd, char *segm_to_go, int n_bytes) {
	int p = rand() % 20;

	// 1/20 % of probability to lose the segment
	if(p != 0) {
		// printf("Send success...\n");
		//printf("Sto inviando %s\n", segm_to_go);
		return send(sockd, segm_to_go, n_bytes, 0);
	}

	return 0;
}

int send_tcp(int sockd, void* buf, size_t size){

	char send_buf[MSS+HEAD_SIZE];
	char data_buf[MSS];
	char recv_ack_buf[HEAD_SIZE];
	memset(send_buf, 0, MSS+HEAD_SIZE);
	memset(data_buf, 0, MSS);

	int bytes_left_to_send = size;
	tcp send_segm[MAX_BUF_SIZE]; // keeps the segment that we send, so that we can perform resending and/or acknowledge.
	tcp recv_segm = (const tcp) { 0 }; // used to unpack the ack and see if everything was good
	int n_read = 0;
	int times_retx = 0;
	sender_wind.on_the_fly = 0;
	sender_wind.bytes_acked_current_transmission = 0;
		
	for(int j=0; j < MAX_BUF_SIZE-1; j++)
		memset(&send_segm[j], 0, sizeof(tcp));
	
	//sender_wind.max_size = MSS; //we accept at most BUFSIZ bytes on the fly at the same time
	//printf("congWin: %d\n", cong->cong_win);
	sender_wind.max_size = cong->cong_win < MAX_WIN-MSS ? cong->cong_win : MAX_WIN-MSS;
	sender_wind.max_size = sender_wind.max_size < size ? sender_wind.max_size : size;
	//printf("sending %d bytes...\n", size);
	//printf("max: %d\nlast_to_ack: %d\n", sender_wind.max_size, sender_wind.last_to_ack);
	time_out send_timeo;
	memset(&send_timeo, 0, sizeof(send_timeo));
	send_timeo.time.tv_sec = 5; // we set the first timeout to 3sec, as it's in TCP standard

	/*struct timeval time_out;
	time_out.tv_sec = 3; // we set 6 sec of timeout, we will estimate it in another moment
	time_out.tv_usec = 0;*/
	struct timeval start_rtt;
	struct timeval finish_rtt;

	if(setsockopt(sockd, SOL_SOCKET, SO_RCVTIMEO, (char *)&send_timeo.time, sizeof(send_timeo.time)) == -1) {
		printf("Sender error setting opt\n%s\n", strerror(errno));
	}

	int index = 0; // sender segments list index

	while(bytes_left_to_send > 0 || sender_wind.bytes_acked_current_transmission < size) {
		/*printf("%d bytes acked out of %d\n", sender_wind.bytes_acked_current_transmission, size);
		printf("%d < %d ?\n", sender_wind.on_the_fly, sender_wind.max_size);*/
		if(sender_wind.on_the_fly < 0){
			fprintf(stderr, "on_the_fly variabile is negative: %d\n", sender_wind.on_the_fly);
			exit(EXIT_FAILURE);
		}
		if((bytes_left_to_send > 0 && sender_wind.on_the_fly < sender_wind.max_size)) {
			int n_to_copy = (bytes_left_to_send > MSS)*MSS + (bytes_left_to_send <= MSS)*bytes_left_to_send;
			//printf("Taking %d bytes from input buf\n", n_to_copy);
			memcpy(data_buf, buf, n_to_copy);
			// printf("Copied: %s\n", data_buf);
			bytes_left_to_send -= n_to_copy;
			data_buf[n_to_copy] = 0;
			n_read += n_to_copy;
			buf += n_to_copy-1 ;
			//printf("Total bytes read : %d\n", n_read);

			// printf("Da copiare : %d\n", n_to_copy);
			if(n_to_copy >= 0) {
				//printf("index: %d, max: %d\n", index, MAX_BUF_SIZE);
				// we check if we can send data without exceeding the max number of bytes on the fly
				prepare_segment(send_segm, &sender_wind, data_buf, index, n_to_copy, 0);
				int n_buf = make_seg(send_segm[index], send_buf); // we put our segment in a buffer that will be sent over the socket
				//int n_send = send(sockd, send_buf, n_buf, 0);
				// printf("Sent %d bytes...\n", n_send);

				int n_send = send_unreliable(sockd, send_buf, n_buf);
				//printf("Sent %d bytes(unreliable)...\n", n_send);
				memset(send_buf, 0, HEAD_SIZE + MSS); //we reset the buffer to send the next segment
				memset(data_buf, 0, MSS); // we reset the buffer so that we can reuse it
				index++;
				index %= MAX_BUF_SIZE;
				//printf("index: %d, max: %d\n", index, MAX_BUF_SIZE);

			}
		}
		gettimeofday(&start_rtt, NULL);

		// we have read the max number of data, we proceed with the sending in pipelining
		//printf("%d >= %d || %d >= %d\n", sender_wind.on_the_fly, sender_wind.max_size, n_read, size);
		if(sender_wind.on_the_fly >= sender_wind.max_size || n_read >= size) {
			printf("Waiting for ack...\n");
			memset(recv_ack_buf, 0, HEAD_SIZE);
			if(recv(sockd, recv_ack_buf, HEAD_SIZE, 0) > 0) { //we expect a buffer with only header and no data

				gettimeofday(&finish_rtt, NULL);
				estimate_timeout(&send_timeo, start_rtt, finish_rtt);
				if(setsockopt(sockd, SOL_SOCKET, SO_RCVTIMEO, (char *)&send_timeo.time, sizeof(send_timeo.time)) == -1) {
					printf("Sender error setting opt\n%s\n", strerror(errno));
				}
				// printf("The next time out will be of %ld sec and %ld usec \n\n", send_timeo.time.tv_sec, send_timeo.time.tv_usec);
				memset(&recv_segm, 0, sizeof(recv_segm));
				extract_segment(&recv_segm, recv_ack_buf);
				memset(recv_ack_buf, 0, HEAD_SIZE);

				/*printf("Received ack: %d\n", recv_segm.ack_number);
				printf("%d == %d ?\n", recv_segm.ack_number, sender_wind.next_to_ack);
				printf("else %d < %d && %d <= %d ?\n", sender_wind.next_to_ack, recv_segm.ack_number, recv_segm.ack_number, sender_wind.last_to_ack);
				*/
				if(recv_segm.ack_number == sender_wind.next_to_ack) {
					printf("Ricevuto un riscontro duplicato\n");
					sender_wind.dupl_ack++; // we increment the number of duplicate acks received
					congestion_control_caseFastRetrasmission_duplicateAck(sender_wind);
					check_size_buffer(sender_wind, recv_segm.receiver_window);

					//fast retransmission
					if(sender_wind.dupl_ack == 3) {
						times_retx++;
						printf("Fast retx\n");
						congestion_control_duplicateAck(sender_wind);
						check_size_buffer(sender_wind, recv_segm.receiver_window);
						retx(send_segm, sender_wind, send_buf, sockd);
					}
				}
				else if((sender_wind.next_to_ack < recv_segm.ack_number) && (recv_segm.ack_number <= sender_wind.last_to_ack)) {
					printf("Ack OK. Sliding the window...\n");
					int bytes_acked = recv_segm.ack_number - sender_wind.next_to_ack;
					if(bytes_acked < 0){
						fprintf(stderr, "Bytes acked is negative: %d\n", bytes_acked);
						return -1;
					}
					sender_wind.bytes_acked_current_transmission += bytes_acked;
					congestion_control_receiveAck(sender_wind);
					check_size_buffer(sender_wind, recv_segm.receiver_window);

					slide_window(&sender_wind, &recv_segm, send_segm);

					times_retx = 0;
					
					//resets the segments that have been acked
					//free_acked_segms_in_sender_segms(sender_wind.tot_acked, send_segm, MAX_BUF_SIZE);
				}
			}

			// we have to retx the last segment not acked due to TO
			else {
				times_retx++;
				printf("TO expired\nRetransmitting segment...\n");
				congestion_control_timeout(sender_wind);
				check_size_buffer(sender_wind, recv_segm.receiver_window);
				retx(send_segm, sender_wind, send_buf, sockd);
			}
			memset(recv_segm.data, 0, recv_segm.data_length);
			memset(send_buf, 0, MSS+HEAD_SIZE);

			if(times_retx >= MAX_ATTMPTS_RETX){
				fprintf(stderr, "Retransmitted %d times but did not receive any reply...\n", times_retx);
				break;
			}
		}
	}
	
	// printf("%d bytes acked out of %d\n", sender_wind.bytes_acked_current_transmission, size);
	
	// resets the time-out
	send_timeo.time.tv_sec = 0; // we set 6 sec of timeout, we will estimate it in another moment
	send_timeo.time.tv_usec = 0;
	if(setsockopt(sockd, SOL_SOCKET, SO_RCVTIMEO, (char *)&send_timeo.time, sizeof(send_timeo.time)) == -1) {
		printf("Sender error setting opt\n");
	}
	memset(send_buf, 0, MSS+HEAD_SIZE);
	//printf("Finished transmission (%d bytes)...\n", size - bytes_left_to_send);
	return size - bytes_left_to_send;
}

// void free_acked_segms_in_sender_segms(int ack, tcp* sender_segms_list, int list_size){
// 	for(int i=0; i<list_size; i++){
// 		tcp current = sender_segms_list[i];
// 		if(current.sequence_number + current.data_length <= ack){

// 		}
// 	}
// }

int calculate_window_dimension() {
	if (cong->cong_win < MAX_WIN) {
		return cong->cong_win;
	}
	else {
		return MAX_WIN;
	}
}

int congestion_control_receiveAck(slid_win sender_wind) {

	switch(cong->state)
	{
		case 0:
			cong->cong_win = cong->cong_win + MSS;
			if (cong->cong_win > cong->threshold) {
				cong->state = 1;
				printf("Entered in Congestion Avoidance\n");
			}
			break;

		case 1:
			cong->support_variable = cong->support_variable + (int)floor(MSS*MSS/cong->cong_win);
			if (cong->support_variable >= MSS) {
				cong->cong_win = cong->cong_win + MSS;
				cong->support_variable = cong->support_variable - MSS;
			}
			break;

		case 2:
			cong->cong_win = cong->threshold;
			cong->state = 1;
			printf("Entered in Congestion Avoidance\n");
			break;
	}
	return 0;
}

int congestion_control_duplicateAck(slid_win sender_wind) {
	switch(cong->state)
	{
		case 0:
			cong->state = 2;
			printf("Entered in Fast Recovery\n");
			cong->threshold = cong->cong_win/2;
			cong->cong_win = cong->threshold + 3*MSS;
			break;

		case 1:
			cong->state = 2;
			printf("Entered in Fast Recovery\n");
			cong->threshold = cong->cong_win/2;
			cong->cong_win = cong->threshold + 3*MSS;
			break;

		case 2:
			// caso già gestito
			break;
	}
	return 0;
}

int congestion_control_caseFastRetrasmission_duplicateAck(slid_win sender_wind) {
	if (cong->state == 2) {
		cong->cong_win = cong->cong_win + MSS;
	}
}

int congestion_control_timeout(slid_win sender_wind) {
	switch(cong->state)
	{
		case 0:
			cong->threshold = cong->cong_win/2;
			cong->cong_win = MSS;
			break;

		case 1:
			cong->state = 0;
			printf("Entered in Slow Start\n");
			cong->threshold = cong->cong_win/2;
			cong->cong_win = MSS;
			break;

		case 2:
			cong->state = 0;
			printf("Entered in Slow Start\n");
			cong->threshold = cong->cong_win/2;
			cong->cong_win = MSS;
			break;
	}
	return 0;
}

int check_size_buffer(slid_win sender_wind, int receiver_window) {
	if (cong->cong_win < receiver_window) {
		sender_wind.max_size = cong->cong_win;
	}
	else {
		sender_wind.max_size = receiver_window;
	}

	//printf("congWin %d\n", cong->cong_win);
	//printf("threshold %d\n", cong->threshold);
	//printf("receiver_window %d\n", receiver_window);
	//printf("max_size %d\n", sender_wind.max_size);
	return 0;
}

int recv_tcp(int sockd, void* buf, size_t size){

	char *buf_ptr = buf;

	int n=0;
	char recv_buf[MSS+HEAD_SIZE];
	memset(recv_buf, 0, MSS+HEAD_SIZE);

	// the head for our segment linked list
	tcp *buf_segm = NULL;

	tcp ack;
	int list_length = 0;
	recv_win.rcvwnd = size;
	
	struct timeval recv_timeout;
	recv_timeout.tv_sec = RECV_TIMEOUT_SEC;
	recv_timeout.tv_usec = 0;
	setsockopt(sockd, SOL_SOCKET, SO_RCVTIMEO, (char *)&recv_timeout, sizeof(recv_timeout));

	bool finished = false;
	bool received_data = false;

	int bytes_rcvd = 0;

	while (!finished && (n = recv(sockd, recv_buf, MSS+HEAD_SIZE, 0)) > 0) {
		//printf("Received %d bytes: %s\n", n, recv_buf);

		recv_timeout.tv_sec = 0;
		recv_timeout.tv_usec = 0;
		if(setsockopt(sockd, SOL_SOCKET, SO_RCVTIMEO, (char *)&recv_timeout, sizeof(recv_timeout)) == -1){
			printf("Error while setting options");
			return -1;
		}

		if( n<0 ) {
			fprintf(stderr, "recv: no bytes read\n%s\n", strerror(errno));
		}
		else if(n == 0){
			printf("TO expired\n");
			finished = true;
			continue;
		};
		
		tcp *segment = malloc(sizeof(tcp));
		memset(segment, 0, sizeof(tcp));
		extract_segment(segment, recv_buf);

		received_data = segment->data_length != 0;
		
		if(segment->fin && !segment->ack){
			close_server_tcp(sockd);
		}
		
		//printf("New segment of %d bytes, seq num %d\n", segment->data_length, segment->sequence_number);
		memset(recv_buf, 0, MSS+HEAD_SIZE);
		
		//printf("%d <= %d && %d <= %d\n", recv_win.next_to_ack, segment->sequence_number, segment->sequence_number, recv_win.last_to_ack);

		// buffer segments
		if(list_length < MAX_BUF_SIZE && (recv_win.next_to_ack <= segment->sequence_number) && ( segment->sequence_number <= recv_win.last_to_ack)) {
			list_length++;
			printf("Buffering segment...\n");
			buffer_in_order(&buf_segm, segment, &recv_win, &bytes_rcvd);
		}

		if(received_data){
			recv_timeout.tv_sec = 0;
			recv_timeout.tv_usec = 500000;
			if(setsockopt(sockd, SOL_SOCKET, SO_RCVTIMEO, (char *)&recv_timeout, sizeof(recv_timeout)) == -1) {
				printf("Error while setting options");
				return -1;
			}
		}
		else{
			recv_timeout.tv_sec = RECV_TIMEOUT_SEC;
			recv_timeout.tv_usec = 0;
			if(setsockopt(sockd, SOL_SOCKET, SO_RCVTIMEO, (char *)&recv_timeout, sizeof(recv_timeout)) == -1) {
				printf("Error while setting options");
				return -1;
			}
		}

		// delayed ack -> check if we get a new segment 
		//printf("%d <= %d ?\n", segment->data_length, recv_win.rcvwnd);
		if(segment->data_length <= recv_win.rcvwnd) {
			if((n = recv(sockd, recv_buf, MSS+HEAD_SIZE, 0) > 0)){
				tcp *second_segm = malloc(sizeof(tcp));
				// we got the new segment
				extract_segment(second_segm, recv_buf);
				
				received_data = second_segm->data_length != 0;

				if(second_segm->fin && !second_segm->ack){
					close_server_tcp(sockd);
				}

				//printf("New segment of %d bytes, seq num %d\n", second_segm->data_length, second_segm->sequence_number);
				memset(recv_buf, 0, MSS+HEAD_SIZE);

				if(list_length < MAX_BUF_SIZE && (recv_win.next_to_ack <= second_segm->sequence_number) && ( second_segm->sequence_number <= recv_win.last_to_ack)) {
					list_length++;
					printf("Buffering segment...\n");
					buffer_in_order(&buf_segm, second_segm, &recv_win, &bytes_rcvd);
				}
			}
			else if(n == 0){
				printf("TO expired\n");
				finished = true;
				continue;
			}
			else {
				fprintf(stderr, "recv: no bytes read\n%s\n", strerror(errno));
			};
		}
		else {
			// segment->data_length > recv_win.rcvwnd)
			// can't receive anymore
			finished = true;
		}

		if(recv_win.rcvwnd <= 0){
			printf("Receiver cannot receive any data...\n");
			finished = true;
		}
		
		ack_segments(&buf_ptr, sockd, &list_length, &buf_segm, &ack, &recv_win);
		//printf("Totale riscontrati %d\n", recv_win.tot_acked);

		if(bytes_rcvd == size) {
			finished = true;
		}
		
		memset(recv_buf, 0, MSS+HEAD_SIZE);
		//printf("Resetto il timeout\n");
		recv_timeout.tv_sec = 0;
		recv_timeout.tv_usec = 0;
		setsockopt(sockd, SOL_SOCKET, SO_RCVTIMEO, (char *)&recv_timeout, sizeof(recv_timeout));

		if(received_data){
			recv_timeout.tv_sec = 0;
			recv_timeout.tv_usec = RECV_TIMEOUT_SHORT_USEC;
			if(setsockopt(sockd, SOL_SOCKET, SO_RCVTIMEO, (char *)&recv_timeout, sizeof(recv_timeout)) == -1) {
				printf("Error while setting options");
				return -1;
			}
		}
		else{
			recv_timeout.tv_sec = RECV_TIMEOUT_SEC;
			recv_timeout.tv_usec = 0;
			if(setsockopt(sockd, SOL_SOCKET, SO_RCVTIMEO, (char *)&recv_timeout, sizeof(recv_timeout)) == -1) {
				printf("Error while setting options");
				return -1;
			}
		}
		memset(&ack, 0, sizeof(ack));
		memset(recv_buf, 0, MSS+HEAD_SIZE);

		//printf("Finished? %d\n", finished);
	}

	if(n < 0){
		fprintf(stderr, "errore recv: %s\n", strerror(errno));
	}
	else{
		printf("recv timeout...\n");
	}

	ack_segments(&buf_ptr, sockd, &list_length, &buf_segm, &ack, &recv_win);
	//free_segms_in_buff(&buf_segm, list_length);

	//resets the time-out
	recv_timeout.tv_sec = 0;
	recv_timeout.tv_usec = 0;
	setsockopt(sockd, SOL_SOCKET, SO_RCVTIMEO, (char *)&recv_timeout, sizeof(recv_timeout));
	memset(recv_buf, 0, MSS+HEAD_SIZE);

	printf("Finished transmission...\n");
	return bytes_rcvd;
}

int recv_tcp_segm(int sockd, tcp* dest_segm){
	char recv_buf[HEAD_SIZE];
	memset(recv_buf, 0, HEAD_SIZE);
	int n = recv(sockd, recv_buf, HEAD_SIZE, 0);

	memset(dest_segm, 0, sizeof(tcp));
	extract_segment(dest_segm, recv_buf);
	
	if(dest_segm->fin && !dest_segm->ack){
		close_server_tcp(sockd);
	}

	return n;
}

// function for timeout estimation

void estimate_timeout(time_out *timeo, struct timeval first_time, struct timeval last_time) {
	struct timeval result; // temp struct to save the result;

	result.tv_sec = last_time.tv_sec - first_time.tv_sec;
	result.tv_usec = last_time.tv_usec - first_time.tv_usec;
	
	// it may be that the microsec is a negative value, so we scale it until it is positive
	while(result.tv_usec < 0) {
		result.tv_usec += 1000000; // we add 1 sec
		result.tv_sec --; // we subtract the sec we added to the microsec
	}

	// calculate the value of the Estimated_RTT
	timeo->est_rtt.tv_sec = 0.125*result.tv_sec + (1-0.125)*timeo->est_rtt.tv_sec;
	timeo->est_rtt.tv_usec = 0.125*result.tv_usec + (1-0.125)*timeo->est_rtt.tv_usec;

	// calculate the value of the Dev_RTT
	timeo->dev_rtt.tv_sec = (1-0.25)*timeo->dev_rtt.tv_sec + 0.25*abs(timeo->est_rtt.tv_sec - result.tv_sec);
	timeo->dev_rtt.tv_usec = (1-0.25)*timeo->dev_rtt.tv_usec + 0.25*abs(timeo->est_rtt.tv_usec - result.tv_usec);

	// set the new value for the timeout
	timeo->time.tv_sec = timeo->est_rtt.tv_sec + 4*timeo->dev_rtt.tv_sec;
	timeo->time.tv_usec = timeo->est_rtt.tv_usec + 4*timeo->dev_rtt.tv_usec;

	while(timeo->time.tv_sec > 1000000) {
		timeo->time.tv_sec += 1;
		timeo->time.tv_usec -= 1000000;
	}
}

int connect_tcp(int socket_descriptor, struct sockaddr_in* addr, socklen_t addr_len){
	tcp head_rcv = (const tcp) {0};
	char recv_buf[HEAD_SIZE];
	char snd_buf[HEAD_SIZE];
	memset(recv_buf, 0, HEAD_SIZE);
	memset(snd_buf, 0, HEAD_SIZE);
	memset(&head_rcv, 0, sizeof(tcp));
	
	memset(&recv_win, 0, sizeof(recv_win));
	memset(&sender_wind, 0, sizeof(recv_win));

	recv_win.last_to_ack = MAX_WIN-MSS;


	struct sockaddr_in server_addr;
	bzero(&server_addr, sizeof(server_addr));

	struct sockaddr_in new_sock_addr;
	memset(&new_sock_addr, 0, sizeof(new_sock_addr));
    new_sock_addr.sin_family      = AF_INET;
	new_sock_addr.sin_addr 		  = addr->sin_addr;

	for(int i=0; i< MAX_LINE_DECOR; i++)
		printf("-");
	printf("\nEnstablishing connection...\n");
	
	char address_string[INET_ADDRSTRLEN];
	int tentatives = 0;
	new_sock_addr.sin_port = getpid() + 1024;

	for (int i = 0; i < MAX_TENTATIVES; i++) {
		if (new_sock_addr.sin_port <= 65536) {
			inet_ntop(new_sock_addr.sin_family, &new_sock_addr.sin_addr, address_string, INET_ADDRSTRLEN);

			printf("Binding to %s(%d)\n", address_string, ntohs(new_sock_addr.sin_port));

			int bind_value;
			if (bind_value = bind(socket_descriptor, (struct sockaddr *) &new_sock_addr, sizeof(new_sock_addr)) < 0 ) {
				fprintf(stderr, "connect: bind error\n%s\n", strerror(errno));
				tentatives++;
				if (tentatives == 2) {
					exit(EXIT_FAILURE);
				}
    		}
			if (bind_value == 0) {
				break;
			}
		}
		new_sock_addr.sin_port = new_sock_addr.sin_port + PROCESSES;
	}

	char server_address_string[INET_ADDRSTRLEN];
	inet_ntop(addr->sin_family, &addr->sin_addr, server_address_string, INET_ADDRSTRLEN);

	printf("Connecting to %s(%d)\n", server_address_string, ntohs(addr->sin_port));

	printf("Opened socket, sending Syn...\n");

	tcp segment;

	memset(&segment, 0, sizeof(segment));
	fill_struct(&segment,0, 0, MAX_WIN, 0, 0, 1);
	printf("ASF: %d%d%d\n", segment.ack, segment.syn, segment.fin);
	make_seg(segment, snd_buf);

	socklen_t len = INET_ADDRSTRLEN;
	if( sendto(socket_descriptor, snd_buf, HEAD_SIZE, 0, (struct sockaddr*) addr, len) < 0 ){
		perror("Error while sending syn...\n");
		return -1;
	}

	printf("Waiting server response...\n");
	if( recvfrom(socket_descriptor, recv_buf, HEAD_SIZE, 0, (struct sockaddr *) &server_addr, &len) < 0 ){
		fprintf(stderr, "recvfrom: error\n%s\n", strerror(errno));
		return -1;
	}

	if( connect(socket_descriptor, (struct sockaddr *) &server_addr, addr_len) < 0){
		fprintf(stderr, "connect: udp connect error\n%s\n", strerror(errno));
		exit(EXIT_FAILURE);
	}

	extract_segment(&head_rcv, recv_buf);

	if((head_rcv.syn & head_rcv.ack) == 0){
		perror("No SYN-ACK from the other end\n");
		return -1;
	}

	printf("Received Syn-Ack, sending Ack...\n");
	if( send_flags(socket_descriptor, Ack) < 0 ){
		perror("Error while sending ack...\n");
		return -1;
	}

	for(int i=0; i < MAX_LINE_DECOR; i++)
		printf("-");
	printf("\n");

	cong = malloc(sizeof(cong_struct));
	cong->state = 0;
	cong->cong_win = MSS;
	cong->threshold = 64000;
	cong->support_variable = 0;
	return 0;
}

int accept_tcp(int sockd, struct sockaddr* addr, socklen_t* addr_len){
	char recv_buf[HEAD_SIZE];
	memset(recv_buf, 0, HEAD_SIZE);
	memset(&recv_win, 0, sizeof(recv_win));
	memset(&sender_wind, 0, sizeof(recv_win));

	tcp head_rcv = { 0 };
	struct sockaddr_in client_address;
	bzero(&client_address, sizeof(client_address));
	struct in_addr addr_from_client;

	recv_win.last_to_ack = MAX_WIN-MSS;

	for(int i=0; i < MAX_LINE_DECOR; i++)
		printf("-");
	printf("\n");

	//printf("Waiting for syn...\n");
	socklen_t len = sizeof(client_address);
	if( recvfrom(sockd, recv_buf, HEAD_SIZE, 0, (struct sockaddr *) &client_address, &len) < 0 ){
		fprintf(stderr, "recvfrom: error\n%s\n", strerror(errno));
		return -1;
	}

	char address_string[INET_ADDRSTRLEN];
	inet_ntop(client_address.sin_family, &client_address.sin_addr, address_string, INET_ADDRSTRLEN);

	printf("Received new packet from %s(%d)\n", address_string, ntohs(client_address.sin_port));

	extract_segment(&head_rcv, recv_buf);

	int sock_conn = socket(AF_INET, SOCKET_TYPE, IPPROTO_UDP);
	
	int port = getpid() + 1024;

	struct sockaddr_in new_sock_addr;
	int new_port = port + PROCESSES;

	memset(&new_sock_addr, 0, sizeof(new_sock_addr));
    new_sock_addr.sin_family      = AF_INET;
	new_sock_addr.sin_addr 		  = client_address.sin_addr;
    new_sock_addr.sin_port        = htons(new_port);

	memset(address_string, 0, INET_ADDRSTRLEN);
	inet_ntop(new_sock_addr.sin_family, &new_sock_addr.sin_addr, address_string, INET_ADDRSTRLEN);

	printf("Binding to %s(%d)\n", address_string, ntohs(new_sock_addr.sin_port));

    if ( bind(sock_conn, (struct sockaddr *) &new_sock_addr, sizeof(new_sock_addr)) < 0 ) {
		fprintf(stderr, "server: errore durante la bind \n%s\n", strerror(errno));
		exit(EXIT_FAILURE);
    }

	char client_address_string[INET_ADDRSTRLEN];
	inet_ntop(client_address.sin_family, &client_address.sin_addr, client_address_string, INET_ADDRSTRLEN);

	printf("Connecting to %s(%d)\n", client_address_string, ntohs(client_address.sin_port));

	if( connect(sock_conn, (struct sockaddr*) &client_address, len) < 0){
		perror("connect: failed to create socket with bound client address\n");
		fprintf(stderr, "%s\n", strerror(errno));
		return -1;
	}
	
	printf("head: %d,%d %d%d%d %d\n", head_rcv.sequence_number, head_rcv.ack_number, head_rcv.ack, head_rcv.syn, head_rcv.fin, head_rcv.data_length);
	if(head_rcv.syn){
		printf("Received Syn, sending Syn-Ack...\n");
		send_flags(sock_conn, Syn | Ack);
	}
	else {
		perror("Missing SYN\n");
		return -1;
	}
	
	head_rcv = (const tcp) { 0 };
	printf("Waiting for ack...\n");
	while ( recv_tcp_segm(sock_conn, &head_rcv) <= 0 );

	printf("head: %d,%d %d%d%d %d\n", head_rcv.sequence_number, head_rcv.ack_number, head_rcv.ack, head_rcv.syn, head_rcv.fin, head_rcv.data_length);
	
	if(!head_rcv.ack || head_rcv.fin || head_rcv.syn){
		perror("Missing ACK, terminating...\n");
		return -1;
	}
	
	printf("Received Ack...\n");

	printf("Connection established\n");
	for(int i=0; i < MAX_LINE_DECOR; i++)
		printf("-");
	printf("\n");
	
	cong = malloc(sizeof(cong_struct));
	cong->state = 0;
	cong->cong_win = MSS;
	cong->threshold = 64000;
	cong->support_variable = 0;

	return sock_conn;
}

int close_client_tcp(int sockd){
	tcp temp = (const tcp) { 0 };

	for(int i=0; i < MAX_LINE_DECOR; i++)
		printf("-");
	printf("\nConnection termination\n");
	
	printf("Sending Fin...\n");
	send_flags(sockd, Fin);

	struct timeval recv_timeout;
	recv_timeout.tv_sec = 3;
	recv_timeout.tv_usec = 0;
	if (setsockopt(sockd, SOL_SOCKET, SO_RCVTIMEO, (char *)&recv_timeout, sizeof(recv_timeout)) < 0) {
		perror("setsockopt failed\n");
	}

	recv_tcp_segm(sockd, &temp);

	if((temp.fin & temp.ack) == 0){
		perror("Missing fin-ack, could not close connection...\n");
		return -1;
	}

	printf("Received Fin-Ack, sending Ack...\n");

	if ( send_flags(sockd, Ack) < 0 ){
		perror("Failed to send ack, could not close connection...\n");
		return -1;
	}

	int res = close(sockd);
	printf("Connection closed\n");
	for(int i=0; i < MAX_LINE_DECOR; i++)
		printf("-");
	printf("\n");

	free(cong);
	
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

	struct timeval recv_timeout;
	recv_timeout.tv_sec = 3;
	recv_timeout.tv_usec = 0;
	if (setsockopt(sockd, SOL_SOCKET, SO_RCVTIMEO, (char *)&recv_timeout, sizeof(recv_timeout)) < 0) {
		perror("setsockopt failed\n");
	}

	if( send_flags(sockd, Fin | Ack) < 0){
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

	free(cong);

	pthread_exit(&res);
}