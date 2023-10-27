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
#define MAX_TEMPSENSORS 50
#define PORT 8080

char* address_port;
int door_open_duration;
int datagram_resend_delay;
char* auth_file;
char* connections_file;
char* layout_file;
char* shared_memory_path;
int shared_memory_offset;

Door doors[MAX_DOORS];
CardReader cardReaders[MAX_CARD_READERS];
FireAlarm fireAlarms[MAX_FIRE_ALARMS];
Simulator simulators[MAX_SIMULATORS];
TempSensor tempSensors[MAX_TEMPSENSORS];

struct SharedMemory {
    char security_alarm; // '-' if inactive, 'A' if active
    pthread_mutex_t mutex;
    pthread_cond_t cond;
};

struct SharedMemory shared_memory = {
    '-', 
    PTHREAD_MUTEX_INITIALIZER, 
    PTHREAD_COND_INITIALIZER, 
};

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
        ssize_t total_bytes_read = 0;
        char current_byte;

        // Read byte-by-byte to find the end of the message or till buffer is full
        while (recv(new_socket, &current_byte, 1, 0) == 1) { 
            if (current_byte == '#' || total_bytes_read >= sizeof(buffer) - 1) { 
                break;
            }
            buffer[total_bytes_read] = current_byte;
            total_bytes_read++;
        }
        buffer[total_bytes_read] = '\0'; 

        if (strstr(buffer, "SCANNED") != NULL) {
            ThreadArgs* args = malloc(sizeof(ThreadArgs));
            args->socket = new_socket;
            strncpy(args->message, buffer, sizeof(buffer));

            pthread_t tid;
            pthread_create(&tid, NULL, handle_scanned_message_thread, args);
            pthread_detach(tid); 
        } else if (total_bytes_read > 0) {
            register_device(buffer);
        }

        close(new_socket);
    }
    return NULL;
}
// A basic structure for the function:

void handle_scanned_message(int client_socket, char* message) {
    // Assuming message is of the form: 'CARDREADER {id} SCANNED {scanned}#'

    // Extract card reader ID and scanned code from the message
    char* token = strtok(message, " "); // Delimit by space
    token = strtok(NULL, " "); // CARDREADER
    char* reader_id = strtok(NULL, " "); // {id}
    token = strtok(NULL, " "); // SCANNED
    char* scanned_code = strtok(NULL, "#"); // {scanned}

    // Lookup scanned code in the 'authorisation.txt' file
    char* access_list = lookup_authorisation(scanned_code); 
    if (!access_list) {
        // No entry found for the scanned code, access is denied
        send(client_socket, "DENIED#", 7, 0);
        close(client_socket);
        return;
    }

    // Lookup card reader's ID in the 'connections.txt' file
    int int_reader_id = atoi(reader_id);
    int door_id = lookup_door_id(int_reader_id);
    if (!door_id) {
        // No door ID found for the card reader ID
        send(client_socket, "DENIED#", 7, 0);
        close(client_socket);
        return;
    }

       char door_id_str[50]; // Ensure the buffer is large enough for the int and null terminator
    sprintf(door_id_str, "%d", door_id);
    if (has_access(access_list, door_id)) {
        // Send the ALLOWED message
        send(client_socket, "ALLOWED#", 8, 0);
        close(client_socket);

        // Connect to the door controller and send OPEN# command
        send_command_to_door(door_id_str, "OPEN#");
        
        int door_socket = get_door_sockfd(door_id_str);

        char response[1024];
        recv(door_socket, response, sizeof(response), 0);
        if (strncmp(response, "OPENING#", 8) == 0) {
            memset(response, 0, sizeof(response)); // Clear the buffer
            recv(door_socket, response, sizeof(response), 0);
            if (strncmp(response, "OPENED#", 7) == 0) {
                close(door_socket);
                
                usleep(door_open_duration); // Wait for {door open duration} microseconds
                
                send_command_to_door(door_id_str, "CLOSE#");
                close(door_socket);
            }
        }
    } else {
        // Access is denied
        send_command_to_door(door_id_str, "DENIED#");
    }
}

