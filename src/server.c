/*
  TCPSERVER.C
  ==========
*/
/*please compile with -pthread */


#include <sys/socket.h>       /*  socket definitions        */
#include <sys/types.h>        /*  socket types              */
#include <sys/stat.h>   	  /*  stat for files transfer   */
#include <arpa/inet.h>        /*  inet (3) funtions         */
#include <unistd.h>           /*  misc. UNIX functions      */

#include "helper.h"           /*  our own helper functions  */
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <errno.h>
#include <sys/sendfile.h>
#include <fcntl.h>
#include "manage_client.h"
#include <dirent.h>

#define MAX_LINE	1000

struct    sockaddr_in servaddr;  /*  socket address structure  */
struct	  sockaddr_in their_addr;

int gen_id;

int ParseCmdLine(int argc, char *argv[], char **szPort);

void *evadi_richiesta(void *socket_desc) {

	char filesName[BUFSIZ];
	char client_request[BUFSIZ];
	char server_response[BUFSIZ];

	char client[MAX_LINE];
	tcp temp;

	struct stat stat_buf;

	int socket = *(int*)socket_desc;
	free((int*)socket_desc);

	Readline(socket, client_request, MAX_LINE-1);
	
	strcpy(client, client_request);
	memset(client_request, 0, sizeof(char)*(strlen(client_request)+1));
	printf("Connection estabilished with %s\n", client);

	do {
		Readline(socket, client_request, MAX_LINE-1);
		if(strcmp(client_request, "") != 0)
			printf("received request %s\n", client_request);
		
		if (strcmp(client_request, "list\n") == 0) {
			printf("command LIST entered\n");
			fflush(stdout);
			
			struct dirent *de;

			DIR *dr = opendir("DirectoryFiles");
		    if (dr == NULL) {
	    		char result[50] = "Could not open current directory\n";
		    	printf("%s\n", result);
	        	send(socket, result, sizeof(result), 0);
	    	}

			while ((de = readdir(dr)) != NULL){
    	        char string[50];
        	    strcpy(string, de->d_name);
	        	printf("%s\n", string);
			    send(socket, string, sizeof(string), 0);
    		}
			char stop[] = "STOP";
			send(socket, stop, sizeof(stop), 0);
	    	closedir(dr);
	    	printf("file listing completed \n");
		}

		else if (strcmp(client_request, "get\n") == 0) {
			printf("command GET entered\n");
			fflush(stdout);
			
			// sets the segment
			fill_struct(&temp, 0,strlen(client_request) + 1, 0, true, false, false, NULL);
			make_seg(temp,server_response);
			send(socket, server_response, strlen(server_response), 0); // sends the ack
			memset(server_response, 0 , sizeof(char)*(strlen(server_response) + 1)); // reset the buffer
			
			recv(socket, filesName, 50,0);
			printf("file name is %s \n", filesName);

			temp.ack_number += strlen(filesName);
			make_seg(temp, server_response);
			send(socket, server_response, strlen(server_response), 0);
			memset(server_response, 0 , sizeof(char)*(strlen(server_response) + 1));

			if (SendFile(socket, filesName, server_response) == 0) {
				printf("file transfer completed \n");
			}
			else {
				printf("file transfer error \n");
				char error[] = "ERROR";
				send(socket, error, sizeof(error), 0);
			}
		}

		else if (strcmp(client_request, "put\n") == 0) {
			printf("command PUT entered\n");
			fflush(stdout);

			fill_struct(&temp, 0, strlen(client_request) + 1, 0, true, false, false, "");
			make_seg(temp,server_response);
			send(socket, server_response, strlen(server_response), 0); // sends the ack
			memset(server_response, 0 , sizeof(char)*(strlen(server_response) + 1)); // reset the buffer

			recv(socket, filesName, 50,0);
			printf("file name is %s \n", filesName);

			temp.ack_number += strlen(filesName);
			make_seg(temp, server_response);
			send(socket, server_response, strlen(server_response), 0);
			memset(server_response, 0 , sizeof(char)*(strlen(server_response) + 1));

			RetrieveFile(socket, filesName);
		}
		else if(strcmp(client_request, "quit\n") == 0){
			int retval;
			printf("Closing connection with client %s", client);
			if(close(socket) < 0){
				printf("...Failure\n");
				retval = EXIT_FAILURE;
				fprintf(stderr, "Error while closing socket\n");
				pthread_exit(&retval);
			}
			printf("...Success\n");
			retval = EXIT_SUCCESS;
			pthread_exit(&retval);
		}
		memset(filesName, 0, sizeof(char)*(strlen(filesName) + 1));
		memset(client_request, 0, sizeof(char)*(strlen(client_request)+1));
		memset(&temp, 0, sizeof(temp));
		memset(server_response, 0, sizeof(char)*(strlen(server_response) + 1));
	} while(1);
}

