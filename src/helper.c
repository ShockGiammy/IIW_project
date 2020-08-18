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
#include <time.h>

#define MAX_LINE  4096


/*  Read a line from a socket  */


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
		printf("error to create file");
		return -1;
	}

	recv_tcp(socket_desc, buffer, 3);
	printf("Received: %s\n", buffer);
	if(strcmp(buffer, "ERR") == 0) {
		if (remove(fname) == 0) 
      		printf("Deleted successfully\n"); 
   		else
      		printf("Unable to delete the file\n"); 
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
	char path[100] = "LogFiles/";
	strncat(path, file_name, strlen(file_name));
	int fd;
	if(file = fopen(path, "r")) {
		printf("Qui\n");
		fd = open(path, O_WRONLY, S_IRWXU);
	}
	else {
		fd = open(path, O_RDWR | O_CREAT | O_TRUNC, S_IRWXU);
	}
	if (fd == -1) {
		printf("error to create file");
		return -1;
	}
	return fd;
}

int print_on_log(int log_fd, char *msg) {
	char *log_msg;
	memset(log_msg, 0, 1024);
	time_t ltime;
	ltime = time(NULL);
	strncpy(log_msg, asctime(localtime(&ltime)), strlen(asctime(localtime(&ltime)))-1);
	strcat(log_msg, " : ");
	strncat(log_msg, msg, strlen(msg));

	lseek(log_fd, 0, SEEK_END);	
	write(log_fd, log_msg, strlen(log_msg));
}