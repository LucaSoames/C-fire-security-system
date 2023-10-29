#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <string.h>
#include <stddef.h>
#include <fcntl.h>

#define MAX_COMPONENTS 110
#define MAX_EVENTS 1000
#define FILE_LINE_MAX_LENGTH 100
#define FILEPATH "/shm"

#define NUM_OF_OVERSEERS 1
#define MAX_NUM_OF_FIREALARMS 1
#define MAX_NUM_OF_CARDREADERS 40
#define MAX_NUM_OF_DOORS 20
#define MAX_NUM_OF_TEMPSENSORS 20
#define MAX_NUM_OF_CALLPOINTS 20

pthread_mutex_t lock; // Process spawning lock


struct overseerMemory {
    char security_alarm; // '-' if inactive, 'A' if active
    pthread_mutex_t mutex;
    pthread_cond_t cond;
};

struct firealarmMemory {
    char alarm; // '-' if inactive, 'A' if active
    pthread_mutex_t mutex;
    pthread_cond_t cond;
};

struct cardreaderMemory {
    char scanned[16];
    pthread_mutex_t mutex;
    pthread_cond_t scanned_cond;
    
    char response; // 'Y' or 'N' (or '\0' at first)
    pthread_cond_t response_cond;
};

struct doorMemory {
    char status; // 'O' for open, 'C' for closed, 'o' for opening, 'c' for closing
    pthread_mutex_t mutex;
    pthread_cond_t cond_start;
    pthread_cond_t cond_end;
};

struct callpointMemory {
    char status; // '-' for inactive, '*' for active
    pthread_mutex_t mutex;
    pthread_cond_t cond;
};

struct tempsensorMemory {
    float temperature;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
};


typedef struct { // Build empty shm structure (to be mapped)
    struct overseerMemory overseerMemoryArray[NUM_OF_OVERSEERS];

    struct firealarmMemory firealarmMemoryArray[MAX_NUM_OF_FIREALARMS];

    struct cardreaderMemory cardreaderMemoryArray[MAX_NUM_OF_CARDREADERS];

    struct doorMemory doorMemoryArray[MAX_NUM_OF_DOORS];

    struct callpointMemory callpointMemoryArray[MAX_NUM_OF_CALLPOINTS];

    struct tempsensorMemory tempsensorMemoryArray[MAX_NUM_OF_CALLPOINTS];

} SharedMemory;

SharedMemory *sharedMemory;


typedef struct { // Define a component
    char type[25];
    char configArray[10][25];
} Component;

Component components[MAX_COMPONENTS]; // Array of components
int component_count = 0;
int firealarm_count = 0;
int cardreader_count = 0;
int door_count = 0;
int callpoint_count = 0;
int tempsensor_count = 0;

typedef struct { // Define an event
    char type[25];
    char configArray[6][25];
} Event;

Event events[MAX_EVENTS]; // Array of events
int event_count = 0;


void parse_file(FILE *scenario_file) { // Parse senario file into components array

    char line[FILE_LINE_MAX_LENGTH]; // Line of file

    while (fgets(line, sizeof(line), scenario_file) && strncmp(line, "INIT", 4) == 0) { // Pare initialisation
        
        const char delims[] = " \n";
        char *token = strtok(line, delims); // Tokenise line
        if (token != NULL) {
            Component new_component; // Define new component
            

            int token_index = 0; // New component config
            while ((token = strtok(NULL, delims)) != NULL && token_index < 10) {
                if (token_index == 0) {
                    strcpy(new_component.type, token); // New component type

                    if (strcmp(token, "firealarm") == 0) { firealarm_count++; }
                    else if (strcmp(token, "cardreader") == 0) { cardreader_count++; }
                    else if (strcmp(token, "door") == 0) { door_count++; }
                    else if (strcmp(token, "callpoint") == 0) { callpoint_count++; }
                    else if (strcmp(token, "tempsensor") == 0) { tempsensor_count++; }

                } else if (token_index > 0) {
                    int config_index = token_index - 1;
                    strcpy(new_component.configArray[(config_index)], token); // New component config element
                }
                
                token_index++;
            }

            if (component_count < MAX_COMPONENTS) { 
                components[component_count] = new_component; // Add new component to component array
                (component_count)++;
            } else {
                printf("Exceeded maximum number of components.\n"); // TO DELETE
                break;
            }
        }
    }
    while (fgets(line, sizeof(line), scenario_file) && strncmp(line, "SCENARIO", 7) != 0) { // Parse events

        const char delims[] = " \n";
        char *token = strtok(line, delims); // Tokenise line
        if (token != NULL) {
            Event new_event; // Define new event

            strcpy(new_event.configArray[0], token); // Event timestamp

            int config_index = 1; // New event config
            while ((token = strtok(NULL, delims)) != NULL && config_index < 6) {
                if (config_index == 1) {
                    strcpy(new_event.type, token); // New event type
                } else if (config_index > 1) {
                    strcpy(new_event.configArray[config_index], token); // New event config element
                }
                
                config_index++;
            }

            if (event_count < MAX_EVENTS) { 
                events[event_count] = new_event; // Add new event to event array
                (event_count)++;
            } else {
                printf("Exceeded maximum number of events.\n");
                break;
            }
        }

    }
}


