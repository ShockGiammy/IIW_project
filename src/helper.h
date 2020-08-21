/*

  HELPER.H
  ========

*/
#include <stdbool.h>
#include <sys/types.h>
#include <stdlib.h>
#include <unistd.h>             /*  for ssize_t data type  */
#include <math.h>
#include <sys/time.h>
#include <pthread.h>

#define BUFSIZE	  159000
#define LOG_MSG_SIZE 1024

typedef struct thread_list {
  pthread_t tid;
  struct thread_list* next;
} thread_list_t;

/*  Function declarations  */

int SendFile(int socket_desc, char* file_name, char *server_response);
int RetrieveFile(int socket_desc, char* fname);
int create_log_file(char *file_name);
int print_on_log(int log_fd, char *msg);
void signal_threads(thread_list_t* list_head, int sigo);
int insert_thread_in_list(int tid, thread_list_t** head);
void free_thread_list(thread_list_t* head);
