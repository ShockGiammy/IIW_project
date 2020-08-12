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

#define MAX_LINE  4096
#define MAX_LINE_DECOR 30
#define CHECK_BIT(var,pos) ((var) & (1<<(pos)))

static int port = 10000;
static int port_host = 13500;
int congWin = MSS;

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


void fill_struct(tcp *segment, int seq_num, int ack_num, int recv, bool is_ack, bool is_fin, bool is_syn, char *data) {
	segment->sequence_number = seq_num;
	segment->ack_number = ack_num;
	segment->receiver_window = recv;
	segment->ack = is_ack;
	segment->fin = is_fin;
	segment->syn = is_syn;
	if(data != NULL) {
		strncpy(segment->data,data, MSS+38);
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
			send(socket_desc, buffer, MSS+HEAD_SIZE, 0);
			printf("Ritrasmetto segmento con numero di sequenza %d\n", segments[i].sequence_number);
			memset(buffer, 0, segments[i].data_length + HEAD_SIZE); //we reset the buffer to send the next segment
			break;
		}
	}
	printf("\n");
}

void buffer_in_order(tcp **segment_head, tcp *to_buf, slid_win *win) {
	tcp *current = *segment_head;

	// check if to_buf is already in the list
	while(current != NULL){
		if(current->sequence_number == to_buf->sequence_number) {
			return;
		}
		current = current->next;
	};

	// if the list is empty, to_buf becomes the head
	if(*segment_head == NULL){
		//printf("Inserisco in testa, (%d bytes of data)\n", to_buf->data_length);
		*segment_head = to_buf;
		to_buf->next = NULL;
		return;
	}
	else {
		// there are segments in the list
		// look for the correct position of to_buf in the list based on seq_num

		// if seq_num of to_buf is > than that of the head, to_buf becomes the new head
		if( ( *segment_head )->sequence_number > to_buf->sequence_number){
			//printf("Inserisco in testa\n");
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
				//printf("Inserisco in posizione %d\n", pos_in_list);
				current->next = to_buf;
				to_buf->next = NULL;
				return;
			}
			else if(next->sequence_number > to_buf->sequence_number) {
				// to_buf must be inserted in the list before the next segment
				pos_in_list++;
				//printf("Inserisco in posizione %d\n", pos_in_list);
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
		//printf(" %d == %d ?\n", current->sequence_number, win->next_to_ack);
		if((current)->sequence_number == win->next_to_ack) {
			//printf("write_all: Manipolo %d, lunghezza dati %ld, copio in %p\n", current->sequence_number, current->data_length, *buf);
			
			int n = current->data_length;
			memcpy(*buf, current->data, n);
			//printf("write_all: \nData: %s\nCopied: %s\n", current->data, *buf);
			
			*buf += n;
			//printf("last_to_ ack %d -> ", win->last_to_ack);
			win->last_to_ack += n;
			//printf("%d\n", win->last_to_ack);
			win->last_correctly_acked = (current)->sequence_number;
			
			win->tot_acked += n;

			win->next_seq_num += n;
			win->next_to_ack += n;
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

	free_segms_in_buff(segm_buff, n_wrote);
	return n_wrote;
}

// function used to prepare a segemnt that will be send and to set the window parameters properly
void prepare_segment(tcp * segment, slid_win *wind, char *data,  int index, int n_byte, int flags) {
	bool ack, syn, fin;

	ack = CHECK_BIT(flags, 0);
	syn = CHECK_BIT(flags, 1);
	fin = CHECK_BIT(flags, 2);
	
	memset(&segment[index], 0, sizeof(segment[index]));
	memcpy(segment[index].data, data, n_byte);
	segment[index].data_length = n_byte;
	fill_struct(&segment[index], wind->next_seq_num, 0, 0, ack, fin, syn, NULL);
	//printf("ASF: %d%d%d\n", segment[index].ack, segment[index].syn, segment[index].fin);
	
	wind->next_seq_num += segment[index].data_length; // sequence number for the next segment
	wind->on_the_fly += n_byte;
	//printf("last_to_ ack %d -> ", wind->last_to_ack);
	wind->last_to_ack += n_byte;
	//printf("%d\n", wind->last_to_ack);
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

int send_flags(int sockd, int flags){
	tcp segment;
	char buf[HEAD_SIZE];
	bool ack, syn, fin;
	
	memset(buf, 0, HEAD_SIZE);

	ack = CHECK_BIT(flags, 0);
	syn = CHECK_BIT(flags, 1);
	fin = CHECK_BIT(flags, 2);
	
	memset(&segment, 0, sizeof(segment));
	fill_struct(&segment,0, 0, 0, ack, fin, syn, NULL);
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
	fill_struct(ack, 0, recv_win->tot_acked, 0, true, false, false, NULL);
	ack->data_length = 0;
	//printf("ack: %d,%d %d%d%d %d\n", ack->sequence_number, ack->ack_number, ack->ack, ack->syn, ack->fin, ack->data_length);
	make_seg(*ack, buff);
	//printf("Attempting to ack %d\n", ack->ack_number);
	int n = send(recv_sock, buff, HEAD_SIZE, 0);
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
void send_unreliable(char *segm_to_go, int sockd) {
	int p = rand() % 10;

	// 30% of possibility to lost the segment
	if(p != 0 && p != 1 && p != 2) {
		printf("Send success...\n");
		send(sockd, segm_to_go, strlen(segm_to_go), 0);
	}
}

int send_tcp(int sockd, void* buf, size_t size){
	char send_buf[MSS+HEAD_SIZE];
	char data_buf[MSS];
	char recv_ack_buf[HEAD_SIZE];
	memset(send_buf, 0, MSS+HEAD_SIZE);
	memset(data_buf, 0, MSS);


	int bytes_left_to_send = size;
	int i = 0; // for tcp struct indexing
	int k;
	tcp send_segm[7]; // keeps the segment that we send, so that we can perform resending and/or acknowledge.
	tcp recv_segm = (const tcp) { 0 }; // used to unpack the ack and see if everything was good
	int n_read = 0;
	slid_win sender_wind; //sliding window for the sender
	
	memset(&sender_wind, 0, sizeof(sender_wind)); // we initialize the struct to all 0s
	
	for(int i=0; i<7; i++)
		memset(&send_segm[i], 0, sizeof(tcp));
	
	//sender_wind.max_size = MSS; //we accept at most BUFSIZ bytes on the fly at the same time
	if (congWin < 160000) {
		sender_wind.max_size = congWin;
	}
	else {
		sender_wind.max_size =160000;
	}

	printf("congWin iniziale %d\n", congWin);
	printf("max size iniziale%d\n", sender_wind.max_size);
	
	time_out send_timeo;
	memset(&send_timeo, 0, sizeof(send_timeo));

	printf("congWin iniziale %d\n", congWin);
	printf("max size iniziale%d\n", sender_wind.max_size);
	
	struct timeval time_out;
	time_out.tv_sec = 3; // we set 6 sec of timeout, we will estimate it in another moment
	time_out.tv_usec = 0;

	if(setsockopt(sockd, SOL_SOCKET, SO_RCVTIMEO, (char *)&time_out, sizeof(time_out)) == -1) {
		printf("Sender error setting opt\n");
	}

	while(sender_wind.tot_acked < size) {
		//printf("%d bytes acked out of %d\n", sender_wind.tot_acked, size);
		if(sender_wind.on_the_fly < sender_wind.max_size && sender_wind.last_to_ack < size) {
			int n_to_copy = (bytes_left_to_send > MSS)*MSS + (bytes_left_to_send <= MSS)*bytes_left_to_send;
			//printf("Taking %d bytes from input buf\n", n_to_copy);
			memcpy(data_buf, buf, n_to_copy);
			//printf("Copied: %s\n", data_buf);
			bytes_left_to_send -= n_to_copy;
			data_buf[n_to_copy] = 0;
			n_read += n_to_copy;
			buf += n_to_copy-1 ;
			//printf("Total bytes read : %d\n", n_read);

			if(n_to_copy >= 0) {
				// we check if we can send data without exceeding the max number of bytes on the fly
				//printf("Flags: %d\n", flags);
				prepare_segment(send_segm, &sender_wind, data_buf, i, n_to_copy, 0);
				int n_buf = make_seg(send_segm[i], send_buf); // we put our segment in a buffer that will be sent over the socket
				int n_send = send(sockd, send_buf, n_buf, 0);
				//printf("Sent %d bytes...\n", n_send);

				//send_unreliable(buffer, socket_desc);
				memset(send_buf, 0, HEAD_SIZE + MSS); //we reset the buffer to send the next segment
				memset(data_buf, 0, MSS); // we reset the buffer so that we can reuse it
				i = (i+1)%6;
			}
		}

		//printf("%d bytes on the fly\n", sender_wind.on_the_fly);

		// we have read the max number of data, we proceed with the sending in pipelining
		if(sender_wind.on_the_fly == sender_wind.max_size || n_read >= size) {
			//printf("Waiting for ack...\n");
			memset(recv_ack_buf, 0, HEAD_SIZE);
			if(recv(sockd, recv_ack_buf, HEAD_SIZE, 0) > 0) { //we expect a buffer with only header and no data
				memset(&recv_segm, 0, sizeof(recv_segm));
				extract_segment(&recv_segm, recv_ack_buf);
				memset(recv_ack_buf, 0, HEAD_SIZE);

				// printf("Received ack: %d\n", recv_segm.ack_number);
				// printf("%d == %d ?\n", recv_segm.ack_number, sender_wind.next_to_ack);
				// printf("else %d < %d && %d <= %d ?\n", sender_wind.next_to_ack, recv_segm.ack_number, recv_segm.ack_number, sender_wind.last_to_ack);

				if(recv_segm.ack_number == sender_wind.next_to_ack) {
					//printf("Ricevuto un riscontro duplicato\n");
					sender_wind.dupl_ack++; // we increment the number of duplicate acks received
						
					//fast retransmission
					if(sender_wind.dupl_ack == 3) {
						printf("Fast retx\n");
						retx(send_segm, sender_wind, send_buf, sockd);
					}
				}
				else if((sender_wind.next_to_ack < recv_segm.ack_number) && (recv_segm.ack_number <= sender_wind.last_to_ack)) {
					
					congWin = congWin + MSS;
					if (congWin < 160000) {
						sender_wind.max_size = congWin;
					}
					else {
						sender_wind.max_size =160000;
					}

					printf("congWin %d\n", congWin);
					printf("max size %d\n", sender_wind.max_size);

					slide_window(&sender_wind, &recv_segm, send_segm);
					
					//resets the segments that have been acked
					for(k = 0; k < sender_wind.n_seg; k++) {
						memset(&send_segm[(i+k)%6], 0, sizeof(send_segm[(i+k)%6]));
						memset(send_segm[(i+k)%6].data, 0, MSS);
					}
					printf("\n");
				}
			}

			// we have to retx the last segment not acked due to TO
			else {
				printf("TO expired\n");
				retx(send_segm, sender_wind, send_buf, sockd);
			}
			memset(recv_segm.data, 0, recv_segm.data_length);
			memset(send_buf, 0, MSS+HEAD_SIZE);
		}
	}
	// resets the time-out
	time_out.tv_sec = 0; // we set 6 sec of timeout, we will estimate it in another moment
	time_out.tv_usec = 0;
	if(setsockopt(sockd, SOL_SOCKET, SO_RCVTIMEO, (char *)&time_out, sizeof(time_out)) == -1) {
		printf("Sender error setting opt\n");
	}
	memset(send_buf, 0, MSS+HEAD_SIZE);
	//printf("Finished transmission...\n");
	return n_read;
}

int actul_window_dimension() {
	if (congWin < 160000) {
		return congWin;
	}
	else {
		return 160000;
	}
}

int recv_tcp(int sockd, void* buf, size_t size){

	char *buf_ptr = buf;

	int n=0;
	char recv_buf[MSS+HEAD_SIZE];
	memset(recv_buf, 0, MSS+HEAD_SIZE);

	// the head for our segment linked list
	tcp *buf_segm = NULL;

	tcp ack;
	slid_win recv_win; // the sliding window for the receiver
	int list_length = 0;

	memset(&recv_win, 0, sizeof(recv_win));
	recv_win.last_to_ack = MAX_WIN-MSS;
	
	struct timeval recv_timeout;
	bool finished = false;

	int bytes_rcvd = 0;

	while (!finished && (n = recv(sockd, recv_buf, MSS+HEAD_SIZE, 0)) > 0) {
		//printf("Received %d bytes: %s\n", n, recv_buf);
		
		tcp *segment = malloc(sizeof(tcp));
		memset(segment, 0, sizeof(tcp));
		extract_segment(segment, recv_buf);
		
		if(segment->fin && !segment->ack){
			close_server_tcp(sockd);
		}
		
		//printf("New segment of %d bytes, seq num %d\n", segment->data_length, segment->sequence_number);
		memset(recv_buf, 0, MSS+HEAD_SIZE);
		
		// buffer segments
		if(list_length < MAX_BUF_SIZE && (recv_win.next_to_ack <= segment->sequence_number) && ( segment->sequence_number <= recv_win.last_to_ack)) {
			list_length++;
			//printf("Buffering segment...\n");
			buffer_in_order(&buf_segm, segment, &recv_win);
		}
		
		recv_timeout.tv_sec = 0;
		recv_timeout.tv_usec = 500000;
		if(setsockopt(sockd, SOL_SOCKET, SO_RCVTIMEO, (char *)&recv_timeout, sizeof(recv_timeout)) == -1) {
			printf("Error while setting options");
			return -1;
		}

		// delayed ack -> check if we get a new segment 
		if(segment->data_length == MSS) {
			if((n = recv(sockd, recv_buf, MSS+HEAD_SIZE, 0) > 0)){
				tcp *second_segm = malloc(sizeof(tcp));
				// we got the new segment
				extract_segment(second_segm, recv_buf);
				if(second_segm->fin && !second_segm->ack){
					close_server_tcp(sockd);
				}

				//printf("New segment of %d bytes, seq num %d\n", second_segm->data_length, second_segm->sequence_number);
				memset(recv_buf, 0, MSS+HEAD_SIZE);

				//printf("Buffering segment...\n");
				buffer_in_order(&buf_segm, second_segm, &recv_win);
				list_length++;
				
				//check if this is the last segment
				if( second_segm->data_length < MSS ) {
					finished = true;
				}
			}
		}
		else {
			finished = true;
		}
		
		//printf("Finished? %d\n", finished);

		ack_segments(&buf_ptr, sockd, &list_length, &buf_segm, &ack, &recv_win);
		//printf("Totale riscontrati %d\n", recv_win.tot_acked);
		bytes_rcvd = recv_win.tot_acked;
		
		memset(recv_buf, 0, MSS+HEAD_SIZE);
		recv_timeout.tv_sec = 0;
		recv_timeout.tv_usec = 0;
		setsockopt(sockd, SOL_SOCKET, SO_RCVTIMEO, (char *)&recv_timeout, sizeof(recv_timeout));
		memset(&ack, 0, sizeof(ack));
		memset(recv_buf, 0, MSS+HEAD_SIZE);
	}

	//resets the time-out
	recv_timeout.tv_sec = 0;
	recv_timeout.tv_usec = 0;
	setsockopt(sockd, SOL_SOCKET, SO_RCVTIMEO, (char *)&recv_timeout, sizeof(recv_timeout));
	memset(recv_buf, 0, MSS+HEAD_SIZE);

	//printf("Finished transmission...\n");
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

int connect_tcp(int socket_descriptor, struct sockaddr_in* addr, socklen_t addr_len){
	tcp head_rcv = (const tcp) {0};
	char recv_buf[HEAD_SIZE];
	char snd_buf[HEAD_SIZE];
	memset(recv_buf, 0, HEAD_SIZE);
	memset(snd_buf, 0, HEAD_SIZE);
	memset(&head_rcv, 0, sizeof(tcp));

	struct sockaddr_in server_addr;
	bzero(&server_addr, sizeof(server_addr));

	struct sockaddr_in new_sock_addr;
	memset(&new_sock_addr, 0, sizeof(new_sock_addr));
    new_sock_addr.sin_family      = AF_INET;
	new_sock_addr.sin_addr 		  = addr->sin_addr;
    new_sock_addr.sin_port        = htons(port_host);

	for(int i=0; i< MAX_LINE_DECOR; i++)
		printf("-");
	printf("\nEnstablishing connection...\n");
	
	char address_string[INET_ADDRSTRLEN];
	inet_ntop(new_sock_addr.sin_family, &new_sock_addr.sin_addr, address_string, INET_ADDRSTRLEN);

	printf("Binding to %s(%d)\n", address_string, ntohs(new_sock_addr.sin_port));
	
	if ( bind(socket_descriptor, (struct sockaddr *) &new_sock_addr, sizeof(new_sock_addr)) < 0 ) {
		fprintf(stderr, "connect: bind error\n%s\n", strerror(errno));
		exit(EXIT_FAILURE);
    }

	char server_address_string[INET_ADDRSTRLEN];
	inet_ntop(addr->sin_family, &addr->sin_addr, server_address_string, INET_ADDRSTRLEN);

	printf("Connecting to %s(%d)\n", server_address_string, ntohs(addr->sin_port));


	printf("Opened socket, sending Syn...\n");

	tcp segment;

	memset(&segment, 0, sizeof(segment));
	fill_struct(&segment,0, 0, 0, 0, 0, 1, NULL);
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

	char address_string[INET_ADDRSTRLEN];
	inet_ntop(client_address.sin_family, &client_address.sin_addr, address_string, INET_ADDRSTRLEN);

	printf("Received new packet from %s(%d)\n", address_string, ntohs(client_address.sin_port));

	extract_segment(&head_rcv, recv_buf);

	int sock_conn = socket(AF_INET, SOCKET_TYPE, IPPROTO_UDP);
	
	struct sockaddr_in new_sock_addr;
	int new_port = port++;
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
	
	printf("Connection closed\n");
	for(int i=0; i < MAX_LINE_DECOR; i++)
		printf("-");
	printf("\n");

	pthread_exit(&res);
}