/*

  TCPSERVER.C
  ==========

*/
/*please compile with -pthread */


#include <sys/socket.h>       /*  socket definitions        */
#include <sys/types.h>        /*  socket types              */
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

#define MAX_LINE	1000

struct    sockaddr_in servaddr;  /*  socket address structure  */
struct	  sockaddr_in their_addr;

int gen_id;

int ParseCmdLine(int argc, char *argv[], char **szPort);

void *evadi_richiesta(void *arg) {
    printf("ciao");
	fflush(stdout);
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

    if ( (list_s = socket(AF_INET, SOCK_STREAM, 0)) < 0 ) {
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
		if ( (conn_s = accept(list_s, (struct sockaddr *)&their_addr, &sin_size) ) < 0 ) {
		    fprintf(stderr, "server: errore nella accept.\n");
	    	    exit(EXIT_FAILURE);
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
