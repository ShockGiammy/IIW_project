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

#define LISTENQ        (1024)   /*  Backlog for listen()   */
#define MSS             1500    // we define the MSS for the TCP segment as a constant value
#define MAX_WIN         9000
#define MAX_BUF_SIZE    6

/*  Function declarations  */

int SendFile(int socket_desc, char* file_name, char *server_response);
int RetrieveFile(int socket_desc, char* fname);