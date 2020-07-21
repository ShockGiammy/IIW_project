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
int retrieveFile(char* fname);

/*void ricevi_msg() {
	char msg[MAX_LINE-1];
	while(1){
		if(recv(conn_s,msg,MAX_LINE-1,0) <= 0) {
			printf("Disconnecting... \n");
			if(close(conn_s) == -1) {
				printf("Errore close ! \n");
				exit(-1);
			}
			exit(0);
		}
		//printf("%s \n",msg);
	}
}*/

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
	char command[BUFSIZ];
	char fname[BUFSIZ];
	char exitBuffer[10];
	char username[40];
	he=NULL;
	ParseCmdLine(argc, argv, &szAddress, &szPort);
    /*  Set the remote port  */
    port = strtol(szPort, &endptr, 0);
    if ( *endptr ) {
		printf("client: porta non riconosciuta.\n");
		exit(EXIT_FAILURE);
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
    /*  connect() to the remote server  */
    if ( connect(conn_s, (struct sockaddr *) &servaddr, sizeof(servaddr) ) < 0 ) {
		printf("client: errore durante la connect.\n");
		exit(EXIT_FAILURE);
    }

	/*pthread_t tid;
	if(pthread_create(&tid,NULL,(void*)ricevi_msg,NULL) != 0) {  
		printf("Errore nella crezione del thread \n");
		exit(-1);
	}*/

	/* connessione */
	printf("Inserire nome utente :");
	if(fgets(username,39,stdin) == NULL) {
		printf("Errore fgets\n");
		if(close(conn_s) == -1) {
			printf("Errore close \n");
			exit(-1);
		}
	}
	Writeline(conn_s, username, strlen(username));

	printf("Benvenuto nel server %s\n", username);
	show_menu();

	do{
		/*if(recv(conn_s, exitBuffer, 10,0) <= 0) {
			printf("Disconnecting... \n");
			if(close(conn_s) == -1) {
				printf("Errore close ! \n");
				exit(-1);
			}
			exit(0);
		}*/
		if(fgets(command,MAX_LINE-1,stdin) == NULL) {
			printf("Errore fgets\n");
			if(close(conn_s) == -1) {
				printf("Errore close \n");
				exit(-1);
			}
		}
		if(strcmp(command, "list\n") == 0) {
			Writeline(conn_s, command, strlen(command));
			memset(command, 0, sizeof(char)*(strlen(command)+1));
			printf("Files in the current directory : \n");
			for(;;){
				memset(&fname, '\0', 50);
				int temp = recv(conn_s, fname, 50, 0);
				if(strcmp(fname, "STOP") == 0){
					printf("No more files in the directory.\n");
					break;
				}
				printf("%s\n", fname);	
			}
		}

		else if(strcmp(command,"get\n") == 0) {
			Writeline(conn_s, command, strlen(command));
			memset(command, 0, sizeof(char)*(strlen(command)+1));
			printf("Enter the name of the file you want to receive: ");
			scanf("%s",fname);
			getchar();	// remove newline
			send(conn_s, fname, sizeof(fname), 0);

			retrieveFile(fname);
		}

		else if(strcmp(command,"put\n") == 0){
			printf("todo\n");
		}
		
		else if(strcmp(command,"help\n") == 0){
			memset(command, 0, sizeof(char)*(strlen(command)+1));
			show_menu();
		}

		else {
			printf("Command not valid\n");
			memset(command, 0, sizeof(char)*(strlen(command)+1));
		}
	}while(1);
}

int retrieveFile(char* fname) {
	char bufferFile[BUFSIZ];

	int fd = open(fname, O_WRONLY|O_CREAT, S_IRWXU);
	if (fd == -1) {
		printf("error to create file");
		//recv(conn_s, bufferFile, BUFSIZ-1, 0);   //only to consume the socket buffer;
		return -1;
	}

	int n;
	while ((n = recv(conn_s, bufferFile, BUFSIZ-1, 0)) > 0) {
		if (strcmp(bufferFile, "ERROR") == 0) {
			printf("file transfer error \n");
			if (remove(fname) != 0) {
      			printf("Unable to delete the file \n");
				fflush(stdout);
				return -1;
			}
		}
		else {
			bufferFile[n] = '\0';
			write(fd, bufferFile, n);
			if( n < BUFSIZ-2) {
				printf("file receiving completed \n");
				fflush(stdout);
				break;
			}
		}
	}
	//close(conn_s);
	close(fd);
	return 0;		
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