void create_shared_memory() { // Map shm structure

    int shm_fd = shm_open(FILEPATH, O_CREAT | O_RDWR, 0666); // Create a shared memory object
    if (shm_fd == -1) {
        perror("shm_open");
        exit(1);
    }

    ftruncate(shm_fd, sizeof(SharedMemory)); // Set the size of the shared memory segment
    
    sharedMemory = mmap(0, sizeof(SharedMemory), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, shm_fd, 0); // Map shm
    if (sharedMemory == MAP_FAILED) {
        perror("mmap failed");
        exit(1);
    }
}


int overseer_count, firealarm_count, cardreader_count, door_count, tempsensor_count, callpoint_count;

void shared_memory_init() { // Initialise

    // Find counts of each component

    overseer_count = NUM_OF_OVERSEERS; // Always 1 overseer

    for (int i = 0; i < component_count; i++) { // For all other components

        if(strcmp(components[i].type, "firealarm") == 0) { firealarm_count++;}          
        else if (strcmp(components[i].type, "cardreader") == 0) { cardreader_count++;}
        else if (strcmp(components[i].type, "doors") == 0) { door_count++;}
        else if (strcmp(components[i].type, "tempsensor") == 0) { tempsensor_count++;}
        else if (strcmp(components[i].type, "callpoints") == 0) { callpoint_count++;}
    }

    // initialise values to corresponding shm component array
    for (int i = 0; i < overseer_count; i++) {
        sharedMemory->overseerMemoryArray[i].security_alarm = '-';
    }
    for (int i = 0; i < firealarm_count; i++) {
        sharedMemory->firealarmMemoryArray[i].alarm = '-';
    }
    for (int i = 0; i < cardreader_count; i++) {
        for (int j = 0; j < 16; j++) {sharedMemory->cardreaderMemoryArray[i].scanned[j] = '\0';}
        sharedMemory->cardreaderMemoryArray[i].response = '\0';
    }
    for (int i = 0; i < door_count; i++) {
        sharedMemory->doorMemoryArray[i].status = 'C';
    }
    for (int i = 0; i < tempsensor_count; i++) {
        sharedMemory->tempsensorMemoryArray[i].temperature = 22.0f;
    }
    for (int i = 0; i < callpoint_count; i++) {
        sharedMemory->callpointMemoryArray[i].status = '-';
    }
}


pid_t pids[MAX_COMPONENTS]; // Array of process IDs
int firealarm_boot_count = 0, cardreader_boot_count = 0, door_boot_count = 0, tempsensor_boot_count = 0, callpoint_boot_count = 0;