int get_door_sockfd(const char* door_id) {
    struct sockaddr_in door_address;
    int sockfd;
    
    // Search for the door by id
    for (int i = 0; strlen(doors[i].id) > 0; i++) {  // Use the actual number of doors here
        if (strcmp(doors[i].id, door_id) == 0) {
            // Found the door, now create the socket
            sockfd = socket(AF_INET, SOCK_STREAM, 0);
            if (sockfd == -1) {
                perror("Could not create socket");
                return -1;
            }

            door_address.sin_family = AF_INET;
            door_address.sin_addr.s_addr = inet_addr(doors[i].address);
            door_address.sin_port = htons(doors[i].port);

            // Connect to the door (assuming it's a TCP connection)
            if (connect(sockfd, (struct sockaddr *)&door_address, sizeof(door_address)) < 0) {
                perror("Connection failed");
                close(sockfd);
                return -1;
            }

            return sockfd;
        }
    }

    // If here, door not found
    printf("Door with id %s not found.\n", door_id);
    return -1;
}

char* lookup_authorisation(const char* scanned_code) {
    FILE* file = fopen(auth_file, "r");
    if (!file) {
        perror("Failed to open authorisation.txt");
        return NULL;
    }

    static char line[256];
    while (fgets(line, sizeof(line), file)) {
        if (strstr(line, scanned_code) != NULL) {
            fclose(file);
            return line;  // Note: returns the whole line. You might need to parse it further.
        }
    }

    fclose(file);
    return NULL;
}

int lookup_door_id(int card_reader_id) {
    FILE* file = fopen(connections_file, "r");
    if (!file) {
        perror("Failed to open connections file");
        return -1;
    }

    char line[256];
    int door_id;
    while (fgets(line, sizeof(line), file)) {
        if (sscanf(line, "DOOR %d %d", &door_id, &card_reader_id) == 2) {
            fclose(file);
            return door_id;
        }
    }

    fclose(file);
    return -1;
}

int has_access(const char* access_data, int door_id) {
    char door_str[16];
    snprintf(door_str, sizeof(door_str), "DOOR:%d", door_id);
    return strstr(access_data, door_str) != NULL;
}

int send_tcp_message(const char* address, int port, const char* message) {
    int sockfd;
    struct sockaddr_in server_addr;
    // Create a socket
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("Error creating socket");
        return -1;
    }
    // Define the server address
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    if (inet_pton(AF_INET, address, &server_addr.sin_addr) <= 0) {
        perror("Error converting address");
        close(sockfd);
        return -1;
    }
    // Connect to the server
    if (connect(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Error connecting to server");
        close(sockfd);
        return -1;
    }
    // Send the message
    int bytes_sent = send(sockfd, message, strlen(message), 0);
    if (bytes_sent < 0) {
        perror("Error sending message");
        close(sockfd);
        return -1;
    }

    close(sockfd);
    return bytes_sent;
}

void register_device(char* msg) {
    char* token = strtok(msg, " ");
    if (!token) return;

    if (strcmp(token, "DOOR") == 0) {
        Door door;

        token = strtok(NULL, " ");
        if (token) strncpy(door.id, token, sizeof(door.id));
        
        token = strtok(NULL, " ");
        if (token) {
            char* address_token = strtok(token, ":");
            if(address_token) strncpy(door.address, address_token, sizeof(door.address));
            
            char* port_token = strtok(NULL, ":");
            if(port_token) door.port = atoi(port_token);
        }

        token = strtok(NULL, " ");
        if (token) strncpy(door.type, token, sizeof(door.type));

        pthread_mutex_lock(&shared_memory.mutex);
        find_or_add_door(door);
        if (is_fire_alarm_registered() && strncmp(door.type, "FAIL_SAFE", 9) == 0) {
            send_door_to_fire_alarm(door); // assuming you have a function to send door to fire alarm
        }
        pthread_mutex_unlock(&shared_memory.mutex);
    }
    else if (strcmp(token, "CARDREADER") == 0) {
        CardReader cardReader;

        token = strtok(NULL, " ");
        if (token) strncpy(cardReader.id, token, sizeof(cardReader.id));
        
        pthread_mutex_lock(&shared_memory.mutex);
        find_or_add_cardReader(cardReader);
        pthread_mutex_unlock(&shared_memory.mutex);
    }
    else if (strcmp(token, "FIREALARM") == 0) {
        FireAlarm fireAlarm;
        
        token = strtok(NULL, " ");
        if (token) {
            char* address_token = strtok(token, ":");
            if(address_token) strncpy(fireAlarm.address, address_token, sizeof(fireAlarm.address));
            
            char* port_token = strtok(NULL, ":");
            if(port_token) fireAlarm.port = atoi(port_token);
        }

        pthread_mutex_lock(&shared_memory.mutex);
        find_or_add_fireAlarm(fireAlarm);
        send_all_saved_doors_to_firealarm();
        pthread_mutex_unlock(&shared_memory.mutex);
    }
}

