#include "rtp.h"
#include "util.h"
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <cstring>
#include <random>
#include <iostream>
#include <poll.h>
#include <malloc.h>
#include <chrono>
#include <fstream>
#include <queue>
#include <set>
using namespace std;

/* seq_num相关helper function */

inline int64_t Rtp::seq32to64(const uint32_t seq)
{
    return seq < this->seq_base ? seq + (1 << 30) : seq;
}
inline uint32_t Rtp::seq64to32(const int64_t seq)
{
    return seq % (1 << 30);
}
inline uint32_t Rtp::inc_seq32(const uint32_t seq_num)
{
    return (seq_num + 1) % (1 << 30);
}
inline uint32_t Rtp::dec_seq32(const uint32_t seq_num)
{
    if (seq_num == 0)
        return (1 << 30) - 1;
    else
    {
        return (seq_num - 1) % (1 << 30);
    }
}

/* 接受一个RtpPacket或者RtpHeader并发送，取决于length字段
 * 仅在发送完整的情况下返回发送的包大小表示发送成功，
 * -1表示sendto失败或发送不完整 */
int Rtp::send_packet(void *buffer)
{
    RtpPacket *pkt = (RtpPacket *)buffer;
    if (pkt == nullptr)
    {
        return -1;
    }
    int ret;
#ifdef LDEBUG
    // 添加测试代码，模拟丢包，丢包率为loss_rate%
    random_device rd;
    mt19937 gen(rd());
    uniform_int_distribution<> dist(0, 99); // [0~99]
    int loss_rate = 0;
    if (dist(gen) < loss_rate)
    {
        LOG_DEBUG("send_packet simulated packet loss, seqnum: %u, type: %s%s%s%s\n",
                  pkt->header.seq_num,
                  pkt->header.flags & RTP_SYN ? "SYN" : "",
                  pkt->header.flags & RTP_ACK ? "ACK" : "",
                  pkt->header.flags & RTP_FIN ? "FIN" : "",
                  pkt->header.flags == RTP_DAT ? "DAT" : "");
        return pkt->header.length + sizeof(RtpHeader);
    }
#endif
    ret = sendto(sockfd, pkt,
                 pkt->header.length + sizeof(RtpHeader),
                 0, (struct sockaddr *)&dest_addr, addrlen);
    if (ret == -1)
    {
        LOG_DEBUG("sendto() failed\n");

        return -1; // sendto错误
    }
    else if (ret != (int)(pkt->header.length + sizeof(RtpHeader)))
    {
        LOG_DEBUG("sendto() sent %d bytes sending %s\n ",
                  ret, pkt->header.length > 0 ? "RtpPacket" : "Rtpheader");
        return -1; // 发送不完整
    }
    else
    {
        LOG_DEBUG("send_packet Sent %s with seq_num %u\n",
                  pkt->header.length > 0 ? "RtpPacket" : "Rtpheader", pkt->header.seq_num);
        return ret; // success
    }
}
/* 接受一个至少为sizeof(RtpPacket)大小的buffer，
 * **非阻塞**
 * 没收到包/CRC错误/大小不正确/不是来自目标主机返回0，
 * recvfrom错误/buffer为nullptr返回-1，
 * 成功接受完整的包且CRC校验通过时返回包大小
 * 第一次收到RTP_SYN的正确报文会记录Rtp类的dest_addr和addrlen */
int Rtp::recv_packet(void *buffer)
{
    if (buffer == nullptr)
    {
        return -1;
    }
    int ret;
    struct sockaddr_in dest_addr;
    socklen_t addrlen;
    ret = recvfrom(sockfd, buffer, sizeof(RtpPacket),
                   MSG_DONTWAIT, (struct sockaddr *)&dest_addr, &addrlen); // 非阻塞
    if (ret == -1)
    {
        LOG_DEBUG("recvfrom() failed\n");
        return -1; // recvfrom错误
    }
    else if ((uint32_t)ret >= sizeof(RtpHeader) && (uint32_t)ret <= sizeof(RtpPacket))
    {
        RtpPacket *pkt = (RtpPacket *)buffer;
        uint32_t checksum = pkt->header.checksum;
        pkt->header.checksum = 0; // 先清零再计算checksum
        if (pkt->header.length > PAYLOAD_MAX || compute_checksum(pkt, pkt->header.length + sizeof(RtpHeader)) != checksum)
        {
            LOG_DEBUG("recv_packet Received %s with seq_num %u, checksum error\n",
                      ret == sizeof(RtpPacket) ? "RtpPacket" : "RtpHeader",
                      pkt->header.seq_num);
            return 0; // checksum 错误
        }
        LOG_DEBUG("recv_packet successfully Received %s %s%s%s%s with seq_num %u\n",
                  pkt->header.length > 0 ? "RtpPacket" : "RtpHeader",
                  pkt->header.flags & RTP_SYN ? "SYN" : "",
                  pkt->header.flags & RTP_ACK ? "ACK" : "",
                  pkt->header.flags & RTP_FIN ? "FIN" : "",
                  pkt->header.flags == RTP_DAT ? "DAT" : "",
                  pkt->header.seq_num);
        pkt->header.checksum = checksum;
        if (this->addrlen != 0) // 已经有连接，需要检查是否来自对方
        {
            if (dest_addr.sin_addr.s_addr != this->dest_addr.sin_addr.s_addr ||
                dest_addr.sin_port != this->dest_addr.sin_port)
            {
                char ip_str[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &(this->dest_addr.sin_addr), ip_str, INET_ADDRSTRLEN);
                LOG_DEBUG("recv_packet Received %s , from %s %d, not from dest_addr\n",
                          ret == sizeof(RtpPacket) ? "RtpPacket" : "RtpHeader",
                          ip_str, ntohs(this->dest_addr.sin_port));
                return 0; // 不是来自目标主机
            }
            // 如果是fin，记录一下，方便收方知晓数据传输完成
            if (pkt->header.flags == RTP_FIN)
            {
                if (this->fin_received == false)
                {
                    LOG_DEBUG("recv_packet Received FIN for the first time with seq_num %u\n", pkt->header.seq_num);
                    this->fin_seq = seq32to64(pkt->header.seq_num);
                    this->fin_received = true;
                }
            }
        }
        if (pkt->header.flags == RTP_SYN && this->addrlen == 0) // 包正确，是SYN包且未记录过addrlen
        {
            this->dest_addr = dest_addr;
            this->addrlen = addrlen;
            char ip_str[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &(this->dest_addr.sin_addr), ip_str, INET_ADDRSTRLEN);
            LOG_DEBUG("recv_packet Recorded dest_addr and addrlen: %s %d\n", ip_str, ntohs(this->dest_addr.sin_port));
        }
        this->last_recv_time = chrono::steady_clock::now(); // 更新最后接收时间
        return ret;                                         // success
    }
    else
    {
        LOG_DEBUG("recvfrom() received %d bytes\n, dissatisfying RtpPacket neither RtpHeader", ret);
        return 0; // 大小错误
    }
}

