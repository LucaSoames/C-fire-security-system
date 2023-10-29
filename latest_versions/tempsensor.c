#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/time.h>
#include <unistd.h>

#define MAX_RECEIVERS 50

struct addr_entry {
    struct in_addr sensor_addr;
    in_port_t sensor_port;
};

struct datagram_format {
    char header[4]; // {'T', 'E', 'M', 'P'}
    struct timeval timestamp;
    float temperature;
    uint16_t id;
    uint8_t address_count;
    struct addr_entry address_list[50];
};

struct shared_memory_structure {
    float temperature;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
};

struct shared_memory_structure *shared_memory;
struct sockaddr_in receiver_addresses[MAX_RECEIVERS];
int num_receivers;
#define MAX_PEERS 10
#define MESSAGE_SIZE 100

char *peer_addresses[MAX_PEERS];
int peer_ports[MAX_PEERS];
int num_peers;
struct sockaddr_in receiver_addresses[50]; 
int num_receivers;
struct shared_memory_structure *shared_memory;
int sockfd;

// Prototype declarations
void broadcast_temperature(int temperature);

int should_send_update(float prev_temperature, float current_temperature, struct timeval *last_sent, struct timeval *current_time, int max_update_wait);
void receive_and_forward(int sockfd, int max_update_wait);
int has_address(struct datagram_format *datagram, struct in_addr addr, in_port_t port);
void append_address(struct datagram_format *datagram, struct in_addr addr, in_port_t port);

int should_send_update(float prev_temperature, float current_temperature, struct timeval *last_sent, struct timeval *current_time, int max_update_wait) {
    if (prev_temperature != current_temperature) {
        return 1;
    }
    int time_diff = (current_time->tv_sec - last_sent->tv_sec) * 1000000 + (current_time->tv_usec - last_sent->tv_usec);
    if (time_diff >= max_update_wait) {
        return 1;
    }
    return 0;
}

void receive_and_forward(int sockfd, int max_update_wait) {
    struct timeval timeout;
    timeout.tv_sec = max_update_wait / 1000000;
    timeout.tv_usec = max_update_wait % 1000000;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout));

    struct datagram_format received_datagram;
    int addr_len = sizeof(struct sockaddr_in);
    struct sockaddr_in src_addr;

    int bytes_received = recvfrom(sockfd, &received_datagram, sizeof(received_datagram), 0, (struct sockaddr *)&src_addr, &addr_len);
    while (bytes_received > 0) {
        // Forward logic
        for (int i = 0; i < num_receivers; i++) {
            if (!has_address(&received_datagram, receiver_addresses[i].sin_addr, receiver_addresses[i].sin_port)) {
                append_address(&received_datagram, receiver_addresses[i].sin_addr, receiver_addresses[i].sin_port);
                sendto(sockfd, &received_datagram, sizeof(received_datagram), 0, (struct sockaddr *)&receiver_addresses[i], sizeof(receiver_addresses[i]));
            }
        }

        bytes_received = recvfrom(sockfd, &received_datagram, sizeof(received_datagram), 0, (struct sockaddr *)&src_addr, &addr_len);
    }
}

void send_udp_datagram(struct datagram_format *datagram) {
    for (int i = 0; i < num_receivers; i++) {
        sendto(sockfd, datagram, sizeof(*datagram), 0, (struct sockaddr *)&receiver_addresses[i], sizeof(receiver_addresses[i]));
    }
}

int has_address(struct datagram_format *datagram, struct in_addr addr, in_port_t port) {
    for (int i = 0; i < datagram->address_count; i++) {
        if (datagram->address_list[i].sensor_addr.s_addr == addr.s_addr && datagram->address_list[i].sensor_port == port) {
            return 1;
        }
    }
    return 0;
}

void append_address(struct datagram_format *datagram, struct in_addr addr, in_port_t port) {
    if (datagram->address_count >= 50) {
        // Shift addresses if list is full
        memmove(&datagram->address_list[0], &datagram->address_list[1], sizeof(struct addr_entry) * 49);
        datagram->address_count = 49;
    }
    datagram->address_list[datagram->address_count].sensor_addr = addr;
    datagram->address_list[datagram->address_count].sensor_port = port;
    datagram->address_count++;
}

int main(int argc, char *argv[]) {
    // Parse command-line arguments
    uint16_t sensor_id = atoi(argv[1]);
    char *local_addr = strtok(argv[2], ":");
    int local_port = atoi(strtok(NULL, ":"));
    int max_condvar_wait = atoi(argv[3]);
    int max_update_wait = atoi(argv[4]);
    num_receivers = argc - 7; //
    
    for(int i = 0; i < num_receivers; i++) {
        char *receiver_addr = strtok(argv[7+i], ":");
        int receiver_port = atoi(strtok(NULL, ":"));
        receiver_addresses[i].sin_family = AF_INET;
        receiver_addresses[i].sin_port = htons(receiver_port);
        inet_pton(AF_INET, receiver_addr, &receiver_addresses[i].sin_addr);
    }

    //shared memory
    int shm_fd = shm_open(argv[5], O_RDONLY, 0666);
    shared_memory = (struct shared_memory_structure *) mmap(0, sizeof(struct shared_memory_structure), PROT_READ, MAP_SHARED, shm_fd, atoi(argv[6]));

    //udp socket
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(local_port);
    inet_pton(AF_INET, local_addr, &server_addr.sin_addr);
    bind(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr));

    //loop for normal operation
    float prev_temperature = -9999; //initial value
    struct timeval last_sent_time, current_time;

    while (1) {
        pthread_mutex_lock(&shared_memory->mutex);
        float current_temperature = shared_memory->temperature;
        pthread_mutex_unlock(&shared_memory->mutex);

        gettimeofday(&current_time, NULL);

        if (should_send_update(prev_temperature, current_temperature, &last_sent_time, &current_time, max_update_wait)) {
            struct datagram_format datagram;
            datagram.header[0] = 'T'; datagram.header[1] = 'E'; datagram.header[2] = 'M'; datagram.header[3] = 'P';
            datagram.temperature = current_temperature;
            datagram.id = sensor_id;
            datagram.address_count = 1;
            inet_pton(AF_INET, local_addr, &datagram.address_list[0].sensor_addr);
            datagram.address_list[0].sensor_port = htons(local_port);
            send_udp_datagram(&datagram);
            prev_temperature = current_temperature;
            last_sent_time = current_time;
        }

        receive_and_forward(sockfd, max_update_wait);

        // waiting
        struct timespec max_wait_time;
        max_wait_time.tv_sec = current_time.tv_sec + (max_condvar_wait / 1000000);
        max_wait_time.tv_nsec = (current_time.tv_usec + (max_condvar_wait % 1000000)) * 1000;
        pthread_mutex_lock(&shared_memory->mutex);
        pthread_cond_timedwait(&shared_memory->cond, &shared_memory->mutex, &max_wait_time);
        pthread_mutex_unlock(&shared_memory->mutex);
    }


    close(sockfd);
    return 0;
}