int is_fire_alarm_registered() {
    return (strlen(fireAlarms[0].address) > 0 );
}

void send_all_saved_doors_to_firealarm() {
    int sockfd;
    struct sockaddr_in fire_alarm_addr;
    fd_set readfds;
    struct timeval tv;

    struct {
        char header[4];
        struct in_addr door_addr;
        in_port_t door_port;
    } door_datagram, confirmation_datagram;

    // Create socket
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd == -1) {
        printf("Could not create socket");
        return;
    }

    // Prepare the sockaddr_in structure for the fire alarm system
    fire_alarm_addr.sin_family = AF_INET;
    fire_alarm_addr.sin_addr.s_addr = inet_addr("127.0.0.1"); // assuming fire alarm system IP address
    fire_alarm_addr.sin_port = htons(8888); // assuming fire alarm system port

    for (int i = 0; strlen(doors[i].id) > 0; i++) {
        if (strncmp(doors[i].type, "FAIL_SAFE", 9) == 0) {
            strcpy(door_datagram.header, "DOOR");
            inet_aton(doors[i].address, &door_datagram.door_addr);
            door_datagram.door_port = doors[i].port;

            int sent = 0;
            int retries = 3; // Adjust based on desired retries

            while (retries > 0) {
                // Send door data to the fire alarm system
                if (sendto(sockfd, &door_datagram, sizeof(door_datagram), 0, (struct sockaddr *)&fire_alarm_addr, sizeof(fire_alarm_addr)) < 0) {
                    puts("Send failed");
                    return;
                }

                // Initialize select parameters
                FD_ZERO(&readfds);
                FD_SET(sockfd, &readfds);
                tv.tv_sec = 0;
                tv.tv_usec = datagram_resend_delay;

                int retval = select(sockfd + 1, &readfds, NULL, NULL, &tv);
                if (retval == -1) {
                    perror("select()");
                    return;
                } else if (retval) {
                    ssize_t len = recvfrom(sockfd, &confirmation_datagram, sizeof(confirmation_datagram), 0, NULL, NULL);
                    if (len > 0 && strncmp(confirmation_datagram.header, "DREG", 4) == 0 &&
                        confirmation_datagram.door_addr.s_addr == door_datagram.door_addr.s_addr &&
                        confirmation_datagram.door_port == door_datagram.door_port) {
                        sent = 1;
                        break;
                    }
                }
                retries--;
            }

            if (!sent) {
                printf("Failed to get confirmation for door with ID %s after multiple attempts.\n", doors[i].id);
            }
        }
    }

    close(sockfd);
}