/* 打包RtpPacket到pkt并计算checksum */
void Rtp::packet_wrapper(RtpPacket *pkt, uint32_t seq_num, uint16_t length, uint16_t advertised_window, void *payload)
{
    memset(pkt, 0, sizeof(RtpPacket));
    pkt->header.seq_num = seq_num;
    pkt->header.length = length;
    pkt->header.checksum = 0;                          // 先清零再计算checksum
    pkt->header.advertised_window = advertised_window; // Set advertised window
    pkt->header.flags = RTP_DAT;
    if (length > 0)
    {
        memcpy(pkt->payload, payload, length);
    }
    pkt->header.checksum = compute_checksum(pkt, pkt->header.length + sizeof(RtpHeader));
}

/* 打包RtpHeader到header并计算checksum */
void Rtp::header_wrapper(RtpHeader *header, uint32_t seq_num, uint16_t advertised_window, uint8_t flags)
{
    memset(header, 0, sizeof(RtpHeader));
    header->seq_num = seq_num;
    header->length = 0;
    header->advertised_window = advertised_window; // Set advertised window
    header->flags = flags;
    header->checksum = 0; // 先清零再计算checksum
    header->checksum = compute_checksum(header, sizeof(RtpHeader));
}

/* 等待一个类型为flag的包，至多等待timeout毫秒，
 * 0表示收到类型正确且完整的包，
 * 1表示超时，
 * -1表示recv_packet或者poll错误 */
int Rtp::waitfor(void *buffer, int flag, int timeout)
{
    if (buffer == nullptr)
    {
        return -1;
    }
    chrono::time_point<chrono::steady_clock> end = chrono::steady_clock::now() + chrono::milliseconds(timeout);
    RtpPacket *pkt = (RtpPacket *)malloc(sizeof(RtpPacket)); // 没收到正确的包时不希望改变buffer
    pollfd fds[1];
    fds[0].fd = sockfd;
    fds[0].events = POLLIN; // 监听可读事件
    int64_t millisec_left;
    while (chrono::steady_clock::now() < end)
    {
        millisec_left = chrono::duration_cast<chrono::milliseconds>(end - chrono::steady_clock::now()).count();
        int poll_ret = poll(fds, 1, millisec_left);
        if (poll_ret > 0 && (fds[0].revents & POLLIN))
        {
            int recv_ret = recv_packet(pkt);
            if (recv_ret == 0)
            {
                continue; // 没收到包/CRC错误/大小不正确，继续等待
            }
            else if (recv_ret == -1)
            {
                free(pkt);
                LOG_DEBUG("waitfor recv_packet() failed\n");
                return -1; // recv_packet错误
            }
            else
            {
                if (pkt->header.flags == flag)
                {
                    memcpy(buffer, pkt, recv_ret); // 根据实际包大小拷贝
                    LOG_DEBUG("waitfor %s%s%s%s Received %s with seq_num %u\n",
                              flag & RTP_SYN ? "SYN" : "",
                              flag & RTP_ACK ? "ACK" : "",
                              flag & RTP_FIN ? "FIN" : "",
                              flag == RTP_DAT ? "DAT" : "",
                              (uint32_t)recv_ret > sizeof(RtpHeader) ? "RtpPacket" : "RtpHeader",
                              pkt->header.seq_num);
                    free(pkt);
                    return 0; // success
                }
            }
        }
        else if (poll_ret == 0)
        {
            free(pkt);
            LOG_DEBUG("waitfor timeout\n");
            return 1; // 超时
        }
        else
        {
            free(pkt);
            LOG_DEBUG("waitfor poll() failed\n");
            return -1; // poll错误
        }
    }
    return 1; // 超时
}

/* 发起连接成功返回0失败返回-1
 * 结束时seq_num为x+1 */
