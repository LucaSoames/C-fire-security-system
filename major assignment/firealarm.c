#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

#define OVERSEER_PORT 8080
#define MAX_DOORS 100
#define MAX_DETECTIONS 50
#define BUFFER_SIZE 512

typedef struct {
    char header[4];
    struct in_addr door_addr;
    in_port_t door_port;
} DoorDatagram;

typedef struct {
    char header[4]; // {'D', 'O', 'O', 'R'}
    struct in_addr door_addr;
    in_port_t door_port;
} Door;

typedef struct {
    char alarm;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} SharedMemory;

Door doors[MAX_DOORS];
int door_count = 0;
uint64_t detections[MAX_DETECTIONS];
int detection_count = 0;
SharedMemory *sharedMem = NULL;

void send_initial_greeting(const char* overseer_addr, int overseer_port, const char* self_addr, int self_port);
void main_loop(int udp_socket, int temp_threshold, uint64_t detection_period, int min_detections, const char *overseer_ip, in_port_t overseer_port);
void process_TEMP_datagram(char *buffer, int len, int temp_threshold, uint64_t detection_period, int min_detections);
void process_DOOR_datagram(char *buffer, int len, const char* overseer_addr, int overseer_port);
void send_open_door_signal(struct in_addr addr, in_port_t port);
void process_FIRE_datagram();
void send_initial_greeting(const char* overseer_addr, int overseer_port, const char* self_addr, int self_port) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in overseer_address;
    overseer_address.sin_family = AF_INET;
    overseer_address.sin_port = htons(overseer_port);
    inet_pton(AF_INET, overseer_addr, &(overseer_address.sin_addr));

    connect(sockfd, (struct sockaddr*)&overseer_address, sizeof(overseer_address));

    char message[100];
    snprintf(message, sizeof(message), "FIREALARM %s:%d HELLO#", self_addr, self_port);
    send(sockfd, message, strlen(message), 0);

    close(sockfd);
}

