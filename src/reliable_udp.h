/*

  RELIABLE_UDP.h
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
#define HEAD_SIZE       19
#define MAX_WIN         9000
#define SOCKET_TYPE     SOCK_STREAM
#define MAX_BUF_SIZE    6

/* Custom FLAGS for send_tcp */
#define SYN 0
#define FIN 1
#define ACK 2

//this struct will be used to send / recive datas and implement the TCP reliable transimssion protocol at level 5

typedef struct tcp_segment
{
  unsigned int sequence_number;
  unsigned int ack_number;
  unsigned int data_length;
  //int header_length;
  unsigned int receiver_window;
  //int checksum;
  char data[MSS];
  //char cwr;
  bool syn;
  bool fin;
  bool ack;

  //this field is usefull to keep the segments in a linked list
  struct tcp_segment *next;
}tcp;

/* to have a more precise implementation of TCP we will talk about bytes, and not segments 
(even if we divide the bytes into chunks, and so into segments)*/

typedef struct sliding_window {
  int on_the_fly; // the number of bytes actually on the fly
  int n_seg; // keeps the number of segments that can be sent
  int next_to_ack; //the left limit of the win
  int next_seq_num; //the next byte we are going to send as soon as possible
  int max_size; // the maximum number of bytes that can be on the fly at the same time
  int last_to_ack; // the right limit of the win
  int tot_acked; // the total byte that have been acked
  int last_correctly_acked; // the last segment correctly acked, usefull for retx in case of loss / 3 dupl. ack
  int dupl_ack; // this field will keep the number of dupicate acks received for a segment
} slid_win;

/* flags for send_tcp */
enum flags {
  Ack = 1 << 0,
  Syn = 1 << 1,
  Fin = 1 << 2
};

#ifndef PG_SOCK_HELP
#define PG_SOCK_HELP

/*  Function declarations  */
int connect_tcp(int socket_descriptor, struct sockaddr* addr, socklen_t addr_len);
int accept_tcp(int socket_descriptor, struct sockaddr* addr, socklen_t* addr_len);
int recv_tcp(int sockd, void* buf, size_t size);
int send_tcp(int sockd, void* buf, size_t size, int flags);
int close_client_tcp(int sockd);
void close_server_tcp(int sockd);
void make_seg(tcp segment, char *send_segm);
void extract_segment(tcp *segment, char *recv_segm);
void fill_struct(tcp *segment, int seq_num, int ack_num, int recv, bool is_ack, bool is_fin, bool is_syn, char *data);
void concat_segm(char *segm, char *to_concat, int max);
int count_acked (int min, int max, int acknum);
void retx(tcp *segments, slid_win win, char *buffer, int socket_desc);
void buffer_in_order(tcp **segment_head, tcp *to_buf, slid_win *win);
int write_all(char** buf, int list_size, tcp **segm_buff, slid_win *win);
void prepare_segment(tcp *segment, slid_win *wind, char *data,  int index, int n_byte, int flags);
void slide_window(slid_win *wind, tcp *recv_segm, tcp *segments);
void ack_segments(char** buf, int recv_sock,  int *list_length, tcp **buf_segm, tcp *ack,  slid_win *recv_win);
void send_unreliable(char *segm_to_go, int sockd);
void reorder_list(tcp *segment_list, int size);
void free_segms_in_buff(tcp ** head, int n_free);


#endif  /*  PG_SOCK_HELP  */