int Rtp::connect(const struct sockaddr *addr, socklen_t addrlen)
{
    this->fin_received = false;
    // 生成随机数
    random_device rd;
    mt19937 gen(rd());
    uniform_int_distribution<> dist(0, (1 << 30) - 1); // [0~2^30-1]
    uint32_t seq_num = dist(gen);                      // x
    this->seq_base = seq_num;
    // 第一次握手，发送SYN
    this->dest_addr = *(struct sockaddr_in *)addr;
    this->addrlen = addrlen;
    RtpHeader send_syn;
    header_wrapper(&send_syn, seq_num, 0, RTP_SYN);
    if (send_packet((void *)&send_syn) == -1)
    {
        LOG_DEBUG("connect send syn failed\n");
        return -1;
    }
    LOG_DEBUG("connect Sent SYN with seq_num %u\n", seq_num);
    // 第二次握手，接受SYN&ACK
    RtpHeader *recv_ack = (RtpHeader *)malloc(sizeof(RtpPacket)); // recv_packet要求预留sizeof(RtpPacket)
    int max_retry = 50;
    int retry = 0;
    LOG_DEBUG("connect Waiting for SYN&ACK\n");
    while (retry <= max_retry)
    {
        int waitfor_ret = waitfor(recv_ack, RTP_SYN | RTP_ACK, 100);
        if (waitfor_ret == 0) // 收到类型正确且完整的包
        {
            if (recv_ack->seq_num == inc_seq32(seq_num)) // seq_num正确,x+1
            {
                LOG_DEBUG("connect Received SYN&ACK with correct seq_num %u\n", recv_ack->seq_num);
                break;
            }
            else // seq_num错误
            {
                LOG_DEBUG("connect Received SYN&ACK with wrong seq_num %u, ignored\n", recv_ack->seq_num);
                continue;
            }
        }
        else if (waitfor_ret == 1) // 超时，重发SYN
        {
            if (send_packet((void *)&send_syn) == -1)
            {
                LOG_DEBUG("connect resend syn failed\n");
                free(recv_ack);
                return -1;
            }
            LOG_DEBUG("connect Resent SYN with seq_num %u\n", seq_num);
            retry++;
        }
        else // waitfor错误
        {
            free(recv_ack);
            LOG_DEBUG("waitfor() failed in connect\n");
            return -1;
        }
    }
    if (retry > max_retry) // 连接失败
    {
        free(recv_ack);
        LOG_DEBUG("connect() failed after %d retries\n", retry);
        return -1;
    }

    this->seq_base = seq32to64(seq_num); // 记录seq_base
    this->seq_num = seq32to64(seq_num);  // 记录seq_num
    // 更新seq_num
    seq_num = inc_seq32(seq_num); // x+1
    // this->seq_num不增长，发文件的时候第一个包是x+1

    // 第三次握手
    RtpHeader send_ack;
    header_wrapper(&send_ack, seq_num, 0, RTP_ACK);
    if (send_packet((void *)&send_ack) == -1)
    {
        LOG_DEBUG("connect send ack failed\n");
        free(recv_ack);
        return -1;
    }
    LOG_DEBUG("connect Sent ACK with seq_num %u\n", seq_num);
    chrono::time_point<chrono::steady_clock> end =
        chrono::steady_clock::now() + chrono::milliseconds(2000); // 等待两秒，没收到SYN&ACK代表ACK送达
    while (chrono::steady_clock::now() < end)
    {
        int64_t millisec_left =
            chrono::duration_cast<chrono::milliseconds>(end - chrono::steady_clock::now()).count();
        int waitfor_ret = waitfor(recv_ack, RTP_ACK | RTP_SYN, millisec_left);
        if (waitfor_ret == 0) // 收到类型正确且完整的包
        {
            if (recv_ack->seq_num == seq_num) // seq_num正确，即x+1，说明第三次握手没送达
            {
                LOG_DEBUG("connect Received SYN&ACK with correct seq_num %u after ACK sent\n", recv_ack->seq_num);
                if (send_packet((void *)&send_ack) == -1)
                {
                    LOG_DEBUG("connect resend ack failed\n");
                    free(recv_ack);
                    return -1;
                }
                LOG_DEBUG("connect Resent ACK with seq_num %u\n", seq_num);
                continue; // 重新等待
            }
            else // seq_num错误，无视掉
            {
                LOG_DEBUG("connect Received ACK with wrong seq_num %u, ignored\n", recv_ack->seq_num);
                continue;
            }
        }
        else if (waitfor_ret == -1) // waitfor错误
        {
            free(recv_ack);
            LOG_DEBUG("waitfor() failed in connect\n");
            return -1;
        }
    }
    // 超时说明没再收到SYN&ACK，连接成功
    LOG_DEBUG("connect() success\n");
    free(recv_ack);
    return 0;
}

/* 等待发送方发起连接，
 * 成功返回0失败返回-1
 * 结束时seq_num为x+1 */