void spawn_processes() {

    pid_t pid; // process ID
    
    int base_port = 3001; // start at 3002 (not including overseer (3000) and firealarm (3001))
    char *overseer_address = "127.0.0.1:3000";
    char *firealarm_address = "127.0.0.1:3001";

    char address_port_str[13] = "127.0.0.1:"; // For building address:port
    char port_str[20];

    char shm_offset_str[20]; // For calculating shm offset

    int component_num;

    pid = fork();
        
    if (pid < 0) {
        fprintf(stderr, "Fork failed");
        exit(1);
        
    } else if (pid == 0) { // Child process
        
        usleep(250000); // 250 milliseconds

        for (component_num = 1; component_num < component_count; component_num++) { //Cycle through components

            if (strcmp(components[component_num].type, "firealarm") == 0) { // FIREALARM THREAD

                pid = fork();

                if (pid < 0) {
                    fprintf(stderr, "Fork failed");
                    exit(1);

                } else if (pid == 0) {


                    size_t shm_offset = offsetof(SharedMemory, firealarmMemoryArray[firealarm_boot_count]);
                    sprintf(shm_offset_str, "%zu", shm_offset);

                    execl("./firealarm", "firealarm", firealarm_address, components[component_num].configArray[0], components[component_num].configArray[1], components[component_num].configArray[2], NULL, FILEPATH, shm_offset_str, overseer_address, (char *) NULL);
                    fprintf(stderr, "Fire alarm execl failed");
                    exit(1);
                } else {

                    firealarm_boot_count++;
                    pids[component_num] = pid;
                    continue;
                }

            } else if (strcmp(components[component_num].type, "cardreader") == 0) { // CARDREADER THREADS

                pid = fork();

                if (pid < 0) {
                fprintf(stderr, "Fork failed");
                exit(1);

                } else if (pid == 0) {

                    int port_number = base_port + component_num; // Create address:port
                    sprintf(port_str, "%d", port_number);
                    strcat(address_port_str, port_str);

                    size_t shm_offset = offsetof(SharedMemory, cardreaderMemoryArray[firealarm_boot_count]); // Calculate shm offset
                    sprintf(shm_offset_str, "%zu", shm_offset);

                    execl("./cardreader", "cardreader", components[component_num].configArray[0], components[component_num].configArray[1], FILEPATH, shm_offset_str, overseer_address, (char *) 0);
                    fprintf(stderr, "Cardreader execl failed");
                    exit(1);
                } else {

                    cardreader_boot_count++;
                    pids[component_num] = pid;
                    continue;
                }
                
            } else if (strcmp(components[component_num].type, "door") == 0) { // DOOR THREADS

                pid = fork();

                if (pid < 0) {
                    fprintf(stderr, "Fork failed");
                   exit(1);

                } else if (pid == 0) {

                    int port_number = base_port + component_num; // Create address:port
                    sprintf(port_str, "%d", port_number);
                    strcat(address_port_str, port_str);

                    size_t shm_offset = offsetof(SharedMemory, doorMemoryArray[door_boot_count]); // Calculate shm offset
                    sprintf(shm_offset_str, "%zu", shm_offset);

                    execl("./door", "door", components[component_num].configArray[0], address_port_str, components[component_num].configArray[1], FILEPATH, shm_offset_str, overseer_address, (char *) NULL);
                    fprintf(stderr, "Door execl failed");
                    exit(1);
                } else {

                    door_boot_count++;
                    pids[component_num] = pid;
                    continue;
                }

            } else if (strcmp(components[component_num].type, "callpoint") == 0) { // CALLPOINT THREAD

                pid = fork();

                if (pid < 0) {
                    fprintf(stderr, "Fork failed");
                   exit(1);

                } else if (pid == 0) {

                    int port_number = base_port + component_num; // Create address:port
                    sprintf(port_str, "%d", port_number);
                    strcat(address_port_str, port_str);

                    size_t shm_offset = offsetof(SharedMemory, callpointMemoryArray[callpoint_boot_count]);
                    sprintf(shm_offset_str, "%zu", shm_offset);

                    execl("./callpoint", "callpoint", components[component_num].configArray[1], FILEPATH, shm_offset_str, firealarm_address, (char *) NULL); // Check firealarm_address_port is working
                    fprintf(stderr, "Callpoint execl failed");
                    exit(1);
                } else {

                    callpoint_boot_count++;
                    pids[component_num] = pid;
                    continue;
                }

            } else if (strcmp(components[component_num].type, "tempsensor") == 0) { // TEMPSENSOR THREADS

                pid = fork();

                if (pid < 0) {
                    fprintf(stderr, "Fork failed");
                   exit(1);

                } else if (pid == 0) {

                    int port_number = base_port + component_num; // Create address:port
                    sprintf(port_str, "%d", port_number);
                    strcat(address_port_str, port_str);
                    
                    size_t shm_offset = offsetof(SharedMemory, tempsensorMemoryArray[tempsensor_boot_count]);
                    sprintf(shm_offset_str, "%zu", shm_offset);

                    execl("./tempsensor", "tempsensor", components[component_num].configArray[0], address_port_str, components[component_num].configArray[1], components[component_num].configArray[2], FILEPATH, shm_offset_str, NULL, (char *) NULL); // FIRST NULL IS RECIEVER LIST
                    fprintf(stderr, "Temperature sensor execl failed");
                    exit(1);
                } else {

                    tempsensor_boot_count++;
                    pids[component_num] = pid;
                    continue;
                }
            }
        }

    } else { // Parent process (Overseer)
            
        pids[0] = pid;

        size_t shm_offset = offsetof(SharedMemory, overseerMemoryArray[0]); // Calculate and cast offset to str
        char *shm_offset_str = (char*)malloc(5 * sizeof(char));
        snprintf(shm_offset_str, 5, "%zu", shm_offset);

        execl("./overseer", "overseer", overseer_address, components[0].configArray[1], components[0].configArray[2], "authorisation.txt", "connections.txt", "layout.txt", FILEPATH, shm_offset_str, (char *) NULL);
        fprintf(stderr, "Overseer execl failed");
        free(shm_offset_str);
        exit(1);
        }   
}

