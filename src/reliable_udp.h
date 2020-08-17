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



#define LISTENQ          (1024)   /*  Backlog for listen()   */
#define MSS               1500    // we define the MSS for the TCP segment as a constant value
#define HEAD_SIZE         19
#define SOCKET_TYPE       SOCK_DGRAM
#define MAX_WIN           MSS * 100
#define MAX_BUF_SIZE      MAX_WIN / MSS
#define MAX_LINE          4096
#define MAX_LINE_DECOR    30
#define MAX_ATTMPTS_RETX  10
#define RECV_TIMEOUT_SEC  1 << 11
#define RECV_TIMEOUT_SHORT_USEC 1 << 19
#define CHECK_BIT(var,pos) ((var) & (1<<(pos)))

//this struct will be used to send / recive datas and implement the TCP reliable transimssion protocol at level 5

typedef struct tcp_segment
{
  unsigned int sequence_number;
  unsigned int ack_number;
  unsigned int data_length;
  unsigned int receiver_window;
  //int checksum;
  char data[MSS];
  //char cwr;
  bool syn;
  bool fin;
  bool ack;

  //this field is usefull to keep the segments in a linked list
  struct tcp_segment *next;
} tcp;

typedef struct congestion_struct
{
  int cong_win;
  int threshold;
  int state;      // 0 = slow_start
                  // 1 = congestion_avoidance
                  // 2 = fast_recovery
} cong_struct;

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
  //int congWin;
  int rcvwnd;
  int last_byte_buffered;
  int bytes_acked_current_transmission;
} slid_win;

typedef struct tcp_timeout_struct {
  struct timeval time; // struct that keeps the sec and microsec that have to be wait
  struct timeval est_rtt; // the avg rtt mesured as TCP standard requires
  struct timeval dev_rtt; // the avg deviance mesured as TCP standard requres
} time_out;

/* flags for send_tcp */
enum flags {
  Ack = 1 << 0,
  Syn = 1 << 1,
  Fin = 1 << 2
};

#ifndef PG_SOCK_HELP
#define PG_SOCK_HELP

/*  Function declarations  */
int connect_tcp(int socket_descriptor, struct sockaddr_in* addr, socklen_t addr_len);
int accept_tcp(int socket_descriptor, struct sockaddr* addr, socklen_t* addr_len);
int recv_tcp(int sockd, void* buf, size_t size);
int send_tcp(int sockd, void* buf, size_t size);
int close_client_tcp(int sockd);
void close_server_tcp(int sockd);
int make_seg(tcp segment, char *send_segm);
int extract_segment(tcp *segment, char *recv_segm);
void fill_struct(tcp *segment, unsigned long seq_num, unsigned long ack_num, unsigned long recv, bool is_ack, bool is_fin, bool is_syn);
void concat_segm(char *segm, char *to_concat, int max);
int count_acked (int min, int max, int acknum);
void retx(tcp *segments, slid_win win, char *buffer, int socket_desc);
void buffer_in_order(tcp **segment_head, tcp *to_buf, slid_win *win, int* bytes_recvd);
int write_all(char** buf, int list_size, tcp **segm_buff, slid_win *win);
void prepare_segment(tcp *segment, slid_win *wind, char *data,  int index, int n_byte, int flags);
void slide_window(slid_win *wind, tcp *recv_segm, tcp *segments);
void ack_segments(char** buf, int recv_sock,  int *list_length, tcp **buf_segm, tcp *ack,  slid_win *recv_win);
int send_unreliable(int sockd, char *segm_to_go, int n_bytes);
void reorder_list(tcp *segment_list, int size);
void free_segms_in_buff(tcp ** head, int n_free);
void estimate_timeout(time_out *timeo, struct timeval first_time, struct timeval last_time);
int calculate_window_dimension();
int congestion_control_receiveAck(slid_win sender_wind);
int congestion_control_caseFastRetrasmission_duplicateAck(slid_win sender_wind);
int congestion_control_duplicateAck(slid_win sender_wind);
int congestion_control_timeout(slid_win sender_wind);
int check_size_buffer(slid_win sender_wind, int receiver_window);

#endif  /*  PG_SOCK_HELP  */