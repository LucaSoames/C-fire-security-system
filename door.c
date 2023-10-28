#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>

#define BUFFER_SIZE 1024

typedef struct {
    char status; 
    pthread_mutex_t mutex;
    pthread_cond_t cond_start;
    pthread_cond_t cond_end;
} SharedMemory;

int send_init_message(const char *id, const char *addr_port, const char *security_mode, const char *overseer_addr, int overseer_port) {
    int sockfd;
    struct sockaddr_in overseer_addr_struct;
    char message[BUFFER_SIZE];

    snprintf(message, sizeof(message), "DOOR %s %s %s#", id, addr_port, security_mode);

    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("Socket creation error");
        return -1;
    }

    overseer_addr_struct.sin_family = AF_INET;
    overseer_addr_struct.sin_port = htons(overseer_port);
    overseer_addr_struct.sin_addr.s_addr = inet_addr(overseer_addr);

    if (connect(sockfd, (struct sockaddr *)&overseer_addr_struct, sizeof(overseer_addr_struct)) < 0) {
        perror("Connection failed");
        return -1;
    }

    send(sockfd, message, strlen(message), 0);
    close(sockfd);
    return 0;
}

void handle_door_operations(int client_sock, SharedMemory *sharedMem, char *buffer) {

    ssize_t n = recv(client_sock, buffer, sizeof(buffer) - 1, 0);
    if (n <= 0) {
        close(client_sock);
        return;
    }

    buffer[n] = '\0';

    pthread_mutex_lock(&(sharedMem->mutex));

    if (strncmp(buffer, "OPEN#", 5) == 0) {
        if (sharedMem->status == 'O') {
            send(client_sock, "ALREADY#", 8, 0);
        } else {
            sharedMem->status = 'o'; // Set status to opening
            pthread_cond_signal(&(sharedMem->cond_start));
            pthread_cond_wait(&(sharedMem->cond_end), &(sharedMem->mutex)); // Wait for the door to open
            sharedMem->status = 'O'; // Set status to open
            send(client_sock, "OPENED#", 7, 0);
        }
    } else if (strncmp(buffer, "CLOSE#", 6) == 0) {
        if (sharedMem->status == 'C') {
            send(client_sock, "ALREADY#", 8, 0);
        } else {
            sharedMem->status = 'c'; // Set status to closing
            pthread_cond_signal(&(sharedMem->cond_start));
            pthread_cond_wait(&(sharedMem->cond_end), &(sharedMem->mutex)); // Wait for the door to close
            sharedMem->status = 'C'; // Set status to closed
            send(client_sock, "CLOSED#", 7, 0);
        }
    } else if (strncmp(buffer, "OPEN_EMERG#", 11) == 0) {
        if (sharedMem->status != 'O') {
            sharedMem->status = 'o'; // Set status to opening
            pthread_cond_signal(&(sharedMem->cond_start));
            pthread_cond_wait(&(sharedMem->cond_end), &(sharedMem->mutex)); // Wait for the door to open
            sharedMem->status = 'O'; // Set status to open
        }
        send(client_sock, "EMERGENCY_MODE#", 15, 0);
        
        // From this point, the door will not respond to CLOSE# commands
        while (1) {
            ssize_t n = recv(client_sock, buffer, sizeof(buffer) - 1, 0);
            if (n <= 0) break;
            buffer[n] = '\0';
            if (strncmp(buffer, "CLOSE#", 6) == 0) {
                send(client_sock, "EMERGENCY_MODE#", 15, 0);
            }
        }
        
    } else if (strncmp(buffer, "CLOSE_SECURE#", 13) == 0) {
        if (sharedMem->status != 'C') {
            sharedMem->status = 'c'; // Set status to closing
            pthread_cond_signal(&(sharedMem->cond_start));
            pthread_cond_wait(&(sharedMem->cond_end), &(sharedMem->mutex)); // Wait for the door to close
            sharedMem->status = 'C'; // Set status to closed
        }
        send(client_sock, "SECURE_MODE#", 12, 0);
        
        // From this point, the door will not respond to OPEN# commands
        while (1) {
            ssize_t n = recv(client_sock, buffer, sizeof(buffer) - 1, 0);
            if (n <= 0) break;
            buffer[n] = '\0';
            if (strncmp(buffer, "OPEN#", 5) == 0) {
                send(client_sock, "SECURE_MODE#", 12, 0);
            }
        }
    }

    pthread_mutex_unlock(&(sharedMem->mutex));
    close(client_sock);
}

int main(int argc, char *argv[]) {
    if (argc != 7) {
        fprintf(stderr, "Usage: %s {id} {address:port} {FAIL_SAFE | FAIL_SECURE} {shared memory path} {shared memory offset} {overseer address:port}\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    char *id = argv[1];
    char *addr_port = argv[2];
    char *security_mode = argv[3];
    char *shm_path = argv[4];
    int shm_offset = atoi(argv[5]);
    char *overseer_addr = strtok(argv[6], ":");
    int overseer_port = atoi(strtok(NULL, ":"));

    int shm_fd = shm_open(shm_path, O_RDWR, 0);
    if (shm_fd == -1) {
        perror("Failed to open shared memory");
        exit(EXIT_FAILURE);
    }

    struct stat shm_stat;
    if (fstat(shm_fd, &shm_stat) == -1) {
        perror("fstat()");
        exit(1);
    }

    char *shm = mmap(NULL, shm_stat.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shm == MAP_FAILED) {
        perror("mmap()");
        exit(1);
    }
    SharedMemory *sharedMem = (SharedMemory *)(shm + shm_offset);
    // Default door status
    sharedMem->status = 'C';

    if (send_init_message(id, addr_port, security_mode, overseer_addr, overseer_port) != 0) {
        fprintf(stderr, "Failed to send initialization message.\n");
        exit(EXIT_FAILURE);
    }

    //bind and listen to the specified TCP port
    int sockfd, newsockfd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t clientlen = sizeof(client_addr);

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1) {
        perror("ERROR opening socket");
        exit(EXIT_FAILURE);
    }
    char ip[16];
    int port;

    sscanf(addr_port, "%15[^:]:%d", ip, &port); // Extract IP and port
    printf("Extracted IP: %s, Port: %d\n", ip, port);

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr = inet_addr(ip);

    if (bind(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("ERROR on binding");
        exit(EXIT_FAILURE);
    }

    listen(sockfd, 5);

    while (1) {
        newsockfd = accept(sockfd, (struct sockaddr *)&client_addr, &clientlen);
        if (newsockfd < 0) {
            perror("ERROR on accept");
            continue;
        }

        char buffer[BUFFER_SIZE];
        int n = read(newsockfd, buffer, BUFFER_SIZE - 1);
        if (n < 0) {
            perror("ERROR reading from socket");
            close(newsockfd);
            continue;
        }
        buffer[n] = '\0';
        handle_door_operations(newsockfd, sharedMem, buffer);

        close(newsockfd);
    }

    munmap(sharedMem, sizeof(SharedMemory));
    close(shm_fd);
    close(sockfd);
    return 0;
}


