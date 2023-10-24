#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>
#include <unistd.h>
#include <stdlib.h> // for atoi function
#include "overseer.h"

#define MAX_DOORS 50
#define MAX_CARD_READERS 50
#define MAX_FIRE_ALARMS 50
#define MAX_SIMULATORS 50
#define PORT 8080

Door doors[MAX_DOORS];
CardReader cardReaders[MAX_CARD_READERS];
FireAlarm fireAlarms[MAX_FIRE_ALARMS];
Simulator simulators[MAX_SIMULATORS];

pthread_mutex_t sharedMemoryMutex = PTHREAD_MUTEX_INITIALIZER;

int main(int argc, char *argv[]) {
    if (argc < 8) {
        fprintf(stderr, "Usage: %s <port> <...other_parameters>\n", argv[0]);
        return 1;
    }

    char *address_port = argv[1];
    int door_open_duration = atoi(argv[2]);
    int datagram_resend_delay = atoi(argv[3]);
    char *auth_file = argv[4];
    char *connections_file = argv[5];
    char *layout_file = argv[6];
    char *shared_memory_path = argv[7];
    int shared_memory_offset = atoi(argv[8]);
    // Initialize global data structures and mutexes
    initialize_global_data();

    // Initialize TCP and UDP servers
    int tcp_sockfd = init_tcp_server(address_port);
    int udp_sockfd = init_udp_server(address_port);

    if (tcp_sockfd == -1 || udp_sockfd == -1) {
        perror("Server initialization failed");
        return 1;
    }

    // Create threads for TCP and UDP servers
    pthread_t tcp_thread, udp_thread;
    pthread_create(&tcp_thread, NULL, tcp_server_thread, &tcp_sockfd);
    pthread_create(&udp_thread, NULL, udp_server_thread, &udp_sockfd);

    // Command-line interface for manual commands
    command_line_interface();

    // Clean up and finalize
    pthread_join(tcp_thread, NULL);
    pthread_join(udp_thread, NULL);
    cleanup_resources();

    return 0;
}

int init_overseer(int port) {
    int sockfd;
    struct sockaddr_in server_addr;

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1) {
        perror("Socket creation failed");
        return -1;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);

    if (bind(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) == -1) {
        perror("Bind failed");
        return -1;
    }

    if (listen(sockfd, 5) == -1) {
        perror("Listen failed");
        return -1;
    }

    return sockfd;
}

