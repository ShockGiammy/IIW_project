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
#include <sys/syscall.h>
#include <netinet/in.h>
#include <netdb.h>
#include <net/if.h>
#include <pthread.h>

static __thread cong_struct cong;
static __thread slid_win recv_win; // the sliding window for the receiver
static __thread slid_win sender_wind; //sliding window for the sender

int new_port = 0;
static int fd = -1;
char msg[LOG_MSG_SIZE] = { 0 };

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
	*send_segm_ptr = htonl(segment.data_length);
	send_segm_ptr++;
	bytes_written += sizeof(unsigned int);

	memcpy(send_segm_ptr, segment.data, segment.data_length);
	bytes_written += segment.data_length;


	snprintf(msg, LOG_MSG_SIZE, "make_segment \nseq num: %d\nack num: %d\nASF: %d%d%d\nData length: %d\nbytes written on send_buf: %d\n", segment.sequence_number, segment.ack_number, segment.ack, segment.syn, segment.fin, segment.data_length, bytes_written);
	print_on_log(fd, msg);
	memset(msg, 0, LOG_MSG_SIZE);
	
	return bytes_written;
}

int extract_segment(tcp *segment, char *recv_segm) {

	memset(segment, 0, sizeof(*segment));
	memset(&(segment->data), 0, MSS);

	//deserialize sequence number
	unsigned int* recv_buf_ptr = (unsigned int*) recv_segm;
	segment->sequence_number = ntohl(*recv_buf_ptr);
	recv_buf_ptr++;
	
	// deserialize ack number
	segment->ack_number = ntohl(*recv_buf_ptr);
	recv_buf_ptr++;
	
	// deserialize flags
	bool* recv_buf_ptr_flag = (bool*) recv_buf_ptr;

	segment->ack = *recv_buf_ptr_flag;
	recv_buf_ptr_flag++;

	segment->syn = *recv_buf_ptr_flag;
	recv_buf_ptr_flag++;

	segment->fin = *recv_buf_ptr_flag;
	recv_buf_ptr_flag++;

	// deserialize rcvwnd
	recv_buf_ptr = (unsigned int*) recv_buf_ptr_flag;
	segment->receiver_window = ntohl(*recv_buf_ptr);
	recv_buf_ptr++;

	// deserialize data field length
	segment->data_length = ntohl(*recv_buf_ptr);
	recv_buf_ptr++;

	memcpy(segment->data, (char*)recv_buf_ptr, segment->data_length);

	snprintf(msg, LOG_MSG_SIZE, "extract_segment \nseq num: %d\nack num: %d\nASF: %d%d%d\nData length: %d\n", segment->sequence_number, segment->ack_number, segment->ack, segment->syn, segment->fin, segment->data_length);
	print_on_log(fd, msg);
	memset(msg, 0, LOG_MSG_SIZE);

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
		snprintf(msg, LOG_MSG_SIZE, "%d == %d ?\n", win.next_to_ack, segments[i].sequence_number);
		print_on_log(fd, msg);
		memset(msg, 0, LOG_MSG_SIZE);
		if(win.next_to_ack == segments[i].sequence_number) {
			make_seg(segments[i], buffer);
			retransmitted = true;
			int n_send = send_unreliable(socket_desc, buffer, segments[i].data_length + HEAD_SIZE);
			snprintf(msg, LOG_MSG_SIZE, "Ritrasmesso segmento con numero di sequenza %d (%d bytes)\n", segments[i].sequence_number, n_send);
			print_on_log(fd, msg);
			memset(msg, 0, LOG_MSG_SIZE);

			memset(buffer, 0, segments[i].data_length + HEAD_SIZE); //we reset the buffer to send the next segment
			break;
		}
	}
	if(!retransmitted){
		fprintf(stderr, "retx failed\n");
		exit(EXIT_FAILURE);
	}
	// printf("\nFinished retx\n");
	snprintf(msg, LOG_MSG_SIZE, "Finished retx\n");
	print_on_log(fd, msg);
	memset(msg, 0, LOG_MSG_SIZE);
}

