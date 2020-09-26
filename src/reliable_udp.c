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

static float loss_prob; // loss probability
static long win_size; // the size of the sending window

// functions to set / obtain the transmission parameters 
void set_params(float loss, long w_size) {
	loss_prob = loss;
	win_size = w_size;
}

void get_params(float *loss, int *size) {
	*loss = loss_prob;
	*size = win_size;
}

int get_win_size() {
	return win_size;
}


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
	char *send_segm_char = (char *)send_segm_ptr;
	send_segm_char += segment.data_length;
	bytes_written += segment.data_length;

	#ifdef ACTIVE_LOG
		snprintf(msg, LOG_MSG_SIZE, "make_segment \nseq num: %d\nack num: %d\nASF: %d%d%d\nData length: %d\nbytes written on send_buf: %d\n", 
			segment.sequence_number, segment.ack_number, segment.ack, segment.syn, segment.fin, segment.data_length, bytes_written);
		print_on_log(fd, msg);
		memset(msg, 0, LOG_MSG_SIZE);
	#endif
	
	return bytes_written;
}

int extract_segment(tcp *segment, char *recv_segm) {

	int bytes_recv = 0; // usefull to calculate the chsum
	memset(segment, 0, sizeof(*segment));
	memset(&(segment->data), 0, MSS);

	//deserialize sequence number
	unsigned int* recv_buf_ptr = (unsigned int*) recv_segm;
	segment->sequence_number = ntohl(*recv_buf_ptr);
	recv_buf_ptr++;
	bytes_recv += sizeof(unsigned int);
	
	// deserialize ack number
	segment->ack_number = ntohl(*recv_buf_ptr);
	recv_buf_ptr++;
	bytes_recv += sizeof(unsigned int);

	// deserialize flags
	bool* recv_buf_ptr_flag = (bool*) recv_buf_ptr;

	segment->ack = *recv_buf_ptr_flag;
	recv_buf_ptr_flag++;
	bytes_recv += sizeof(bool);

	segment->syn = *recv_buf_ptr_flag;
	recv_buf_ptr_flag++;
	bytes_recv += sizeof(bool);

	segment->fin = *recv_buf_ptr_flag;
	recv_buf_ptr_flag++;
	bytes_recv += sizeof(bool);

	// deserialize rcvwnd
	recv_buf_ptr = (unsigned int*) recv_buf_ptr_flag;
	segment->receiver_window = ntohl(*recv_buf_ptr);
	recv_buf_ptr++;
	bytes_recv += sizeof(unsigned int);

	// deserialize data field length
	segment->data_length = ntohl(*recv_buf_ptr);
	recv_buf_ptr++;
	bytes_recv += sizeof(unsigned int);

	memcpy(segment->data, (char*)recv_buf_ptr, segment->data_length);
	bytes_recv += segment->data_length;

	char *recv_buf_char = (char *)recv_buf_ptr;
	recv_buf_char += segment->data_length;

	#ifdef ACTIVE_LOG
		snprintf(msg, LOG_MSG_SIZE, "extract_segment \nseq num: %d\nack num: %d\nASF: %d%d%d\nData length: %d\n", segment->sequence_number, segment->ack_number, segment->ack, segment->syn, segment->fin, segment->data_length);
		print_on_log(fd, msg);
		memset(msg, 0, LOG_MSG_SIZE);
	#endif

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

// function called when it's necessary to retx a segment with TCP fast retx or due to a timeout
void retx(tcp *segments, slid_win win, char *buffer, int socket_desc) {
	bool retransmitted = false;

	// finds the correct segment that have to be retransmitted
	for(int i = 0; i < MAX_BUF_SIZE; i++) {
		#ifdef ACTIVE_LOG
			snprintf(msg, LOG_MSG_SIZE, "%d == %d ?\n", win.next_to_ack, segments[i].sequence_number);
			print_on_log(fd, msg);
			memset(msg, 0, LOG_MSG_SIZE);
		#endif

		if(win.next_to_ack == segments[i].sequence_number) {
			make_seg(segments[i], buffer);
			retransmitted = true;
			int n_send = send_unreliable(socket_desc, buffer, segments[i].data_length + HEAD_SIZE);
			
			#ifdef ACTIVE_LOG
				snprintf(msg, LOG_MSG_SIZE, "Ritrasmesso segmento con numero di sequenza %d (%d bytes)\n", segments[i].sequence_number, n_send);
				print_on_log(fd, msg);
				memset(msg, 0, LOG_MSG_SIZE);
			#endif

			memset(buffer, 0, segments[i].data_length + HEAD_SIZE); //we reset the buffer to send the next segment
			break;
		}
	}

	// means that the segment is not in the list anymore
	if(!retransmitted){
		fprintf(stderr, "retx failed\n");
		printf("Connection closed\n");
		for(int i=0; i < MAX_LINE_DECOR; i++)
			printf("-");
		printf("\n");
		send_tcp(socket_desc, "ERRCONG", strlen("ERRCONG"));
		close(socket_desc);
		fflush(stdout);
		exit(EXIT_FAILURE);
	}
	
	#ifdef ACTIVE_LOG
		snprintf(msg, LOG_MSG_SIZE, "Finished retx\n");
		print_on_log(fd, msg);
		memset(msg, 0, LOG_MSG_SIZE);
	#endif
}

void buffer_in_order(tcp **segment_head, tcp *to_buf, slid_win *win, int* list_length) {
	tcp *current = *segment_head;

	// check if to_buf was already received
	#ifdef ACTIVE_LOG
		snprintf(msg, LOG_MSG_SIZE, "buff_in_order: %d < %d ? \n", to_buf->sequence_number, win->tot_acked);
		print_on_log(fd, msg);
		memset(msg, 0, LOG_MSG_SIZE);
	#endif
	
	if(to_buf->sequence_number < win->tot_acked){
		#ifdef ACTIVE_LOG
			snprintf(msg, LOG_MSG_SIZE, "buff_in_order: won't buffer segment\n");
			print_on_log(fd, msg);
			memset(msg, 0, LOG_MSG_SIZE);
		#endif
		return;
	}


	// check if to_buf is already in the list
	while(current != NULL){
		if(current->sequence_number == to_buf->sequence_number) {
			#ifdef ACTIVE_LOG
				snprintf(msg, LOG_MSG_SIZE, "buff_in_order: segment already in list\n");
				print_on_log(fd, msg);
				memset(msg, 0, LOG_MSG_SIZE);
			#endif
			return;
		}
		current = current->next;
	};

	// update last_byte_buffered and how much free memory is free in recv buffer
	#ifdef ACTIVE_LOG
		snprintf(msg, LOG_MSG_SIZE, "buff_in_order: (%d + %d) mod INT_MAX = %d > %d?\n", to_buf->sequence_number, to_buf->data_length, (to_buf->data_length + to_buf->sequence_number)%INT_MAX, win->rcvwnd);
		print_on_log(fd, msg);
		memset(msg, 0, LOG_MSG_SIZE);
	#endif

	if(((to_buf->sequence_number) + (to_buf->data_length))%INT_MAX > win->last_byte_buffered){
		win->last_byte_buffered = (to_buf->sequence_number + to_buf->data_length)%INT_MAX;
	}

	win->rcvwnd -= to_buf->data_length;
	if(win->rcvwnd < 0) {
		#ifdef ACTIVE_LOG
			snprintf(msg, LOG_MSG_SIZE, "buff_in_order: rcvwnd is negative so it can't be delivered, this segment won't be buffered\n");
			print_on_log(fd, msg);
			memset(msg, 0, LOG_MSG_SIZE);
		#endif
		//exit(EXIT_FAILURE);
		win->rcvwnd = 0;
		return;
	}

	(*list_length)++;

	#ifdef ACTIVE_LOG
		snprintf(msg, LOG_MSG_SIZE, "buff_in_order: Updating rcvwnd to %d and list_length to %d\n", win->rcvwnd, *list_length);
		print_on_log(fd, msg);
		memset(msg, 0, LOG_MSG_SIZE);
	#endif

	// if the list is empty, to_buf becomes the head
	if(*segment_head == NULL){
		#ifdef ACTIVE_LOG
			snprintf(msg, LOG_MSG_SIZE, "buff_in_order: Inserting in list as head, (%d bytes of data)\n", to_buf->data_length);
			print_on_log(fd, msg);
			memset(msg, 0, LOG_MSG_SIZE);
		#endif
		
		*segment_head = to_buf;
		to_buf->next = NULL;
		return;
	}
	else {
		// there are segments in the list
		// look for the correct position of to_buf in the list based on seq_num

		// if seq_num of to_buf is > than that of the head, to_buf becomes the new head
		if( ( *segment_head )->sequence_number > to_buf->sequence_number){
			#ifdef ACTIVE_LOG
				snprintf(msg, LOG_MSG_SIZE, "buff_in_order: Inserting in list as head\n");
				print_on_log(fd, msg);
				memset(msg, 0, LOG_MSG_SIZE);
			#endif

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

				#ifdef ACTIVE_LOG
					snprintf(msg, LOG_MSG_SIZE, "buff_in_order: Inserting in list in position %d\n", pos_in_list);
					print_on_log(fd, msg);
					memset(msg, 0, LOG_MSG_SIZE);
				#endif

				current->next = to_buf;
				to_buf->next = NULL;
				return;
			}
			else if(next->sequence_number > to_buf->sequence_number) {
				// to_buf must be inserted in the list before the next segment
				pos_in_list++;

				#ifdef ACTIVE_LOG
					snprintf(msg, LOG_MSG_SIZE, "buff_in_order: Inserting in list in position %d\n", pos_in_list);
					print_on_log(fd, msg);
					memset(msg, 0, LOG_MSG_SIZE);
				#endif

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
	
	#ifdef ACTIVE_LOG
		snprintf(msg, LOG_MSG_SIZE, "write_all:\n");
		print_on_log(fd, msg);
		memset(msg, 0, LOG_MSG_SIZE);
	#endif

	while(current != NULL) {
		
		#ifdef ACTIVE_LOG
			snprintf(msg, LOG_MSG_SIZE, "%d == %d ?\n", current->sequence_number, win->next_to_ack);
			print_on_log(fd, msg);
			memset(msg, 0, LOG_MSG_SIZE);
		#endif
		
		// increments the next_to_ack so that multiple segments can be acked
		if((current)->sequence_number == win->next_to_ack) {
			#ifdef ACTIVE_LOG
				snprintf(msg, LOG_MSG_SIZE, "write_all: Manipolo %d, lunghezza dati %d, copio in %p\n", current->sequence_number, current->data_length, *buf);
				print_on_log(fd, msg);
				memset(msg, 0, LOG_MSG_SIZE);
			#endif

			int n = current->data_length;
			memcpy(*buf, current->data, n);
			//printf("write_all: \nData: %s\nCopied: %s\n", current->data, *buf);

			// sildes the window
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

			#ifdef ACTIVE_LOG
				snprintf(msg, LOG_MSG_SIZE, "write_all: Bytes received this session: %d\nwrite_all: %d bytes acked / next_to_ack: %d\n", *bytes_recvd, win->tot_acked, win->next_to_ack);
				print_on_log(fd, msg);
				memset(msg, 0, LOG_MSG_SIZE);
			#endif
		}
		else {
			// the segment is out of order, we can't write it now
			break;
		}
		//printf("current = %p\n", current->next);
		current = current->next;
	}

	#ifdef ACTIVE_LOG
		snprintf(msg, LOG_MSG_SIZE, "write_all: list_length %d, freeing %d\n", list_size, n_wrote);
		print_on_log(fd, msg);
		memset(msg, 0, LOG_MSG_SIZE);
	#endif

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
	fill_struct(&segment[index], (wind->next_seq_num%= INT_MAX), (ack_num%= INT_MAX), wind->rcvwnd, ack, fin, syn);

	#ifdef ACTIVE_LOG
		snprintf(msg, LOG_MSG_SIZE, "prepare_segment\nSEQ_NUM: %d\nASF: %d%d%d\n", wind->next_seq_num, segment[index].ack, segment[index].syn, segment[index].fin);
		print_on_log(fd, msg);
		memset(msg, 0, LOG_MSG_SIZE);
	#endif
	
	wind->next_seq_num += n_byte; // sequence number for the next segment
	wind->next_seq_num %= INT_MAX;

	wind->on_the_fly += n_byte;
	wind->on_the_fly %= INT_MAX;
	
	int prev_last_to_ack = wind->last_to_ack;
	wind->last_to_ack += n_byte;
	wind->last_to_ack %= INT_MAX;
	
	#ifdef ACTIVE_LOG
		snprintf(msg, LOG_MSG_SIZE, "prepare_segment: last_to_ack %d -> %d\nCopying %d to position %d\n", prev_last_to_ack, wind->last_to_ack, segment[index].sequence_number, index);
		print_on_log(fd, msg);
		memset(msg, 0, LOG_MSG_SIZE);
	#endif
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
	fill_struct(&segment,0, 0, win_size, ack, fin, syn);

	#ifdef ACTIVE_LOG
		snprintf(msg, LOG_MSG_SIZE, "send_flags\nASF: %d%d%d\n", segment.ack, segment.syn, segment.fin);
		print_on_log(fd, msg);
		memset(msg, 0, LOG_MSG_SIZE);
	#endif

	make_seg(segment, buf);
	int ret = send_unreliable(sockd, buf, HEAD_SIZE);
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

	#ifdef ACTIVE_LOG
		snprintf(msg, LOG_MSG_SIZE, "ack_segments\nack: %d,%d %d%d%d %d\n", ack->sequence_number, ack->ack_number, ack->ack, ack->syn, ack->fin, ack->data_length);
		print_on_log(fd, msg);
		memset(msg, 0, LOG_MSG_SIZE);
	#endif

	make_seg(*ack, buff);
	int n = send_unreliable(recv_sock, buff, HEAD_SIZE);

	#ifdef ACTIVE_LOG
		snprintf(msg, LOG_MSG_SIZE, "ack_segments: Sent %d bytes for ack...\n", n);
		print_on_log(fd, msg);
		memset(msg, 0, LOG_MSG_SIZE);
	#endif
}

// called from the receiver to free the segments it has acked
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
	float p = ((float)rand()/(float)(RAND_MAX)) *100;

	// we check if we will "lose" the segment
	if(p >= loss_prob) {
		int n_send = send(sockd, segm_to_go, n_bytes, 0);

		#ifdef ACTIVE_LOG
			snprintf(msg, LOG_MSG_SIZE, "send_unreliable: Send success(%d bytes), asked to send %d bytes...\n", n_send, n_bytes);
			print_on_log(fd, msg);
			memset(msg, 0, LOG_MSG_SIZE);
		#endif
	}
	else {
		#ifdef ACTIVE_LOG
			snprintf(msg, LOG_MSG_SIZE, "send_unreliable: lost %d bytes \n", n_bytes);
			print_on_log(fd, msg);
			memset(msg, 0, LOG_MSG_SIZE);
		#endif
	}
	return 0;
}

int send_tcp(int sockd, void* buf, size_t size){
	char send_buf[MSS+HEAD_SIZE];
	char data_buf[MSS];
	char recv_ack_buf[HEAD_SIZE];
	memset(send_buf, 0, MSS+HEAD_SIZE);
	memset(data_buf, 0, MSS);

	/* used to set the congestion window if it's the first time the client/server send a packet*/
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
	int ret = 0;
	
	// initialize the structs
	for(int j=0; j < MAX_BUF_SIZE-1; j++)
		memset(&send_segm[j], 0, sizeof(tcp));
	
	//sender_wind.max_size = MSS; //we accept at most BUFSIZ bytes on the fly at the same time
	#ifdef ACTIVE_LOG
		snprintf(msg, LOG_MSG_SIZE, "send_tcp\ncongWin: %d\n", cong.cong_win);
		print_on_log(fd, msg);
		memset(msg, 0, LOG_MSG_SIZE);
	#endif

	sender_wind.max_size = cong.cong_win < win_size-MSS ? cong.cong_win : win_size-MSS;
	sender_wind.max_size = sender_wind.max_size < size ? sender_wind.max_size : size;

	#ifdef ACTIVE_LOG
		snprintf(msg, LOG_MSG_SIZE, "send_tcp\nmax: %d\nlast_to_ack: %d\n", sender_wind.max_size, sender_wind.last_to_ack);
		print_on_log(fd, msg);
		memset(msg, 0, LOG_MSG_SIZE);
	#endif

	time_out send_timeo; // the tcp timeout will be kept in this struct
	memset(&send_timeo, 0, sizeof(send_timeo));
	send_timeo.time.tv_sec = TIME_START_SEC; // we set the first timeout to 3sec, as it's in TCP standard
	send_timeo.time.tv_usec = TIME_START_USEC;

	struct timeval rtt_timeout;
	memset(&rtt_timeout, 0, sizeof(rtt_timeout));
	rtt_timeout.tv_sec = send_timeo.time.tv_sec;
	rtt_timeout.tv_usec = send_timeo.time.tv_sec;
	
	#ifdef TCP_TO
		// useful to get strat time and end time
		struct timeval start_rtt;
		struct timeval finish_rtt;
	#endif

	int index = 0; // sender segments list index

	// we continue to send new segment / recevie acks untile we have sent and acked size bytes
	while(bytes_left_to_send > 0 || sender_wind.bytes_acked_current_transmission < size) {

		#ifdef ACTIVE_LOG
			sprintf(msg, "Attulamente il timeout è di %ld sec e %ld usec\n", send_timeo.time.tv_sec,
			send_timeo.time.tv_usec);
			print_on_log(fd, msg);
			memset(msg, 0, LOG_MSG_SIZE);

			snprintf(msg, LOG_MSG_SIZE, "send_tcp: %d bytes acked out of %ld\n%d < 0 ?\n", sender_wind.bytes_acked_current_transmission, size, sender_wind.on_the_fly);
			print_on_log(fd, msg);
			memset(msg, 0, LOG_MSG_SIZE);
		#endif

		if(sender_wind.on_the_fly < 0){
			fprintf(stderr, "send_tcp: on_the_fly variabile is negative: %d\n", sender_wind.on_the_fly);
			return -1;
		}

		#ifdef ACTIVE_LOG
			snprintf(msg, LOG_MSG_SIZE, "send_tcp: Bytes left to send: %d / %ld\n", bytes_left_to_send, size);
			print_on_log(fd, msg);
			memset(msg, 0, LOG_MSG_SIZE);

			snprintf(msg, LOG_MSG_SIZE, "send_tcp: Need to send segments? -> %d > 0 && %d < %d\n", bytes_left_to_send, sender_wind.on_the_fly, sender_wind.max_size);
			print_on_log(fd, msg);
			memset(msg, 0, LOG_MSG_SIZE);
		#endif
		
		// we can try to send bytes, there is still data to send
        // need to check if receiver can still receive data
		if((bytes_left_to_send > 0 && sender_wind.on_the_fly < sender_wind.max_size)) {

			fd_set set_sock_send = { 0 };
			FD_ZERO(&set_sock_send);
			FD_SET(sockd, &set_sock_send);

            // wait for socket to be ready for writing data
			if( select(sockd + 1, NULL, &set_sock_send, NULL, NULL) < 0 ){
				perror("select error\n");
				exit(EXIT_FAILURE);
			}
			
			if(FD_ISSET(sockd, &set_sock_send)){
				int n_to_copy = (bytes_left_to_send > MSS)*MSS + (bytes_left_to_send <= MSS)*bytes_left_to_send;
				memcpy(data_buf, buf, n_to_copy);

				#ifdef ACTIVE_LOG
					snprintf(msg, LOG_MSG_SIZE, "send_tcp: Taking %d bytes from input buf\n", n_to_copy);
					print_on_log(fd, msg);
					memset(msg, 0, LOG_MSG_SIZE);
				#endif

				bytes_left_to_send -= n_to_copy;
				data_buf[n_to_copy] = 0;
				n_read += n_to_copy;
				buf += n_to_copy;

				if(n_to_copy >= 0) {
					
					#ifdef ACTIVE_LOG
						snprintf(msg, LOG_MSG_SIZE, "send_tcp\nindex: %d, max: %d\n", index, MAX_BUF_SIZE);
						print_on_log(fd, msg);
						memset(msg, 0, LOG_MSG_SIZE);
					#endif

					// we check if we can send data without exceeding the max number of bytes on the fly
					prepare_segment(send_segm, &sender_wind, data_buf, recv_win.tot_acked, index, n_to_copy, Ack);
					int n_buf = make_seg(send_segm[index], send_buf); // we put our segment in a buffer that will be sent over the socket

					#ifdef ACTIVE_LOG
						snprintf(msg, LOG_MSG_SIZE, "send_tcp\nNew segment (seq_num, data_length): (%d, %d)\n", send_segm[index].sequence_number, send_segm[index].data_length);
						print_on_log(fd, msg);
						memset(msg, 0, LOG_MSG_SIZE);
					#endif

					int n_send = send_unreliable(sockd, send_buf, n_buf);

					memset(send_buf, 0, HEAD_SIZE + MSS); //we reset the buffer to send the next segment
					memset(data_buf, 0, MSS); // we reset the buffer so that we can reuse it
					index++;
					index %= MAX_BUF_SIZE;
					#ifdef TCP_TO	
						gettimeofday(&start_rtt, NULL);
					#endif
				}
			}

		}

		#ifdef ACTIVE_LOG
			snprintf(msg, LOG_MSG_SIZE, "send_tcp: %d >= %d || %d >= %ld\n", sender_wind.on_the_fly, sender_wind.max_size, n_read, size);
			print_on_log(fd, msg);
			memset(msg, 0, LOG_MSG_SIZE);
		#endif

		// we check if we have sent the maximum allowed bytes on the fly, in that case we need to wait for an ACK
		if(sender_wind.on_the_fly >= sender_wind.max_size || n_read >= size) {
			#ifdef ACTIVE_LOG
				snprintf(msg, LOG_MSG_SIZE, "send_tcp: Waiting for ack...\n");
				print_on_log(fd, msg);
				memset(msg, 0, LOG_MSG_SIZE);
			#endif

			fd_set set_sock_recv = { 0 };
			FD_ZERO(&set_sock_recv);
			FD_SET(sockd, &set_sock_recv);

			if(select(sockd + 1, &set_sock_recv, NULL, NULL, &(rtt_timeout)) < 0 ){
				perror("select error\n");
				exit(EXIT_FAILURE);
			}

			if(FD_ISSET(sockd, &set_sock_recv)) { //we expect a buffer with only header and no data
					memset(recv_ack_buf, 0, HEAD_SIZE);

					recv(sockd, recv_ack_buf, HEAD_SIZE, 0);
					
					#ifdef TCP_TO
						// acquire the time to estimate the rtt
						gettimeofday(&finish_rtt, NULL);
						estimate_timeout(&send_timeo, start_rtt, finish_rtt);
					#endif

					rtt_timeout.tv_sec = send_timeo.time.tv_sec;
					rtt_timeout.tv_usec = send_timeo.time.tv_usec;
					
					//printf("The next time out will be of %ld sec and %ld usec \n\n", send_timeo.time.tv_sec, send_timeo.time.tv_usec);
					memset(&recv_segm, 0, sizeof(recv_segm));
					ret = extract_segment(&recv_segm, recv_ack_buf);
						memset(recv_ack_buf, 0, HEAD_SIZE);

						if(recv_segm.fin && !recv_segm.ack && !recv_segm.syn){
							close_receiver_tcp(sockd);
						}

						#ifdef ACTIVE_LOG
							snprintf(msg, LOG_MSG_SIZE, "send_tcp\nReceived ack: %d\n%d == %d ?\nelse %d < %d && %d <= %d ?\n", recv_segm.ack_number, recv_segm.ack_number,
									sender_wind.next_to_ack, sender_wind.next_to_ack, recv_segm.ack_number, recv_segm.ack_number, sender_wind.last_to_ack);
							print_on_log(fd, msg);
							memset(msg, 0, LOG_MSG_SIZE);
						#endif

						// check if it is a duplicate ack
						if(recv_segm.ack_number == sender_wind.next_to_ack) {
							#ifdef ACTIVE_LOG
								snprintf(msg, LOG_MSG_SIZE, "send_tcp: received duplicate ack\n");
								print_on_log(fd, msg);
								memset(msg, 0, LOG_MSG_SIZE);
							#endif

							sender_wind.dupl_ack++; // we increment the number of duplicate acks received

							/*to update the congetion window*/
							congestion_control_caseFastRetrasmission_duplicateAck(sender_wind);
							check_size_buffer(sender_wind, recv_segm.receiver_window);

							//fast retransmission
							if(sender_wind.dupl_ack == 3) {
								times_retx++;

								#ifdef ACTIVE_LOG
									snprintf(msg, LOG_MSG_SIZE, "send_tcp: Fast retx\n");
									print_on_log(fd, msg);
									memset(msg, 0, LOG_MSG_SIZE);
								#endif

								/*to update the congetion window*/
								congestion_control_duplicateAck(sender_wind);
								check_size_buffer(sender_wind, recv_segm.receiver_window);

								retx(send_segm, sender_wind, send_buf, sockd);
								#ifdef TCP_TO
									gettimeofday(&start_rtt, NULL);
								#endif
							}
						}

						// the segment contains a new ack, unpack it and slide the window
						else if((sender_wind.next_to_ack < recv_segm.ack_number) && (recv_segm.ack_number <= sender_wind.last_to_ack)) {
							// printf("Ack OK. Sliding the window...\n");
							int bytes_acked = recv_segm.ack_number - sender_wind.next_to_ack;
							if(bytes_acked < 0){
								fprintf(stderr, "Bytes acked is negative: %d\n", bytes_acked);
								return -1;
							}
							sender_wind.bytes_acked_current_transmission += bytes_acked;

							/*to update the congetion window*/
							congestion_control_receiveAck(sender_wind);
							check_size_buffer(sender_wind, recv_segm.receiver_window);

							slide_window(&sender_wind, &recv_segm, send_segm);

							times_retx = 0;
						}
			}
			// we have to retx the last segment not acked due to TO
			else {

				#ifdef TCP_TO
					// estimates the timeout and sets the new values
					gettimeofday(&finish_rtt, NULL);
					estimate_timeout(&send_timeo, start_rtt, finish_rtt);
				#endif

				rtt_timeout.tv_sec = send_timeo.time.tv_sec;
				rtt_timeout.tv_usec = send_timeo.time.tv_usec;

				times_retx++;
				
				#ifdef ACTIVE_LOG
					snprintf(msg, LOG_MSG_SIZE, "send_tcp: TO expired\nRetransmitting segment...\n");
					print_on_log(fd, msg);
					memset(msg, 0, LOG_MSG_SIZE);
				#endif

				/*to update the congetion window*/
				congestion_control_timeout(sender_wind);
				check_size_buffer(sender_wind, recv_segm.receiver_window);

				retx(send_segm, sender_wind, send_buf, sockd);
				#ifdef TCP_TO
					gettimeofday(&start_rtt, NULL);
				#endif
			}
			memset(recv_segm.data, 0, recv_segm.data_length);
			memset(send_buf, 0, MSS+HEAD_SIZE);

			if(times_retx >= MAX_ATTMPTS_RETX){
				fprintf(stderr, "send_tcp: Retransmitted many times but did not receive any reply...\n");
				send_tcp(sockd, "ERRCONG", strlen("ERRCONG"));
				close(sockd);
				printf("Connection closed\n");
				for(int i=0; i < MAX_LINE_DECOR; i++)
					printf("-");
				printf("\n");
				fflush(stdout);
				exit(EXIT_FAILURE);
			}
		}
	}
	
	snprintf(msg, LOG_MSG_SIZE, "send_tcp: %d bytes acked out of %ld bytes to send\n", sender_wind.bytes_acked_current_transmission, size);
	print_on_log(fd, msg);
	memset(msg, 0, LOG_MSG_SIZE);

	memset(send_buf, 0, MSS+HEAD_SIZE);

	#ifdef ACTIVE_LOG
		snprintf(msg, LOG_MSG_SIZE, "send_tcp: Finished transmission (%d bytes)...\n", sender_wind.bytes_acked_current_transmission);
		print_on_log(fd, msg);
		memset(msg, 0, LOG_MSG_SIZE);
	#endif

	return sender_wind.bytes_acked_current_transmission;
}

/*function to implement the congestion control in case of Ack correctly received*/
int congestion_control_receiveAck(slid_win sender_wind) {

	switch(cong.state)
	{
		case 0:		//slow_start
			cong.cong_win = cong.cong_win + MSS;
			if (cong.cong_win > cong.threshold) {
				cong.state = 1;		// go to congestion_avoidance
				
				#ifdef ACTIVE_LOG
					snprintf(msg, LOG_MSG_SIZE, "congestion_control_receiveAck: Entered in Congestion Avoidance\n");
					print_on_log(fd, msg);
					memset(msg, 0, LOG_MSG_SIZE);
				#endif
			}
			break;

		case 1:		//congestion_avoidance
			cong.support_variable = cong.support_variable + (int)floor(MSS*MSS/cong.cong_win);

			#ifdef ACTIVE_LOG
				snprintf(msg, LOG_MSG_SIZE, "congestion_control_receiveAck: support variable: %d\n", cong.support_variable);
				print_on_log(fd, msg);
				memset(msg, 0, LOG_MSG_SIZE);
			#endif

			if (cong.support_variable >= CONG_SCALING_MSS_THRESHOLD) {
				cong.cong_win = cong.cong_win + MSS;
				cong.support_variable = 0;		// go to slow_stat
			}
			break;

		case 2:		//fast_recovery
			cong.cong_win = cong.threshold;
			cong.state = 1;		// go to congestion_avoidance

			#ifdef ACTIVE_LOG
				snprintf(msg, LOG_MSG_SIZE, "congestion_control_receiveAck: Entered in Congestion Avoidance\n");
				print_on_log(fd, msg);
				memset(msg, 0, LOG_MSG_SIZE);
			#endif

			break;
	}
	return 0;
}

/*function to implement the congestion control in case of duplicate ACK*/
int congestion_control_duplicateAck(slid_win sender_wind) {
	switch(cong.state)
	{
		case 0:		//slow_start
			cong.state = 2;		// go to fast_recovery
			
			#ifdef ACTIVE_LOG
				snprintf(msg, LOG_MSG_SIZE, "congestion_control_duplicateAck: Entered in Fast Recovery\n");
				print_on_log(fd, msg);
				memset(msg, 0, LOG_MSG_SIZE);
			#endif

			cong.threshold = cong.cong_win/2;
			cong.cong_win = cong.threshold + 3*MSS;
			break;

		case 1:		//congestion_avoidance
			cong.state = 2;		// go to fast_recovery

			#ifdef ACTIVE_LOG
				snprintf(msg, LOG_MSG_SIZE, "congestion_control_duplicateAck: Entered in Fast Recovery\n");
				print_on_log(fd, msg);
				memset(msg, 0, LOG_MSG_SIZE);
			#endif

			cong.threshold = cong.cong_win/2;
			cong.cong_win = cong.threshold + 3*MSS;
			break;

		case 2:		//fast_recovery
			// case already managed
			break;
	}
	return 0;
}

/*function to implement the congestion control when status is Fast Recovery*/
int congestion_control_caseFastRetrasmission_duplicateAck(slid_win sender_wind) {
	if (cong.state == 2) {
		cong.cong_win = cong.cong_win + MSS;
	}
}

/*function to implement the congestion control in case of timeout*/
int congestion_control_timeout(slid_win sender_wind) {
	switch(cong.state)
	{
		case 0:		//slow_start
			cong.threshold = cong.cong_win/2;
			cong.cong_win = MSS;
			break;

		case 1:		//congestion_avoidance
			cong.state = 0;		// go to slow_start

			#ifdef ACTIVE_LOG
				snprintf(msg, LOG_MSG_SIZE, "congestion_control_timeout: Entered in Slow Start\n");
				print_on_log(fd, msg);
				memset(msg, 0, LOG_MSG_SIZE);
			#endif

			cong.threshold = cong.cong_win/2;
			cong.cong_win = MSS;
			break;

		case 2:		//fast_recovery
			cong.state = 0;		// go to slow_start

			#ifdef ACTIVE_LOG
				snprintf(msg, LOG_MSG_SIZE, "congestion_control_timeout: Entered in Slow Start\n");
				print_on_log(fd, msg);
				memset(msg, 0, LOG_MSG_SIZE);
			#endif

			cong.threshold = cong.cong_win/2;
			cong.cong_win = MSS;
			break;
	}
	return 0;
}

/*function to update the size of the sender windows*/
int check_size_buffer(slid_win sender_wind, int receiver_window) {
	if (cong.cong_win < receiver_window) {
		sender_wind.max_size = cong.cong_win;
	}
	else {
		sender_wind.max_size = receiver_window;
	}

	#ifdef ACTIVE_LOG
		snprintf(msg, LOG_MSG_SIZE, "check_size_buffer\n congWin %d\nthreshold %d\nreceiver_window %d\nmax_size %d\n",
			cong.cong_win, cong.threshold, receiver_window, sender_wind.max_size);
		print_on_log(fd, msg);
		memset(msg, 0, LOG_MSG_SIZE);
	#endif

	return 0;
}

int recv_tcp(int sockd, void* buf, size_t size){

	#ifdef ACTIVE_LOG
		snprintf(msg, LOG_MSG_SIZE, "recv_tcp: New session...\n");
		print_on_log(fd, msg);
		memset(msg, 0, LOG_MSG_SIZE);
	#endif

	char *buf_ptr = buf;

	int n=0;
	char recv_buf[MSS+HEAD_SIZE];
	memset(recv_buf, 0, MSS+HEAD_SIZE);

	// the head for our segment linked list
	tcp *buf_segm = NULL;

	if(recv_win.last_to_ack == 0){
		recv_win.last_to_ack = win_size-MSS;
	}

	tcp ack; // the struct used to ack the segments
	int list_length = 0;
	int res_extract = 0;
	recv_win.rcvwnd = size;
	
	struct timeval recv_timeout;
	recv_timeout.tv_sec = RECV_TIMEOUT_SEC;
	recv_timeout.tv_usec = 0;
	setsockopt(sockd, SOL_SOCKET, SO_RCVTIMEO, (char *)&recv_timeout, sizeof(recv_timeout));

	bool finished = false;
	bool received_data = false;

	int bytes_rcvd = 0;
	int n_delayed_ack = 0;

	// goes on until all the bytes are received and acked
	while (!finished && ((n = recv(sockd, recv_buf, MSS+HEAD_SIZE, 0)) > 0)) {
		
		#ifdef ACTIVE_LOG
			snprintf(msg, LOG_MSG_SIZE, "recv_tcp\n Received %d bytes: %s\nrcvwnd: %d\n", n, recv_buf, recv_win.rcvwnd);
			print_on_log(fd, msg);
			memset(msg, 0, LOG_MSG_SIZE);
		#endif

		recv_timeout.tv_sec = 0;
		recv_timeout.tv_usec = 0;
		if(setsockopt(sockd, SOL_SOCKET, SO_RCVTIMEO, (char *)&recv_timeout, sizeof(recv_timeout)) == -1){
			fprintf(stderr, "recv_tcp: Error while setting options");
			return -1;
		}
		
		tcp *segment = malloc(sizeof(tcp));
		memset(segment, 0, sizeof(tcp));
		res_extract = extract_segment(segment, recv_buf);

		// invalid segment -> bad checksum
		if(res_extract < 0){
			#ifdef ACTIVE_LOG
				snprintf(msg, LOG_MSG_SIZE, "recv_tcp: Received invalid segment\n");
				print_on_log(fd, msg);
				memset(msg, 0, LOG_MSG_SIZE);
			#endif

			recv_timeout.tv_sec = RECV_TIMEOUT_SEC;
			recv_timeout.tv_usec = 0;
			if(setsockopt(sockd, SOL_SOCKET, SO_RCVTIMEO, (char *)&recv_timeout, sizeof(recv_timeout)) == -1) {
				fprintf(stderr, "recv_tcp: Error while setting options, RECV_TIMEOUT_SEC\n");
				return -1;
			}

			free(segment);
			memset(recv_buf, 0, MSS+HEAD_SIZE);
			continue;
		}

		if (strcmp(segment->data, "ERRCONG") == 0) {
			printf("Channel too congested, try again later...\n\n");
			fflush(stdout);
			close(sockd);
			printf("Connection closed\n");
			for(int i=0; i < MAX_LINE_DECOR; i++)
				printf("-");
			printf("\n");
			exit(EXIT_FAILURE);
		}

		// decide how long next to will be
		received_data = segment->data_length != 0;
		
		// received connection termination?
		if(segment->fin && !segment->ack && !segment->syn){
			close_receiver_tcp(sockd);
		}

		if(size == 0){
			return 0;
		}
		
		#ifdef ACTIVE_LOG
			snprintf(msg, LOG_MSG_SIZE, "recv_tcp: Extracted segment of %d bytes, seq num %d\n", segment->data_length, segment->sequence_number);
			print_on_log(fd, msg);
			memset(msg, 0, LOG_MSG_SIZE);
		#endif

		memset(recv_buf, 0, MSS+HEAD_SIZE);
		

		// buffer segments
		if((list_length < MAX_BUF_SIZE) && (recv_win.next_to_ack <= segment->sequence_number) 
			&& ( segment->sequence_number <= recv_win.last_to_ack) && (segment->data_length != 0)) {

			#ifdef ACTIVE_LOG
				snprintf(msg, LOG_MSG_SIZE, "recv_tcp: Buffering segment...\n");
				print_on_log(fd, msg);
				memset(msg, 0, LOG_MSG_SIZE);
			#endif

			buffer_in_order(&buf_segm, segment, &recv_win, &list_length);
		}

		recv_timeout.tv_sec = 0;
		recv_timeout.tv_usec = 500000;
		if(setsockopt(sockd, SOL_SOCKET, SO_RCVTIMEO, (char *)&recv_timeout, sizeof(recv_timeout)) == -1) {
			fprintf(stderr, "recv_tcp: Error while setting options, RECV_TIMEOUT_SEC\n");
			return -1;
		}

		// delayed ack -> check if we get a new segment 
		if(segment->data_length <= recv_win.rcvwnd) {
			if((n_delayed_ack = recv(sockd, recv_buf, MSS+HEAD_SIZE, 0) > 0)){
				#ifdef ACTIVE_LOG
					snprintf(msg, LOG_MSG_SIZE, "recv_tcp\n Received %d bytes: %s\nrcvwnd: %d\n", n, recv_buf, recv_win.rcvwnd);
					print_on_log(fd, msg);
					memset(msg, 0, LOG_MSG_SIZE);
				#endif

				tcp *second_segm = malloc(sizeof(tcp));
				// we got the new segment
				res_extract = extract_segment(second_segm, recv_buf);
				
				if(res_extract < 0){
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
					continue;
				}

				received_data = second_segm->data_length != 0;

				#ifdef ACTIVE_LOG
					snprintf(msg, LOG_MSG_SIZE, "recv_tcp\nExtracted segment (seq_num, data_length): (%d, %d)\n", second_segm->sequence_number, second_segm->data_length);
					print_on_log(fd, msg);
					memset(msg, 0, LOG_MSG_SIZE);
				#endif

				if(second_segm->fin && !second_segm->ack && !second_segm->syn){
					close_receiver_tcp(sockd);
				}

				memset(recv_buf, 0, MSS+HEAD_SIZE);

				if((list_length < MAX_BUF_SIZE) && (recv_win.next_to_ack <= second_segm->sequence_number) 
					&& ( second_segm->sequence_number <= recv_win.last_to_ack) && (second_segm->data_length != 0)) {

					#ifdef ACTIVE_LOG
						snprintf(msg, LOG_MSG_SIZE, "recv_tcp: Buffering segment...\n");
						print_on_log(fd, msg);
						memset(msg, 0, LOG_MSG_SIZE);
					#endif

					buffer_in_order(&buf_segm, second_segm, &recv_win, &list_length);
				}
			}
			else if(n_delayed_ack == 0){
				#ifdef ACTIVE_LOG
					snprintf(msg, LOG_MSG_SIZE, "recv_tcp: TO delayed ack expired\n");
					print_on_log(fd, msg);
					memset(msg, 0, LOG_MSG_SIZE);
				#endif
			}
			else {
				#ifdef ACTIVE_LOG
					snprintf(msg, LOG_MSG_SIZE, "recv_tcp: no bytes read\n%s\n", strerror(errno));
					print_on_log(fd, msg);
					memset(msg, 0, LOG_MSG_SIZE);
				#endif
			};
		}
		else {
			// can't receive anymore
			finished = true;
		}

		if(recv_win.rcvwnd <= 0){
			#ifdef ACTIVE_LOG
				snprintf(msg, LOG_MSG_SIZE, "recv_tcp: Receiver cannot receive any more data...\nrcvwnd: %d\n", recv_win.rcvwnd);
				print_on_log(fd, msg);
				memset(msg, 0, LOG_MSG_SIZE);
			#endif

			finished = true;
		}

		// acks all the segment received in order
		if(received_data) 
			ack_segments(&buf_ptr, sockd, &list_length, &buf_segm, &ack, &recv_win, &bytes_rcvd);
			
		#ifdef ACTIVE_LOG
			snprintf(msg, LOG_MSG_SIZE, "recv_tcp\n Tot bytes acked: %d\n", recv_win.tot_acked);
			print_on_log(fd, msg);
			memset(msg, 0, LOG_MSG_SIZE);
		#endif

		if(bytes_rcvd == size) {
			finished = true;
		}
		
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
		#ifdef ACTIVE_LOG
			snprintf(msg, LOG_MSG_SIZE, "recv_tcp: recv_error\n%s\n", strerror(errno));
			print_on_log(fd, msg);
			memset(msg, 0, LOG_MSG_SIZE);
		#endif
	}
	else{
		#ifdef ACTIVE_LOG
			snprintf(msg, LOG_MSG_SIZE, "recv_tcp: Main timeout expired...\n");
			print_on_log(fd, msg);
			memset(msg, 0, LOG_MSG_SIZE);
		#endif
	}
	
	//resets the time-out
	recv_timeout.tv_sec = 0;
	recv_timeout.tv_usec = 0;
	setsockopt(sockd, SOL_SOCKET, SO_RCVTIMEO, (char *)&recv_timeout, sizeof(recv_timeout));
	memset(recv_buf, 0, MSS+HEAD_SIZE);

	#ifdef ACTIVE_LOG
		snprintf(msg, LOG_MSG_SIZE, "recv_tcp: Finished transmission...\n");
		print_on_log(fd, msg);
		memset(msg, 0, LOG_MSG_SIZE);
	#endif

	return bytes_rcvd;
}

int recv_tcp_segm(int sockd, tcp* dest_segm){
	char recv_buf[HEAD_SIZE];
	memset(recv_buf, 0, HEAD_SIZE);
	int n = recv(sockd, recv_buf, HEAD_SIZE, 0);

	memset(dest_segm, 0, sizeof(tcp));
	extract_segment(dest_segm, recv_buf);
	
	if(dest_segm->fin && !dest_segm->ack && !dest_segm->syn){
		close_receiver_tcp(sockd);
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

	#ifdef ACTIVE_LOG
		snprintf(msg, LOG_MSG_SIZE, "estimate_timeout: TO: %ld s, %ld us\n", timeo->time.tv_sec, timeo->time.tv_usec);
		print_on_log(fd, msg);
		memset(msg, 0, LOG_MSG_SIZE);
	#endif
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
	new_sock_addr.sin_addr.s_addr = htonl(INADDR_ANY);

	for(int i=0; i< MAX_LINE_DECOR; i++)
		printf("-");
	printf("\nEnstablishing connection...\n");
	
	char address_string[INET_ADDRSTRLEN];
	int bind_attmpts = 0;
	new_sock_addr.sin_port = htons(getpid() + 8000);

	//look for a port for the current thread
	//try to bind to the port, if unsuccessful, abort
	for (int i = 0; i < MAX_ATTMPTS_PORT_SEARCH; i++) {
		if (new_sock_addr.sin_port <= 65536) {
			inet_ntop(new_sock_addr.sin_family, &new_sock_addr.sin_addr, address_string, INET_ADDRSTRLEN);

			printf("Binding to %s(%d)\n", address_string, ntohs(new_sock_addr.sin_port));

			int bind_value;
			if (bind_value = bind(socket_descriptor, (struct sockaddr *) &new_sock_addr, sizeof(new_sock_addr)) < 0 ) {
				fprintf(stderr, "connect: bind error\n%s\n", strerror(errno));
				bind_attmpts++;
				if (bind_attmpts == MAX_BIND_ATTMPTS) {
					perror("Could not bind to a port...");
					exit(EXIT_FAILURE);
				}
    		}
			if (bind_value == 0) {
				break;
			}
		}
		// skip 10 ports at a time so that there is 
		// a lower probability that another process takes the same port
		new_sock_addr.sin_port = new_sock_addr.sin_port + PROCESSES;
	}

	char server_address_string[INET_ADDRSTRLEN];
	inet_ntop(addr->sin_family, &addr->sin_addr, server_address_string, INET_ADDRSTRLEN);

	printf("Connecting to %s(%d)\n", server_address_string, ntohs(addr->sin_port));
	printf("Opened socket, sending Syn...\n");

	// prepare SYN message
	tcp segment;
	memset(&segment, 0, sizeof(segment));
	fill_struct(&segment,0, 0, win_size, 0, 0, 1);

	printf("ASF: %d%d%d\n", segment.ack, segment.syn, segment.fin);
	make_seg(segment, snd_buf);
	printf("sd: %d\n", socket_descriptor);

	// send SYN message
	if( sendto(socket_descriptor, snd_buf, HEAD_SIZE, 0, (struct sockaddr*) addr, sizeof(*addr)) < 0 ){
		fprintf(stderr, "socket_descriptor: %d\nError while sending syn...\n%s\n", socket_descriptor, strerror(errno));
		return -1;
	}

	socklen_t len = INET_ADDRSTRLEN;
	printf("Waiting server response...\n");
	// wait for syn-ack with information about the new port, which is dedicated to the  to use for communication
	if( recvfrom(socket_descriptor, recv_buf, HEAD_SIZE, 0, (struct sockaddr *) &server_addr, &len) < 0 ){
		fprintf(stderr, "socket_descriptor: %d\nrecvfrom: error\n%s\n", socket_descriptor, strerror(errno));
		return -1;
	}

	// connect to the new port allocated to this client by the server
	if( connect(socket_descriptor, (struct sockaddr *) &server_addr, addr_len) < 0){
		fprintf(stderr, "socket_descriptor: %d\nconnect: udp connect error\n%s\n", socket_descriptor, strerror(errno));
		exit(EXIT_FAILURE);
	}

	// extract flags and check if the message was a FIN-ACK
	extract_segment(&head_rcv, recv_buf);
	if((head_rcv.syn & head_rcv.ack) == 0){
		perror("No SYN-ACK from the other end\n");
		return -1;
	}

	// send ACK
	printf("Received Syn-Ack, sending Ack...\n");
	if( send_flags(socket_descriptor, Ack) < 0 ){
		perror("Error while sending ack...\n");
		return -1;
	}

	for(int i=0; i < MAX_LINE_DECOR; i++)
		printf("-");
	printf("\n");

	// connection enstablished
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
	// wait for SYN message from client
	if( recvfrom(sockd, recv_buf, HEAD_SIZE, 0, (struct sockaddr *) &client_address, &len) < 0 ){
		fprintf(stderr, "recvfrom: error\n%s\n", strerror(errno));
		return -1;
	}

	printf("Client address is %s:%d\n", inet_ntoa(client_address.sin_addr), ntohs(client_address.sin_port));

	// get client address and port information
	char address_string[INET_ADDRSTRLEN];
	inet_ntop(client_address.sin_family, &client_address.sin_addr, address_string, INET_ADDRSTRLEN);

	#ifdef ACTIVE_LOG
		snprintf(msg, LOG_MSG_SIZE, "Received new packet from %s(%d)\n", address_string, ntohs(client_address.sin_port));
		print_on_log(fd, msg);
		memset(msg, 0, LOG_MSG_SIZE);
	#endif

	extract_segment(&head_rcv, recv_buf);

	// set up a port for the client
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

	new_sock_addr.sin_addr.s_addr = htonl(INADDR_ANY);
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
	
	// connect to the client using the information from before
	if( connect(sock_conn, (struct sockaddr*) &client_address, len) < 0){
		perror("connect: failed to connect to client\n");
		fprintf(stderr, "%s\n", strerror(errno));
		return -1;
	}
	
	printf("head: %d,%d %d%d%d %d\n", head_rcv.sequence_number, head_rcv.ack_number, head_rcv.ack, head_rcv.syn, head_rcv.fin, head_rcv.data_length);

	// check the message received from the client
	if(head_rcv.syn){
		printf("Received Syn, sending Syn-Ack...\n");
		//send SYN-ACK
		if(send_flags(sock_conn, Syn | Ack) < 0){
			perror("send_flags failed\n");
			return -1;
		}
	}
	else {
		perror("Missing SYN\n");
		return -1;
	}
	
	head_rcv = (const tcp) { 0 };
	printf("Waiting for ack...\n");
	
	// wait for ACK
	if(recv_tcp_segm(sock_conn, &head_rcv) < 0){
		perror("recv_segm error\n");
		return -1;
	}

	printf("head: %d,%d %d%d%d %d\n", head_rcv.sequence_number, head_rcv.ack_number, head_rcv.ack, head_rcv.syn, head_rcv.fin, head_rcv.data_length);

	// check if message is a proper ACK
	if(!head_rcv.ack || head_rcv.fin || head_rcv.syn){
		perror("Missing ACK, terminating...\n");
		return -1;
	}
	
	printf("Received Ack...\n");

	printf("Connection established\n");

	for(int i=0; i < MAX_LINE_DECOR; i++)
		printf("-");
	printf("\n");

	// connection enstablished
	return sock_conn;
}

// allows to initiate a close sequence
int close_initiator_tcp(int sockd){
	printf("\n");
	for(int i=0; i < MAX_LINE_DECOR; i++)
		printf("-");
	printf("\nConnection termination on socket %d\n", sockd);
	
	// there are multiple attempts
	for(int i=0; i<MAX_ATTMPTS_CLOSE; i++){
		// first host sends a FIN message
		tcp temp = (const tcp) { 0 };
		printf("Sending Fin...\n");
		if( send_flags(sockd, Fin) < 0){
			printf("Failed to send fin, could not close connection...\n");
			
			#ifdef ACTIVE_LOG
				snprintf(msg, LOG_MSG_SIZE, "Failed to send fin, could not close connection...\n%s\n", strerror(errno));
				print_on_log(fd, msg);
				memset(msg, 0, LOG_MSG_SIZE);
			#endif
			exit(EXIT_FAILURE);
		}

		// sets a timeout so that it doesn't wait indefinitely
		struct timeval recv_timeout;
		recv_timeout.tv_sec = RECV_TIMEOUT_SHORT_SEC;
		recv_timeout.tv_usec = RECV_TIMEOUT_SHORT_USEC;
		if (setsockopt(sockd, SOL_SOCKET, SO_RCVTIMEO, (char *)&recv_timeout, sizeof(recv_timeout)) < 0) {
			perror("setsockopt failed\n");
			return -1;
		}

		// waits for a response
		recv_tcp_segm(sockd, &temp);
		
		recv_timeout.tv_sec = RECV_TIMEOUT_SHORT_SEC;
		recv_timeout.tv_usec = RECV_TIMEOUT_SHORT_USEC;
		if (setsockopt(sockd, SOL_SOCKET, SO_RCVTIMEO, (char *)&recv_timeout, sizeof(recv_timeout)) < 0) {
			perror("setsockopt failed\n");
			return -1;
		}

		// checks is the message is a FIN-ACK
		if((temp.fin && temp.ack) == 0 || (temp.syn)){
			if(i == MAX_ATTMPTS_CLOSE)
				perror("Missing fin-ack, could not close connection...\n");
			continue;
		}

		printf("Received Fin-Ack, sending Ack...\n");

		// send ACK
		if ( send_flags(sockd, Ack) < 0 ){
			if(i == MAX_ATTMPTS_CLOSE)
				perror("Failed to send ack, could not close connection...\n");
			continue;
		}
		break;
	}

	int res = close(sockd);
	close(fd);
	printf("Connection closed\n");
	for(int i=0; i < MAX_LINE_DECOR; i++)
		printf("-");
	printf("\n");
	
	// connection closed
	return res;
}

void close_receiver_tcp(int sockd){

	// a FIN message was received, so we need to reply with a FIN-ACK
	tcp temp = (const tcp) { 0 };
	int res = -1;

	char response[MAX_LINE];

	printf("\n");
	for(int i=0; i < MAX_LINE_DECOR; i++)
		printf("-");
	printf("\n");

	printf("Received Fin, closing connection...\nSending Fin-Ack...\n");

	if( send_flags(sockd, Fin | Ack) < 0){
		perror("Failed to send fin-ack, could not close connection\n");
		pthread_exit(&res);
	}

	struct timeval recv_timeout;
	recv_timeout.tv_sec = RECV_TIMEOUT_SHORT_SEC;
	recv_timeout.tv_usec = RECV_TIMEOUT_SHORT_USEC;

	fd_set set_sock_recv = { 0 };
	FD_ZERO(&set_sock_recv);
	FD_SET(sockd, &set_sock_recv);

	// multiple attempts to close the connection
	for(int i=0; i<MAX_ATTMPTS_CLOSE; i++){

		// waits for ACK
		if( select(sockd + 1, &set_sock_recv, NULL, NULL, &recv_timeout) < 0 ){
			perror("select error\n");
			exit(EXIT_FAILURE);
		}
		
		if(FD_ISSET(sockd, &set_sock_recv)){
			if (setsockopt(sockd, SOL_SOCKET, SO_RCVTIMEO, (char *)&recv_timeout, sizeof(recv_timeout)) < 0) {
				perror("setsockopt failed\n");
			}
			recv_tcp_segm(sockd, &temp);
		}
		
		recv_timeout.tv_sec = 0;
		recv_timeout.tv_usec = 0;
		if (setsockopt(sockd, SOL_SOCKET, SO_RCVTIMEO, (char *)&recv_timeout, sizeof(recv_timeout)) < 0) {
			perror("setsockopt failed\n");
		}

		// checks if the message is a proper ACK
		if( !temp.ack || temp.fin || temp.syn){
			if(i == MAX_ATTMPTS_CLOSE){
				perror("Missing ack, could not close connection...\n");
				pthread_exit(&res);
			}
		}
		else break;		
	}

	printf("Received Ack\n");

	res = close(sockd);
	close(fd);
	
	printf("Connection closed\n");
	for(int i=0; i < MAX_LINE_DECOR; i++)
		printf("-");
	printf("\n");

	// connection closed

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