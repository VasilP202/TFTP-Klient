#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <net/if.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>

/* TFTP opcodes */
#define RRQ 1
#define WRQ 2
#define DATA 3
#define ACK 4
#define ERROR 5
#define OACK 6
#define OACK_ERROR 8

typedef struct Command{
    int operation;
    char *file;
    int timeout;
    int block_size; 
    char *mode;
    char *address;
    char *port_num;
} Command;


int get_mtu()
{
    /* Get MTU for Blocksize option*/
    struct ifconf ifc;
    struct ifreq *ifr;

    int sock; 
    sock = socket(AF_INET, SOCK_DGRAM, 0);    // Create new socket
    if (sock == -1) {
        fprintf(stderr, "error: opening stream socket");
        return -1;
    }
    char buf[1024];
    ifc.ifc_len = sizeof(buf);
    ifc.ifc_buf = buf;
    if(ioctl(sock, SIOCGIFCONF, &ifc) < 0)
        return -1;

    ifr = ifc.ifc_req;
    int len = ifc.ifc_len / sizeof(struct ifreq);

    struct ifreq *tmp = &ifr[0];
    ioctl(sock, SIOCGIFMTU, tmp);
    int min_mtu = tmp->ifr_mtu;
    for(int i = 1; i < len; i++) {
        tmp = &ifr[i];
        ioctl(sock, SIOCGIFMTU, tmp);
        if(tmp->ifr_mtu < min_mtu)
            min_mtu = tmp->ifr_mtu;
    }
    // size of an Ethernet MTU minus the headers of TFTP (4 bytes), UDP (8 bytes) and IP (20 bytes).
    return min_mtu - 32;
}

int set_blksize(int client_blksize) 
{
    /* Set data blocksize for transfer */
    int limit = get_mtu();

    if(client_blksize > limit) {
        fprintf(stdout, "Block size is limited to range [8 B - %d B].\nMaximum transfer block size is set to %d.\n", limit, limit);
        return limit;
    }
    return client_blksize;

}

void print_time() 
{   
    char time_buff[50];
    time_t rawtime;
    time(&rawtime);
    struct tm *timeinfo;
    timeinfo = localtime(&rawtime);
    strftime(time_buff, 50, "%Y-%m-%d %X", timeinfo);
    struct timeval time;
    gettimeofday(&time, NULL); // Get current time

    /* Print timestamp */
    printf("[%s.%d]", time_buff, (int)(time.tv_usec / 1000));
}

bool tftp_parse_command(char *line, Command *command)
{
    bool RW_opt = false;
    bool d_opt = false;
    bool s_opt = false;
    bool t_opt = false;

    char *token;
    token = strtok(line, " ");

    while(token) {
        if(strcmp(token, "-R")==0) { 
            command->operation = RRQ;
            RW_opt = true;
        } else if(strcmp(token, "-W")==0) {
            command->operation = WRQ;
            RW_opt = true;
        } else if(strcmp(token, "-t")==0) {
            token = strtok(NULL, " ");
            if(token) {
                char *ptr;
                command->timeout = (int)strtol(token, &ptr, 10);
                if (command->timeout == 0 || !strcmp(ptr, "")==0)
                    return false;
            } else
                return false;

            t_opt = true;
        } else if(strcmp(token, "-a")==0) {
            token = strtok(NULL, " ");  //   "address,port"

            command->address = strsep(&token, ",");
            command->port_num = strsep(&token, ",");
            if(!command->address || !command->port_num)
                return false;
        
        } else if(strcmp(token, "-d")==0) {
            d_opt = true;
            command->file = strtok(NULL, " ");

            if(! command->file)
                return false;
        } else if(strcmp(token, "-c")==0) {
            char *tmp = strtok(NULL, " ");
            if(strcmp(tmp, "ascii")==0 || strcmp(tmp, "netascii")==0)
                command->mode = "netascii";
            else if(strcmp(tmp, "binary")==0 || strcmp(tmp, "octet")==0)
                command->mode = "octet";
            else
                return false;
        } else if(strcmp(token, "-s")==0) {
            char *bsptr;
            bsptr = strtok(NULL, " ");
            if(bsptr) {
                command->block_size = (int)strtol(bsptr, &bsptr, 10);
                if (command->block_size == 0 || !strcmp(bsptr, "")==0){
                    return false;
                }
                s_opt = true;
            } else {
                return false;
            }
        } else { // Unknown parameter
            return false;
        }
        token = strtok(NULL, " ");       
    }
    if(!RW_opt || !d_opt) // Required options 
        return false;

    if(!s_opt) {
        command->block_size = 512; // Default
    } else {
        if(command->block_size <= 0)
            return false;
        command->block_size = set_blksize(command->block_size);
    }
    if(!t_opt) {
        command->timeout = 0;
    } else {
        printf("%d ", command->timeout);
        if(command->timeout < 1 || command->timeout > 255)
            return false;
    }
    return true;
}