void send_door_to_fire_alarm(Door door) {
    int sockfd;
    struct sockaddr_in fire_alarm_addr;
    char message[1024];
    fd_set readfds;
    struct timeval tv;

    struct {
        char header[4];
        struct in_addr door_addr;
        in_port_t door_port;
    } door_datagram, confirmation_datagram;

    // Create socket
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd == -1) {
        printf("Could not create socket");
        return;
    }

    // Prepare the sockaddr_in structure for the fire alarm system
    fire_alarm_addr.sin_family = AF_INET;
    fire_alarm_addr.sin_addr.s_addr = inet_addr("127.0.0.1"); // assuming fire alarm system IP address
    fire_alarm_addr.sin_port = htons(8888); // assuming fire alarm system port

    // Initialize the door_datagram
    strcpy(door_datagram.header, "DOOR");
    inet_aton(door.address, &door_datagram.door_addr);
    door_datagram.door_port = door.port;

    int sent = 0;
    int retries = 3; // You can adjust this number based on how many retries you want.
    while (retries > 0) {
        // Send door data to the fire alarm system
        if (sendto(sockfd, &door_datagram, sizeof(door_datagram), 0, (struct sockaddr *)&fire_alarm_addr, sizeof(fire_alarm_addr)) < 0) {
            puts("Send failed");
            return;
        }

        // Initialize select parameters
        FD_ZERO(&readfds);
        FD_SET(sockfd, &readfds);
        tv.tv_sec = 0;
        tv.tv_usec = datagram_resend_delay; // assuming datagram_resend_delay is globally defined

        // Waiting for a response for the given delay
        int retval = select(sockfd + 1, &readfds, NULL, NULL, &tv);

        if (retval == -1) {
            perror("select()");
            return;
        } else if (retval) {
            // Data is available now
            ssize_t len = recvfrom(sockfd, &confirmation_datagram, sizeof(confirmation_datagram), 0, NULL, NULL);
            if (len > 0 && strncmp(confirmation_datagram.header, "DREG", 4) == 0 &&
                confirmation_datagram.door_addr.s_addr == door_datagram.door_addr.s_addr &&
                confirmation_datagram.door_port == door_datagram.door_port) {
                sent = 1;
                break;
            }
        }
        retries--;
    }

    if (!sent) {
        puts("Failed to get confirmation after multiple attempts.");
    }

    close(sockfd);
}

void* handle_scanned_message_thread(void* arg) {
    ThreadArgs* thread_args = (ThreadArgs*) arg;
    handle_scanned_message(thread_args->socket, thread_args->message);
    free(thread_args);  // Important to free allocated memory
    pthread_exit(NULL); // Properly exit thread when done
}

