/*
  HELPER.C
  ========
  
*/

#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <sys/socket.h>       /*  socket definitions        */
#include <sys/types.h>        /*  socket types              */
#include <sys/stat.h>   	  /*  stat for files transfer   */
#include <arpa/inet.h>        /*  inet (3) funtions         */
#include <unistd.h>           /*  misc. UNIX functions      */
#include "reliable_udp.h"
#include "helper.h"
#include <time.h>
#include <signal.h>
#include <pthread.h>

#define MAX_LINE  4096
static __thread pthread_t thread_id = -1;

int SendFile(int socket_desc, char* file_name, char* response) {

	int first_byte = 0;
	struct stat	file_stat;
	//int buf_size = BUFSIZ;
	//char* buffer = malloc(sizeof(char)*buf_size); 
	char buffer[BUFSIZE]; //we use this buffer for now, we'll try to use only response buffer

	printf("opening file\n");
	memset(buffer, 0, BUFSIZE);
	char path[100] = "DirectoryFiles/";
	strcat(path, file_name);
	int fd = open(path, O_RDONLY);
	if (fstat(fd, &file_stat) == -1) {
		printf("Error: file not found\n");
		send_tcp(socket_desc, "ERR", 3);
		fflush(stdout);
		return -1;
	}

	send_tcp(socket_desc, "OK", 2);
	printf("Sent OK\n");

	int filesize = htonl(file_stat.st_size);
	send_tcp(socket_desc, &filesize, sizeof(filesize));

	int offset = 0;
	int remain_data = file_stat.st_size;
	int sent_bytes = 0;
	int n_send = 0;

	/* Sending file data */
	int n_read = 0;
	while( (n_read = read(fd, buffer, BUFSIZE)) > 0){
		if ((n_send = send_tcp(socket_desc, buffer, n_read)) < 0 ){
			perror("File transmission error...\n");
			return -1;
		}
		// if(n_read != n_send){
		// 	fprintf(stderr, "Did not send as much data as read!\nread: %d\nsent: %d\n", n_read, n_send);
		// 	return -1;
		// }
		printf("Sent %d bytes\n\n", n_send);
		sent_bytes += n_read;
		printf("%d / %d sent...\n\n", sent_bytes, remain_data);
		memset(buffer, 0, BUFSIZE);
		/*buf_size = calculate_window_dimension();
		free(buffer);
		char* buffer = malloc(sizeof(char)*buf_size);*/
	}
	
	return 0;
}

int RetrieveFile(int socket_desc, char* fname) {
	char buffer[BUFSIZE];
	memset(buffer, 0, BUFSIZE);

	int fd = open(fname, O_WRONLY|O_CREAT, S_IRWXU);
	if (fd == -1) {
		perror("Unable to create file\n");
		return -1;
	}

	recv_tcp(socket_desc, buffer, 3);
	printf("Received: %s\n", buffer);
	if(strcmp(buffer, "ERR") == 0) {
		printf("Cannot retrieve file...\n");
		if (remove(fname) == 0) 
      		printf("Deleted created file successfully\n"); 
   		else
      		perror("Unable to delete the file\n"); 
		return -1;
	}

	memset(buffer, 0, BUFSIZE);

	int filesize;
	recv_tcp(socket_desc, &filesize, sizeof(filesize));
	filesize = ntohl(filesize);

	printf("File size is: %d\n", filesize);

	int tot_bytes_recvd = 0;
	int bytes_recvd = 0;
	int bytes_wrttn = 0;
	int tot_bytes_wr = 0;
	int recv_bytes_buffer = BUFSIZE < filesize ? BUFSIZE : filesize;

	while(tot_bytes_wr < filesize ){
		if ( (bytes_recvd = recv_tcp(socket_desc, buffer, recv_bytes_buffer)) < 0){
			fprintf(stderr, "RetrieveFile: %s\n", strerror(errno));
			return -1;
		}
		tot_bytes_recvd += bytes_recvd;
		printf("Received %d new bytes...\n", bytes_recvd);
		if( (bytes_wrttn = write(fd, buffer, bytes_recvd)) < 0){
			perror("File transmission error...\n");
			return -1;
		}
		tot_bytes_wr += bytes_wrttn;
		printf("%d / %d bytes written...\n", tot_bytes_wr, filesize);
		memset(buffer, 0, BUFSIZE);
		recv_bytes_buffer = (filesize - tot_bytes_recvd) < BUFSIZE ? (filesize - tot_bytes_recvd) : BUFSIZE;
	}
	close(fd);
	printf("File transfer complete!\n");
	return 0;		
}


