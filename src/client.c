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
char *path = "client_files";

int ParseCmdLine(int , char **, char **, char **);
void show_menu();

void _handler(int sigo) {
	if(close_initiator_tcp(conn_s) == -1) {
		printf("Close error\n");
		exit(-1);
	}
	exit(0);
}


int main(int argc, char *argv[]) { 
    short int port;                  /*  port number               */
    struct    sockaddr_in servaddr;  /*  socket address structure  */
    char     *szAddress;             /*  Holds remote IP address   */
    char     *szPort;                /*  Holds remote port         */
    char     *endptr;                /*  for strtol()              */
	struct	  hostent *he;

	char command[COMMAND_SIZE];
	char exitBuffer[10];
	char username[40];
	char server_response[BUFSIZ];

	memset(username, 0, sizeof(username));

	he=NULL;
	ParseCmdLine(argc, argv, &szAddress, &szPort);

	check_args(argc, argv, 5);

	init_log("_client_log_");

    /*  Set the remote port  */
    port = strtol(szPort, &endptr, 0);
    if ( *endptr ) {
		printf("client: unknown port\n");
		exit(EXIT_FAILURE);
    }

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

	/* connessione */
	//printf("Insert a username: ");
	//fflush(stdout);

	/*if( select(maxd, &set_sock_stdin, NULL, NULL, NULL) < 0 ){
		perror("select error\n");
		exit(EXIT_FAILURE);
	}*/

	// might receive connection termination from server
	/*if(FD_ISSET(conn_s, &set_sock_stdin)){
		char recv_buf[HEAD_SIZE] = { 0 };
		recv_tcp(conn_s, recv_buf, HEAD_SIZE);
	}*/

	/*if(fgets(username,39,stdin) == NULL) {
		printf("fgets error\n");
		if(close(conn_s) == -1) {
			printf("Close error \n");
		}
		exit(EXIT_FAILURE);
	}

	username[strlen(username)-1] = '\0';
	send_tcp(conn_s, username, strlen(username));*/

	printf("Welcome to the server, %s\n", username);
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
			if(strcmp(command, "list") == 0) {
				send_tcp(conn_s, command, strlen(command));
				memset(command, 0, sizeof(char)*(strlen(command)+1));
				printf("Files in the current directory : \n");
				for(;;){
					memset(fname, 0, len_filename);
					recv_tcp(conn_s, fname, len_filename);
					if(strstr(fname, "STOP") != NULL){
						strremove(fname, "STOP");
						printf("%s", fname);
						printf("No more files in the directory.\n");
						break;
					}
					printf("%s", fname);
				}
			}

			else if(strcmp(command,"get") == 0) {
				char response[BUFSIZ];
				send_tcp(conn_s, command, strlen(command));
				memset(command, 0, sizeof(char)*(strlen(command)+1));

				int n = recv_tcp(conn_s, response, BUFSIZ);
				if( n < 0 || ( strcmp(response, "ready") != 0 )){
					fprintf(stderr, "Server side error, received %s\n", response);
					exit(EXIT_FAILURE);
				}

				printf("Enter the name of the file you want to receive: ");
				scanf("%s",fname);
				getchar();	// remove newline
				
				n = send_tcp(conn_s, fname, strlen(fname));
				if( n < 0 ){
					perror("Send error...\n");
					exit(EXIT_FAILURE);
				}

				if( RetrieveFile(conn_s, fname, path) < 0 ){
					//fprintf(stderr, "RetrieveFile: error...\n");
				}
				memset(fname, 0, sizeof(char)*(strlen(fname)+1));
				memset(response, 0, BUFSIZ);
			}

			else if(strcmp(command,"put") == 0){
				char bufferFile[BUFSIZ];

				int n = send_tcp(conn_s, command, strlen(command));
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
				
				printf("Enter the name of the file you want to update: ");
				scanf("%s",fname);
				getchar();	// remove newline
				
				n = send_tcp(conn_s, fname, strlen(fname));
				if( n < 0 ){
					perror("Could not send filename...\n");
					exit(EXIT_FAILURE);
				}

				n = recv_tcp(conn_s, server_response, BUFSIZ);
				if( n < 0 || ( strcmp(server_response, "rcvd fn") != 0 )){
					fprintf(stderr, "Server side did not receive filename, response: %s\n", server_response);
					exit(EXIT_FAILURE);
				}

				if (SendFile(conn_s, fname, bufferFile, path) == 0) {
					printf("file transfer completed \n");
				}
				else {
					printf("file transfer error \n");
					char error[] = "ERROR";
					send_tcp(conn_s, error, strlen(error));
				}
				memset(fname, 0, sizeof(char)*(strlen(fname)+1));
				memset(server_response, 0, BUFSIZ);
			}
			
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
	printf("list: per visualizzare i file disponibili al download\n");
	printf("get: per richiedere un determinato file\n");
	printf("put: per caricare sul server un certo file\n");
	printf("help: per visualizzare il menù\n\n");
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