int main(int argc, char *argv[]) {
	
	pthread_t tid;
	char     *endptr;                /*  for strtol()              */
    int 	  sin_size;   
    short int port;                  /*  port number               */
	int       list_s;                /*  listening socket          */
	/*  Get command line arguments  */
	int conn_s;
	
	ParseCmdLine(argc, argv, &endptr);
	port = strtol(endptr, &endptr, 0);
	if ( *endptr ) { 
	    fprintf(stderr, "server: porta non riconosciuta.\n");
	    exit(EXIT_FAILURE);
	}
    
	printf("Server in ascolto sulla porta %d\n",port);
	
	/*  Create the listening socket  */

    if ( (list_s = socket(AF_INET, SOCKET_TYPE, 0)) < 0 ) {
		fprintf(stderr, "server: errore nella creazione della socket.\n");
		exit(EXIT_FAILURE);
    }


    /*  Set all bytes in socket address structure to
        zero, and fill in the relevant data members   */

    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family      = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port        = htons(port);

    /*  Bind our socket addresss to the 
	listening socket, and call listen()  */

    if ( bind(list_s, (struct sockaddr *) &servaddr, sizeof(servaddr)) < 0 ) {
		fprintf(stderr, "server: errore durante la bind.\n");
		exit(EXIT_FAILURE);
    }

    if ( listen(list_s, LISTENQ) < 0 ) {
		fprintf(stderr, "server: errore durante la listen.\n");
		exit(EXIT_FAILURE);
    }

    
    /*  Enter an infinite loop to respond to client requests  */
    
	/* Il semaforo ha due token : visto l'accesso concorrente di molti client,il secondo token 
	serve per segnalare al thread evadi_richiesta che l'utente è stato aggiunto alla chat corretta */
	gen_id = semget(IPC_PRIVATE,2,IPC_CREAT | 0666);
	if(gen_id == -1) {
		printf("Errore semget \n");
		exit(-1);
	}
	if(semctl(gen_id,0,SETVAL,1) == -1) {
		printf("Errore semctl \n");
		exit(-1);
	}
	if(semctl(gen_id,1,SETVAL,0) == -1) {
		printf("Errore sul secondo token in semctl \n");
		exit(-1);
	}
	//signal(SIGINT,close_handler);
    while ( 1 ) {
		
		int *conn = malloc(sizeof(int));
		/*  Wait for a connection, then accept() it  */
		sin_size = sizeof(struct sockaddr_in);
		if ( (conn_s = accept_tcp(list_s, (struct sockaddr *)&their_addr, &sin_size) ) < 0 ) {
		    fprintf(stderr, "server: errore nella accept.\n");
	    	continue;
		}
		*conn = conn_s;
		if(pthread_create(&tid,NULL,(void*)evadi_richiesta,(void*)conn) != 0) {
			printf("Errore server : impossibile creare il thread \n");
			exit(-1);
		}
	}
}

int ParseCmdLine(int argc, char *argv[], char **szPort){
    int n = 1;

    while ( n < argc ) {
		if ( !strncmp(argv[n], "-p", 2) || !strncmp(argv[n], "-P", 2) )
			*szPort = argv[++n];
		else 
			if ( !strncmp(argv[n], "-h", 2) || !strncmp(argv[n], "-H", 2) ) {
			    printf("Sintassi:\n\n");
	    		    printf("    server -p (porta) [-h]\n\n");
			    exit(EXIT_SUCCESS);
			}
		++n;
    }
    if (argc==1) {
	printf("Sintassi:\n\n");
    	printf("    server -p (porta) [-h]\n\n");
	exit(EXIT_SUCCESS);
    }
    return 0;
}