/* This set of function aims to create a log file to keep tracks of server-client interaction, usefull to debug the code
in case of failure*/


int create_log_file(char *file_name) {
	FILE *file;

	char* log_filename = malloc(sizeof(char)*strlen(file_name)+1);
	memset(log_filename, 0, sizeof(char)*strlen(file_name)+1);
	strcpy(log_filename, file_name);

	replace_char(log_filename, ' ', '-');
	replace_char(log_filename, '\n', '.');
	replace_char(log_filename, ':', '_');

	printf("Creating log, filename: %s\n", log_filename);
	char path[100] = "LogFiles/";
	strncat(path, log_filename, strlen(log_filename));
	int fd;
	if(file = fopen(path, "r")) {
		fd = open(path, O_WRONLY, S_IRWXU);
	}
	else {
		fd = open(path, O_RDWR | O_CREAT | O_TRUNC, S_IRWXU);
	}
	if (fd == -1) {
		fprintf(stderr, "log file creation error\n");
		return -1;
	}
	printf("Log file created, fd: %d\n", fd);
	return fd;
}

int print_on_log(int log_fd, char *msg) {
	char log_msg[LOG_MSG_SIZE];
	memset(log_msg, 0, LOG_MSG_SIZE);
	time_t ltime;
	ltime = time(NULL);
	strncpy(log_msg, asctime(localtime(&ltime)), strlen(asctime(localtime(&ltime)))-1);
	strcat(log_msg, " : ");
	strncat(log_msg, msg, strlen(msg));

	lseek(log_fd, 0, SEEK_END);	
	write(log_fd, log_msg, strlen(log_msg));
}

char* replace_char(char* str, char find, char replace){
    char *current_pos = strchr(str,find);
    while (current_pos){
        *current_pos = replace;
        current_pos = strchr(current_pos,find);
    }
    return str;
}

void free_thread_list(thread_list_t* head){
	if( head == NULL )
		return;
	
	thread_list_t* prev = head;
	thread_list_t* curr = head->next;
	
	if(curr == NULL){
		free(prev);
		return;
	}
	while(curr != NULL){
		free(prev);
		prev = curr;
		curr = curr->next;
	}
}

int insert_thread_in_list(pthread_t tid, thread_list_t** head){
	if(head == NULL){
		perror("Pointer to list head is null!");
		return -1;
	}
	if(*head == NULL){
		thread_list_t* new_head = malloc(sizeof(thread_list_t));
		memset(new_head, 0, sizeof(thread_list_t));
		new_head->tid = tid;
		new_head->next = NULL;
		*head = new_head;
		return 0;
	}
	else{
		thread_list_t* new_element = malloc(sizeof(thread_list_t));
		if(new_element == NULL){
			return -1;
		}
		memset(new_element, 0, sizeof(thread_list_t));
		new_element->next = *head;
		new_element->tid = tid;
		*head = new_element;
		return 0;
	}
}

void signal_threads(thread_list_t* list_head, int sigo){
	thread_list_t* current = list_head;
	while(current != NULL){
		thread_id = current->tid;
		pthread_kill(thread_id, sigo);
		current = current->next;
	}
}

char *strremove(char *str, const char *sub) {
    char *p, *q, *r;
    if ((q = r = strstr(str, sub)) != NULL) {
        size_t len = strlen(sub);
        while ((r = strstr(p = r + len, sub)) != NULL) {
            memmove(q, p, r - p);
            q += r - p;
        }
        memmove(q, p, strlen(p) + 1);
    }
    return str;
}