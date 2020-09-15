/*
  HELPER.C
  ========
  
*/
#define _FILE_OFFSET_BITS 64

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

int SendFile(int socket_desc, char* file_name, char *directory_path) {
	int first_byte = 0;
	struct stat	file_stat;
	int win_size = get_win_size();
	char buffer[win_size]; //we use this buffer for now, we'll try to use only response buffer

	printf("opening file\n");
	memset(buffer, 0, win_size);
	
	char path[100];
	memset(path, 0, 100);
	strncpy(path, directory_path, strlen(directory_path));
	strncat(path, file_name, strlen(file_name));
	
	int fd = open(path, O_RDONLY);
	if (fstat(fd, &file_stat) == -1) {
		printf("Error: file not found\n");
		send_tcp(socket_desc, "ERR", 3);
		fflush(stdout);
		return -1;
	}

	memset(path, 0, sizeof(char)*(strlen(path)+1));
	send_tcp(socket_desc, "OK", 2);
	printf("Sent OK\n");

	FILE *fp = fdopen(fd, "r");
	fseeko(fp, 0L, SEEK_END);
	unsigned long long sz = ftell(fp);
	rewind(fp);
	
	unsigned long long filesize = htobe64(sz);

	printf("Size : %ld, converted : %lld\n", file_stat.st_size, filesize);
	send_tcp(socket_desc, &filesize, sizeof(filesize));

	unsigned long long offset = 0;
	unsigned long long remain_data = sz;
	unsigned long long sent_bytes = 0;
	unsigned long long n_send = 0;

	/* Sending file data */
	unsigned long long n_read = 0;
	while( (n_read = read(fd, buffer, win_size)) > 0){
		if ((n_send = send_tcp(socket_desc, buffer, n_read)) < 0 ){
			perror("File transmission error...\n");
			return -1;
		}
		// if(n_read != n_send){
		// 	fprintf(stderr, "Did not send as much data as read!\nread: %d\nsent: %d\n", n_read, n_send);
		// 	return -1;
		// }
		printf("Sent %lld bytes\n\n", n_send);
		sent_bytes += n_read;
		printf("%lld / %lld sent...\n\n", sent_bytes, remain_data);
		memset(buffer, 0, win_size);
	}
	
	return 0;
}

int RetrieveFile(int socket_desc, char* fname, char *directory_path) {
	int win_size = get_win_size();
	char buffer[win_size];
	memset(buffer, 0, win_size);

	char path[100];
	memset(path, 0, 100);
	strncpy(path, directory_path, strlen(directory_path));
	strncat(path, fname, strlen(fname));

	//int fd = open(path, O_WRONLY|O_CREAT, S_IRWXU);
	int fd = open(path, O_CREAT|O_RDWR|O_TRUNC, 0777);
	if (fd == -1) {
		perror("Unable to create file\n");
		return -1;
	}

	recv_tcp(socket_desc, buffer, 3);
	if(strcmp(buffer, "ERR") == 0) {
		printf("Cannot retrieve file...\n");
		if (remove(path) == 0) 
      		printf("File successfully deleted\n"); 
   		else
      		perror("Unable to delete the file"); 
		return -1;
	}
	memset(path, 0, sizeof(char)*(strlen(path)+1));

	memset(buffer, 0, win_size);

	unsigned long long filesize;
	recv_tcp(socket_desc, &filesize, sizeof(filesize));
	filesize = be64toh(filesize);

	printf("File size is: %lld\n", filesize);

	unsigned long long tot_bytes_recvd = 0;
	unsigned long long bytes_recvd = 0;
	unsigned long long bytes_wrttn = 0;
	unsigned long long tot_bytes_wr = 0;
	unsigned long long recv_bytes_buffer = win_size < filesize ? win_size : filesize;

	while(tot_bytes_wr < filesize ){
		if ( (bytes_recvd = recv_tcp(socket_desc, buffer, recv_bytes_buffer)) < 0){
			fprintf(stderr, "\nRetrieveFile: %s\n", strerror(errno));
			return -1;
		}
		tot_bytes_recvd += bytes_recvd;
		printf("Received %lld new bytes...\n", bytes_recvd);
		if( (bytes_wrttn = write(fd, buffer, bytes_recvd)) < 0){
			perror("File transmission error...\n");
			return -1;
		}
		tot_bytes_wr += bytes_wrttn;
		printf("%lld / %lld bytes written...\n", tot_bytes_wr, filesize);
		memset(buffer, 0, win_size);
		recv_bytes_buffer = (filesize - tot_bytes_recvd) < win_size ? (filesize - tot_bytes_recvd) : win_size;
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

// used to check if the loss probability and the window size are passed correctly as input
void check_args(int argc, char *argv[], int start) {
	if(argc < 3) {
		printf("Sintassi: ./server (valore probabilità di perdita) (valore finestra spedizione)\n");
		exit(EXIT_FAILURE);
	}


	float prob;
	if((prob = strtof(argv[start], NULL)) == 0) {
		printf("Invalid loss probability\n");
		exit(EXIT_FAILURE);
	}
	else if(prob == 100) {
		printf("Loss prob = 100%%, the software will not be that trivial :D \n");
		exit(EXIT_FAILURE);
	}

	long win_size;
	if((win_size = strtol(argv[start+1], NULL, 10)) == 0) {
		printf("Invalid window size\n");
		exit(EXIT_FAILURE);
	}

	printf("Parameters for this execution: probability of loss: %f%%\n", prob);
	set_params(prob, win_size);
}