/*
  CLIENT.C
  ========
  
*/

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
#include "reliable_udp.h"
#include "helper.h"

#define MAX_LINE  4096
#define STDIN 0
#define COMMAND_SIZE 10

char cmd[10];
long conn_s;                /*  connection socket         */
char *path = "client_files/";

int ParseCmdLine(int , char **, char **);
void show_menu();

void _handler(int sigo) {
	if(close_initiator_tcp(conn_s) == -1) {
		printf("Close error\n");
		exit(-1);
	}
	exit(0);
}


int main(int argc, char *argv[]) { 

    short int port = 7000;           /*  port number, fixed        */
    struct    sockaddr_in servaddr;  /*  socket address structure  */
    char     *szAddress;             /*  Holds remote IP address   */
	struct	  hostent *he;

	char command[COMMAND_SIZE];
	char exitBuffer[10];
	char server_response[BUFSIZ];

	he=NULL;

	ParseCmdLine(argc, argv, &szAddress);

	check_args(argc-1, argv, 2);

	#ifdef ACTIVE_LOG
		init_log("_client_log_");
	#endif

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
		printf("client: IP address not valid.\nclient: IP address lookup...");
		
		if ((he=gethostbyname(szAddress)) == NULL) {
			printf("failed\n");
  			exit(EXIT_FAILURE);
		}
		printf("succeeded\n\n");
		servaddr.sin_addr = *((struct in_addr *)he->h_addr_list);
    }
   	signal(SIGINT,_handler);

    /*  connect() to the remote server  */
	char address_string[INET_ADDRSTRLEN];
	inet_ntop(servaddr.sin_family, &servaddr.sin_addr, address_string, INET_ADDRSTRLEN);
	printf("Enstablishing connection with %s\n", address_string);
    
	socklen_t addr_len = INET_ADDRSTRLEN;
	if ( connect_tcp(conn_s, &servaddr, addr_len ) < 0 ) {
		printf("client: connect error\n");
		exit(EXIT_FAILURE);
    }

	fd_set set_sock_stdin = { 0 };
	FD_ZERO(&set_sock_stdin);
	FD_SET(STDIN_FILENO, &set_sock_stdin);
	FD_SET(conn_s, &set_sock_stdin);

	int maxd = (STDIN_FILENO < conn_s) ? (conn_s + 1): (STDIN_FILENO + 1);

	printf("Welcome to the server\n");
	show_menu();

	do{
		memset(&set_sock_stdin, 0, sizeof(set_sock_stdin));
		FD_ZERO(&set_sock_stdin);
		FD_SET(STDIN_FILENO, &set_sock_stdin);
		FD_SET(conn_s, &set_sock_stdin);
		if( select(maxd, &set_sock_stdin, NULL, NULL, NULL) < 0 ){
			perror("select error\n");
			exit(EXIT_FAILURE);
		}

		// might receive connection termination from server
		if(FD_ISSET(conn_s, &set_sock_stdin)){
			recv_tcp(conn_s, NULL, 0);
		}

		if(FD_ISSET(STDIN_FILENO, &set_sock_stdin)){
			int len_filename = 50;
			char fname[len_filename];
			memset(fname, 0, len_filename);
			if(fgets(command, len_filename, stdin) == NULL) {
				printf("fgets error\n");
				if(close(conn_s) == -1) {
					printf("close error\n");
					exit(-1);
				}
			}
			command[strlen(command)-1] = '\0';

			/*command LIST*/
			if(strcmp(command, "list") == 0) {
				send_tcp(conn_s, command, strlen(command));
				memset(command, 0, sizeof(char)*(strlen(command)+1));

				short int tot_size = 0;
				recv_tcp(conn_s, &tot_size, sizeof(tot_size));
				tot_size = ntohs(tot_size);
				char files[tot_size];

				printf("Files in the current directory: \n");
				memset(fname, 0, len_filename);
				recv_tcp(conn_s, files, tot_size);
				printf("%s", files);
				printf("----------END----------\n");
				memset(files, 0, sizeof(char)*(strlen(files)+1));
			}

			/*command GET and PUT*/
			else if(strcmp(command,"get") == 0 || strcmp(command,"put") == 0) {
				char response[BUFSIZ];
				send_tcp(conn_s, command, strlen(command));

				int n = recv_tcp(conn_s, response, BUFSIZ);
				if( n < 0 || ( strcmp(response, "ready") != 0 )){
					fprintf(stderr, "Server side error, received %s\n", response);
					exit(EXIT_FAILURE);
				}

				if(strcmp(command, "get") == 0)
					printf("Enter the name of the file you want to download: ");
				else if(strcmp(command, "put") == 0)
					printf("Enter the name of the file you want to upload: ");

				fgets(fname, len_filename, stdin);

    			/* Remove trailing newline, if there. */

    			if ((strlen(fname) > 0) && (fname[strlen (fname) - 1] == '\n'))
        			fname[strlen (fname) - 1] = '\0';
				
				if (strstr(fname, "__temp") != NULL) {
					n = send_tcp(conn_s, "invalid", strlen("invalid"));
					printf("Cannot use '__temp' files...\n");
				}
				else {
					n = send_tcp(conn_s, fname, strlen(fname));

					if( n < 0 ){
						perror("Send error...\n");
						exit(EXIT_FAILURE);
					}

					char response[9];
					n = recv_tcp(conn_s, response, 9);
					if( n < 0 ){
						perror("Send error...\n");
						exit(EXIT_FAILURE);
					} else if(strcmp(response, "recvd fn")!=0){
						perror("Server did not receive filename properly\n");
						continue;
					} else if(strcmp(response, "ERR")==0){
						perror("Server side error, file not found...\n");
						continue;
					}

					if(strcmp(command, "get") == 0) {
						if( RetrieveFile(conn_s, fname, path) < 0 ){
							fprintf(stderr, "RetrieveFile: error...\n");
						}
					}

					/*command PUT*/
					else if(strcmp(command, "put") == 0) {
						if (SendFile(conn_s, fname, path) == 0) {
							printf("file transfer completed! \n");
						}
						else {
							printf("file transfer error \n");
							char error[] = "ERROR";
							send_tcp(conn_s, error, strlen(error));
						}
					}
				}

				/*clean buffers*/
				memset(fname, 0, sizeof(char)*(strlen(fname)+1));
				memset(command, 0, sizeof(char)*(strlen(command)+1));
				memset(response, 0, BUFSIZ);
			}
			
			/*command HELP*/
			else if(strcmp(command,"help") == 0){
				memset(command, 0, sizeof(char)*(strlen(command)+1));
				show_menu();
			}

			else {
				printf("Command not valid\n");
				memset(command, 0, sizeof(char)*(strlen(command)+1));
			}
		}
	}while(1);
}

void show_menu() {
	printf("\nMenù: \n");
	printf("list: to visualize the files available for download\n");
	printf("get: to request the download of a certain file\n");
	printf("put: to upload on server a certain file\n");
	printf("help: to show the menù\n\n");
}

int ParseCmdLine(int argc, char *argv[], char **szAddress) {
    int n = 1;

	if (argc != 4) {
   		printf("Syntax:\n./client serverIP loss_probability (xx.xx..) window_size\n");
	    exit(EXIT_SUCCESS);
	}

	*szAddress = argv[n];

    return 0;
}