int init_udp_server(char* address_port) {
    int sockfd;
    struct sockaddr_in server_addr;

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("Error opening UDP socket");
        return -1;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(atoi(address_port));
    
    if (bind(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Error binding UDP socket");
        return -1;
    }

    return sockfd;
}

int init_tcp_server(char* address_port) {
   
    int port = atoi(address_port); // Convert port from string to integer
    return init_overseer(port);
}

void initialize_global_data() {
    memset(doors, 0, sizeof(doors));
    memset(cardReaders, 0, sizeof(cardReaders));
    memset(fireAlarms, 0, sizeof(fireAlarms));
    memset(simulators, 0, sizeof(simulators));
    // Additional initialization code if necessary
}

void* udp_server_thread(void* arg) {
    int sockfd;
    struct sockaddr_in server_addr, client_addr;
    
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("Error opening UDP socket");
        return NULL;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);
    
    if (bind(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Error binding UDP socket");
        return NULL;
    }

    while (1) {
        char buffer[1024];
        socklen_t len = sizeof(client_addr);
        int n = recvfrom(sockfd, buffer, sizeof(buffer) - 1, 0, (struct sockaddr*)&client_addr, &len);
        if (n > 0) {
            buffer[n] = '\0'; // Ensure null-termination
            process_udp_message(buffer); // Function to handle the processing of the message
        }
    }
    return NULL;
}

void* tcp_server_thread(void* arg) {
    int sockfd = *(int*)arg;
    int new_socket;
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    
    while (1) {
        new_socket = accept(sockfd, (struct sockaddr*)&client_addr, &addr_len);
        if (new_socket == -1) {
            perror("Accept failed");
            continue;
        }

        char buffer[1024] = {0};
        ssize_t bytes_read = read(new_socket, buffer, sizeof(buffer) - 1);
        if (bytes_read > 0) {
            buffer[bytes_read] = '\0'; // Ensure null-termination
            register_device(buffer); // Handle registration and other messages
        }

        close(new_socket);
    }
    return NULL;
}

void register_device(char* msg) {
    char* token = strtok(msg, " ");
    if (!token) return;

    if (strcmp(token, "DOOR") == 0) {
        register_door(msg);
    }
    else if (strcmp(token, "CARDREADER") == 0) {
        register_card_reader(msg);
    }
    else if (strcmp(token, "FIREALARM") == 0) {
        register_fire_alarm(msg);

        // Sending all previously saved FAIL_SAFE doors to the fire alarm
        send_all_saved_doors_to_firealarm();
    }
    else if (strcmp(token, "CAMERA") == 0) {
        register_camera(msg);
    }
    else if (strcmp(token, "ELEVATOR") == 0) {
        register_elevator(msg);
    }
    else if (strcmp(token, "DESTSELECT") == 0) {
        register_destination_select(msg);
    }
}

void register_door(char* msg) {
    char* token = strtok(msg, ",");
    if (!token) return;

    if (strcmp(token, "DOOR") == 0) {
        Door door;
        
        token = strtok(NULL, ",");
        if (token) strncpy(door.id, token, sizeof(door.id));
        token = strtok(NULL, ",");
        if (token) strncpy(door.address, token, sizeof(door.address));
        token = strtok(NULL, ",");
        if (token) door.port = atoi(token);
        token = strtok(NULL, ",");
        if (token) strncpy(door.type, token, sizeof(door.type));

        pthread_mutex_lock(&sharedMemoryMutex);
        find_or_add_door(door);
        pthread_mutex_unlock(&sharedMemoryMutex);
    }
    else if (strcmp(token, "CARD_READER") == 0) {
        CardReader cardReader;
        
        token = strtok(NULL, ",");
        if (token) strncpy(cardReader.id, token, sizeof(cardReader.id));
        token = strtok(NULL, ",");
        if (token) strncpy(cardReader.address, token, sizeof(cardReader.address));
        token = strtok(NULL, ",");
        if (token) cardReader.port = atoi(token);

        pthread_mutex_lock(&sharedMemoryMutex);
        find_or_add_cardReader(cardReader);
        pthread_mutex_unlock(&sharedMemoryMutex);
    }
    else if (strcmp(token, "FIRE_ALARM") == 0) {
        FireAlarm fireAlarm;
        
        token = strtok(NULL, ",");
        if (token) strncpy(fireAlarm.id, token, sizeof(fireAlarm.id));
        token = strtok(NULL, ",");
        if (token) strncpy(fireAlarm.address, token, sizeof(fireAlarm.address));
        token = strtok(NULL, ",");
        if (token) fireAlarm.port = atoi(token);

        pthread_mutex_lock(&sharedMemoryMutex);
        find_or_add_fireAlarm(fireAlarm);
        pthread_mutex_unlock(&sharedMemoryMutex);
    }
    else if (strcmp(token, "SIMULATOR") == 0) {
        Simulator simulator;
        
        token = strtok(NULL, ",");
        if (token) strncpy(simulator.id, token, sizeof(simulator.id));
        token = strtok(NULL, ",");
        if (token) strncpy(simulator.address, token, sizeof(simulator.address));
        token = strtok(NULL, ",");
        if (token) simulator.port = atoi(token);

        pthread_mutex_lock(&sharedMemoryMutex);
        find_or_add_simulator(simulator);
        pthread_mutex_unlock(&sharedMemoryMutex);
    }
}

void register_card_reader(char* msg) {
    // Extract details from the message and populate a CardReader struct
    CardReader cardReader;
    // Code to populate cardReader based on msg
    
    pthread_mutex_lock(&sharedMemoryMutex);
    find_or_add_cardReader(cardReader);
    pthread_mutex_unlock(&sharedMemoryMutex);
}

int find_or_add_door(Door new_door) {
    for (int i = 0; i < MAX_DOORS; i++) {
        // Check if door with this ID already exists
        if (strcmp(doors[i].id, new_door.id) == 0) {
            doors[i] = new_door; // Update existing
            return i;
        }
    }

    for (int i = 0; i < MAX_DOORS; i++) {
        if (strlen(doors[i].id) == 0) { // Empty slot
            doors[i] = new_door; // Add new
            return i;
        }
    }

    // If here, no space left
    fprintf(stderr, "Error: Door storage full!\n");
    return -1;
}

int find_or_add_cardReader(CardReader new_cardReader) {
    for (int i = 0; i < MAX_CARD_READERS; i++) {
        if (strcmp(cardReaders[i].id, new_cardReader.id) == 0) {
            cardReaders[i] = new_cardReader;
            return i;
        }
    }

    for (int i = 0; i < MAX_CARD_READERS; i++) {
        if (strlen(cardReaders[i].id) == 0) {
            cardReaders[i] = new_cardReader;
            return i;
        }
    }

    fprintf(stderr, "Error: CardReader storage full!\n");
    return -1;
}

void register_fire_alarm(char* msg) {
    char* token;
    char* address;
    char* port;

    // Extracting the address and port from the message
    token = strtok(msg, " "); // FIREALARM token
    token = strtok(NULL, " "); // address token
    if (token) {
        address = strdup(token);
    } else {
        // Handle error: missing address
        return;
    }

    token = strtok(NULL, " "); // port token
    if (token) {
        port = strdup(token);
    } else {
        // Handle error: missing port
        free(address);
        return;
    }

    // You might want to save the address and port of the fire alarm for later use
    // save_fire_alarm_info(address, port);

    // Getting all the FAIL_SAFE doors that have been registered before the fire alarm
    char** fail_safe_doors = get_all_fail_safe_doors();
    if (fail_safe_doors) {
        int i = 0;
        while (fail_safe_doors[i] != NULL) {
            // Sending each FAIL_SAFE door info to the fire alarm for registration
            send_door_registration_to_firealarm(fail_safe_doors[i], address, port);
            i++;
        }
        // Don't forget to free the memory allocated for fail_safe_doors if necessary
        // free_fail_safe_doors(fail_safe_doors);
    }

    // Don’t forget to free the duplicated strings
    free(address);
    free(port);
}



int find_or_add_fireAlarm(FireAlarm new_fireAlarm) {
    for (int i = 0; i < MAX_FIRE_ALARMS; i++) {
        if (strcmp(fireAlarms[i].id, new_fireAlarm.id) == 0) {
            fireAlarms[i] = new_fireAlarm;
            return i;
        }
    }

    for (int i = 0; i < MAX_FIRE_ALARMS; i++) {
        if (strlen(fireAlarms[i].id) == 0) {
            fireAlarms[i] = new_fireAlarm;
            return i;
        }
    }

    fprintf(stderr, "Error: FireAlarm storage full!\n");
    return -1;
}

int find_or_add_simulator(Simulator new_simulator) {
    for (int i = 0; i < MAX_SIMULATORS; i++) {
        if (strcmp(simulators[i].id, new_simulator.id) == 0) {
            simulators[i] = new_simulator;
            return i;
        }
    }

    for (int i = 0; i < MAX_SIMULATORS; i++) {
        if (strlen(simulators[i].id) == 0) {
            simulators[i] = new_simulator;
            return i;
        }
    }

    fprintf(stderr, "Error: Simulator storage full!\n");
    return -1;
}

void cleanup_resources() {
    // Code to free up any dynamically allocated memory or resources
    // For example, closing any remaining socket connections
}

void command_line_interface() {
    char command[256];
    while (1) {
        printf("Enter command: ");
        fgets(command, sizeof(command), stdin);
        // Process and execute the command here
        // You might want to create different functions for each type of command
    }
}

void process_udp_message(char* msg) {

    // Extract message details and process accordingly
    // For example, if it's a temperature update, you might update the temperature info in a global data structure
}

void process_received_message(char* msg, char* source_address, int source_port) {
    if (strncmp(msg, "OPENED#", 7) == 0) {
        printf("Door at %s:%d has opened.\n", source_address, source_port);
    } else if (strncmp(msg, "CLOSED#", 7) == 0) {
        printf("Door at %s:%d has closed.\n", source_address, source_port);
    } 
    // Add more conditions to handle other types of messages
}

void send_command_to_door(char* door_id, char* command) {
    for (int i = 0; i < MAX_DOORS; i++) {
        if (strcmp(doors[i].id, door_id) == 0) {
            // Assume send_tcp_message() is a function that sends a TCP message to a specific address and port
            send_tcp_message(doors[i].address, doors[i].port, command);
            return;
        }
    }
    fprintf(stderr, "Error: Door not found.\n");
}



// void handle_emergency() {
//     for (int i = 0; i < MAX_DOORS; i++) {
//         if (doors[i].type == FAIL_SAFE) {
//             send_command_to_door(doors[i].id, "OPEN_EMERG#");
//         }
//     }
// }

// void handle_security_breach() {
//     for (int i = 0; i < MAX_DOORS; i++) {
//         if (doors[i].type == FAIL_SECURE) {
//             send_command_to_door(doors[i].id, "CLOSE_SECURE#");
//         }
//     }
// }