int find_or_add_door(Door new_door) {
    for (int i = 0; i < strlen(doors[i].id) > 0; i++) {
        // Check if door with this ID already exists
        if (strcmp(doors[i].id, new_door.id) == 0) {
            doors[i] = new_door; // Update existing
            return i;
        }
    }

    for (int i = 0; i < strlen(doors[i].id) > 0; i++) {
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

void manual_access() {
    char command[100];
    int running = 1;

    while (running) {
        printf("Enter command (or 'EXIT' to quit): ");
        fgets(command, sizeof(command), stdin);

        //remove newline character
        size_t len = strlen(command);
        if (len > 0 && command[len-1] == '\n') {
            command[len-1] = '\0';
        }

        if (strcmp(command, "DOOR LIST") == 0) {
            list_doors();
        } 
        else if (strncmp(command, "DOOR OPEN", 9) == 0) {
            char door_id[10];
            sscanf(command, "DOOR OPEN %s", door_id);
            open_door(door_id);
        } 
        else if (strncmp(command, "DOOR CLOSE", 10) == 0) {
            char door_id[10];
            sscanf(command, "DOOR CLOSE %s", door_id);
            close_door(door_id);
        } 
        else if (strcmp(command, "FIRE ALARM") == 0) {
            while (1) {
                send_udp_datagram_to_fire_alarm_unit(); 
                usleep(datagram_resend_delay);
            }
        }
        else if (strcmp(command, "SECURITY ALARM") == 0) {
            raise_security_alarm();
        }
        else if (strcmp(command, "EXIT") == 0) {
            running = 0;
        } 
        else {
            printf("Invalid command.\n");
        }
    }
}

void list_doors() {
    printf("List of Doors:\n");
    printf("ID\tIP Address\tPort\tType\n");
    for (int i = 0; i < strlen(doors[i].id) > 0; i++) {
        printf("%s\t%s\t%d\t%s\n", doors[i].id, doors[i].address, doors[i].port, doors[i].type);
    }
}

void open_door(char* door_id) {
    send_command_to_door(door_id, "OPEN#");
}

void close_door(char* door_id) {
    send_command_to_door(door_id, "CLOSE#");
}

void process_udp_message(char* msg) {
    // struct temperature_entry *incoming_datagram = (struct temperature_entry *)msg;

    // // Check if the message is a temperature update
    // if (strncmp(incoming_datagram->header, "TEMP", 4) == 0) {
    //     // Find if this sensor's data already exists in the database
    //     int found = -1;
    //     for (int i = 0; i < MAX_SENSORS; i++) {
    //         if (temperature_db[i].sensor_addr.s_addr == incoming_datagram->address_list[0].sensor_addr.s_addr &&
    //             temperature_db[i].sensor_port == incoming_datagram->address_list[0].sensor_port) {
    //             found = i;
    //             break;
    //         }
    //     }

    //     // If the sensor is not found, find an empty slot for it
    //     if (found == -1) {
    //         for (int i = 0; i < MAX_SENSORS; i++) {
    //             if (temperature_db[i].sensor_addr.s_addr == 0 && temperature_db[i].sensor_port == 0) {
    //                 found = i;
    //                 break;
    //             }
    //         }
    //     }

    //     // Update the database with new temperature data
    //     if (found != -1) {
    //         if (timercmp(&incoming_datagram->timestamp, &temperature_db[found].timestamp, >)) {
    //             temperature_db[found].sensor_addr = incoming_datagram->address_list[0].sensor_addr;
    //             temperature_db[found].sensor_port = incoming_datagram->address_list[0].sensor_port;
    //             temperature_db[found].timestamp = incoming_datagram->timestamp;
    //             temperature_db[found].temperature = incoming_datagram->temperature;
    //         }
    //     } else {
    //         // Handle case where there's no space left in the temperature_db (or other error conditions)
    //         printf("Temperature database full or other error occurred.\n");
    //     }
    // }
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
    for (int i = 0; i < strlen(doors[i].id) > 0; i++) {
        if (strcmp(doors[i].id, door_id) == 0) {
            // Assume send_tcp_message() is a function that sends a TCP message to a specific address and port
            send_tcp_message(doors[i].address, doors[i].port, command);
            return;
        }
    }
    fprintf(stderr, "Error: Door not found.\n");
}

void send_udp_datagram_to_fire_alarm_unit() {
    int sockfd;
    struct sockaddr_in servaddr;
    char datagram[] = {'F', 'I', 'R', 'E'};

    // Create socket
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd == -1) {
        perror("Socket creation failed");
        return;
    }

    memset(&servaddr, 0, sizeof(servaddr));

    // Iterate through all fire alarms and send the datagram
    for (int i = 0; i < MAX_FIRE_ALARMS; i++) {
        // If the fire alarm ID is empty, it's likely the end of the registered alarms
        if (strlen(fireAlarms[i].id) == 0) {
            break;
        }

        servaddr.sin_family = AF_INET;
        servaddr.sin_port = htons(fireAlarms[i].port);
        inet_pton(AF_INET, fireAlarms[i].address, &(servaddr.sin_addr));

        sendto(sockfd, datagram, sizeof(datagram), 0, (struct sockaddr*)&servaddr, sizeof(servaddr));
    }

    close(sockfd);
}

void raise_security_alarm() {
    // Lock the shared memory mutex
    pthread_mutex_lock(&shared_memory.mutex);

    // Set 'security_alarm' to A
    shared_memory.security_alarm = 'A';

    // unlock the mutex
    pthread_mutex_unlock(&shared_memory.mutex);

    // Signal the condition variable
    pthread_cond_signal(&shared_memory.cond);

    // Loop through every FAIL_SECURE door
    for (int i = 0; i < strlen(doors[i].id) > 0; i++) {
        if (strcmp(doors[i].type, "FAIL_SECURE") == 0) {
            send_tcp_message(doors[i].address, doors[i].port, "CLOSE_SECURE#");
        }
    }
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

int main(int argc, char *argv[]) {
    if (argc < 8) {
        fprintf(stderr, "Usage: %s <port> <...other_parameters>\n", argv[0]);
        return 1;
    }

    address_port = argv[1];
    door_open_duration = atoi(argv[2]);
    datagram_resend_delay = atoi(argv[3]);
    auth_file = argv[4];
    connections_file = argv[5];
    layout_file = argv[6];
    shared_memory_path = argv[7];
    shared_memory_offset = atoi(argv[8]);
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
    manual_access();

    // Clean up and finalize
    pthread_join(tcp_thread, NULL);
    pthread_join(udp_thread, NULL);
    cleanup_resources();

    return 0;
}