void buffer_in_order(tcp **segment_head, tcp *to_buf, slid_win *win) {
	tcp *current = *segment_head;

	// check if to_buf was already received
	snprintf(msg, LOG_MSG_SIZE, "buff_in_order: %d < %d ? \n", to_buf->sequence_number, win->tot_acked);
	print_on_log(fd, msg);
	memset(msg, 0, LOG_MSG_SIZE);
	
	if(to_buf->sequence_number < win->tot_acked){
		snprintf(msg, LOG_MSG_SIZE, "buff_in_order: won't buffer segment\n");
		print_on_log(fd, msg);
		memset(msg, 0, LOG_MSG_SIZE);
		return;
	}


	// check if to_buf is already in the list
	while(current != NULL){
		if(current->sequence_number == to_buf->sequence_number) {
			snprintf(msg, LOG_MSG_SIZE, "buff_in_order: segment already in list\n");
			print_on_log(fd, msg);
			memset(msg, 0, LOG_MSG_SIZE);
			return;
		}
		current = current->next;
	};

	// update last_byte_buffered and how much free memory is free in recv buffer
	snprintf(msg, LOG_MSG_SIZE, "buff_in_order: (%d + %d) mod INT_MAX = %d > %d?\n", to_buf->sequence_number, to_buf->data_length, (to_buf->data_length + to_buf->sequence_number)%INT_MAX, win->rcvwnd);
	print_on_log(fd, msg);
	memset(msg, 0, LOG_MSG_SIZE);

	if(((to_buf->sequence_number) + (to_buf->data_length))%INT_MAX > win->last_byte_buffered){
		win->last_byte_buffered = (to_buf->sequence_number + to_buf->data_length)%INT_MAX;
	}

	win->rcvwnd -= to_buf->data_length;
	if(win->rcvwnd < 0) {
		snprintf(msg, LOG_MSG_SIZE, "buff_in_order: rcvwnd is negative so it can't be delivered, this segment won't be buffered\n");
		print_on_log(fd, msg);
		memset(msg, 0, LOG_MSG_SIZE);
		//exit(EXIT_FAILURE);
		win->rcvwnd = 0;
		return;
	}

	snprintf(msg, LOG_MSG_SIZE, "buff_in_order: Updating rcvwnd to %d\n", win->rcvwnd);
	print_on_log(fd, msg);
	memset(msg, 0, LOG_MSG_SIZE);

	// if the list is empty, to_buf becomes the head
	if(*segment_head == NULL){
		snprintf(msg, LOG_MSG_SIZE, "buff_in_order: Inserting in list as head, (%d bytes of data)\n", to_buf->data_length);
		print_on_log(fd, msg);
		memset(msg, 0, LOG_MSG_SIZE);
		
		*segment_head = to_buf;
		to_buf->next = NULL;
		return;
	}
	else {
		// there are segments in the list
		// look for the correct position of to_buf in the list based on seq_num

		// if seq_num of to_buf is > than that of the head, to_buf becomes the new head
		if( ( *segment_head )->sequence_number > to_buf->sequence_number){
			snprintf(msg, LOG_MSG_SIZE, "buff_in_order: Inserting in list as head\n");
			print_on_log(fd, msg);
			memset(msg, 0, LOG_MSG_SIZE);

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

				snprintf(msg, LOG_MSG_SIZE, "buff_in_order: Inserting in list in position %d\n", pos_in_list);
				print_on_log(fd, msg);
				memset(msg, 0, LOG_MSG_SIZE);

				current->next = to_buf;
				to_buf->next = NULL;
				return;
			}
			else if(next->sequence_number > to_buf->sequence_number) {
				// to_buf must be inserted in the list before the next segment
				pos_in_list++;

				snprintf(msg, LOG_MSG_SIZE, "buff_in_order: Inserting in list in position %d\n", pos_in_list);
				print_on_log(fd, msg);
				memset(msg, 0, LOG_MSG_SIZE);

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
int write_all(char** buf, int list_size, tcp **segm_buff, slid_win *win, int* bytes_recvd) {
	int n_wrote = 0;
	tcp *current = *segm_buff;
	snprintf(msg, LOG_MSG_SIZE, "write_all:\n");
	print_on_log(fd, msg);
	memset(msg, 0, LOG_MSG_SIZE);

	while(current != NULL) {
		snprintf(msg, LOG_MSG_SIZE, "%d == %d ?\n", current->sequence_number, win->next_to_ack);
		print_on_log(fd, msg);
		memset(msg, 0, LOG_MSG_SIZE);

		if((current)->sequence_number == win->next_to_ack) {
			snprintf(msg, LOG_MSG_SIZE, "write_all: Manipolo %d, lunghezza dati %d, copio in %p\n", current->sequence_number, current->data_length, *buf);
			print_on_log(fd, msg);
			memset(msg, 0, LOG_MSG_SIZE);

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
			*bytes_recvd += n;
			win->tot_acked %= INT_MAX;

			win->next_seq_num += n;
			win->next_seq_num %= INT_MAX;

			win->next_to_ack += n;
			win->next_to_ack %= INT_MAX;

			n_wrote++;

			snprintf(msg, LOG_MSG_SIZE, "write_all: Bytes received this session: %d\nwrite_all: %d bytes acked / next_to_ack: %d\n", *bytes_recvd, win->tot_acked, win->next_to_ack);
			print_on_log(fd, msg);
			memset(msg, 0, LOG_MSG_SIZE);
		}
		else {
			// the segment is out of order, we can't write it now
			break;
		}
		//printf("current = %p\n", current->next);
		current = current->next;
	}

	snprintf(msg, LOG_MSG_SIZE, "write_all: list_length %d, freeing %d\n", list_size, n_wrote);
	print_on_log(fd, msg);
	memset(msg, 0, LOG_MSG_SIZE);

	free_segms_in_buff(segm_buff, n_wrote);
	return n_wrote;
}

// function used to prepare a segemnt that will be send and to set the window parameters properly
void prepare_segment(tcp * segment, slid_win *wind, char *data, int ack_num,  int index, int n_byte, int flags) {
	bool ack, syn, fin;

	ack = CHECK_BIT(flags, 0);
	syn = CHECK_BIT(flags, 1);
	fin = CHECK_BIT(flags, 2);
	
	memset(&(segment[index]), 0, sizeof(*segment));
	memcpy(segment[index].data, data, n_byte);
	segment[index].data_length = n_byte;
	fill_struct(&segment[index], wind->next_seq_num, ack_num, wind->rcvwnd, ack, fin, syn);

	snprintf(msg, LOG_MSG_SIZE, "prepare_segment\nSEQ_NUM: %d\nASF: %d%d%d\n", wind->next_seq_num, segment[index].ack, segment[index].syn, segment[index].fin);
	print_on_log(fd, msg);
	memset(msg, 0, LOG_MSG_SIZE);
	
	wind->next_seq_num += n_byte; // sequence number for the next segment
	wind->next_seq_num %= INT_MAX;

	wind->on_the_fly += n_byte;
	wind->on_the_fly %= INT_MAX;
	
	int prev_last_to_ack = wind->last_to_ack;
	wind->last_to_ack += n_byte;
	wind->last_to_ack %= INT_MAX;
	snprintf(msg, LOG_MSG_SIZE, "prepare_segment: last_to_ack %d -> %d\nCopying %d to position %d\n", prev_last_to_ack, wind->last_to_ack, segment[index].sequence_number, index);
	print_on_log(fd, msg);
	memset(msg, 0, LOG_MSG_SIZE);
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

	snprintf(msg, LOG_MSG_SIZE, "send_flags\nASF: %d%d%d\n", segment.ack, segment.syn, segment.fin);
	print_on_log(fd, msg);
	memset(msg, 0, LOG_MSG_SIZE);

	make_seg(segment, buf);
	int ret = write(sockd, buf, HEAD_SIZE);
	return ret;
}

// function that writes all the segments in order received and reset the buffered segments list length
void ack_segments(char** buf, int recv_sock,  int *list_length, tcp **buf_segm, tcp *ack,  slid_win *recv_win, int* bytes_recvd) {
	char buff[BUFSIZ];
	memset(buff, 0 , BUFSIZ);
	memset(ack->data, 0, MSS);
	*list_length -= write_all(buf, *list_length,  buf_segm, recv_win, bytes_recvd);
	fill_struct(ack, 0, recv_win->tot_acked, recv_win->rcvwnd, true, false, false);
	ack->data_length = 0;

	snprintf(msg, LOG_MSG_SIZE, "ack_segments\nack: %d,%d %d%d%d %d\n", ack->sequence_number, ack->ack_number, ack->ack, ack->syn, ack->fin, ack->data_length);
	print_on_log(fd, msg);
	memset(msg, 0, LOG_MSG_SIZE);

	make_seg(*ack, buff);
	int n = send_unreliable(recv_sock, buff, HEAD_SIZE);

	snprintf(msg, LOG_MSG_SIZE, "ack_segments: Sent %d bytes for ack...\n", n);
	print_on_log(fd, msg);
	memset(msg, 0, LOG_MSG_SIZE);
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
		snprintf(msg, LOG_MSG_SIZE, "send_unreliable: Send success...\n", segm_to_go);
		print_on_log(fd, msg);
		memset(msg, 0, LOG_MSG_SIZE);
		
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

	if(cong.cong_win == 0){
		cong.state = 0;
		cong.cong_win = MSS;
		cong.threshold = 64000;
		cong.support_variable = 0;
	}

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
	snprintf(msg, LOG_MSG_SIZE, "send_tcp\ncongWin: %d\n", cong.cong_win);
	print_on_log(fd, msg);
	memset(msg, 0, LOG_MSG_SIZE);

	sender_wind.max_size = cong.cong_win < MAX_WIN-MSS ? cong.cong_win : MAX_WIN-MSS;
	sender_wind.max_size = sender_wind.max_size < size ? sender_wind.max_size : size;

	snprintf(msg, LOG_MSG_SIZE, "send_tcp\nmax: %d\nlast_to_ack: %d\n", sender_wind.max_size, sender_wind.last_to_ack);
	print_on_log(fd, msg);
	memset(msg, 0, LOG_MSG_SIZE);

	time_out send_timeo;
	memset(&send_timeo, 0, sizeof(send_timeo));
	send_timeo.time.tv_sec = 5; // we set the first timeout to 3sec, as it's in TCP standard

	/*struct timeval time_out;
	time_out.tv_sec = 3; // we set 6 sec of timeout, we will estimate it in another moment
	time_out.tv_usec = 0;*/
	struct timeval start_rtt;
	struct timeval finish_rtt;

	if(setsockopt(sockd, SOL_SOCKET, SO_RCVTIMEO, (char *)&send_timeo.time, sizeof(send_timeo.time)) == -1) {
		fprintf(stderr, "send_tcp: Sender error setting opt\n%s\n", strerror(errno));
		return -1;
	}

	int index = 0; // sender segments list index

	while(bytes_left_to_send > 0 || sender_wind.bytes_acked_current_transmission < size) {

		snprintf(msg, LOG_MSG_SIZE, "send_tcp: %d bytes acked out of %ld\n%d < 0 ?\n", sender_wind.bytes_acked_current_transmission, size, sender_wind.on_the_fly);
		print_on_log(fd, msg);
		memset(msg, 0, LOG_MSG_SIZE);

		if(sender_wind.on_the_fly < 0){
			fprintf(stderr, "send_tcp: on_the_fly variabile is negative: %d\n", sender_wind.on_the_fly);
			return -1;
		}

		snprintf(msg, LOG_MSG_SIZE, "send_tcp: Bytes left to send: %d / %ld\n", bytes_left_to_send, size);
		print_on_log(fd, msg);
		memset(msg, 0, LOG_MSG_SIZE);

		snprintf(msg, LOG_MSG_SIZE, "send_tcp: Need to send segments? -> %d > 0 && %d < %d\n", bytes_left_to_send, sender_wind.on_the_fly, sender_wind.max_size);
		print_on_log(fd, msg);
		memset(msg, 0, LOG_MSG_SIZE);

		if((bytes_left_to_send > 0 && sender_wind.on_the_fly < sender_wind.max_size)) {
			int n_to_copy = (bytes_left_to_send > MSS)*MSS + (bytes_left_to_send <= MSS)*bytes_left_to_send;
			memcpy(data_buf, buf, n_to_copy);

			snprintf(msg, LOG_MSG_SIZE, "send_tcp: Taking %d bytes from input buf\n", n_to_copy);
			print_on_log(fd, msg);
			memset(msg, 0, LOG_MSG_SIZE);

			bytes_left_to_send -= n_to_copy;
			data_buf[n_to_copy] = 0;
			n_read += n_to_copy;
			buf += n_to_copy;

			if(n_to_copy >= 0) {
				snprintf(msg, LOG_MSG_SIZE, "send_tcp\nindex: %d, max: %d\n", index, MAX_BUF_SIZE);
				print_on_log(fd, msg);
				memset(msg, 0, LOG_MSG_SIZE);

				// we check if we can send data without exceeding the max number of bytes on the fly
				prepare_segment(send_segm, &sender_wind, data_buf, recv_win.tot_acked, index, n_to_copy, Ack);
				int n_buf = make_seg(send_segm[index], send_buf); // we put our segment in a buffer that will be sent over the socket

				snprintf(msg, LOG_MSG_SIZE, "send_tcp\nNew segment (seq_num, data_length): (%d, %d)\n", send_segm[index].sequence_number, send_segm[index].data_length);
				print_on_log(fd, msg);
				memset(msg, 0, LOG_MSG_SIZE);

				int n_send = send_unreliable(sockd, send_buf, n_buf);

				memset(send_buf, 0, HEAD_SIZE + MSS); //we reset the buffer to send the next segment
				memset(data_buf, 0, MSS); // we reset the buffer so that we can reuse it
				index++;
				index %= MAX_BUF_SIZE;
			}
		}
		gettimeofday(&start_rtt, NULL);

		// we have read the max number of data, we proceed with the sending in pipelining
		snprintf(msg, LOG_MSG_SIZE, "send_tcp: %d >= %d || %d >= %ld\n", sender_wind.on_the_fly, sender_wind.max_size, n_read, size);
		print_on_log(fd, msg);
		memset(msg, 0, LOG_MSG_SIZE);

		if(sender_wind.on_the_fly >= sender_wind.max_size || n_read >= size) {
			snprintf(msg, LOG_MSG_SIZE, "send_tcp: Waiting for ack...\n");
			print_on_log(fd, msg);
			memset(msg, 0, LOG_MSG_SIZE);

			memset(recv_ack_buf, 0, HEAD_SIZE);
			if(recv(sockd, recv_ack_buf, HEAD_SIZE, 0) > 0) { //we expect a buffer with only header and no data

				gettimeofday(&finish_rtt, NULL);
				estimate_timeout(&send_timeo, start_rtt, finish_rtt);
				if(setsockopt(sockd, SOL_SOCKET, SO_RCVTIMEO, (char *)&send_timeo.time, sizeof(send_timeo.time)) == -1) {
					printf("Sender error setting opt(1)\n%s\n", strerror(errno));
				}
				// printf("The next time out will be of %ld sec and %ld usec \n\n", send_timeo.time.tv_sec, send_timeo.time.tv_usec);
				memset(&recv_segm, 0, sizeof(recv_segm));
				extract_segment(&recv_segm, recv_ack_buf);
				memset(recv_ack_buf, 0, HEAD_SIZE);

				snprintf(msg, LOG_MSG_SIZE, "send_tcp\nReceived ack: %d\n%d == %d ?\nelse %d < %d && %d <= %d ?\n", recv_segm.ack_number, recv_segm.ack_number,
					 sender_wind.next_to_ack, sender_wind.next_to_ack, recv_segm.ack_number, recv_segm.ack_number, sender_wind.last_to_ack);
				print_on_log(fd, msg);
				memset(msg, 0, LOG_MSG_SIZE);

				if(recv_segm.ack_number == sender_wind.next_to_ack) {
					snprintf(msg, LOG_MSG_SIZE, "send_tcp: received duplicate ack\n");
					print_on_log(fd, msg);
					memset(msg, 0, LOG_MSG_SIZE);

					sender_wind.dupl_ack++; // we increment the number of duplicate acks received
					congestion_control_caseFastRetrasmission_duplicateAck(sender_wind);
					check_size_buffer(sender_wind, recv_segm.receiver_window);

					//fast retransmission
					if(sender_wind.dupl_ack == 3) {
						times_retx++;

						snprintf(msg, LOG_MSG_SIZE, "send_tcp: Fast retx\n");
						print_on_log(fd, msg);
						memset(msg, 0, LOG_MSG_SIZE);

						congestion_control_duplicateAck(sender_wind);
						check_size_buffer(sender_wind, recv_segm.receiver_window);
						retx(send_segm, sender_wind, send_buf, sockd);
					}
				}
				else if((sender_wind.next_to_ack < recv_segm.ack_number) && (recv_segm.ack_number <= sender_wind.last_to_ack)) {
					// printf("Ack OK. Sliding the window...\n");
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
				}
			}

			// we have to retx the last segment not acked due to TO
			else {
				times_retx++;
				
				snprintf(msg, LOG_MSG_SIZE, "send_tcp: TO expired\nRetransmitting segment...\n");
				print_on_log(fd, msg);
				memset(msg, 0, LOG_MSG_SIZE);

				congestion_control_timeout(sender_wind);
				check_size_buffer(sender_wind, recv_segm.receiver_window);
				retx(send_segm, sender_wind, send_buf, sockd);
			}
			memset(recv_segm.data, 0, recv_segm.data_length);
			memset(send_buf, 0, MSS+HEAD_SIZE);

			if(times_retx >= MAX_ATTMPTS_RETX){
				fprintf(stderr, "send_tcp: Retransmitted %d times but did not receive any reply...\n", times_retx);
				break;
			}
		}
	}
	
	snprintf(msg, LOG_MSG_SIZE, "send_tcp: %d bytes acked out of %ld bytes to send\n", sender_wind.bytes_acked_current_transmission, size);
	print_on_log(fd, msg);
	memset(msg, 0, LOG_MSG_SIZE);
	
	// resets the time-out
	send_timeo.time.tv_sec = 0; // we set 6 sec of timeout, we will estimate it in another moment
	send_timeo.time.tv_usec = 0;
	if(setsockopt(sockd, SOL_SOCKET, SO_RCVTIMEO, (char *)&send_timeo.time, sizeof(send_timeo.time)) == -1) {
		fprintf(stderr, "Sender error setting opt(2)\n");
	}
	memset(send_buf, 0, MSS+HEAD_SIZE);

	snprintf(msg, LOG_MSG_SIZE, "send_tcp: Finished transmission (%d bytes)...\n", sender_wind.bytes_acked_current_transmission);
	print_on_log(fd, msg);
	memset(msg, 0, LOG_MSG_SIZE);

	return sender_wind.bytes_acked_current_transmission;
}

int calculate_window_dimension() {
	if (cong.cong_win < MAX_WIN) {
		return cong.cong_win;
	}
	else {
		return MAX_WIN;
	}
}

int congestion_control_receiveAck(slid_win sender_wind) {

	switch(cong.state)
	{
		case 0:
			cong.cong_win = cong.cong_win + MSS;
			if (cong.cong_win > cong.threshold) {
				cong.state = 1;
				snprintf(msg, LOG_MSG_SIZE, "congestion_control_receiveAck: Entered in Congestion Avoidance\n");
				print_on_log(fd, msg);
				memset(msg, 0, LOG_MSG_SIZE);
			}
			break;

		case 1:
			cong.support_variable = cong.support_variable + (int)floor(MSS*MSS/cong.cong_win);

			snprintf(msg, LOG_MSG_SIZE, "congestion_control_receiveAck: support variable: %d\n", cong.support_variable);
			print_on_log(fd, msg);
			memset(msg, 0, LOG_MSG_SIZE);

			if (cong.support_variable >= CONG_SCALING_MSS_THRESHOLD) {
				cong.cong_win = cong.cong_win + MSS;
				cong.support_variable = 0;
			}
			break;

		case 2:
			cong.cong_win = cong.threshold;
			cong.state = 1;

			snprintf(msg, LOG_MSG_SIZE, "congestion_control_receiveAck: Entered in Congestion Avoidance\n");
			print_on_log(fd, msg);
			memset(msg, 0, LOG_MSG_SIZE);

			break;
	}
	return 0;
}

int congestion_control_duplicateAck(slid_win sender_wind) {
	switch(cong.state)
	{
		case 0:
			cong.state = 2;
			
			snprintf(msg, LOG_MSG_SIZE, "congestion_control_duplicateAck: Entered in Fast Recovery\n");
			print_on_log(fd, msg);
			memset(msg, 0, LOG_MSG_SIZE);

			cong.threshold = cong.cong_win/2;
			cong.cong_win = cong.threshold + 3*MSS;
			break;

		case 1:
			cong.state = 2;

			snprintf(msg, LOG_MSG_SIZE, "congestion_control_duplicateAck: Entered in Fast Recovery\n");
			print_on_log(fd, msg);
			memset(msg, 0, LOG_MSG_SIZE);

			cong.threshold = cong.cong_win/2;
			cong.cong_win = cong.threshold + 3*MSS;
			break;

		case 2:
			// caso già gestito
			break;
	}
	return 0;
}

int congestion_control_caseFastRetrasmission_duplicateAck(slid_win sender_wind) {
	if (cong.state == 2) {
		cong.cong_win = cong.cong_win + MSS;
	}
}

int congestion_control_timeout(slid_win sender_wind) {
	switch(cong.state)
	{
		case 0:
			cong.threshold = cong.cong_win/2;
			cong.cong_win = MSS;
			break;

		case 1:
			cong.state = 0;

			snprintf(msg, LOG_MSG_SIZE, "congestion_control_timeout: Entered in Slow Start\n");
			print_on_log(fd, msg);
			memset(msg, 0, LOG_MSG_SIZE);

			cong.threshold = cong.cong_win/2;
			cong.cong_win = MSS;
			break;

		case 2:
			cong.state = 0;

			snprintf(msg, LOG_MSG_SIZE, "congestion_control_timeout: Entered in Slow Start\n");
			print_on_log(fd, msg);
			memset(msg, 0, LOG_MSG_SIZE);

			cong.threshold = cong.cong_win/2;
			cong.cong_win = MSS;
			break;
	}
	return 0;
}

int check_size_buffer(slid_win sender_wind, int receiver_window) {
	if (cong.cong_win < receiver_window) {
		sender_wind.max_size = cong.cong_win;
	}
	else {
		sender_wind.max_size = receiver_window;
	}

	snprintf(msg, LOG_MSG_SIZE, "check_size_buffer\n congWin %d\nthreshold %d\nreceiver_window %d\nmax_size %d\n",
		cong.cong_win, cong.threshold, receiver_window, sender_wind.max_size);
	print_on_log(fd, msg);
	memset(msg, 0, LOG_MSG_SIZE);

	//printf("congwin = %d\n", cong.cong_win);

	return 0;
}

int recv_tcp(int sockd, void* buf, size_t size){

	snprintf(msg, LOG_MSG_SIZE, "recv_tcp: New session...\n");
	print_on_log(fd, msg);
	memset(msg, 0, LOG_MSG_SIZE);

	char *buf_ptr = buf;

	int n=0;
	char recv_buf[MSS+HEAD_SIZE];
	memset(recv_buf, 0, MSS+HEAD_SIZE);

	// the head for our segment linked list
	tcp *buf_segm = NULL;

	if(recv_win.last_to_ack == 0){
		recv_win.last_to_ack = MAX_WIN-MSS;
	}

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
	int n_delayed_ack = 0;

	while (!finished && ((n = recv(sockd, recv_buf, MSS+HEAD_SIZE, 0)) > 0)) {
		snprintf(msg, LOG_MSG_SIZE, "recv_tcp\n Received %d bytes: %s\nrcvwnd: %d\n", n, recv_buf, recv_win.rcvwnd);
		print_on_log(fd, msg);
		memset(msg, 0, LOG_MSG_SIZE);

		recv_timeout.tv_sec = 0;
		recv_timeout.tv_usec = 0;
		if(setsockopt(sockd, SOL_SOCKET, SO_RCVTIMEO, (char *)&recv_timeout, sizeof(recv_timeout)) == -1){
			fprintf(stderr, "recv_tcp: Error while setting options");
			return -1;
		}
		
		tcp *segment = malloc(sizeof(tcp));
		memset(segment, 0, sizeof(tcp));
		extract_segment(segment, recv_buf);

		received_data = segment->data_length != 0;
		
		if(segment->fin && !segment->ack){
			close_server_tcp(sockd);
		}
		
		snprintf(msg, LOG_MSG_SIZE, "recv_tcp: New segment of %d bytes, seq num %d\n", segment->data_length, segment->sequence_number);
		print_on_log(fd, msg);
		memset(msg, 0, LOG_MSG_SIZE);

		memset(recv_buf, 0, MSS+HEAD_SIZE);
		
		//printf("%d <= %d && %d <= %d\n", recv_win.next_to_ack, segment->sequence_number, segment->sequence_number, recv_win.last_to_ack);

		// buffer segments
		if(list_length < MAX_BUF_SIZE && (recv_win.next_to_ack <= segment->sequence_number) 
			&& ( segment->sequence_number <= recv_win.last_to_ack) && (segment->data_length != 0)) {
			
			list_length++;

			snprintf(msg, LOG_MSG_SIZE, "recv_tcp: Buffering segment...\n");
			print_on_log(fd, msg);
			memset(msg, 0, LOG_MSG_SIZE);

			buffer_in_order(&buf_segm, segment, &recv_win);
		}

		if(received_data){
			recv_timeout.tv_sec = RECV_TIMEOUT_SHORT_SEC;
			recv_timeout.tv_usec = RECV_TIMEOUT_SHORT_USEC;
			if(setsockopt(sockd, SOL_SOCKET, SO_RCVTIMEO, (char *)&recv_timeout, sizeof(recv_timeout)) == -1) {
				fprintf(stderr, "recv_tcp: Error while setting options, RECV_TIMEOUT_SHORT_USEC\n");
				return -1;
			}
		}
		else{
			recv_timeout.tv_sec = RECV_TIMEOUT_SEC;
			recv_timeout.tv_usec = 0;
			if(setsockopt(sockd, SOL_SOCKET, SO_RCVTIMEO, (char *)&recv_timeout, sizeof(recv_timeout)) == -1) {
				fprintf(stderr, "recv_tcp: Error while setting options, RECV_TIMEOUT_SEC\n");
				return -1;
			}
		}

		// delayed ack -> check if we get a new segment 
		if(segment->data_length <= recv_win.rcvwnd) {
			if((n_delayed_ack = recv(sockd, recv_buf, MSS+HEAD_SIZE, 0) > 0)){
				tcp *second_segm = malloc(sizeof(tcp));
				// we got the new segment
				extract_segment(second_segm, recv_buf);

				received_data = second_segm->data_length != 0;

				snprintf(msg, LOG_MSG_SIZE, "recv_tcp\nNew segment (seq_num, data_length): (%d, %d)\nreceived_data: %d\n", second_segm->sequence_number, second_segm->data_length, received_data);
				print_on_log(fd, msg);
				memset(msg, 0, LOG_MSG_SIZE);

				if(second_segm->fin && !second_segm->ack){
					close_server_tcp(sockd);
				}

				memset(recv_buf, 0, MSS+HEAD_SIZE);

				if(list_length < MAX_BUF_SIZE && (recv_win.next_to_ack <= second_segm->sequence_number) 
					&& ( second_segm->sequence_number <= recv_win.last_to_ack) && (second_segm->data_length != 0)) {
					
					list_length++;

					snprintf(msg, LOG_MSG_SIZE, "recv_tcp: Buffering segment...\n");
					print_on_log(fd, msg);
					memset(msg, 0, LOG_MSG_SIZE);

					buffer_in_order(&buf_segm, second_segm, &recv_win);
				}
			}
			else if(n_delayed_ack == 0){
				snprintf(msg, LOG_MSG_SIZE, "recv_tcp: TO delayed ack expired\n");
				print_on_log(fd, msg);
				memset(msg, 0, LOG_MSG_SIZE);

				finished = true;
				continue;
			}
			else {
				snprintf(msg, LOG_MSG_SIZE, "recv_tcp: no bytes read\n%s\n", strerror(errno));
				print_on_log(fd, msg);
				memset(msg, 0, LOG_MSG_SIZE);
			};
		}
		else {
			// segment->data_length > recv_win.rcvwnd)
			// can't receive anymore
			finished = true;
		}

		if(recv_win.rcvwnd <= 0){
			snprintf(msg, LOG_MSG_SIZE, "recv_tcp: Receiver cannot receive any more data...\nrcvwnd: %d\n", recv_win.rcvwnd);
			print_on_log(fd, msg);
			memset(msg, 0, LOG_MSG_SIZE);

			finished = true;
		}
		
		ack_segments(&buf_ptr, sockd, &list_length, &buf_segm, &ack, &recv_win, &bytes_rcvd);

		snprintf(msg, LOG_MSG_SIZE, "recv_tcp\n Tot bytes acked: %d\n", recv_win.tot_acked);
		print_on_log(fd, msg);
		memset(msg, 0, LOG_MSG_SIZE);

		if(bytes_rcvd == size) {
			finished = true;
		}
		
		memset(recv_buf, 0, MSS+HEAD_SIZE);
		//printf("Resetto il timeout\n");
		recv_timeout.tv_sec = 0;
		recv_timeout.tv_usec = 0;
		setsockopt(sockd, SOL_SOCKET, SO_RCVTIMEO, (char *)&recv_timeout, sizeof(recv_timeout));

		if(received_data){
			recv_timeout.tv_sec = RECV_TIMEOUT_SHORT_SEC;
			recv_timeout.tv_usec = RECV_TIMEOUT_SHORT_USEC;
			if(setsockopt(sockd, SOL_SOCKET, SO_RCVTIMEO, (char *)&recv_timeout, sizeof(recv_timeout)) == -1) {
				fprintf(stderr, "recv_tcp: Error while setting options");
				return -1;
			}
		}
		else{
			recv_timeout.tv_sec = RECV_TIMEOUT_SEC;
			recv_timeout.tv_usec = 0;
			if(setsockopt(sockd, SOL_SOCKET, SO_RCVTIMEO, (char *)&recv_timeout, sizeof(recv_timeout)) == -1) {
				fprintf(stderr, "recv_tcp: Error while setting options");
				return -1;
			}
		}
		memset(&ack, 0, sizeof(ack));
		memset(recv_buf, 0, MSS+HEAD_SIZE);

	}
	if(n < 0){
		snprintf(msg, LOG_MSG_SIZE, "recv_tcp: recv_error\n%s\n", strerror(errno));
		print_on_log(fd, msg);
		memset(msg, 0, LOG_MSG_SIZE);
	}
	else{
		snprintf(msg, LOG_MSG_SIZE, "recv_tcp: Main timeout expired...\n");
		print_on_log(fd, msg);
		memset(msg, 0, LOG_MSG_SIZE);
	}

	ack_segments(&buf_ptr, sockd, &list_length, &buf_segm, &ack, &recv_win, &bytes_rcvd);
	//free_segms_in_buff(&buf_segm, list_length);

	//resets the time-out
	recv_timeout.tv_sec = 0;
	recv_timeout.tv_usec = 0;
	setsockopt(sockd, SOL_SOCKET, SO_RCVTIMEO, (char *)&recv_timeout, sizeof(recv_timeout));
	memset(recv_buf, 0, MSS+HEAD_SIZE);

	snprintf(msg, LOG_MSG_SIZE, "recv_tcp: Finished transmission...\n");
	print_on_log(fd, msg);
	memset(msg, 0, LOG_MSG_SIZE);

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

	while(timeo->time.tv_usec > 1000000) {
		timeo->time.tv_sec += 1;
		timeo->time.tv_usec -= 1000000;
	}

	snprintf(msg, LOG_MSG_SIZE, "estimate_timeout: TO: %ld s, %ld us\n", timeo->time.tv_sec, timeo->time.tv_usec);
	print_on_log(fd, msg);
	memset(msg, 0, LOG_MSG_SIZE);
}

int connect_tcp(int socket_descriptor, struct sockaddr_in* addr, socklen_t addr_len){
		
	tcp head_rcv = (const tcp) {0};
	char recv_buf[HEAD_SIZE];
	char snd_buf[HEAD_SIZE];
	memset(recv_buf, 0, HEAD_SIZE);
	memset(snd_buf, 0, HEAD_SIZE);
	memset(&head_rcv, 0, sizeof(tcp));
	
	memset(&recv_win, 0, sizeof(recv_win));
	memset(&sender_wind, 0, sizeof(sender_wind));

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

	for (int i = 0; i < MAX_ATTMPTS_PORT_SEARCH; i++) {
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

	printf("sd: %d\n", socket_descriptor);

	socklen_t len = INET_ADDRSTRLEN;
	if( sendto(socket_descriptor, snd_buf, HEAD_SIZE, 0, (struct sockaddr*) addr, len) < 0 ){
		fprintf(stderr, "socket_descriptor: %d\nError while sending syn...\n%s\n", socket_descriptor, strerror(errno));
		return -1;
	}

	printf("Waiting server response...\n");
	if( recvfrom(socket_descriptor, recv_buf, HEAD_SIZE, 0, (struct sockaddr *) &server_addr, &len) < 0 ){
		fprintf(stderr, "socket_descriptor: %d\nrecvfrom: error\n%s\n", socket_descriptor, strerror(errno));
		return -1;
	}

	if( connect(socket_descriptor, (struct sockaddr *) &server_addr, addr_len) < 0){
		fprintf(stderr, "socket_descriptor: %d\nconnect: udp connect error\n%s\n", socket_descriptor, strerror(errno));
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

	return 0;
}

int accept_tcp(int sockd, struct sockaddr* addr, socklen_t* addr_len){

	char recv_buf[HEAD_SIZE];
	memset(recv_buf, 0, HEAD_SIZE);

	tcp head_rcv = { 0 };
	struct sockaddr_in client_address;
	bzero(&client_address, sizeof(client_address));
	struct in_addr addr_from_client;

	for(int i=0; i < MAX_LINE_DECOR; i++)
		printf("-");
	printf("\n");

	//printf("Waiting for syn...\n");
	socklen_t len = sizeof(client_address);
	if( recvfrom(sockd, recv_buf, HEAD_SIZE, 0, (struct sockaddr *) &client_address, &len) < 0 ){
		fprintf(stderr, "recvfrom: error\n%s\n", strerror(errno));
		return -1;
	}

	// get client information
	char address_string[INET_ADDRSTRLEN];
	inet_ntop(client_address.sin_family, &client_address.sin_addr, address_string, INET_ADDRSTRLEN);

	snprintf(msg, LOG_MSG_SIZE, "Received new packet from %s(%d)\n", address_string, ntohs(client_address.sin_port));
	print_on_log(fd, msg);
	memset(msg, 0, LOG_MSG_SIZE);

	extract_segment(&head_rcv, recv_buf);

	int sock_conn = socket(AF_INET, SOCKET_TYPE, IPPROTO_UDP);
	
	int port = getpid() + 1024;

	struct sockaddr_in new_sock_addr;
	if (new_port == 0) {
		new_port = port + PROCESSES;
	}
	else {
		new_port = new_port + PROCESSES;
	}

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
	close(fd);
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
	close(fd);
	
	printf("Connection closed\n");
	for(int i=0; i < MAX_LINE_DECOR; i++)
		printf("-");
	printf("\n");

	pthread_exit(&res);
}

void init_log(char* part_filename){
	// Create log file
	char log_filename[256] = {0};
	int tid = syscall(__NR_gettid);
	snprintf(log_filename, 256, "%d", tid);

	strcat(log_filename, part_filename);
	
	time_t ltime = time(NULL);
	strcat(log_filename, asctime(localtime(&ltime)));

	strcat(log_filename, "txt");

	fd = create_log_file(log_filename);
}