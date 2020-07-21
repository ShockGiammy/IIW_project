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
#include "helper.h"
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <ctype.h>

#define MAX_LINE  4096
#define STDIN 0

char cmd[10];
long conn_s;                /*  connection socket         */

int ParseCmdLine(int , char **, char **, char **);
void show_menu();

void ricevi_msg() {
	char msg[MAX_LINE-1];
	while(1){
		if(recv(conn_s,msg,MAX_LINE-1,0) <= 0) {
			printf("disconnecting \n");
			if(close(conn_s) == -1) {
				printf("Errore close ! \n");
				exit(-1);
			}
			exit(0);
		}
		/*else if(strcmp(msg,"end_chat") == 0) {
			printf("La chat è stata chiusa \n");
			Writeline(conn_s, "end_chat", 9);
			if(close(conn_s) == -1) {
				printf("Errore close : %d\n",errno);
				exit(-1);
			}
			exit(0);
		}*/
		printf("%s \n",msg);
		memset(msg,0,sizeof(char)*(strlen(msg)+1));
	}
}

void _handler(int sigo) {
	printf("\nChiuso \n");
	Writeline(conn_s, "quit", 5);
	if(close(conn_s) == -1) {
		printf("Errore close \n");
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
	char buffer[MAX_LINE];
	char username[40];
	he=NULL;
	ParseCmdLine(argc, argv, &szAddress, &szPort);
    /*  Set the remote port  */
    port = strtol(szPort, &endptr, 0);
    if ( *endptr ) {
		printf("client: porta non riconosciuta.\n");
		exit(EXIT_FAILURE);
    }

	printf("Inserire nome utente :");
	if(fgets(username,39,stdin) == NULL) {
		printf("Errore fgets\n");
		if(close(conn_s) == -1) {
		printf("Errore close \n");
		exit(-1);
		}
	}

    /*  Create the listening socket  */

    if ((conn_s = socket(AF_INET, SOCK_STREAM, 0)) < 0 ) {
		fprintf(stderr, "client: errore durante la creazione della socket.\n");
		exit(EXIT_FAILURE);
    }

    /*  Set all bytes in socket address structure to
        zero, and fill in the relevant data members   */
	memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family      = AF_INET;
    servaddr.sin_port        = htons(port);

    /*  Set the remote IP address  */

    if ( inet_aton(szAddress, &servaddr.sin_addr) <= 0 ) {
		printf("client: indirizzo IP non valido.\nclient: risoluzione nome...");
		
		if ((he=gethostbyname(szAddress)) == NULL) {
			printf("fallita.\n");
  			exit(EXIT_FAILURE);
		}
		printf("riuscita.\n\n");
		servaddr.sin_addr = *((struct in_addr *)he->h_addr_list);
    }
   	signal(SIGINT,_handler);
    /*  connect() to the remote echo server  */
    if ( connect(conn_s, (struct sockaddr *) &servaddr, sizeof(servaddr) ) < 0 ) {
		printf("client: errore durante la connect.\n");
		exit(EXIT_FAILURE);
    }

	/* connessione */
	pthread_t tid;
	if(pthread_create(&tid,NULL,(void*)ricevi_msg,NULL) != 0) {  
		printf("Errore nella crezione del thread \n");
		exit(-1);
	}
	printf("Benvenuto nel server\n");
	show_menu();
	
	do{
		if(fgets(buffer,MAX_LINE-1,stdin) == NULL) {
			printf("Errore fgets\n");
			if(close(conn_s) == -1) {
				printf("Errore close \n");
				exit(-1);
			}
		}
		if(strcmp(buffer, "list\n") == 0) {
			printf("Ecco la lista\n");
			
		}
		if(strcmp(buffer,"get\n") == 0) {
			printf("todo\n");
			
		}
		if(strcmp(buffer,"put\n") == 0){
			printf("todo\n");
		}
		if(strcmp(buffer,"help\n") == 0){
			show_menu();
		}
		else {
			Writeline(conn_s, buffer, strlen(buffer));
			memset(buffer, 0, sizeof(char)*(strlen(buffer)+1));
		}
	}while(1);
}

void show_menu() {
	printf("\nMenù: \n");
	printf("list: per visualizzare i file disponibili al download\n");
	printf("get _nomeFile_: per richiedere un determinato file\n");
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