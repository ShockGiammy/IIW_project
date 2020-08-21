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
#include <sys/syscall.h>

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

static __thread int sock_descriptor = -1;
thread_list_t* thread_list = NULL;

int gen_id;

int ParseCmdLine(int argc, char *argv[], char **szPort);

void _handler(int sigo) {
	int res = EXIT_SUCCESS;
	if(sock_descriptor == -1){
		if(thread_list != NULL){
			signal_threads(thread_list, sigo);
		}
		free_thread_list(thread_list);
		pthread_exit(&res);
	}
	else if(close_initiator_tcp(sock_descriptor) == -1) {
		fprintf(stderr, "Close error\n");
		res = EXIT_FAILURE;
	}
	sock_descriptor = -1;
	pthread_exit(&res);
}

void *evadi_richiesta(void *socket_desc) {

	char filesName[BUFSIZ];
	char client_request[BUFSIZ];
	char server_response[BUFSIZ];

	char client[BUFSIZ];
	tcp temp;

	struct stat stat_buf;

	int socket = *(int*)socket_desc;
	free((int*)socket_desc);

	init_log("_server_log_");

	sock_descriptor = socket;

	signal(SIGINT, _handler);

	int res = recv_tcp(socket, client_request, BUFSIZ);
	if(res < 0){
		res = 0;
		pthread_exit(&res);
	}

	strncpy(client, client_request, BUFSIZ);
	memset(client_request, 0, BUFSIZ);
	printf("Connection estabilished with %s\n", client);

	do {
		printf("Waiting client request...\n");
		recv_tcp(socket, client_request, BUFSIZ);
		
		if (strcmp(client_request, "list\n") == 0) {
			printf("command LIST entered\n");
			fflush(stdout);
			
			struct dirent *de;

			DIR *dr = opendir("DirectoryFiles");
		    if (dr == NULL) {
	    		char result[] = "Could not open current directory\n";
		    	printf("%s\n", result);
	        	send_tcp(socket, result, strlen(result));
	    	}

			while ((de = readdir(dr)) != NULL){
    	        char string[50];
        	    strcpy(string, de->d_name);
	        	printf("%s\n", string);
			    send_tcp(socket, string, strlen(string));
    		}
			char stop[] = "STOP";
			send_tcp(socket, stop, strlen(stop));
	    	closedir(dr);
	    	printf("file listing completed \n");
		}

		else if (strcmp(client_request, "get\n") == 0) {
			printf("command GET entered\n");
			fflush(stdout);
			memset(filesName, 0, BUFSIZ);
			
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

void wake_up(int signum) {
	if (signum == SIGUSR1)
    {
        printf("process %d is awake\n", getpid());
    }
}

int process_manager(int list_s) {
	int sin_size;
	int conn_s;
	pthread_t tid;
	int *conn = malloc(sizeof(int));

	signal(SIGUSR1, wake_up);

	printf("process %d is waiting to accept connection\n", getpid());
	
	while(1) {
		pause();

		/*  Wait for a connection, then accept() it  */
		sin_size = sizeof(struct sockaddr_in);
		if ( (conn_s = accept_tcp(list_s, (struct sockaddr *)&their_addr, &sin_size) ) < 0 ) {
		    fprintf(stderr, "server: errore nella accept.\n");
	   		exit(-1);
		}
		*conn = conn_s;
		if(pthread_create(&tid,NULL,(void*)evadi_richiesta,(void*)conn) != 0) {
			printf("Errore server : impossibile creare il thread \n");
			exit(-1);
		}
		insert_thread_in_list(tid, &thread_list);
	}
}

int main(int argc, char *argv[]) {
	
	char     *endptr;                /*  for strtol()              */   
    short int port;                  /*  port number               */
	int       list_s;                /*  listening socket          */
	/*  Get command line arguments  */
	pid_t pids[PROCESSES];
	
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

	for (int i = 0; i < PROCESSES; i++) {
		int pid;
		pid = fork();
		if (pid == -1) {
			printf("Cannot fork!\n");
		}
		else if (pid == 0) {
			process_manager(list_s);
		}
		else {
			pids[i] = pid;
		}
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

	fd_set rset;
	int process_to_wake_up = 0;
	int selector = 0;
    while ( 1 ) {

		FD_ZERO(&rset); /* inizializza a 0 il set dei descrittori in lettura */
		FD_SET(list_s, &rset); /* inserisce il descrittore del socket */

		printf("non togliete questo print\n");

		if (selector == 0) {

			if (select(list_s + 1, &rset, NULL, NULL, NULL) < 0 ) { /* attende descrittore pronto in lettura */
				perror("errore in select");
				exit(1);
			}
			//printf("new connection\n");
			/* Se è arrivata una richiesta di connessione, il socket di ascolto
			è leggibile: viene invocata accept() e creato un socket di connessione */
			if (FD_ISSET(list_s, &rset)) {
				kill(pids[process_to_wake_up], SIGUSR1);
				//printf("signal sended\n");
			}
			process_to_wake_up = (process_to_wake_up+1)%10;
		}
		selector = (selector+1)%3;
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