#include "rtp.h"
#include "util.h"
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

void receiver_routine(char **argv)
{
    int port = atoi(argv[1]);
    char *file_path = argv[2];
    int window_size = atoi(argv[3]);
    int sockfd;
    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
    {
        LOG_FATAL("socket() failed\n");
    }
    struct sockaddr_in receiver_addr;
    receiver_addr.sin_family = AF_INET;
    receiver_addr.sin_addr.s_addr = INADDR_ANY;
    receiver_addr.sin_port = htons(port);
    // /* 为了避免端口在程序关闭后TIME_WAIT而在几分钟内无法重新bind，进行的设置 */
    // int opt = 1;
    // if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
    // {
    //     LOG_FATAL("setsockopt() failed\n");
    //     return;
    // }

    if (bind(sockfd, (struct sockaddr *)&receiver_addr, sizeof(receiver_addr)) < 0)
    {
        LOG_FATAL("bind() failed\n");
        close(sockfd);
    }
    LOG_DEBUG("RTP receiver is listening on port %d...\n", port);
    Rtp rtp(sockfd, window_size);
    if (rtp.wait_connect() == -1)
    {
        close(sockfd);
        LOG_FATAL("receiver_routine wait_connect failed\n");
    }
    LOG_DEBUG("RTP receiver connected\n");
    if (rtp.recv_file(file_path) != 0)
    {
        close(sockfd);
        LOG_FATAL("receiver_routine recv_file failed\n");
    }
    LOG_DEBUG("RTP receiver received file\n");
    if (rtp.wait_close() == -1)
    {
        close(sockfd);
        LOG_FATAL("receiver_routine wait_close failed\n");
    }
    close(sockfd);
    return;
}

int main(int argc, char **argv)
{
    if (argc != 4)
    {
        LOG_FATAL("Usage: ./receiver [listen port] [file path] [window size] \n");
    }
    // your code here
    receiver_routine(argv);

    LOG_DEBUG("Receiver: exiting...\n");
    return 0;
}