int Rtp::wait_connect()
{
    this->fin_received = false;
    // 第一次握手，等待SYN
    chrono::time_point<chrono::steady_clock> end =
        chrono::steady_clock::now() + chrono::milliseconds(5000); // 等待五秒
    RtpHeader *recv_syn = (RtpHeader *)malloc(sizeof(RtpPacket)); // recv_packet要求预留sizeof(RtpPacket)
    LOG_DEBUG("wait_connect Waiting for SYN\n");
    bool syn_received = false;
    while (chrono::steady_clock::now() < end)
    {
        int64_t millisec_left =
            chrono::duration_cast<chrono::milliseconds>(end - chrono::steady_clock::now()).count();
        int waitfor_ret = waitfor(recv_syn, RTP_SYN, millisec_left);
        if (waitfor_ret == 0) // 收到类型正确且完整的包
        {
            LOG_DEBUG("wait_connect Received SYN with seq_num %u\n", recv_syn->seq_num);
            syn_received = true;
            break;
        }
        else if (waitfor_ret == -1) // waitfor错误
        {
            LOG_DEBUG("waitfor() failed in wait_connect\n");
            free(recv_syn);
            return -1;
        }
    }
    if (!syn_received) // 超时连接失败
    {
        LOG_DEBUG("wait_connect() timeout\n");
        free(recv_syn);
        return -1;
    }
    uint32_t seq_num = recv_syn->seq_num; // x
    this->seq_base = seq32to64(seq_num);  // 记录seq_base
    this->seq_num = seq32to64(seq_num);   // 记录seq_num
    seq_num = inc_seq32(seq_num);         // x+1
    // this->seq_num不增长，发文件的时候第一个包是x+1

    free(recv_syn);
    // 第二次握手，发送SYN&ACK
    RtpHeader send_syn_ack;
    header_wrapper(&send_syn_ack, seq_num, 0, RTP_SYN | RTP_ACK);
    if (send_packet((void *)&send_syn_ack) == -1)
    {
        LOG_DEBUG("wait_connect send syn_ack failed\n");
        return -1;
    }
    LOG_DEBUG("wait_connect Sent SYN&ACK with seq_num %u\n", seq_num);
    // 第三次握手，等待ACK
    end = chrono::steady_clock::now() + chrono::milliseconds(5000); // 等待五秒
    RtpHeader *recv_ack = (RtpHeader *)malloc(sizeof(RtpPacket));   // recv_packet要求预留sizeof(RtpPacket)
    LOG_DEBUG("wait_connect Waiting for ACK\n");
    bool connected = false;
    while (chrono::steady_clock::now() < end)
    {
        int waitfor_ret = waitfor(recv_ack, RTP_ACK, 100);
        if (waitfor_ret == 0) // 收到类型正确且完整的包
        {
            if (recv_ack->seq_num == seq_num) // seq_num正确，即x+1
            {
                LOG_DEBUG("wait_connect Received ACK with correct seq_num %u\n", recv_ack->seq_num);
                connected = true;
                break;
            }
            else // seq_num错误
            {
                LOG_DEBUG("wait_connect Received ACK with wrong seq_num %u, ignored\n", recv_ack->seq_num);
                continue;
            }
        }
        else if (waitfor_ret == 1) // 超时，重发SYN&ACK
        {
            if (send_packet((void *)&send_syn_ack) == -1)
            {
                LOG_DEBUG("wait_connect resend syn_ack failed\n");
                free(recv_ack);
                return -1;
            }
            LOG_DEBUG("wait_connect Resent SYN&ACK with seq_num %u\n", seq_num);
        }
        else // waitfor错误
        {
            LOG_DEBUG("waitfor() failed in wait_connect\n");
            free(recv_ack);
            return -1;
        }
    }
    if (!connected) // 连接失败
    {
        LOG_DEBUG("wait_connect() timeout\n");
        free(recv_ack);
        return -1;
    }
    LOG_DEBUG("wait_connect() success\n");
    free(recv_ack);
    return 0;
}

/* 两次挥手，成功返回0，失败返回-1 */
int Rtp::close()
{
    // 第一次挥手，发送FIN
    this->seq_num += 1;
    uint32_t seq_num = seq64to32(this->seq_num);
    RtpHeader send_fin;
    header_wrapper(&send_fin, seq_num, 0, RTP_FIN);
    if (send_packet((void *)&send_fin) == -1)
    {
        LOG_DEBUG("close send fin failed\n");
        return -1;
    }
    LOG_DEBUG("close Sent FIN with seq_num %u\n", seq_num);
    chrono::time_point<chrono::steady_clock> end =
        chrono::steady_clock::now() + chrono::milliseconds(5000);    // 等待五秒
    RtpHeader *recv_finack = (RtpHeader *)malloc(sizeof(RtpPacket)); // recv_packet要求预留sizeof(RtpPacket)
    LOG_DEBUG("close Waiting for fin&ACK\n");
    bool finack_received = false;
    while (chrono::steady_clock::now() < end)
    {
        int waitfor_ret = waitfor(recv_finack, RTP_FIN | RTP_ACK, 100);
        if (waitfor_ret == 0) // 收到类型正确且完整的包
        {
            if (recv_finack->seq_num == seq_num) // seq_num正确
            {
                LOG_DEBUG("close Received FIN&ACK with correct seq_num %u\n", recv_finack->seq_num);
                finack_received = true;
                break;
            }
            else // seq_num错误
            {
                LOG_DEBUG("close Received FIN&ACK with wrong seq_num %u, ignored\n", recv_finack->seq_num);
                continue;
            }
        }
        else if (waitfor_ret == 1) // 超时，重发FIN
        {
            if (send_packet((void *)&send_fin) == -1)
            {
                LOG_DEBUG("close resend fin failed\n");
                free(recv_finack);
                return -1;
            }
            LOG_DEBUG("close Resent FIN with seq_num %u\n", seq_num);
        }
        else // waitfor错误
        {
            LOG_DEBUG("waitfor() failed in close\n");
            free(recv_finack);
            return -1;
        }
    }
    if (!finack_received) // 没收到第二次挥手
    {
        LOG_DEBUG("close() timeout\n");
        free(recv_finack);
        return -1;
    }
    LOG_DEBUG("close() success\n");
    free(recv_finack);
    this->addrlen = 0; // 清零addrlen
    return 0;
}

