#ifndef __RTP_H
#define __RTP_H

#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <cstring>
#include <random>
#include <iostream>
#include <poll.h>
#include <chrono>
#include <map>
#include <set>

#ifdef __cplusplus
extern "C"
{
#endif

#define PAYLOAD_MAX 1461

    // flags in the rtp header
    typedef enum RtpHeaderFlag
    {
        RTP_SYN = 0b0001,
        RTP_ACK = 0b0010,
        RTP_FIN = 0b0100,
        RTP_DAT = 0b0000,
    } rtp_header_flag_t;

    /* 根据文档，简便起见都采用小端法 */
    typedef struct __attribute__((__packed__)) RtpHeader
    {
        uint32_t seq_num;           // Sequence number
        uint16_t length;            // Length of data; 0 for SYN, ACK, and FIN packets
        uint32_t checksum;          // 32-bit CRC
        uint16_t advertised_window; // Receiver's available window size **FLOW CONTROL NOT IMPLEMENTED**
        uint8_t flags;              // See at `RtpHeaderFlag`
    } rtp_header_t;

#ifdef __cplusplus
}
#endif
typedef struct __attribute__((__packed__)) RtpPacket
{
    rtp_header_t header;       // header
    char payload[PAYLOAD_MAX]; // data
} rtp_packet_t;

/* 一个类似TCP功能的类 */
class Rtp
{
private:
    int sockfd;                   // socket file descriptor
    struct sockaddr_in dest_addr; // destination address
    socklen_t addrlen = 0;        // length of dest_addr,连接关闭后记得清零

    double cwnd;           // Congestion window size
    double ssthresh;       // Slow start threshold
    int dup_ack_count;     // Duplicate ACK counter for fast retransmit
    int64_t last_ack_seq;  // Last received ACK seq number
    bool in_fast_recovery; // Flag for fast recovery state

    int64_t seq_num;                                          // 下一个功能模块发送/接收的第一个包的序号为这个值+1
    uint32_t seq_base;                                        // base of sequence number
    std::chrono::steady_clock::time_point last_recv_time;     // last time received a packet
    int send_packet(void *buffer);                            // send a packet or header depend on the length
    int recv_packet(void *buffer);                            // receive a packet
    inline int64_t seq32to64(const uint32_t seq);             // get the 64-bit sequence number
    inline uint32_t seq64to32(const int64_t seq);             // get the 32-bit sequence number
    static inline uint32_t inc_seq32(const uint32_t seq_num); // increase the sequence number
    static inline uint32_t dec_seq32(const uint32_t seq_num); // decrease the sequence number
    int waitfor(void *buffer, int flag, int timeout);         // wait for a desired packet
    int send_file_sr(uint32_t packet_num);                    // send a file using SR
    int recv_file_sr();                                       // receive a file using SR
    /* 以下函数是在connect和close写完之后才加的，故在这两个函数中没有使用 */
    int waitfor_ack(int64_t *seqnum, int64_t begin, int64_t end, int timeout); // wait for an ACK
    int waitfor_dat(void *buffer, int64_t begin, int64_t end, int timeout);    // wait for a DAT
    int waitfor_dat(void *buffer, int timeout);
    int waitfor_ack(int64_t *seq_num_p, int timeout);
    std::map<int64_t, RtpPacket *> data_map;                                   // 存放data的map
    bool fin_received;                                                         // 是否收到了FIN包
    int64_t fin_seq;                                                           // 收到的FIN包的seq_num

public:
    Rtp(int sockfd)
        : sockfd(sockfd), cwnd(1.0), 
          ssthresh(1<<16), dup_ack_count(0), last_ack_seq(-1), in_fast_recovery(false) {}
    ~Rtp() {}
    int connect(const struct sockaddr *addr,
                socklen_t addrlen); // connect to a remote host
    int wait_connect();             // listen for incoming connections and accept
    int close();                    // close the connection
    int wait_close();               // wait for the connection to close
    static void packet_wrapper(RtpPacket *pkt, uint32_t seq_num,
                               uint16_t length, uint16_t advertised_window, void *payload); // wrap a packet
    static void header_wrapper(RtpHeader *header,
                               uint32_t seq_num, uint16_t advertised_window, uint8_t flags); // wrap a header
    int send_file(const char *filename);                                                     // send a file
    int recv_file(const char *filename);                                                     // receive a file
};

#endif // __RTP_H