void tftp_process_command(Command *command) 
{
    int sock;
    struct addrinfo hints, *server;
    memset(&hints, 0, sizeof(hints));

    hints.ai_family = AF_UNSPEC; // IPv4 or IPv6
    hints.ai_socktype = SOCK_DGRAM; //UDP

    if(getaddrinfo(command->address, command->port_num, &hints, &server) != 0) {
        fprintf(stderr, "error: getaddrinfo()\n");
        return;
    }
    // Create socket 
    sock = socket(server->ai_family, server->ai_socktype, server->ai_protocol);

    FILE *fp;
    int bufsize = command->block_size + 100;
    char buf[bufsize];

    unsigned int count;

    if(command->operation == RRQ) { // Read file from the server 
        char *filename = strrchr(command->file, '/');  // Get filename from the path
        if(!filename)
            filename = command->file;
        else
            filename = filename + 1;
        /* Opcode(2 bytes) | Filename | 0 | Mode | 0 | tsize | 0 | size | 0 | [blksize] | 0 | [size] | [timout] | [#secs]0*/
        char *p;

        *(int *)buf = htons(RRQ);
        p = buf + 2;
        strcpy(p, command->file);
        p += strlen(command->file) + 1;  
        strcpy(p, command->mode);
        p += strlen(command->mode) + 1;
        strcpy(p, "tsize");
        p += 6;  
        strcpy(p, "0");
        p += 2;
        if(command->block_size != 512) { // Include blksize option 
            strcpy(p, "blksize");
            p += 8;
            char blksize[10];
            sprintf(blksize, "%d", command->block_size);
            strcpy(p, blksize);
            p += strlen(blksize) + 1;
        }
        if(command->timeout != 0) { // Include timout option 
            strcpy(p, "timeout");
            p += 8;
            char timeout[10];
            sprintf(timeout, "%d", command->timeout);
            strcpy(p, timeout);
            p += strlen(timeout) + 1;
        }

        sendto(sock, buf, p-buf, 0, server->ai_addr, server->ai_addrlen);    // Send read request
        print_time();
        fprintf(stdout, " Requesting READ from server %s:%s\n", command->address, command->port_num);

        char tsize[100];

        count = recvfrom(sock, buf, bufsize, 0, server->ai_addr, &(server->ai_addrlen)); // Receive response

        if(buf[1] == OACK) {     //acknowledge of RRQ and the options
            if((fp = fopen(filename, "w")) == NULL) {
                fprintf(stderr, "error: opening a file");
                return;
            }
            tsize[0] = '\0';
            int i;
            for(i=8; i<count-1; i++) {
                int tmp = (int)(buf[i]);
                if(tmp == 0) {
                    break;
                }

                sprintf(tsize+strlen(tsize), "%d", tmp-48);
            }

            if(command->block_size != 512) {
                if(strcmp(buf+i+1, "blksize")!=0) {   // blksize option is rejected - it's omitted from OACK packet
                    fprintf(stdout,  "Block size is omitted by server. Maximum transfer block size is set by default to 512.\n");
                    command->block_size = 512;
                    bufsize = 600;
                }
            }
            count = sprintf(buf, "%c%c%c%c", 0, ACK, 0, 0);
            sendto(sock, buf, count, 0, server->ai_addr, server->ai_addrlen);    // Send ACK - response to an OACK
        } else if(buf[1] == DATA) {      // acknowledge of RRQ but not the options
            if((fp = fopen(filename, "w")) == NULL) {
                fprintf(stderr, "error: opening a file");
                return;
            }
            strcpy(tsize, "total");
            print_time();
            fprintf(stdout, " Receiving DATA ... %d B of %s B\n", count-4, tsize);
            fwrite(buf+4, sizeof(char), count-4, fp);

        } else {    // RRQ was rejected -> error
            fprintf(stderr, "tftp: %s\n", buf+4);
            return;
        }

        bufsize = command->block_size + 100;
        char data_buf[bufsize];
        unsigned int blocknum = 1, data_sum = 0;
        
        while(1) {
            count = recvfrom(sock, data_buf, bufsize, 0, server->ai_addr, &(server->ai_addrlen)); // Receive data

            if(data_buf[1] == DATA) {
                data_sum += count-4;
                print_time();
                fprintf(stdout, " Receiving DATA ... %d B of %s B\n", data_sum, tsize);
                fwrite(data_buf+4, sizeof(char), count-4, fp);
                blocknum++;

                data_buf[1] = ACK;
                sendto(sock, data_buf, 4, 0, server->ai_addr, server->ai_addrlen); // Send ACK

                if(count < command->block_size+4) {
                    print_time();
                    fprintf(stdout, " Transfer succesfully completed.\n");
                    break;
                }
            
            } else if(data_buf[1] == OACK_ERROR) {
                fprintf(stderr, "tftp: %s\n", data_buf+4);
                break;
            }
        }
    } 
    
    if(command->operation == WRQ) { //Write file to a server 
        if((fp = fopen(command->file, "r")) == NULL) {
            fprintf(stderr, "error: opening a file");
            return;
        }
        
        char *filename = strrchr(command->file, '/');  // Get filename from the path
        if(!filename)
            filename = command->file;
        else
            filename = filename + 1;

        char *p;
        *(int *)buf = htons(WRQ);
        
        p = buf + 2;
        strcpy(p, filename);
        p += strlen(filename) + 1;  
        strcpy(p, command->mode);
        p += strlen(command->mode) + 1;
        strcpy(p, "tsize");
        p += 6;

        // Find file size
        fseek(fp, 0L, SEEK_END);
        char fsize[30];
        long tsize = ftell(fp);
        sprintf(fsize , "%ld", tsize);
        fseek(fp, 0, SEEK_SET);

        strcpy(p, fsize);
        p += strlen(fsize) + 1;

        if(command->block_size != 512) { // Include blksize option 
            strcpy(p, "blksize");
            p += 8;
            char blksize[10];
            sprintf(blksize, "%d", command->block_size);
            strcpy(p, blksize);
            p += strlen(blksize) + 1;
        } 
        if(command->timeout != 0) {  // Include timout option 
            strcpy(p, "timeout");
            p += 8;
            char timeout[10];
            sprintf(timeout, "%d", command->timeout);
            strcpy(p, timeout);
            p += strlen(timeout) + 1;
        }

        sendto(sock, buf, p-buf, 0, server->ai_addr, server->ai_addrlen); // Send write request
        print_time();
        fprintf(stdout, " Requesting WRITE from server %s:%s\n", command->address, command->port_num);
        
        count = recvfrom(sock, buf, bufsize, 0, server->ai_addr, &(server->ai_addrlen)); // Receive response (OACK expected)

        if(buf[1] == OACK) {     //acknowledge of RRQ and the options
            int i;
            for(i=8; i<count-1; i++) {
                int tmp = (int)(buf[i]);
                if(tmp == 0) {
                    break;
                }
            }
            if(command->block_size != 512) {
                if(strcmp(buf+i+1, "blksize")!=0) {   // blksize option is rejected - it's omitted from OACK packet
                    fprintf(stdout,  "Block size is omitted by server. Maximum transfer block size is set by default to 512.\n");
                    command->block_size = 512;
                    bufsize = 600;
                }
            }

        } else if(buf[1] == OACK_ERROR) {
            fprintf(stderr, "tftp: %s\n", buf+4);
        }

        unsigned int blocknum = 1, bn_first_byte = 0, data_sum = 0;
        char data_buf[bufsize];
        char message[bufsize];
        int bufsize = command->block_size + 100;
        
        while(1) {
            memset(data_buf, 0, sizeof(data_buf));
            
            if(blocknum != 1) {
                recvfrom(sock, message, bufsize, 0, server->ai_addr, &(server->ai_addrlen)); // ACK or ERROR
                if(buf[1] == ERROR) {
                    fprintf(stderr, "tftp: %s", message+4);
                    break;
                }
            }
            size_t nmemb = fread(data_buf, 1, command->block_size, fp);
            data_sum += nmemb;

            if(blocknum > 255)
                bn_first_byte = (int)blocknum / 256;

            count = sprintf(message, "%c%c%c%c", 0, DATA, bn_first_byte, blocknum);

            memcpy(message+count, data_buf, nmemb);

            sendto(sock, message, count+nmemb, 0, server->ai_addr, server->ai_addrlen); // Send data 
            print_time();
            fprintf(stdout, " Sending DATA ... %d B of %ld B\n", data_sum, tsize);
            
            if(nmemb < command->block_size) { // Last block of data
                print_time();
                fprintf(stdout, " Transfer succesfully completed.\n");
                break;
            }
            blocknum++;

        }

        fclose(fp);
    }

}

void tftp_client()
{
    Command *command = (Command *) malloc(sizeof(Command)); // Alocate space for command 

    // Default values
    command->address = "127.0.0.1";
    command->port_num = "69";
    command->mode = "octet";
    char *line = NULL;
    size_t n = 256;

    while(1) {
        printf(">");
        if(getline(&line, &n, stdin) == -1)
            break;
        if(strcmp(line, "\n") == 0)
            break;

        int len = strlen(line);
        line[len-1] = '\0';
        

        if(!tftp_parse_command(line, command)) {
            fprintf(stderr, "Invalid command.\nUsage: -R/W -d adresar/soubor -t timeout -s velikost -a adresa,port -c mód\n");
        } else {
            tftp_process_command(command);
            printf("\n");
        }
        line = NULL; 
    
    }
    free(command);
}

int main(int argc, char *argv[])
{
    
    tftp_client();
    return 0;
}