#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>

const int CONNUM = 500;
int counts = 0;
int loopsize = 0;
int main(int argc, char** argv)
{
    if(argc <= 2)
    {
        printf("\nuseage: %s, ip_address port_number\n", argv[0]);
        return 1;
    }
    const char* ip = argv[1];
    int port = atoi(argv[2]);

    sockaddr_in address;
    address.sin_family = AF_INET;
    inet_pton(AF_INET, ip, &address.sin_addr);
    address.sin_port = htons(port);

    int* socks = new int [CONNUM + 1];
    int size = 0;

    while(counts < CONNUM && size < CONNUM)
    {
        ++ loopsize;
        int sockfd = socket(PF_INET, SOCK_STREAM, 0);
        if(sockfd < 0)
        {
            printf("creat socket failed\n");
            exit(1);
        }
        socks[size++] = sockfd;
        if(connect(sockfd, (sockaddr*)&address, sizeof(address)) < 0)
        {
            printf("%d connected failed!\n", sockfd);
            printf("already connected %d clients\n", counts);
        }
        else
        {
            if(counts % 100 == 99 || counts == CONNUM)
            {
                printf("sockfd is %d \n", sockfd);
            }
            ++ counts;
        }
    }
    printf("connection counts is %d\n", counts);
    printf("loopsize is %d\n", loopsize);
    for(int i=0; i<size; ++i)
        close(socks[i]);
    delete [] socks;
    return 0;
}