/* 等待关闭，成功返回0失败返回-1 */
int Rtp::wait_close()
{
    this->seq_num += 1;
    uint32_t seq_num = seq64to32(this->seq_num);
    // 如果已经收到FIN，直接发一个finack
    if (this->fin_received)
    {
        RtpHeader send_fin_ack;
        header_wrapper(&send_fin_ack, seq_num, 0, RTP_FIN | RTP_ACK);
        if (send_packet((void *)&send_fin_ack) == -1)
        {
            LOG_DEBUG("wait_close send fin_ack failed\n");
            return -1;
        }
        LOG_DEBUG("wait_close Sent FIN&ACK with seq_num %u before waiting\n", seq_num);
        return 0;
    }
    // 第一次挥手，等待FIN
    chrono::time_point<chrono::steady_clock> end =
        chrono::steady_clock::now() + chrono::milliseconds(5000); // 等待五秒
    RtpHeader *recv_fin = (RtpHeader *)malloc(sizeof(RtpPacket)); // recv_packet要求预留sizeof(RtpPacket)
    LOG_DEBUG("wait_close Waiting for FIN\n");
    bool fin_received = false;
    while (chrono::steady_clock::now() < end)
    {
        int64_t millisec_left =
            chrono::duration_cast<chrono::milliseconds>(end - chrono::steady_clock::now()).count();
        int waitfor_ret = waitfor(recv_fin, RTP_FIN, millisec_left);
        if (waitfor_ret == 0) // 收到类型正确且完整的包
        {
            if (recv_fin->seq_num == seq64to32(this->seq_num)) // seq_num正确
            {
                LOG_DEBUG("wait_close Received FIN with correct seq_num %u\n", recv_fin->seq_num);
                fin_received = true;
                break;
            }
            else // seq_num错误
            {
                LOG_DEBUG("wait_close Received FIN with wrong seq_num %u, ignored\n", recv_fin->seq_num);
                continue;
            }
        }
        else if (waitfor_ret == -1) // waitfor错误
        {
            LOG_DEBUG("waitfor() failed in wait_close\n");
            free(recv_fin);
            return -1;
        }
    }
    if (!fin_received) // 超时连接失败
    {
        LOG_DEBUG("wait_close() timeout\n");
        free(recv_fin);
        return -1;
    }
    // free(recv_fin);
    // 第二次挥手，发送FIN&ACK
    RtpHeader send_fin_ack;
    header_wrapper(&send_fin_ack, seq_num, 0, RTP_FIN | RTP_ACK);
    if (send_packet((void *)&send_fin_ack) == -1)
    {
        LOG_DEBUG("wait_close send fin_ack failed\n");
        free(recv_fin);
        return -1;
    }
    LOG_DEBUG("wait_close Sent FIN&ACK with seq_num %u\n", seq_num);
    end = chrono::steady_clock::now() + chrono::milliseconds(2000); // 等待两秒，没收到FIN代表FIN&ACK送达
    while (chrono::steady_clock::now() < end)
    {
        int64_t millisec_left =
            chrono::duration_cast<chrono::milliseconds>(end - chrono::steady_clock::now()).count();
        int waitfor_ret = waitfor(recv_fin, RTP_FIN, millisec_left);
        if (waitfor_ret == 0) // 收到类型正确且完整的包
        {
            if (recv_fin->seq_num == seq64to32(this->seq_num)) // seq_num正确
            {
                LOG_DEBUG("wait_close Received FIN with correct seq_num %u after FIN&ACK sent\n", recv_fin->seq_num);
                if (send_packet((void *)&send_fin_ack) == -1)
                {
                    LOG_DEBUG("wait_close resend fin_ack failed\n");
                    free(recv_fin);
                    return -1;
                }
                LOG_DEBUG("wait_close Resent FIN&ACK with seq_num %u\n", seq_num);
                continue; // 重新等待
            }
            else // seq_num错误，无视掉
            {
                LOG_DEBUG("wait_close Received FIN with wrong seq_num %u, ignored\n", recv_fin->seq_num);
                continue;
            }
        }
        else if (waitfor_ret == -1) // waitfor错误
        {
            LOG_DEBUG("wait_close waitfor() failed \n");
            free(recv_fin);
            return -1;
        }
    }
    // 超时说明没再收到FIN，关闭成功
    LOG_DEBUG("wait_close() succeed\n");
    this->addrlen = 0; // 清零addrlen
    free(recv_fin);
    return 0;
}

/* 等待一个ACK，序号在[begin, end)范围内
 * 收到返回0，超时返回1，其他错误返回-1
 * 结果放在seqnum里 */
int Rtp::waitfor_ack(int64_t *seq_num_p, int64_t begin, int64_t end, int timeout)
{
    chrono::time_point<chrono::steady_clock> end_time = chrono::steady_clock::now() + chrono::milliseconds(timeout);
    RtpHeader *recv_ack = (RtpHeader *)malloc(sizeof(RtpPacket)); // recv_packet要求预留sizeof(RtpPacket)
    while (chrono::steady_clock::now() < end_time)
    {
        int millisec_left = chrono::duration_cast<chrono::milliseconds>(end_time - chrono::steady_clock::now()).count();
        int waitfor_ret = waitfor(recv_ack, RTP_ACK, millisec_left);
        if (waitfor_ret == 0) // 收到类型正确且完整的包
        {
            int64_t seq_num = seq32to64(recv_ack->seq_num);
            if (seq_num >= begin && seq_num < end) // seq_num在范围内
            {
                *seq_num_p = seq_num;
                LOG_DEBUG("waitfor_ack Received ACK with seq_num %u in range [%ld,%ld)\n", recv_ack->seq_num, begin, end);
                free(recv_ack);
                return 0; // success
            }
            else // seq_num不在范围内
            {
                LOG_DEBUG("waitfor_ack Received ACK with seq_num %u out of range [%ld,%ld), ignored\n", recv_ack->seq_num, begin, end);
                continue;
            }
        }
        else if (waitfor_ret == 1) // 超时
        {
            LOG_DEBUG("waitfor_ack timeout\n");
            free(recv_ack);
            return 1; // timeout
        }
        else // waitfor错误
        {
            LOG_DEBUG("waitfor_ack waitfor() failed\n");
            free(recv_ack);
            return -1; // waitfor错误
        }
    }
    free(recv_ack);
    LOG_DEBUG("waitfor_ack timeout\n");
    return 0; // timeout
}

/* 等待一个DAT，序号在[begin, end)范围内
 * 收到返回0，超时返回1，其他错误返回-1
 * 结果放在buffer里 */
