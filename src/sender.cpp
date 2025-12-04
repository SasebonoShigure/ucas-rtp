#include "rtp.h"
#include "util.h"
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <fstream>

/* sender */
void sender_routine(char **argv)
{
    char *receiver_ip = argv[1];
    int port = atoi(argv[2]);
    char *file_path = argv[3];
    int sockfd;
    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
    {
        LOG_FATAL("socket() failed\n");
        return;
    }
    struct sockaddr_in receiver_addr;
    receiver_addr.sin_family = AF_INET;
    receiver_addr.sin_port = htons(port);
    receiver_addr.sin_addr.s_addr = inet_addr(receiver_ip);
    Rtp rtp(sockfd);
    if (rtp.connect((struct sockaddr *)&receiver_addr, sizeof(receiver_addr))==-1)
    {
        close(sockfd);
        LOG_FATAL("sender_routine connect failed\n");
        return;
    }
    if (rtp.send_file(file_path) != 0)
    {
        close(sockfd);
        LOG_FATAL("sender_routine send_file failed\n");
        return;
    }
    if (rtp.close() == -1)
    {
        close(sockfd);
        LOG_DEBUG("sender_routine close failed\n"); // 不影响
        return;
    }
    close(sockfd);
}

int main(int argc, char **argv)
{
    if (argc != 4)
    {
        LOG_FATAL("Usage: ./sender [receiver ip] [receiver port] [file path]\n");
    }

    // your code here
    sender_routine(argv);

    LOG_DEBUG("Sender: exiting...\n");
    return 0;
}
