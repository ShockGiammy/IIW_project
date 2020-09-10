/* First test files*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>       /*  socket definitions        */
#include <sys/types.h>        /*  socket types              */
#include <arpa/inet.h>        /*  inet (3) funtions         */
#include <unistd.h>           /*  misc. UNIX functions      */
#include <netinet/in.h>
#include <netdb.h>
#include <pthread.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <ctype.h>
#include <netinet/tcp.h>
#include <sys/stat.h>
#include "reliable_udp.h"
#include "helper.h"

#define MAX_LINE  4096
#define STDIN 0
#define COMMAND_SIZE 10

char cmd[10];
long conn_s;                /*  connection socket         */
char *path = "client_files";
int test_fd; // the file descriptor of the file we will use for our tests

typedef struct test_results {
    int win_size;
    float loss_prob;
    struct timeval res_time;
}results;

int ParseCmdLine(int , char **, char **, char **);
void create_file();
void save_test_values(results result);


int main(int argc, char *argv[]) {
    struct timeval start;
    struct timeval end;
    results results[10];
    int i = 0;

    short int port = 2000;                  /*  port number               */
    struct    sockaddr_in servaddr;  /*  socket address structure  */
    char     *szAddress = "127.0.0.1";             /*  Holds remote IP address   */
    char     *szPort;                /*  Holds remote port         */
    char     *endptr;                /*  for strtol()              */
	struct	  hostent *he;

	char command[COMMAND_SIZE];
	char exitBuffer[10];
	char username[40];
	char server_response[BUFSIZ];

	memset(username, 0, sizeof(username));

	he=NULL;
	check_args(argc, argv, 1);
	
	float loss_prob = 0.0; 
	int win_size = 0;
	get_params(&loss_prob, &win_size);

	for(int j = 0; j < 10; j++) {
		results[j].loss_prob = loss_prob;
	}

	init_log("_client_log_");
    create_file();

    /*  Create the listening socket  */

    if ((conn_s = socket(AF_INET, SOCKET_TYPE, 0)) < 0 ) {
		fprintf(stderr, "client: creation socket error\n");
		exit(EXIT_FAILURE);
    }

    /*  Set all bytes in socket address structure to
        zero, and fill in the relevant data members   */
	memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family      = AF_INET;
    servaddr.sin_port        = htons(port);

    /*  Set the remote IP address  */

    if ( inet_aton(szAddress, &servaddr.sin_addr) <= 0 ) {
		printf("client: IP address not valid.\nclient: IP addresso lookup...");
		
		if ((he=gethostbyname(szAddress)) == NULL) {
			printf("failed\n");
  			exit(EXIT_FAILURE);
		}
		printf("succeeded\n\n");
		servaddr.sin_addr = *((struct in_addr *)he->h_addr_list);
    }

    /*  connect() to the remote server  */
	char address_string[INET_ADDRSTRLEN];
	inet_ntop(servaddr.sin_family, &servaddr.sin_addr, address_string, INET_ADDRSTRLEN);
	printf("Enstablishing connection with %s\n", address_string);
    
	socklen_t addr_len = INET_ADDRSTRLEN;
	if ( connect_tcp(conn_s, &servaddr, addr_len ) < 0 ) {
		printf("client: connect error\n");
		exit(EXIT_FAILURE);
    }

	do{
		if(strcmp(argv[3], "get") == 0) {
			results[i].win_size = win_size;
			char response[BUFSIZ];
			send_tcp(conn_s, "get", 3);
			memset(command, 0, sizeof(char)*(strlen(command)));

			int n = recv_tcp(conn_s, response, BUFSIZ);
			if( n < 0 || ( strcmp(response, "ready") != 0 )){
				fprintf(stderr, "Server side error, received %s\n", response);
				exit(EXIT_FAILURE);
			}

		//sleep(2);

			n = send_tcp(conn_s, "lcet10.txt", 10);
			if( n < 0 ){
				perror("Send error...\n");
				exit(EXIT_FAILURE);
			}

        	gettimeofday(&start, NULL);
			if( RetrieveFile(conn_s, "lcet10.txt", path) < 0 ){
				fprintf(stderr, "RetrieveFile: error...\n");
			}

        	gettimeofday(&end, NULL);
        	results[i].res_time.tv_sec = end.tv_sec - start.tv_sec;
        	results[i].res_time.tv_usec = end.tv_usec - start.tv_usec;
        	while(results[i].res_time.tv_usec < 0) {
            	results[i].res_time.tv_usec += 1000000;
            	results[i].res_time.tv_sec -= 1;
        	}
		}
		else if(strcmp(argv[3], "put") == 0) {
			results[i].win_size = win_size;
			char bufferFile[BUFSIZ];

			int n = send_tcp(conn_s, "put", 3);
			if( n < 0 ){
				perror("Could not send command...\n");
				exit(EXIT_FAILURE);
			}

			memset(command, 0, sizeof(char)*(strlen(command)+1));

			n = recv_tcp(conn_s, server_response, BUFSIZ);
			if( n < 0 || ( strcmp(server_response, "ready") != 0 )){
				fprintf(stderr, "Server side error, received: %s\n", server_response);
				exit(EXIT_FAILURE);
			}

			memset(server_response, 0, BUFSIZ);
				
			n = send_tcp(conn_s, "lcet10.txt", 10);
			if( n < 0 ){
				perror("Could not send filename...\n");
				exit(EXIT_FAILURE);
			}

			n = recv_tcp(conn_s, server_response, BUFSIZ);
			if( n < 0 || ( strcmp(server_response, "rcvd fn") != 0 )){
				fprintf(stderr, "Server side did not receive filename, response: %s\n", server_response);
				exit(EXIT_FAILURE);
			}
			gettimeofday(&start, NULL);
			if (SendFile(conn_s, "lcet10.txt", bufferFile, path) == 0) {
				printf("file transfer completed \n");
				gettimeofday(&end, NULL);
        		results[i].res_time.tv_sec = end.tv_sec - start.tv_sec;
        		results[i].res_time.tv_usec = end.tv_usec - start.tv_usec;
        		while(results[i].res_time.tv_usec < 0) {
            		results[i].res_time.tv_usec += 1000000;
            		results[i].res_time.tv_sec -= 1;
        		}
			}
			else {
				printf("file transfer error \n");
				char error[] = "ERROR";
				send_tcp(conn_s, error, strlen(error));
			}
			//memset(fname, 0, sizeof(char)*(strlen(fname)+1));
			memset(server_response, 0, BUFSIZ);
		}
        save_test_values(results[i]);
        i++;
		win_size += 10000;
		sleep(3);

	}while(i < 10);
}


