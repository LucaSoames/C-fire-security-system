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
#include <sys/stat.h>

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
    char alarm; // '-' if inactive, 'A' if active
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} shm_firealarm;

shm_firealarm *shared;

Door doors[MAX_DOORS];
int door_count = 0;
uint64_t detections[MAX_DETECTIONS];
int detection_count = 0;


int send_init_message(const char *firealarm_addr, const char *addr, int port) {
  
    char message[BUFFER_SIZE]; // Write message
    snprintf(message, sizeof(message), "FIREALARM %s HELLO#", firealarm_addr);

    int sockfd;
    struct sockaddr_in overseer_addr;
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
            pthread_mutex_lock(&shared->mutex);
            shared->alarm = 'A';
            pthread_cond_signal(&shared->cond);
            pthread_mutex_unlock(&shared->mutex);
            // Send open door signal to all registered doors
            for (int i = 0; i < door_count; i++) {
                // send_open_door_signal(doors[i].addr, doors[i].port);
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
    pthread_mutex_lock(&shared->mutex);
    shared->alarm = 'A';
    pthread_cond_signal(&shared->cond);
    pthread_mutex_unlock(&shared->mutex);

    // Open all registered doors
    for (int i = 0; i < door_count; i++) {
        send_open_door_signal(doors[i].door_addr, doors[i].door_port);
    }
}

int bind_udp_port(int port) {
    
    int sockfd;
    struct sockaddr_in udp_addr;

    // Create a UDP socket
    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("Socket creation error");
        return -1;
    }

    // Set up the address structure
    memset(&udp_addr, 0, sizeof(udp_addr));
    udp_addr.sin_family = AF_INET;
    udp_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    udp_addr.sin_port = htons(port);

    // Bind the socket to the specified port
    if (bind(sockfd, (struct sockaddr *)&udp_addr, sizeof(udp_addr)) < 0) {
        perror("Binding failed");
        return -1;
    }

    return sockfd;
}

int main(int argc, char *argv[]) {

    if (argc != 9) {
        fprintf(stderr, "Usage: %s {address:port} {temperature threshold} {min detections} {detection period (in microseconds)} {reserved argument} {shared memory path} {shared memory offset} {overseer address:port}\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    // Parsing command-line arguments
    char *addr_str = strtok(argv[1], ":");
    int port = atoi(strtok(NULL, ":"));
    int temp_threshold = atoi(argv[2]);
    int min_detections = atoi(argv[3]);
    uint64_t detection_period = atoll(argv[4]);
    // argv[5] is reserved argument
    char *shm_path = argv[6];
    int shm_offset = atoi(argv[7]);
    char *overseer_addr_str = strtok(argv[8], ":");
    int overseer_port = atoi(strtok(NULL, ":"));

    
    // Bind the UDP port
    int sockfd = bind_udp_port(port);
    if (sockfd < 0) {
        fprintf(stderr, "Error binding UDP port\n");
        return -1;
    }

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
    shared = (shm_firealarm *)(shm + shm_offset);

    // Send init message
    send_init_message(addr_str, overseer_addr_str, port);

    // Main loop
    char buffer[1024];
    while (1) {

        struct sockaddr_in sender_address;
        socklen_t sender_len = sizeof(sender_address);
        int len = recvfrom(sockfd, buffer, sizeof(buffer), 0, (struct sockaddr *)&sender_address, &sender_len);
        
        if (len >= 4) {
            if (strncmp(buffer, "TEMP", 4) == 0) {
                process_TEMP_datagram(buffer, len, temp_threshold, detection_period, min_detections);
            } else if (strncmp(buffer, "FIRE", 4) == 0) {
                process_FIRE_datagram();
            } else if (strncmp(buffer, "DOOR", 4) == 0) {
                process_DOOR_datagram(buffer, len, overseer_addr_str, overseer_port);
            } else {
                // Log or handle unknown datagram type
            }
        }
    }

    // Cleanup and exit

    munmap(shared, sizeof(shm_firealarm));
    close(sockfd);
    //close(shm_fd);
    return 0;
}
