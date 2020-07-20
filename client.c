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
			printf("Chat chiusa \n");
			if(close(conn_s) == -1) {
				printf("Errore close ! \n");
				exit(-1);
			}
			exit(0);
		}
		else if(strcmp(msg,"end_chat") == 0) {
			printf("La chat è stata chiusa \n");
			Writeline(conn_s, "end_chat", 9);
			if(close(conn_s) == -1) {
				printf("Errore close : %d\n",errno);
				exit(-1);
			}
			exit(0);
		}
		else if(strcmp(msg, "Sei il nuovo admin") == 0) {
			memset(cmd,0,sizeof(char)*(strlen(cmd)+1));
			strcpy(cmd, "create\n");
		}
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
    char      buffer[MAX_LINE];      /*  character buffer          */
    char     *szAddress;             /*  Holds remote IP address   */
    char     *szPort;                /*  Holds remote port         */
    char     *endptr;                /*  for strtol()              */
	struct	  hostent *he;
	char answare[MAX_LINE];
	char  mychat[MAX_LINE];
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
	while(1) {
		printf("inserire il comando da eseguire(create, join)\n");
		if(fgets(buffer,MAX_LINE,stdin) == NULL) {
			printf("Errore fgets\n");
			if(close(conn_s) == -1) {
			printf("Errore close \n");
			exit(-1);
			}
		}
		int i;
		for(i = 0; i < strlen(buffer); i++){
			buffer[i] = tolower(buffer[i]);
		}
		strcpy(cmd,buffer);
		if(strcmp(buffer, "create\n") != 0 && strcmp(buffer, "join\n") != 0) {
			printf("Comando non valido \n");
			continue;
		}
		if(strcmp(buffer, "create\n")== 0) {
			Writeline(conn_s,buffer,strlen(buffer));
			memset(buffer,0,sizeof(char)*(strlen(buffer)+1));
			printf("Ecco le chat già esistenti. Scegliere un nome diverso da questi\n");
			read(conn_s,buffer,MAX_LINE-1);
			printf("%s\n",buffer);
			memset(buffer,0,sizeof(char)*(strlen(buffer)+1));
			printf("Inserisci nome chat : ");
			if(fgets(buffer,MAX_LINE-1,stdin) == NULL) {
				printf("Errore fgets\n");
				if(close(conn_s) == -1) {
				printf("Errore close \n");
				exit(-1);
				}
			}
			memcpy(mychat,buffer,strlen(buffer));
			Writeline(conn_s,buffer,strlen(buffer));
			read(conn_s,buffer,MAX_LINE-1);
			if(strstr(buffer,"Err") != NULL) {
				printf("Errore create,la chat esiste già. Scegliere un'altro nome e riprovare \n");
				if(close(conn_s) == -1) {
					printf("Errore close \n");
					exit(-1);
				}
				exit(0);
			}	
			Writeline(conn_s,username,strlen(username));
			memset(buffer,0,sizeof(char)*(strlen(buffer)+1));
			break;
		}
		if(strcmp(buffer, "join\n") == 0) {
			Writeline(conn_s,buffer,strlen(buffer));
			memset(buffer,0,sizeof(char)*(strlen(buffer)+1));
			read(conn_s,buffer,MAX_LINE-1);
			if(strstr(buffer,"Err") != NULL) {
				printf("Non esistono chat attive.\n");
				close(conn_s);
				exit(0);
			}
			printf("Ecco le chat attualmente attive : \n");
			printf("%s",buffer);	
			printf("Inserire nome chat da joinare : ");
			if(fgets(buffer,MAX_LINE-1,stdin) == NULL){
				printf("Errore fgets\n");
				if(close(conn_s) == -1) {
				printf("Errore close \n");
				exit(-1);
				}
			}
			memcpy(mychat,buffer,strlen(buffer));
			Writeline(conn_s,buffer,strlen(buffer));
			read(conn_s,buffer,MAX_LINE-1);
			if(strstr(buffer,"Err") != NULL) {
				printf("Errore join,la chat desiderata non esiste.Verificare il nome della chat e riprovare\n");
				if(close(conn_s) == -1){
					printf("Errore close : %d\n",errno);
					exit(-1);
				}
				exit(0);
			}
			Writeline(conn_s,username,strlen(username));
			memset(buffer,0,sizeof(char)*(strlen(buffer)+1));
			break;
		}
	}
	/* connessione */
	printf("Benvenuto nella chat %s",mychat);
	pthread_t tid;
	if(pthread_create(&tid,NULL,(void*)ricevi_msg,NULL) != 0) {  
		printf("Errore nella crezione del thread \n");
		exit(-1);
	}
	show_menu();
	do{
		if(fgets(buffer,MAX_LINE-1,stdin) == NULL) {
			printf("Errore fgets\n");
			if(close(conn_s) == -1) {
				printf("Errore close \n");
				exit(-1);
			}
		}
		if(strcmp(buffer, "1\n") == 0) {
			printf("Sei sicuro di voler uscire (si/no)?\n");
			if(fgets(answare, MAX_LINE-1, stdin) == NULL) {
				printf("Errore fgets\n");
				if(close(conn_s) == -1) {
				printf("Errore close \n");
				exit(-1);
				}
			}
			if(strcmp(answare, "si\n") == 0) {
				strcpy(buffer, "quit");
				Writeline(conn_s,buffer,strlen(buffer));
				memset(buffer, 0, sizeof(char)*(strlen(buffer)+1));
				if(close(conn_s) == -1) {
					printf("Errore close : %d\n",errno);
					exit(-1);
				}
				exit(0);
			}
			else {
				Writeline(conn_s,buffer,strlen(buffer));
				memset(buffer, 0, sizeof(char)*(strlen(buffer)+1));
			}
		}
		if(strcmp(buffer,"2\n") == 0) {
			if(strcmp(cmd, "create\n") == 0){
				printf("Sei sicuro di voler chiudere la chat (si/no)?\n");
				if(fgets(answare, MAX_LINE-1, stdin) == NULL) {
					printf("Errore fgets\n");
					if(close(conn_s) == -1) {
						printf("Errore close \n");
						exit(-1);
					}
				}
				if(strcmp(answare, "si\n") == 0) {
					strcpy(buffer, "close");
					Writeline(conn_s,buffer,strlen(buffer));
					memset(buffer, 0, sizeof(char)*(strlen(buffer)+1));
					if(close(conn_s) == -1) {
						printf("Errore close : %d\n",errno);
						exit(-1);
					}
					exit(0);
				}
				else {
					Writeline(conn_s,buffer,strlen(buffer));
					memset(buffer, 0, sizeof(char)*(strlen(buffer)+1));	
				}
			}
			else{
				printf("Non sei il creatore. Quindi non puoi chiudere\n");
				Writeline(conn_s,buffer,strlen(buffer));
				memset(buffer, 0, sizeof(char)*(strlen(buffer)+1));
			}
		}
		if(strcmp(buffer,"help\n") == 0){
			printf("Sei sicuro di voler visualizzare il menù (si/no)?\n");
			if(fgets(answare, MAX_LINE-1, stdin) == NULL) {
				printf("Errore fgets\n");
				if(close(conn_s) == -1) {
					printf("Errore close \n");
					exit(-1);
				}
			}
			if(strcmp(answare, "si\n") == 0) {
				show_menu();
			}
			else{
				Writeline(conn_s,buffer,strlen(buffer));
				memset(buffer, 0, sizeof(char)*(strlen(buffer)+1));
			}
		}
		else {
			Writeline(conn_s, buffer, strlen(buffer));
			memset(buffer, 0, sizeof(char)*(strlen(buffer)+1));
		}
	}while(1);
}

void show_menu() {
	printf("\nMenù : \n\n");
	printf("1 Esci dalla chat\n");
	printf("2 Chiudi la chat\n");
	printf("help per visualizzare il menù\n");
	printf("Oppure manda il tuo messaggio\n\n");
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