int main(int argc, char *argv[]) {
    if (argc != 9) {
        fprintf(stderr, "Usage: %s {address:port} {temperature threshold} {min detections} {detection period (in microseconds)} {reserved argument} {shared memory path} {shared memory offset} {overseer address:port}\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    // Parsing command-line arguments
    char *addr_str = strtok(argv[1], ":");
    in_port_t port = atoi(strtok(NULL, ":"));
    int temp_threshold = atoi(argv[2]);
    int min_detections = atoi(argv[3]);
    uint64_t detection_period = atoll(argv[4]);
    // argv[5] is reserved argument
    char *shm_path = argv[6];
    off_t shm_offset = atoi(argv[7]);

    char *overseer_addr_str = strtok(argv[8], ":");
    in_port_t overseer_port = atoi(strtok(NULL, ":"));

    // Initialize UDP socket
    int udp_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_socket == -1) {
        perror("Failed to create UDP socket");
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in bind_addr;
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(port);
    bind_addr.sin_addr.s_addr = inet_addr(addr_str);

    if (bind(udp_socket, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) == -1) {
        perror("Failed to bind UDP socket");
        close(udp_socket);
        exit(EXIT_FAILURE);
    }

    // Initialize shared memory
    int shm_fd = shm_open(shm_path, O_RDWR, 0666);
    if (shm_fd == -1) {
        perror("Failed to open shared memory");
        close(udp_socket);
        exit(EXIT_FAILURE);
    }

    sharedMem = mmap(NULL, sizeof(SharedMemory), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, shm_offset);
    if (sharedMem == MAP_FAILED) {
        perror("Failed to map shared memory");
        close(udp_socket);
        close(shm_fd);
        exit(EXIT_FAILURE);
    }

    // Send initialization message to overseer
     // Assuming this function exists and will send the required TCP message

    // Main loop
    void main_loop(int udp_socket, int temp_threshold, uint64_t detection_period, int min_detections, const char *overseer_ip, in_port_t overseer_port);

    // Cleanup and exit (although in this scenario it won't ever reach here under normal circumstances)
    munmap(sharedMem, sizeof(SharedMemory));
    close(udp_socket);
    close(shm_fd);
    return 0;
}

void main_loop(int udp_socket, int temp_threshold, uint64_t detection_period, int min_detections, const char *overseer_ip, in_port_t overseer_port) {
    char buffer[1024];  // adjust size as necessary
    while (1) {
        struct sockaddr_in sender_address;
        socklen_t sender_len = sizeof(sender_address);
        int len = recvfrom(udp_socket, buffer, sizeof(buffer), 0, (struct sockaddr *)&sender_address, &sender_len);
        
        if (len >= 4) {
            if (strncmp(buffer, "TEMP", 4) == 0) {
                process_TEMP_datagram(buffer, len, temp_threshold, detection_period, min_detections);
            } else if (strncmp(buffer, "FIRE", 4) == 0) {
                process_FIRE_datagram();
            } else if (strncmp(buffer, "DOOR", 4) == 0) {
                process_DOOR_datagram(buffer, len, overseer_ip, overseer_port);
            } else {
                // Log or handle unknown datagram type
            }
        }
    }
}

void process_TEMP_datagram(char *buffer, int len, int temp_threshold, uint64_t detection_period, int min_detections) {
    // Assuming the TEMP datagram format is known, extract temperature and timestamp
    int temperature;
    uint64_t timestamp;
    // decode(buffer, &temperature, &timestamp); // placeholder
    
    if (temperature >= temp_threshold) {
        uint64_t current_time = (uint64_t) time(NULL) * 1000000;  // convert to microseconds

        // Remove old detections
        int idx = 0;
        while (idx < detection_count && detections[idx] < current_time - detection_period) {
            idx++;
        }
        if (idx > 0) {
            memmove(detections, detections + idx, (detection_count - idx) * sizeof(uint64_t));
            detection_count -= idx;
        }

        // Add new detection
        detections[detection_count++] = timestamp;

        if (detection_count >= min_detections) {
            // Trigger alarm
            pthread_mutex_lock(&sharedMem->mutex);
            sharedMem->alarm = 'A';
            pthread_cond_signal(&sharedMem->cond);
            pthread_mutex_unlock(&sharedMem->mutex);
            // Send open door signal to all registered doors
            for (int i = 0; i < door_count; i++) {
                // send_open_door_signal(doors[i].addr, doors[i].port); // placeholder
            }
        }
    }
}

void process_DOOR_datagram(char *buffer, int len, const char* overseer_addr, int overseer_port) {
    DoorDatagram *datagram = (DoorDatagram *) buffer;
    
    // Check if door already registered
    int found = 0;
    for (int i = 0; i < door_count; i++) {
        if (doors[i].door_addr.s_addr == datagram->door_addr.s_addr && doors[i].door_port == datagram->door_port) {
            found = 1;
            break;
        }
    }

    if (!found) {
        doors[door_count].door_addr = datagram->door_addr;
        doors[door_count].door_port = datagram->door_port;
        door_count++;

        // Send confirmation to overseer
        int udp_socket = socket(AF_INET, SOCK_DGRAM, 0);
        struct sockaddr_in overseer_address;
        overseer_address.sin_family = AF_INET;
        overseer_address.sin_port = htons(overseer_port);
        inet_pton(AF_INET, overseer_addr, &(overseer_address.sin_addr));

        DoorDatagram confirm = {.header = "DREG", .door_addr = datagram->door_addr, .door_port = datagram->door_port};
        sendto(udp_socket, &confirm, sizeof(confirm), 0, (struct sockaddr*)&overseer_address, sizeof(overseer_address));
        close(udp_socket);
    }
}

// Placeholder function
void send_open_door_signal(struct in_addr addr, in_port_t port) {
    int tcp_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (tcp_socket == -1) {
        perror("Cannot create TCP socket");
        return;
    }

    struct sockaddr_in door_address;
    door_address.sin_family = AF_INET;
    door_address.sin_port = port;
    door_address.sin_addr = addr;

    if (connect(tcp_socket, (struct sockaddr *)&door_address, sizeof(door_address)) != 0) {
        perror("Cannot connect to door");
        close(tcp_socket);
        return;
    }

    const char *message = "OPEN_EMERG#";
    send(tcp_socket, message, strlen(message), 0);

    close(tcp_socket);
}

void process_FIRE_datagram() {
    pthread_mutex_lock(&sharedMem->mutex);
    sharedMem->alarm = 'A';
    pthread_cond_signal(&sharedMem->cond);
    pthread_mutex_unlock(&sharedMem->mutex);

    // Open all registered doors
    for (int i = 0; i < door_count; i++) {
        send_open_door_signal(doors[i].door_addr, doors[i].door_port);
    }
}

int create_udp_socket(int port) {
    int sockfd;
    struct sockaddr_in server_addr;

    // Create socket
    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("cannot create socket");
        return -1;
    }

    memset((char *)&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(port);

    // Bind the socket to the port
    if (bind(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind failed");
        return -1;
    }

    return sockfd;
}

void send_initialization_message(int udp_port, const char* overseer_ip, int overseer_tcp_port) {
    int sockfd;
    struct sockaddr_in overseer_addr;
    char message[256];

    // Create socket
    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("cannot create socket");
        return;
    }

    memset((char*)&overseer_addr, 0, sizeof(overseer_addr));
    overseer_addr.sin_family = AF_INET;
    overseer_addr.sin_port = htons(overseer_tcp_port);
    if (inet_pton(AF_INET, overseer_ip, &overseer_addr.sin_addr) <= 0) {
        perror("inet_pton error");
        return;
    }

    // Connect to overseer
    if (connect(sockfd, (struct sockaddr *)&overseer_addr, sizeof(overseer_addr)) < 0) {
        perror("connect error");
        return;
    }

    // Send initialization message
    snprintf(message, sizeof(message), "FIREALARM {127.0.0.1:%d} HELLO#", udp_port);
    if (send(sockfd, message, strlen(message), 0) < 0) {
        perror("send error");
        return;
    }

    close(sockfd);
}

