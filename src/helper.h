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

#define BUFSIZE	  160000

/*  Function declarations  */

int SendFile(int socket_desc, char* file_name, char *server_response);
int RetrieveFile(int socket_desc, char* fname);
int create_log_file(char *file_name);
int print_on_log(int log_fd, char *msg);