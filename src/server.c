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
#include <dirent.h>
#include <netinet/tcp.h>
#include "reliable_udp.h"
#include "helper.h"           /*  our own helper functions  */
#include "manage_client.h"

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

	int res = recv_tcp(socket, client_request, MAX_LINE-1);
	if(res < 0){
		res = 0;
		pthread_exit(&res);
	}

	strcpy(client, client_request);
	memset(client_request, 0, sizeof(char)*(strlen(client_request)+1));
	printf("Connection estabilished with %s\n", client);

	do {
		recv_tcp(socket, client_request, MAX_LINE-1);
		
		if (strcmp(client_request, "list\n") == 0) {
			printf("command LIST entered\n");
			fflush(stdout);
			
			struct dirent *de;

			DIR *dr = opendir("DirectoryFiles");
		    if (dr == NULL) {
	    		char result[50] = "Could not open current directory\n";
		    	printf("%s\n", result);
	        	send_tcp(socket, result, sizeof(result));
	    	}

			while ((de = readdir(dr)) != NULL){
    	        char string[50];
        	    strcpy(string, de->d_name);
	        	printf("%s\n", string);
			    send_tcp(socket, string, sizeof(string) + 1);
    		}
			char stop[] = "STOP";
			send_tcp(socket, stop, sizeof(stop) +1);
	    	closedir(dr);
	    	printf("file listing completed \n");
		}

		else if (strcmp(client_request, "get\n") == 0) {
			printf("command GET entered\n");
			fflush(stdout);
			
			// tell client that the server is ready
			send_tcp(socket, "ready", 6);
			
			int n = recv_tcp(socket, filesName, 50);
			if( n < 0 ){
				printf("Invalid filename, abort...\n");
				int ret = -1;
				pthread_exit(&ret);
			}
			printf("file name is %s \n", filesName);
			memset(server_response, 0, BUFSIZ);

			if (SendFile(socket, filesName, server_response) == 0) {
				printf("file transfer completed \n");
			}
			else {
				printf("file transfer error \n");
			}
			memset(filesName, 0, BUFSIZ);
			memset(server_response, 0, BUFSIZ);
		}

		else if (strcmp(client_request, "put\n") == 0) {
			char* resp;
			printf("command PUT entered\n");
			fflush(stdout);

			resp = "ready";
			send_tcp(socket, "ready", 6);

			recv_tcp(socket, filesName, 50);
			printf("file name is %s \n", filesName);

			resp = "rcvd fn";
			send_tcp(socket, resp, strlen(resp)+1);

			if(RetrieveFile(socket, filesName) < 0){
				fprintf(stderr, "RetrieveFile: error...\n");
			}
		}
		memset(filesName, 0, sizeof(char)*(strlen(filesName) + 1));
		memset(client_request, 0, BUFSIZ);
		memset(&temp, 0, sizeof(temp));
		memset(server_response, 0, BUFSIZ);
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
    
	printf("Server in ascolto sulla porta %d\n\n\n",port);
	
	/*  Create the listening socket  */

    if ( (list_s = socket(PF_INET, SOCKET_TYPE, IPPROTO_UDP)) < 0 ) {
		fprintf(stderr, "server: errore nella creazione della socket.\n%s\n", strerror(errno));
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

    /*  Enter an infinite loop to respond to client requests  */
    
	/* Il semaforo ha due token : visto l'accesso concorrente di molti client,il secondo token 
	serve per segnalare al thread evadi_richiesta che l'utente è stato aggiunto alla chat corretta */
	gen_id = semget(IPC_PRIVATE,2, IPC_CREAT | 0666);
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
	    	break;
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