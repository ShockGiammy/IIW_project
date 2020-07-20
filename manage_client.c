/* questo file contiene tutte le funzioni utili alla gestione dei clients da parte del server */


#include <sys/socket.h>       /*  socket definitions        */
#include <sys/types.h>        /*  socket types              */
#include <arpa/inet.h>        /*  inet (3) funtions         */
#include <unistd.h>           /*  misc. UNIX functions      */
#include "helper.h"           /*  our own helper functions  */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/ipc.h>
#include <errno.h>
#include <sys/sem.h>
#include "manage_client.h"

#define MAX_LINE (1000)

void get_token(int sem_key,int n_sem,int val){
	struct sembuf _sembuf;
	_sembuf.sem_num = n_sem;
	_sembuf.sem_op = -val;
	_sembuf.sem_flg = 0;
	if(semop(sem_key, &_sembuf,1) == -1)
	{
		printf("Thread : errore semop \n");
		exit(-1);
	}
}

void set_token(int sem_key,int n_sem,int val){
	struct sembuf _sembuf;
	_sembuf.sem_num = n_sem;
	_sembuf.sem_op = val;
	_sembuf.sem_flg = 0;
	if(semop(sem_key, &_sembuf,1) == -1)
	{
		printf("Thread : errore semop \n");
		exit(-1);
	}
}