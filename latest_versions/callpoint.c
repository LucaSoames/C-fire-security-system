#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>

typedef struct {
    char status; 
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} SharedMemory;

typedef struct {
    char header[4];
} FireEmergencyDatagram;

void send_fire_emergency_datagram(const char *fire_alarm_addr, int fire_alarm_port) {
    int sockfd;
    struct sockaddr_in fire_alarm_addr_struct;
    FireEmergencyDatagram datagram = {{'F', 'I', 'R', 'E'}};

    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("Socket creation error");
        return;
    }

    fire_alarm_addr_struct.sin_family = AF_INET;
    fire_alarm_addr_struct.sin_port = htons(fire_alarm_port);
    fire_alarm_addr_struct.sin_addr.s_addr = inet_addr(fire_alarm_addr);

    sendto(sockfd, &datagram, sizeof(datagram), 0, (struct sockaddr *)&fire_alarm_addr_struct, sizeof(fire_alarm_addr_struct));
    close(sockfd);
}

int main(int argc, char *argv[]) {
    if (argc != 5) {
        fprintf(stderr, "Usage: %s {resend delay (in microseconds)} {shared memory path} {shared memory offset} {fire alarm unit address:port}\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    int resend_delay = atoi(argv[1]);
    char *shm_path = argv[2];
    off_t shm_offset = atoi(argv[3]);
    char *fire_alarm_addr = strtok(argv[4], ":");
    int fire_alarm_port = atoi(strtok(NULL, ":"));

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
    while (1) { 
        pthread_mutex_lock(&sharedMem->mutex);

        while (sharedMem->status != '*') {
            pthread_cond_wait(&sharedMem->cond, &sharedMem->mutex);
        }
        if (sharedMem->status == '*') {
            send_fire_emergency_datagram(fire_alarm_addr, fire_alarm_port);
            usleep(resend_delay);
        }

        pthread_mutex_unlock(&sharedMem->mutex);
    }

    munmap(sharedMem, sizeof(SharedMemory));
    close(shm_fd);
    return 0;
}
