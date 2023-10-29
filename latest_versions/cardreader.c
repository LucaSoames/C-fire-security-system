#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>

#define BUFFER_SIZE 64

// Shared memory structure definition
typedef struct {
    char scanned[16];
    pthread_mutex_t mutex;
    pthread_cond_t scanned_cond;
    char response; // 'Y' or 'N' (or '\0' at first)
    pthread_cond_t response_cond;
} shm_cardreader;

// Function to send initialization message to overseer
int send_init_message(const char *id, const char *addr, int port) {
    int sockfd;
    struct sockaddr_in overseer_addr;
    char message[BUFFER_SIZE];

    snprintf(message, sizeof(message), "CARDREADER %s HELLO#", id);

    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("Socket creation error");
        return -1;
    }

    memset(&overseer_addr, '0', sizeof(overseer_addr));
    overseer_addr.sin_family = AF_INET;
    overseer_addr.sin_port = htons(port);

    if (inet_pton(AF_INET, addr, &overseer_addr.sin_addr) <= 0) {
        perror("Invalid address / Address not supported");
        return -1;
    }

    if (connect(sockfd, (struct sockaddr *)&overseer_addr, sizeof(overseer_addr)) < 0) {
        perror("Connection failed");
        return -1;
    }

    send(sockfd, message, strlen(message), 0);
    close(sockfd);
    return 0;
}

int main(int argc, char *argv[]) {
    
    if (argc != 6) {
        fprintf(stderr, "Usage: %s {id} {wait time} {shared memory path} {shared memory offset} {overseer address:port}\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    char *id = argv[1];
    // Ignore wait time: argv[2]
    const char *shm_path = argv[3];
    int shm_offset = atoi(argv[4]);
    const char *overseer_address = argv[5];
    char *overseer_addr_str = strtok(argv[5], ":");
    int overseer_port = atoi(strtok(NULL, ":"));

    // Initialize shared memory  

    int shm_fd = shm_open(shm_path, O_RDWR, 0);
    if (shm_fd == -1) {
        perror("shm_open()");
        exit(1);
    }

    struct stat shm_stat;
    if (fstat(shm_fd, &shm_stat) == -1) {
        perror("fstat()");
        exit(1);
    }

    char *shm = mmap(NULL, shm_stat.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0); // NULL because "we don't have to supply the address"
    if (shm == MAP_FAILED) {
        perror("mmap()");
        exit(1);
    }
    shm_cardreader *shared = (shm_cardreader *)(shm + shm_offset);

    // Init message to overseer
    if (send_init_message(id, overseer_addr_str, overseer_port) != 0) {
        fprintf(stderr, "Failed to send initialization message.\n");
        exit(EXIT_FAILURE);
    }

    pthread_mutex_lock(&shared->mutex);

    for(;;) { // Main loop
        if(shared->scanned[0] != '\0') {          

            // Connect to overseer and send scanned data

            char message[BUFFER_SIZE]; // Create message
            snprintf(message, sizeof(message), "CARDREADER %s SCANNED %s#", id, shared->scanned);

            int sockfd;
            struct sockaddr_in overseer_addr;
            char response[BUFFER_SIZE];

            sockfd = socket(AF_INET, SOCK_STREAM, 0);
            overseer_addr.sin_family = AF_INET;
            overseer_addr.sin_port = htons(overseer_port);
            overseer_addr.sin_addr.s_addr = inet_addr(overseer_addr_str);

            if (sockfd < 0) {
                perror("ERROR opening socket");
                shared->response = 'N';
                pthread_cond_signal(&(shared->response_cond));
                pthread_mutex_unlock(&(shared->mutex)); // Ensure unlocking mutex before continuing.
                continue;
            }

            if (connect(sockfd, (struct sockaddr *)&overseer_addr, sizeof(overseer_addr)) < 0) {
                perror("ERROR connecting");
                close(sockfd); // Ensure socket is closed to release the resource.
                shared->response = 'N';
                pthread_cond_signal(&(shared->response_cond));
                pthread_mutex_unlock(&(shared->mutex)); // Ensure unlocking mutex before continuing.
                continue;
            }

            ssize_t bytes_sent = send(sockfd, message, strlen(message), 0);
            if (bytes_sent < 0 || bytes_sent != strlen(message)) {
                perror("ERROR sending message");
                close(sockfd); // Ensure socket is closed to release the resource.
                shared->response = 'N';
                pthread_cond_signal(&(shared->response_cond));
                pthread_mutex_unlock(&(shared->mutex)); // Ensure unlocking mutex before continuing.
                continue;
            }

            ssize_t bytes_read = read(sockfd, response, sizeof(response)-1);
            if (bytes_read < 0) {
                perror("ERROR reading from socket");
                close(sockfd); // Ensure socket is closed to release the resource.
                shared->response = 'N';
                pthread_cond_signal(&(shared->response_cond));
                pthread_mutex_unlock(&(shared->mutex)); // Ensure unlocking mutex before continuing.
                continue;
            }

            if (connect(sockfd, (struct sockaddr *)&overseer_addr, sizeof(overseer_addr)) == 0) {
                send(sockfd, message, strlen(message), 0);
                read(sockfd, response, sizeof(response));
                close(sockfd);

                if (strncmp(response, "ALLOWED#", 8) == 0) {
                    shared->response = 'Y';
                } else {
                    shared->response = 'N';
                }
                pthread_cond_signal(&(shared->response_cond));
            } else {
                shared->response = 'N';
                pthread_cond_signal(&(shared->response_cond));
            }

            memset(shared->scanned, 0, sizeof(shared->scanned));

            pthread_mutex_unlock(&(shared->mutex));

        }
        pthread_cond_wait(&shared->scanned_cond, &shared->mutex); // Wait until scanned_cond is updated
    }

    munmap(shm, shm_stat.st_size);
    close(shm_fd);
    return(0);
}