void save_test_values(results result) {
    char file_msg[1024];
    char temp[20];

    sprintf(temp, "%f ", result.loss_prob);
    strcat(file_msg, "Loss probability: ");
    strcat(file_msg, temp);
	strcat(file_msg, "% ");

    memset(temp, 0, sizeof(char)*(strlen(temp)+1));

    sprintf(temp, "%d ", result.win_size);
    strcat(file_msg, "Window size: ");
    strcat(file_msg, temp);

    memset(temp, 0, sizeof(char)*(strlen(temp)+1));

    sprintf(temp, "%ld ", result.res_time.tv_sec);
    strcat(file_msg, "Time: ");
    strcat(file_msg, temp);

    memset(temp, 0, sizeof(char)*(strlen(temp)+1));
    
    sprintf(temp, "%ld ", result.res_time.tv_usec);
    strcat(file_msg, temp);

    memset(temp, 0, sizeof(char)*(strlen(temp)+1));
	strcat(file_msg, "\n");

    lseek(test_fd, 0, SEEK_END);
    write(test_fd, file_msg, strlen(file_msg));

    memset(file_msg, 0, sizeof(char)*(strlen(file_msg)+1));
}


void create_file() {
    FILE *file;
    
    if(file = fopen("test1.txt", "r"))
        test_fd = open("test1.txt", O_WRONLY, S_IRWXU);
    else
        test_fd = open("test1.txt", O_RDWR | O_CREAT | O_TRUNC, S_IRWXU);
}


int ParseCmdLine(int argc, char *argv[], char **szAddress, char **szPort) {
    int n = 1;

    while ( n < argc ) {
		if ( !strncmp(argv[n], "-a", 2) || !strncmp(argv[n], "-A", 2) ) {
		    *szAddress = argv[++n];
		}
		else 
			if ( !strncmp(argv[n], "-p", 2) || !strncmp(argv[n], "-P", 2) ) {
			    *szPort = argv[++n];
			}
			else
				if ( !strncmp(argv[n], "-h", 2) || !strncmp(argv[n], "-H", 2) ) {
		    		printf("Sintassi:\n\n");
			    	printf("    client -a (indirizzo server) -p (porta del server) [-h].\n\n");
			    	exit(EXIT_SUCCESS);
				}
		++n;
    }
	if (argc==1) {
   		printf("Sintassi:\n\n");
    	printf("    client -a (indirizzo server) -p (porta del server) [-h].\n\n");
	    exit(EXIT_SUCCESS);
	}
    return 0;
}