int Rtp::waitfor_dat(void *buffer, int64_t begin, int64_t end, int timeout)
{
    chrono::time_point<chrono::steady_clock> end_time = chrono::steady_clock::now() + chrono::milliseconds(timeout);
    RtpPacket *recv_pkt = (RtpPacket *)malloc(sizeof(RtpPacket));
    while (chrono::steady_clock::now() < end_time)
    {
        int64_t millisec_left = chrono::duration_cast<chrono::milliseconds>(end_time - chrono::steady_clock::now()).count();
        int waitfor_ret = waitfor(recv_pkt, RTP_DAT, millisec_left);
        if (waitfor_ret == 0) // 收到类型正确且完整的包
        {
            int64_t seq_num = seq32to64(recv_pkt->header.seq_num);
            if (seq_num >= begin && seq_num < end) // seq_num在范围内
            {
                memcpy(buffer, recv_pkt, sizeof(RtpPacket));
                LOG_DEBUG("waitfor_dat Received DAT with seq_num %u in range [%ld,%ld)\n", recv_pkt->header.seq_num, begin, end);
                free(recv_pkt);
                return 0; // success
            }
            else // seq_num不在范围内
            {
                LOG_DEBUG("waitfor_data Received DAT with seq_num %u out of range [%ld,%ld), ignored\n", recv_pkt->header.seq_num, begin, end);
                continue;
            }
        }
        else if (waitfor_ret == 1) // 超时
        {
            LOG_DEBUG("waitfor_data timeout\n");
            free(recv_pkt);
            return 1; // timeout
        }
        else // waitfor错误
        {
            LOG_DEBUG("waitfor_data waitfor() failed\n");
            free(recv_pkt);
            return -1; // waitfor错误
        }
    }
    free(recv_pkt);
    LOG_DEBUG("waitfor_data timeout\n");
    return 0; // timeout
}

// 没有seqnum限制版的
int Rtp::waitfor_ack(int64_t *seq_num_p, int timeout)
{
    RtpHeader *recv_ack = (RtpHeader *)malloc(sizeof(RtpPacket));
    int waitfor_ret = waitfor(recv_ack, RTP_ACK, timeout);
    if (waitfor_ret == 0)
    {
        *seq_num_p = seq32to64(recv_ack->seq_num);
        free(recv_ack);
        return 0;
    }
    free(recv_ack);
    return waitfor_ret; // 1 for timeout, -1 for error
}

int Rtp::waitfor_dat(void *buffer, int timeout)
{
    return waitfor(buffer, RTP_DAT, timeout);
}

/* 接受文件名，发送，sr
 * 成功返回0，超时返回1，失败返回-1
 * 负责读取文件，打包放在data_map里
 * 结束时清空data_map */
int Rtp::send_file(const char *filename)
{
    ifstream file(filename, ios::binary | ios::ate);
    if (!file.is_open())
    {
        LOG_FATAL("send_file() failed to open file\n");
        return -1;
    }
    uint32_t file_size = file.tellg();
    file.seekg(0, ios::beg);
    char *file_buffer = (char *)malloc(file_size);
    if (!file_buffer)
    {
        LOG_FATAL("Failed to allocate buffer for file\n");
        file.close();
        return -1;
    }
    file.read(file_buffer, file_size);
    file.close();
    // 计算文件总包数
    uint32_t total_packets = (file_size + PAYLOAD_MAX - 1) / PAYLOAD_MAX;
    // 对所有文件数据打包
    for (uint32_t i = 0; i < total_packets; i++)
    {
        RtpPacket *pkt = (RtpPacket *)malloc(sizeof(RtpPacket)); // 在data_map被销毁时统一free
        uint16_t length = (i == total_packets - 1 && file_size % PAYLOAD_MAX != 0) ? file_size % PAYLOAD_MAX : PAYLOAD_MAX;
        packet_wrapper(pkt, seq64to32(this->seq_num + 1 + i), length, 0, file_buffer + i * PAYLOAD_MAX);
        this->data_map.insert(pair<int64_t, RtpPacket *>(this->seq_num + 1 + i, pkt));
    }
    free(file_buffer);
    // 发送
    LOG_DEBUG("send_file() using SR with Congestion Control\n");
    int ret = send_file_sr(total_packets);

    // 清空data_map

    for (auto const &[key, val] : this->data_map)
    {
        free(val);
    }
    this->data_map.clear();
    this->seq_num += total_packets; // 加上文件总字节数的包和文件数据包
    return ret;
}

/* 接受文件名，接收，sr
 * 成功返回0，超时返回1，失败返回-1
 * 负责从data_map里读取并写入文件
 * 结束时清空data_map */
int Rtp::recv_file(const char *filename)
{
    LOG_DEBUG("recv_file() using SR\n");
    int ret = recv_file_sr();

    if (ret != 0)
    {
        LOG_DEBUG("recv_file() failed with code %d\n", ret);
        // 清空data_map
        for (auto const &[key, val] : this->data_map)
        {
            free(val);
        }
        this->data_map.clear();
        return ret;
    }
    // 写入文件
    LOG_DEBUG("recv_file() writing to file %s\n", filename);
    ofstream file(filename, ios::binary);
    if (!file.is_open())
    {
        LOG_FATAL("recv_file() failed to open file\n");
        // 清空data_map
        for (auto const &[key, val] : this->data_map)
        {
            free(val);
        }
        this->data_map.clear();
        return -1;
    }

    // 从data_map里读取并写入文件

    for (auto const &[key, val] : this->data_map)
    {
        file.write(val->payload, val->header.length);
    }
    file.close();
    this->seq_num += this->data_map.size();
    // 清空data_map
    for (auto const &[key, val] : this->data_map)
    {
        free(val);
    }
    this->data_map.clear();
    return 0;
}

