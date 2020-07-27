/*

  HELPER.H
  ========

*/
#include <stdbool.h>
#include <sys/types.h>

// this struct will be used to send / recive datas and implement the TCP reliable transimssion protocol at level 5
typedef struct tcp_segment
{
  int sequence_number;
  int ack_number;
  //int header_length;
  int reciver_window;
  //int checksum;
  char data[8192];
  //char cwr;
  bool syn;
  bool fin;
  bool ack;
}tcp;



#ifndef PG_SOCK_HELP
#define PG_SOCK_HELP


#include <unistd.h>             /*  for ssize_t data type  */

#define LISTENQ        (1024)   /*  Backlog for listen()   */


/*  Function declarations  */

ssize_t Readline(int fd, void *vptr, size_t maxlen);
ssize_t Writeline(int fc, const void *vptr, size_t maxlen);
int SendFile(int socket_desc, char* file_name, char *server_response);
int RetrieveFile(int socket_desc, char* fname);
void serialize_struct(char *buffer, tcp segment);
void deserialize_struct(char *buffer, tcp segment);

#endif  /*  PG_SOCK_HELP  */

