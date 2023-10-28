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
#define FILE_LINE_MAX_LENGTH 100

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


void parse_file(FILE *scenario_file) { // Parse senario file into components array

    char line[FILE_LINE_MAX_LENGTH]; // Line of file

    while (fgets(line, sizeof(line), scenario_file) && strncmp(line, "INIT", 4) == 0) { // Read lines starting with INIT
        
        const char delims[] = " \n";
        char *token = strtok(line, delims); // Tokenise line
        if (token != NULL) {
            Component new_component; // Define new component
            

            int config_index = 0; // New component config
            while ((token = strtok(NULL, delims)) != NULL && config_index < 10) {
                if (config_index == 0) {
                    strcpy(new_component.type, token); // New component type
                } else if (config_index > 0) {
                    strcpy(new_component.configArray[config_index], token); // New component configuration
                }
                
                config_index++;
            }

            if (component_count < MAX_COMPONENTS) { // Add new component to component array
                components[component_count] = new_component;
                (component_count)++;
            } else {
                printf("Exceeded maximum number of components.\n"); // TO DELETE
                break;
            }
        }
    }
}


void create_shared_memory() { // Map shm structure

    const char *name = "/shared_memory"; // Shared memory object name

    int shm_fd = shm_open(name, O_CREAT | O_RDWR, 0666); // Create a shared memory object
    if (shm_fd == -1) {
        perror("shm_open");
        exit(1);
    }

    ftruncate(shm_fd, sizeof(SharedMemory)); // Set the size of the shared memory segment
    
    sharedMemory = mmap(NULL, sizeof(SharedMemory), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, shm_fd, 0); // Map shm
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
int overseer_boot_count = -1, firealarm_boot_count = -1, cardreader_boot_count = -1, door_boot_count = -1, tempsensor_boot_count = -1, callpoint_boot_count = -1; // start at 0

void spawn_processes() { // SOME CHILD PROCESSES BROKEN

    int i;
    pid_t pid;
    
    int port_number = 2999; // start at 3000
    char firealarm_address_port[13];
    char overseer_address_port[13];

    pid = fork();
        
    if (pid < 0) {
        fprintf(stderr, "Fork failed");
        exit(1);
        
    } else if (pid == 0) { // Child process
        
        usleep(250000); // 250 milliseconds

        for (i = 1; i < component_count; i++) {

            pid = fork();

            if (pid < 0) {
                fprintf(stderr, "Fork failed");
                exit(1);

            } else if (pid == 0) {
                port_number ++;

                char address_port[13] = "127.0.0.1:";
                char port_str[20];
                sprintf(port_str, "%d", port_number);
                strcat(address_port, port_str);

                if (strcmp(components[i].type, "firealarm") == 0) {

                    firealarm_boot_count ++;
                    strcpy(firealarm_address_port, address_port);
                    size_t shm_offset = offsetof(SharedMemory, firealarmMemoryArray[firealarm_boot_count]);
                    char shm_offset_str[20];
                    sprintf(shm_offset_str, "%zu", shm_offset);
                    execl("./firealarm", address_port, components[i].configArray[0], components[i].configArray[1], components[i].configArray[2], NULL, "/dev/shm/shared_memory", shm_offset_str, "127.0.0.1:3000", (char *) NULL);
                    fprintf(stderr, "Fire alarm execl failed");
                    exit(1);

                } else if (strcmp(components[i].type, "cardreader") == 0) {
                
                    cardreader_boot_count ++; // Boot new card reader

                    size_t shm_offset = offsetof(SharedMemory, cardreaderMemoryArray[cardreader_boot_count]);
                    char shm_offset_str[20];
                    sprintf(shm_offset_str, "%zu", shm_offset);

                    execl("./cardreader", components[i].configArray[1], components[i].configArray[2], "/dev/shm/shared_memory", shm_offset_str, "127.0.0.1:3000", (char *) 0);
                    fprintf(stderr, "Cardreader execl failed");
                    exit(1);

                } else if (strcmp(components[i].type, "door") == 0) {

                    door_boot_count ++;
                    size_t shm_offset = offsetof(SharedMemory, doorMemoryArray[door_boot_count]);
                    char shm_offset_str[20];
                    sprintf(shm_offset_str, "%zu", shm_offset);
                    execl("./door", components[i].configArray[0], address_port, components[i].configArray[1], "/dev/shm/shared_memory", shm_offset_str, "127.0.0.1:3000", (char *) NULL);
                    fprintf(stderr, "Door execl failed");
                    exit(1);
                
                } else if (strcmp(components[i].type, "callpoint") == 0) {
                
                    callpoint_boot_count ++;
                    size_t shm_offset = offsetof(SharedMemory, callpointMemoryArray[callpoint_boot_count]);
                    char shm_offset_str[20];
                    sprintf(shm_offset_str, "%zu", shm_offset);
                    execl("./callpoint", components[i].configArray[1], "/dev/shm/shared_memory", shm_offset_str, firealarm_address_port, (char *) NULL); // Check firealarm_address_port is working
                    fprintf(stderr, "Callpoint execl failed");
                    exit(1);

                } else if (strcmp(components[i].type, "tempsensor") == 0) {
                
                    callpoint_boot_count ++;
                    size_t shm_offset = offsetof(SharedMemory, callpointMemoryArray[callpoint_boot_count]);
                    char shm_offset_str[20];
                    sprintf(shm_offset_str, "%zu", shm_offset);
                    execl("./tempsensor", components[i].configArray[0], address_port, components[i].configArray[1], components[i].configArray[2], "/dev/shm/shared_memory", shm_offset_str, NULL, (char *) NULL); // FIRST NULL IS RECIEVER LIST
                    fprintf(stderr, "Temperature sensor execl failed");
                    exit(1);

                } else {
                printf("Unknown component type: %s\n", components[i].type);
                exit(1);
                }
            }
        }

        exit(0);

    } else { // Parent process
            
        pids[0] = pid;
            
        port_number ++;
        overseer_boot_count ++;
        
        strcpy(overseer_address_port, "127.0.0.1:"); // Create address:port stringS
        char port_str[20];
        sprintf(port_str, "%d", port_number);
        strcat(overseer_address_port, port_str);        

        size_t shm_offset = offsetof(SharedMemory, overseerMemoryArray[overseer_boot_count]); // Calculate and cast offset to str
        char *shm_offset_str = (char*)malloc(5 * sizeof(char));
        snprintf(shm_offset_str, 5, "%zu", shm_offset);

        execl("./overseer", overseer_address_port, components[0].configArray[1], components[0].configArray[2], "authorisation.txt", "connections.txt", "layout.txt", "/dev/shm/shared_memory", shm_offset_str, (char *) NULL);
        //execl("./overseer", overseer_address_port, components[0].configArray[1], components[0].configArray[2], "authorisation.txt", "connections.txt", "layout.txt", "/dev/shm/shared_memory", shm_offset_str, (char *) NULL); // Execute overseer
        fprintf(stderr, "Overseer execl failed");
        free(shm_offset_str);
        exit(1);
        }   
}

/*void simulate_scenario(FILE *scenario_file) {
    char line[256];
    char component[10];
    char action[20];
    float value;

    while (fgets(line, sizeof(line), scenario_file)) {
        if (sscanf(line, "%s %s %f", component, action, &value) == 3) {
            if (strcmp(component, "MOTOR") == 0) {
                if (strcmp(action, "speed") == 0) {
                    sharedMemory->motor.speed = (int)value;
                } else if (strcmp(action, "direction") == 0) {
                    sharedMemory->motor.direction = (int)value;
                } else if (strcmp(action, "status") == 0) {
                    sharedMemory->motor.status = (int)value;
                }
            } else if (strcmp(component, "SENSOR") == 0) {
                if (strcmp(action, "temperature") == 0) {
                    sharedMemory->sensor.temperature = value;
                } else if (strcmp(action, "pressure") == 0) {
                    sharedMemory->sensor.pressure = (int)value;
                }
            }
            // You can extend this to handle communication and other components
        }
    }
}*/

void cleanup() {
    for (int i = 0; i < component_count; i++) {
        kill(pids[i], SIGTERM); // Send a termination signal
        waitpid(pids[i], NULL, 0); // Wait for child process to terminate
    }

    // Unmap the shared memory
    if (munmap(sharedMemory, sizeof(SharedMemory)) == -1) {
        perror("munmap");
        exit(1);
    }

    if (shm_unlink("/shared_memory") == -1) {
        perror("shm_unlink");
        exit(1);
    }
}



int main(int argc, char *argv[]) {

    if (argc != 2) { // Check if CLI arguments are valid
        printf("Usage: %s {scenario file}\n", argv[0]);
        return 1;
    }

    if (pthread_mutex_init(&lock, NULL) != 0) { // Initialise mutex lock
        printf("\nMutex initialization failed.\n");
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

    for (int i = 0; i < component_count; i++) {
        printf("Component number %d has type %s\n", i, components[i].type);
    }
    
    spawn_processes();

    //simulate_scenario(scenario_file);

    cleanup();

    fclose(scenario_file);

    return 0;
}