void simulate_events() {

    for (int i = 0; i < event_count; i++) {
        if (strcmp(events[i].type, "CARD_SCAN") == 0) {
            
            int num = atoi(events[i].configArray[2]); // which cardreader?

            pthread_mutex_lock(&sharedMemory->cardreaderMemoryArray[num].mutex); // mutex lock

            strcpy(sharedMemory->cardreaderMemoryArray[num].scanned, events[i].configArray[3]); // Update scanned

            pthread_mutex_unlock(&sharedMemory->cardreaderMemoryArray[num].mutex);// mutex unlock

            pthread_cond_signal(&(sharedMemory->cardreaderMemoryArray[num].scanned_cond)); // update scanned_cond

        } else if (strcmp(events[i].type, "CALLPOINT_TRIGGER") == 0) {

            int num = atoi(events[i].configArray[2]); // which callpoint?

            pthread_mutex_lock(&sharedMemory->callpointMemoryArray[num].mutex); // mutex lock

            sharedMemory->callpointMemoryArray[num].status = '*'; // Update status

            pthread_mutex_unlock(&sharedMemory->callpointMemoryArray[num].mutex);// mutex unlock

            pthread_cond_signal(&(sharedMemory->callpointMemoryArray[num].cond)); // update cond

        } else if (strcmp(events[i].type, "TEMP_CHANGE") == 0) {

            int num = atoi(events[i].configArray[2]); // which tempsensor?

            pthread_mutex_lock(&sharedMemory->tempsensorMemoryArray[num].mutex); // mutex lock

            sharedMemory->tempsensorMemoryArray[num].temperature = atof(events[i].configArray[2]); // Update temperature

            pthread_mutex_unlock(&sharedMemory->tempsensorMemoryArray[num].mutex);// mutex unlock

            pthread_cond_signal(&(sharedMemory->tempsensorMemoryArray[num].cond)); // update cond
        }
    }
}

void cleanup() {

    for (int i = 1; i < component_count; i++) {
        kill(pids[i], SIGTERM); // Send a termination signal
        waitpid(pids[i], NULL, 0); // Wait for child process to terminate
    }

    // Unmap the shared memory
    if (munmap(sharedMemory, sizeof(SharedMemory)) == -1) {
        perror("munmap");
        exit(1);
    }

    if (shm_unlink(FILEPATH) == -1) {
        perror("shm_unlink");
        exit(1);
    }

    kill(pids[0], SIGTERM); // Suicide
}



int main(int argc, char *argv[]) {

    if (argc != 2) { // Check if CLI arguments are valid
        printf("Usage: %s {scenario file}\n", argv[0]);
        return 1;
    }

    FILE *scenario_file = fopen(argv[1], "r"); // load scenario file

    if (!scenario_file) { // Check can access scenario file
        perror("Failed to open scenario file");
        return 1;
    }

    parse_file(scenario_file); // Parse scenario file

    create_shared_memory(); // Create shm structure
    shared_memory_init(); // Load shm init values

    spawn_processes();

    usleep(1000000); // 1 second

    simulate_events();

    usleep(1000000); // 1 second

    cleanup();

    fclose(scenario_file);
    return 0;
}
