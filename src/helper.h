/*

  HELPER.H
  ========

*/
#include <stdbool.h>
#include <sys/types.h>
#include <stdlib.h>

#define LISTENQ        (1024)   /*  Backlog for listen()   */
#define MSS             1500    // we define the MSS for the TCP segment as a constant value
#define MAX_WIN         9000

//this struct will be used to send / recive datas and implement the TCP reliable transimssion protocol at level 5

typedef struct tcp_segment
{
  unsigned int sequence_number;
  unsigned int ack_number;
  //int header_length;
  unsigned int reciver_window;
  //int checksum;
  char data[MSS];
  //char cwr;
  bool syn;
  bool fin;
  bool ack;
}tcp;

/* to have a more precise implementation of TCP we will talk about bytes, and not segments 
(even if we divide the bytes into chunks, and so into segments)*/

typedef struct sliding_window {
  int on_the_fly; // the number of bytes actually on the fly
  int n_seg; // keeps the number of segments that can be sent
  int next_to_ack; //the next byte we except to be acked from the receiver
  int next_seq_num; //the next byte we are going to send as soon as possible
  int max_size; // the maximum number of bytes that can be on the fly at the same time
  int first; // the left limit of the win
  int last; // the right limit of the win
  int acked;
}slid_win;




#ifndef PG_SOCK_HELP
#define PG_SOCK_HELP


#include <unistd.h>             /*  for ssize_t data type  */

/*  Function declarations  */

ssize_t Readline(int fd, void *vptr, size_t maxlen);
ssize_t Writeline(int fc, const void *vptr, size_t maxlen);
int SendFile(int socket_desc, char* file_name, char *server_response);
int RetrieveFile(int socket_desc, char* fname);
void make_seg(tcp segment, char *send_segm);
void extract_segment(tcp *segment, char *recv_segm);
void fill_struct(tcp *segment, int seq_num, int ack_num, int recv, bool is_ack, bool is_fin, bool is_syn, char *data);
void concat_segm(char *segm, char *to_concat, int max);
int copy_with_ret(char * dest, char *src, int max);
int count_acked (int min, int acked);

#endif  /*  PG_SOCK_HELP  */

