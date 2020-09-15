/*
  TCPSERVER.C
  ==========
*/

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
#include <math.h>
#include "reliable_udp.h"
#include "helper.h"           /*  our own helper functions  */
#include "manage_client.h"


char *path = "server_files/";

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

// manage the requests of a single client
void *evadi_richiesta(void *socket_desc) {

	char filesName[BUFSIZ];
	char client_request[BUFSIZ];
	tcp temp;

	struct stat stat_buf;

	int socket = *(int*)socket_desc;
	free((int*)socket_desc);

	#ifdef ACTIVE_LOG
		init_log("_server_log_");
	#endif

	sock_descriptor = socket;

	signal(SIGINT, _handler); // handler sigint

	do {
		printf("Waiting client request...\n");
		recv_tcp(socket, client_request, BUFSIZ);
		
		/*command LIST*/
		if (strcmp(client_request, "list") == 0) {
			printf("command LIST entered\n");
			fflush(stdout);
			
			struct dirent *de;

			DIR *dr = opendir(path);
		    if (dr == NULL) {
	    		char result[] = "Could not open current directory\n";
		    	printf("%s\n", result);
	        	send_tcp(socket, result, strlen(result));
	    	}

			short int tot_size = 0;
			while ((de = readdir(dr)) != NULL) {
				tot_size += de->d_reclen;
			}
			tot_size = ntohs(tot_size);
			send_tcp(socket, &tot_size, sizeof(tot_size));

			int len_filename = 50;
			DIR *new_dr = opendir(path);
			while ((de = readdir(new_dr)) != NULL){
				if(strcmp(de->d_name, ".") != 0 && strcmp(de->d_name, "..") != 0) {
    	        	char string[len_filename];
					memset(string, 0, len_filename);
        	    	strncpy(string, de->d_name, len_filename - 2);
					string[strlen(string)] = '\n';
	        		printf("%s", string);
			    	send_tcp(socket, string, strlen(string));
				}
    		}
	    	closedir(dr);
			closedir(new_dr);
	    	printf("file listing completed\n");
		}

		/*command GET and PUT*/
		else if (strcmp(client_request, "get") == 0 || strcmp(client_request, "put") == 0) {
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
			printf("file name is %s\n", filesName);

			/*command GET*/
			if(strcmp(client_request, "get") == 0) {
				if (SendFile(socket, filesName, path) == 0) {
					printf("file transfer completed\n");
				}
				else {
					printf("file transfer error\n");
				}
			}

			/*command PUT*/
			else if(strcmp(client_request, "put") == 0) {
				char *resp = "rcvd fn";
				send_tcp(socket, resp, strlen(resp)+1);

				if(RetrieveFile(socket, filesName, path) < 0){
					fprintf(stderr, "RetrieveFile: error...\n");
				}
			}
		}

		memset(filesName, 0, sizeof(char)*(strlen(filesName) + 1));
		memset(client_request, 0, BUFSIZ);
		memset(&temp, 0, sizeof(temp));
	} while(1);
}

/* signal used to wake up a child process*/
void wake_up(int signum) {
	if (signum == SIGUSR1)
    {
        printf("process %d is awake\n", getpid());
    }
}

/*function where processes are waiting new connections*/
int process_manager(int list_s) {

	int sin_size;
	int conn_s;
	pthread_t tid;
	int *conn = malloc(sizeof(int));

	/*to manage the arrival of the signal*/
	signal(SIGUSR1, wake_up);

	printf("process %d is waiting to accept connection\n", getpid());
	
	while(1) {

		/*  Wait for a connection, then accept() it  */
		pause();
		sin_size = sizeof(struct sockaddr_in);

		/* If a connection request has arrived, the listening socket
			is readable: accept_tcp() is called and a connection socket is created */
		if ( (conn_s = accept_tcp(list_s, (struct sockaddr *)&their_addr, &sin_size) ) < 0 ) {
		    perror("Server: error while accepting client connection\n");
	   		exit(EXIT_FAILURE);
		}
		*conn = conn_s;

		/*create a new thread to manage the connection*/
		if(pthread_create(&tid,NULL,(void*)evadi_richiesta,(void*)conn) != 0) {
			perror("Server: cannot create thread\n");
			exit(EXIT_FAILURE);
		}
		insert_thread_in_list(tid, &thread_list);
	}
}

int main(int argc, char *argv[]) {
	
	char     *endptr;                /*  for strtol()              */   
    short int port = 7000;           /*  port number, fixed        */
	int       list_s;                /*  listening socket          */
	pid_t pids[PROCESSES];

	/*  Get command line arguments  */
	if(argc < 3) {
		printf("Syntax: ./server loss_probability (xx.xx...) windows_size");
		exit(EXIT_FAILURE);
	}
	check_args(argc, argv, 1);
    
	printf("Server listening on port %d\n\n\n",port);
	
	/*  Create the listening socket  */

    if ( (list_s = socket(PF_INET, SOCKET_TYPE, IPPROTO_UDP)) < 0 ) {
		fprintf(stderr, "Server: creation socket error\n%s\n", strerror(errno));
		exit(EXIT_FAILURE);
    }

	/*pre_forking of the child processes*/
	for (int i = 0; i < PROCESSES; i++) {
		int pid;
		pid = fork();
		if (pid == -1) {
			perror("Cannot fork!\n");
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
		fprintf(stderr, "Server: error during the bind.\n");
		exit(EXIT_FAILURE);
    }

    /*  Enter an infinite loop to respond to client requests  */
    
	/* Il semaforo ha due token : visto l'accesso concorrente di molti client,il secondo token 
	serve per segnalare al thread evadi_richiesta che l'utente è stato aggiunto alla chat corretta */
	gen_id = semget(IPC_PRIVATE,2, IPC_CREAT | 0666);
	if(gen_id == -1) {
		printf("Error semget \n");
		exit(-1);
	}
	if(semctl(gen_id,0,SETVAL,1) == -1) {
		printf("Error semctl \n");
		exit(-1);
	}
	if(semctl(gen_id,1,SETVAL,0) == -1) {
		printf("Error on second token in semctl \n");
		exit(-1);
	}

	fd_set rset;
	int process_to_wake_up = 0;
	int selector = 0;
    while ( 1 ) {

		FD_ZERO(&rset); /* initializes the set of read descriptors to 0 */
		FD_SET(list_s, &rset); /* inserts the socket descriptor */

		if (selector == 0) {

			if (select(list_s + 1, &rset, NULL, NULL, NULL) < 0 ) { /* waits for the descriptor to be ready to read */
				perror("Error on select");
				exit(1);
			}
			/* If a connection request has arrived, signals to a process to wake up and manage that request */
			if (FD_ISSET(list_s, &rset)) {
				kill(pids[process_to_wake_up], SIGUSR1);
			}
			process_to_wake_up = (process_to_wake_up+1)%10;
		}
		selector = (selector+1)%3;
		printf("\n");
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
	    		    printf("    server -p (port) [-h]\n\n");
			    exit(EXIT_SUCCESS);
			}
		++n;
    }
    if (argc==1) {
	printf("Sintassi:\n\n");
    	printf("    server -p (port) [-h]\n\n");
	exit(EXIT_SUCCESS);
    }
    return 0;
}