#include "rtp.h"
#include "util.h"
// ... (您代码的其他部分，从头文件到 recv_file 函数都保持不变) ...

/* 
* 以下是您原始代码中从 send_file_sr() 开始的部分
* 我将直接替换 send_file_sr() 和 recv_file_sr() 的实现
* 并标注出修改。
*/

/* SR方式发送数量为total_packets的包，从data_map里取
 * 成功返回0，超时（5秒没收到任何包）返回1，失败返回-1
 * 
 * === MODIFIED: 本函数现在实现带有累积确认的滑动窗口协议 (类似GBN/TCP) ===
 * ACK为累积确认，确认收到的连续包的最大编号。
 */
int Rtp::send_file_sr(uint32_t total_packets)
{
    if (total_packets == 0)
    {
        LOG_DEBUG("send_file_sr: No packets to send for empty file.\n");
        return 0;
    }

    int64_t base = this->seq_num + 1;
    int64_t next_seq_num = this->seq_num + 1;
    int64_t highest_seq = this->seq_num + total_packets;
    LOG_DEBUG("send_file_sr: Starting to send %u packets from seq %ld to %ld\n", total_packets, base, highest_seq);
    
    // === MODIFIED START: 删除了 unacked_packets map ===
    // 我们不再需要跟踪每个包的发送时间，而是只关心窗口的 base。
    // map<int64_t, chrono::steady_clock::time_point> unacked_packets; 
    chrono::steady_clock::time_point base_send_time; // 计时器只针对base
    // === MODIFIED END ===

    this->last_recv_time = chrono::steady_clock::now();             // Initialize last receive time

    while (base <= highest_seq)
    {
        if (chrono::steady_clock::now() - this->last_recv_time > chrono::seconds(5))
        {
            LOG_FATAL("send_file_sr: Connection timed out (5s no ACK).\n");
            return 1; // Timeout
        }

        // Send new packets if allowed by congestion window
        while (next_seq_num < base + this->cwnd && next_seq_num <= highest_seq)
        {
            auto it = this->data_map.find(next_seq_num);
            if (it != this->data_map.end())
            {
                if (send_packet(it->second) == -1)
                {
                    LOG_DEBUG("send_file_sr: Failed to send packet %ld\n", next_seq_num);
                    return -1;
                }
                
                // === MODIFIED START: 更新base的发送时间 ===
                if (next_seq_num == base) {
                    base_send_time = chrono::steady_clock::now();
                }
                // === MODIFIED END ===

                LOG_DEBUG("send_file_sr: Sent packet %ld. cwnd=%.1f, ssthresh=%.1f\n", next_seq_num, cwnd, ssthresh);
                next_seq_num++;
            }
        }
        LOG_DEBUG("send_file_sr: Window [%ld, %ld), cwnd=%.1f, ssthresh=%.1f\n", base, next_seq_num, cwnd, ssthresh);

        // === MODIFIED START: 超时重传逻辑修改为只检查 base ===
        // Check for retransmission due to timeout for the base of the window
        if (chrono::steady_clock::now() - base_send_time > chrono::milliseconds(200)) { // 200ms RTO
            auto it = this->data_map.find(base);
            if (it != this->data_map.end()) {
                if (send_packet(it->second) == -1) {
                    return -1;
                }
                LOG_DEBUG("send_file_sr: TIMEOUT retransmit packet %ld (base of window)\n", base);
                base_send_time = chrono::steady_clock::now(); // Reset timer for the base

                // TCP Reno-style timeout reaction
                ssthresh = max(cwnd / 2.0, 2.0);
                cwnd = 1.0;
                dup_ack_count = 0;
                in_fast_recovery = false;
                LOG_DEBUG("send_file_sr: Timeout event. ssthresh=%.1f, cwnd=%.1f\n", ssthresh, cwnd);
            }
        }
        // === MODIFIED END ===


        // Wait for ACKs
        RtpHeader ack_header;
        int wait_ret = waitfor(&ack_header, RTP_ACK, 50); // Wait for 50ms

        if (wait_ret == 0)
        { // Received an ACK
            this->last_recv_time = chrono::steady_clock::now();
            int64_t ack_seq = seq32to64(ack_header.seq_num);

            // === MODIFIED START: ACK处理逻辑完全重写为累积确认 ===
            
            // ack_seq 是接收方已经收到的连续包的最大序号
            // 所以我们期望的下一个包是 ack_seq + 1
            if (ack_seq + 1 > base) { 
                // 这是个新的有效ACK，可以滑动窗口
                LOG_DEBUG("send_file_sr: Received new cumulative ACK for %ld. Window base was %ld\n", ack_seq, base);
                base = ack_seq + 1; // 滑动窗口
                last_ack_seq = ack_seq;

                if (base < next_seq_num) {
                    // 如果窗口中还有未确认的包，重置base的计时器
                    base_send_time = chrono::steady_clock::now();
                }

                if (in_fast_recovery) {
                    // 收到新ACK，退出快速恢复
                    cwnd = ssthresh;
                    in_fast_recovery = false;
                    dup_ack_count = 0;
                    LOG_DEBUG("send_file_sr: Exiting Fast Recovery. cwnd set to ssthresh %.1f\n", cwnd);
                } else {
                    // 正常拥塞控制
                    if (cwnd < ssthresh) {
                        // Slow Start: exponential growth
                        cwnd += 1.0;
                        LOG_DEBUG("send_file_sr: Slow Start, cwnd increased to %.1f\n", cwnd);
                    } else {
                        // Congestion Avoidance: linear growth
                        cwnd += (1.0 / cwnd);
                        LOG_DEBUG("send_file_sr: Congestion Avoidance, cwnd increased to %.1f\n", cwnd);
                    }
                }
                dup_ack_count = 0; // 重置重复ACK计数

            } else if (ack_seq + 1 == base) { 
                // 这是个重复的ACK (ack_seq == last_ack_seq)
                if (!in_fast_recovery) {
                    dup_ack_count++;
                }
                LOG_DEBUG("send_file_sr: Received duplicate ACK for %ld (count=%d)\n", ack_seq, dup_ack_count);

                if (dup_ack_count == 3) {
                    // 触发快速重传
                    LOG_DEBUG("send_file_sr: 3 duplicate ACKs for %ld. Triggering Fast Retransmit for %ld.\n", ack_seq, base);
                    auto it = this->data_map.find(base); // 重传 base, 因为 base 是期望收到的包
                    if (it != this->data_map.end()) {
                        send_packet(it->second); // Retransmit missing packet
                        base_send_time = chrono::steady_clock::now();

                        // 进入快速恢复 (Fast Recovery)
                        in_fast_recovery = true;
                        ssthresh = max(cwnd / 2.0, 2.0);
                        cwnd = ssthresh + 3; // 窗口膨胀
                        LOG_DEBUG("send_file_sr: Entering Fast Recovery. ssthresh=%.1f, cwnd=%.1f\n", ssthresh, cwnd);
                    }
                } else if (in_fast_recovery) {
                    // 在快速恢复状态下，每个重复ACK表示一个包离开了网络
                    cwnd += 1.0;
                    LOG_DEBUG("send_file_sr: In Fast Recovery, inflating cwnd to %.1f\n", cwnd);
                }
            } else {
                 // ack_seq + 1 < base, 这是一个过时的ACK，忽略
                 LOG_DEBUG("send_file_sr: Received old cumulative ACK for %ld, ignoring.\n", ack_seq);
            }
            // === MODIFIED END ===
            
            LOG_DEBUG("send_file_sr: Window base is now %ld\n", base);
        }
    }
    LOG_DEBUG("send_file_sr() success\n");
    return 0;
}

