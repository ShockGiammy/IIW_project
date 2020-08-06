/*
  HELPER.C
  ========
  
*/

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
#include "reliable_udp.h"
#include "helper.h"


#define MAX_LINE  4096


/*  Read a line from a socket  */


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
	time_out.tv_sec = 5; // we set 3 sec of timeout, we will estimate it in another moment
	if(setsockopt(socket_desc, SOL_SOCKET, SO_RCVTIMEO, (char *)&time_out, sizeof(time_out)) == -1) {
		printf("Sender error setting opt\n");
	}

	while((n = read(file_desc, response, MSS)) > 0 || sender_wind.tot_acked < file_size) {
		n_read += n;

		// we check if we can send data without exceeding the max number of bytes on the fly
		if(sender_wind.on_the_fly <= sender_wind.max_size && n > 0) {
			prepare_segment(send_segm, &sender_wind, response, i, n);
			make_seg(send_segm[i], buffer); // we put our segment in a buffer that will be sent over the socket
			send_tcp(socket_desc, buffer, strlen(buffer), 0);
			memset(buffer, 0, sizeof(char)*(strlen(buffer)+1)); //we reset the buffer to send the next segment
			memset(response, 0, sizeof(char)*(strlen(response)+1)); // we reset the buffer so taht we can reuse it
			i = (i+1)%6;
		}

		// we have read the max number of data, we proceed with the sending in pipelining
		if(sender_wind.on_the_fly == sender_wind.max_size || n_read == file_size) {
			if(recv_tcp(socket_desc, buffer, 37) > 0) { //we expect a buffer with only header and no data
				extract_segment(&recv_segm, buffer);
				memset(buffer, 0, sizeof(char)*(strlen(buffer) + 1));

				//we check if we received a segment in order
				if(sender_wind.next_to_ack <= recv_segm.ack_number <= sender_wind.last_to_ack) {
					slide_window(&sender_wind, &recv_segm);
				}

				// we received an ack out of order, which is a duplicate ack
				else {
					if(recv_segm.ack_number == sender_wind.last_correctly_acked) {
						sender_wind.dupl_ack++; // we increment the number of duplicate acks received
						
						//fast retransmission
						if(sender_wind.dupl_ack == 3) {
							printf("Fast retx\n");
							retx(send_segm, sender_wind, buffer, socket_desc);
						}
					}
				}
			}

			// we have to retx the last segment not acked due to TO
			else {
				printf("TO expired\n");
				retx(send_segm, sender_wind, buffer, socket_desc);
			}
			memset(recv_segm.data, 0, sizeof(char)*(strlen(recv_segm.data) + 1));
			memset(response, 0, sizeof(char)*(strlen(response)+1));
			for(k = 0; k < sender_wind.n_seg; k++) {
				memset(&send_segm[(i+k)%6], 0, sizeof(send_segm[(i+k)%6]));
				memset(send_segm[(i+k)%6].data, 0, sizeof(char)*(strlen(send_segm[(i+k)%6].data) + 1));
			}
		}
	}
	fill_struct(&send_segm[7], 0, 0, 0, false, false, false, "END");
	make_seg(send_segm[7], response);
	send_tcp(socket_desc, response, strlen(response), 0);
	close(file_desc);
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
	tcp buf_segm[7];
	tcp segment;
	tcp ack;
	slid_win recv_win; // the sliding wondow for the receiver
	int list_length = 0; 
	recv_win.next_to_ack = 0;
	recv_win.last_to_ack = 7500;
	
	// we set 0.5 sec of timeout, we will estimate it in another moment
	struct timeval recv_timeout;
	bool got_second = false; // usefull to know if we got another segment

	while ((n = recv_tcp(socket_desc, retrieveBuffer, MSS+37)) > 0) {

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
			memset(retrieveBuffer, 0, sizeof(char)*(strlen(retrieveBuffer)+1));
			if(strcmp(segment.data, "END") == 0) {
				printf("file receiving completed \n");
				fflush(stdout);
				break;
			}
			else {
				// we can still buffer segments
				if(list_length < MAX_BUF_SIZE) {
					strcat(segment.data, "\0");
					buffer_in_order(list_length, buf_segm, segment, &recv_win);
					// the segment is in order
					if(segment.sequence_number == recv_win.next_to_ack) {
						recv_win.next_to_ack+=strlen(segment.data); // now we expect this sequence number
					}
					list_length++;
				}
				recv_timeout.tv_sec = 0;
				recv_timeout.tv_usec = 500000;
				if(setsockopt(socket_desc, SOL_SOCKET, SO_RCVTIMEO, (char *)&recv_timeout, sizeof(recv_timeout)) == -1) {
					printf("Error while setting options");
				}

				// we are in delayed ack and check if we get a new segment 
				if(recv_tcp(socket_desc, retrieveBuffer, MSS+37) > 0) {
					
					//we got the new segment
					memset(&segment, 0, sizeof(segment));
					extract_segment(&segment, retrieveBuffer);
					memset(retrieveBuffer, 0, sizeof(char)*(strlen(retrieveBuffer)+1));
					
					//check if this is an EOF message 
					if(strcmp(segment.data, "END") == 0) {
						if(list_length != 0) {

							// writes all the data still in buffer
							ack_segments(fd, &list_length, buf_segm, &ack,  &recv_win, retrieveBuffer);
						}
						else {
							printf("file receiving completed \n");
							fflush(stdout);
							break;
						}
					}

					else if(list_length < MAX_BUF_SIZE) {
						got_second = true;
						strcat(segment.data, "\0");
						buffer_in_order(list_length, buf_segm, segment, &recv_win);
						list_length++;
					}
				}

				// we received a segment in order;
				if(got_second && segment.sequence_number == recv_win.next_to_ack) {
					recv_win.next_to_ack += strlen(segment.data);
					ack_segments(fd, &list_length, buf_segm, &ack,  &recv_win, retrieveBuffer);
					got_second = false;
				}
				else if(!got_second) {
					ack_segments(fd, &list_length, buf_segm, &ack,  &recv_win, retrieveBuffer);
				}
				// we received a segment out of order
				else if(recv_win.next_to_ack < segment.sequence_number <= recv_win.last_to_ack){
					printf("Invio riscontro duplicato\n");
					fill_struct(&ack, 0, recv_win.last_correctly_acked, 0, true, false, false, NULL);
					make_seg(ack, retrieveBuffer);
				}
				send_tcp(socket_desc, retrieveBuffer, strlen(retrieveBuffer), 0);
				memset(retrieveBuffer, 0, sizeof(char)*(strlen(retrieveBuffer)+1));
				recv_timeout.tv_sec = 0;
				recv_timeout.tv_usec = 0;
				setsockopt(socket_desc, SOL_SOCKET, SO_SNDTIMEO, (char *)&recv_timeout, sizeof(recv_timeout));
				memset(&segment, 0, sizeof(segment));
			}
		}
		memset(&ack, 0, sizeof(ack));
		memset(retrieveBuffer, 0, sizeof(char)*(strlen(retrieveBuffer)+1));
	}
	close(fd);
	memset(retrieveBuffer, 0, sizeof(char)*(strlen(retrieveBuffer)+1));
	return 0;		
}
