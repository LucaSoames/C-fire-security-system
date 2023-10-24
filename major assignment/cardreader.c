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

#define BUFFER_SIZE 1024

// Shared memory structure definition
typedef struct {
    char scanned[16];
    pthread_mutex_t mutex;
    pthread_cond_t scanned_cond;
    char response;
    pthread_cond_t response_cond;
} SharedMemory;

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
    char *shm_path = argv[3];
    off_t shm_offset = atoi(argv[4]);
    char *overseer_addr_str = strtok(argv[5], ":");
    int overseer_port = atoi(strtok(NULL, ":"));

    // Initialize shared memory
    int shm_fd = shm_open(shm_path, O_RDWR, 0666);
    if (shm_fd == -1) {
        perror("Failed to open shared memory");
        exit(EXIT_FAILURE);
    }

    SharedMemory *sharedMem = (SharedMemory *)mmap(NULL, sizeof(SharedMemory), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, shm_offset);
    if (sharedMem == MAP_FAILED) {
        perror("Failed to map shared memory");
        close(shm_fd);
        exit(EXIT_FAILURE);
    }

    if (send_init_message(id, overseer_addr_str, overseer_port) != 0) {
        fprintf(stderr, "Failed to send initialization message.\n");
        exit(EXIT_FAILURE);
    }

    while (1) {
        pthread_mutex_lock(&(sharedMem->mutex));

        if (strlen(sharedMem->scanned) == 0) {
            pthread_cond_wait(&(sharedMem->scanned_cond), &(sharedMem->mutex));
        }

        // Connect to overseer and send scanned data
        char message[BUFFER_SIZE];
        snprintf(message, sizeof(message), "CARDREADER %s SCANNED %s#", id, sharedMem->scanned);

        int sockfd;
        struct sockaddr_in overseer_addr;
        char response[BUFFER_SIZE];

        sockfd = socket(AF_INET, SOCK_STREAM, 0);
        overseer_addr.sin_family = AF_INET;
        overseer_addr.sin_port = htons(overseer_port);
        overseer_addr.sin_addr.s_addr = inet_addr(overseer_addr_str);

        if (sockfd < 0) {
            perror("ERROR opening socket");
            sharedMem->response = 'N';
            pthread_cond_signal(&(sharedMem->response_cond));
            pthread_mutex_unlock(&(sharedMem->mutex)); // Ensure unlocking mutex before continuing.
            continue;
        }

        if (connect(sockfd, (struct sockaddr *)&overseer_addr, sizeof(overseer_addr)) < 0) {
            perror("ERROR connecting");
            close(sockfd); // Ensure socket is closed to release the resource.
            sharedMem->response = 'N';
            pthread_cond_signal(&(sharedMem->response_cond));
            pthread_mutex_unlock(&(sharedMem->mutex)); // Ensure unlocking mutex before continuing.
            continue;
        }

        ssize_t bytes_sent = send(sockfd, message, strlen(message), 0);
        if (bytes_sent < 0 || bytes_sent != strlen(message)) {
            perror("ERROR sending message");
            close(sockfd); // Ensure socket is closed to release the resource.
            sharedMem->response = 'N';
            pthread_cond_signal(&(sharedMem->response_cond));
            pthread_mutex_unlock(&(sharedMem->mutex)); // Ensure unlocking mutex before continuing.
            continue;
        }

        ssize_t bytes_read = read(sockfd, response, sizeof(response)-1);
        if (bytes_read < 0) {
            perror("ERROR reading from socket");
            close(sockfd); // Ensure socket is closed to release the resource.
            sharedMem->response = 'N';
            pthread_cond_signal(&(sharedMem->response_cond));
            pthread_mutex_unlock(&(sharedMem->mutex)); // Ensure unlocking mutex before continuing.
            continue;
        }

        if (connect(sockfd, (struct sockaddr *)&overseer_addr, sizeof(overseer_addr)) == 0) {
            send(sockfd, message, strlen(message), 0);
            read(sockfd, response, sizeof(response));
            close(sockfd);

            if (strncmp(response, "ALLOWED#", 8) == 0) {
                sharedMem->response = 'Y';
            } else {
                sharedMem->response = 'N';
            }
            pthread_cond_signal(&(sharedMem->response_cond));
        } else {
            sharedMem->response = 'N';
            pthread_cond_signal(&(sharedMem->response_cond));
        }

        memset(sharedMem->scanned, 0, sizeof(sharedMem->scanned));

        pthread_mutex_unlock(&(sharedMem->mutex));
    }

    munmap(sharedMem, sizeof(SharedMemory));
    close(shm_fd);
    return 0;
}