/* SR方式接收包，放到data_map里
 * 成功（收到fin）返回0，超时（5秒没收到任何包）返回1，失败返回-1
 * 
 * === MODIFIED: 本函数现在实现累积确认的接收方逻辑 ===
 * 只ACK连续收到的最大序号的包。
 */
int Rtp::recv_file_sr()
{
    int64_t recv_base = this->seq_num + 1; // 这是我们期望收到的下一个包的序号
    
    // === MODIFIED START: 不再需要 set 来跟踪乱序包 ===
    // data_map 本身就起到了缓存乱序包的作用
    // set<int64_t> received_but_not_contiguous;
    // === MODIFIED END ===

    this->last_recv_time = chrono::steady_clock::now(); // Initialize

    while (true)
    {
        if (chrono::steady_clock::now() - this->last_recv_time > chrono::seconds(10))
        {
            if (this->fin_received && this->fin_seq > recv_base)
            {
                LOG_DEBUG("recv_file_sr: FIN received and processed. Exiting successfully.\n");
                break;
            }
            LOG_FATAL("recv_file_sr: Connection timed out (10s no data).\n");
            return 1;
        }

        if (this->fin_received && recv_base >= this->fin_seq)
        {
            LOG_DEBUG("recv_file_sr: All packets before FIN (seq %ld) have been received.\n", this->fin_seq);
            break;
        }

        RtpPacket *recv_pkt = (RtpPacket *)malloc(sizeof(RtpPacket));
        if (!recv_pkt)
            return -1;

        int ret = waitfor_dat(recv_pkt, 200);

        if (ret == 0) // Received a data packet
        {
            this->last_recv_time = chrono::steady_clock::now();
            int64_t pkt_seq = seq32to64(recv_pkt->header.seq_num);
            LOG_DEBUG("recv_file_sr: Received DAT with seq %ld. Expecting base %ld.\n", pkt_seq, recv_base);

            // === MODIFIED START: 接收和ACK逻辑完全重写 ===

            // 如果收到的包是期望的或未来的包，并且还没有被存储过，则存起来
            if (pkt_seq >= recv_base && this->data_map.find(pkt_seq) == this->data_map.end())
            {
                RtpPacket *stored_pkt = (RtpPacket *)malloc(sizeof(RtpPacket));
                memcpy(stored_pkt, recv_pkt, sizeof(RtpPacket));
                this->data_map.insert({pkt_seq, stored_pkt});
                LOG_DEBUG("recv_file_sr: Packet %ld buffered.\n", pkt_seq);
            }

            // 检查是否可以滑动窗口
            // 如果收到了期望的包，就向前移动recv_base
            while (this->data_map.count(recv_base))
            {
                recv_base++;
            }
            LOG_DEBUG("recv_file_sr: Next expected packet is now %ld.\n", recv_base);
            
            // 发送累积ACK
            // ACK的序号是 recv_base - 1, 表示这个序号以及之前的所有包都已收到
            RtpHeader ack_pkt;
            uint16_t available_window = UINT16_MAX;
            uint32_t ack_seq_32 = seq64to32(recv_base - 1);
            header_wrapper(&ack_pkt, ack_seq_32, available_window, RTP_ACK);
            if (send_packet(&ack_pkt) == -1)
            {
                LOG_FATAL("recv_file_sr() failed to send ACK\n");
                free(recv_pkt);
                return -1;
            }
            LOG_DEBUG("recv_file_sr: Sent cumulative ACK for %ld. (i.e., expecting %ld)\n", recv_base - 1, recv_base);

            // === MODIFIED END ===
        }
        free(recv_pkt);
    }
    LOG_DEBUG("recv_file_sr() success\n");
    return 0;
}
