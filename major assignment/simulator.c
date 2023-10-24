#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>

#define MAX_COMPONENTS 200 // Maximum number of components for all types combined
#define MAP_ANONYMOUS MAP_ANON

typedef struct {
    // Shared memory structures for each component
    // Add them as defined in the specs
    struct {
        int speed;         // Motor speed
        int direction;     // Motor direction: 0 for forward, 1 for reverse
        int status;        // Motor status: 0 for off, 1 for on
    } motor;

    // If you have a sensor component
    struct {
        float temperature;     // Temperature reading
        int pressure;          // Pressure reading
    } sensor;

    // If you have a communication component
    struct {
        char message[100];     // Shared message buffer
        int message_length;    // Length of the current message
    } communication;

} SharedMemory;

SharedMemory *sharedMemory;

typedef struct {
    char type;
    int config;
} Component;

Component components[MAX_COMPONENTS];
int component_count = 0;

void parse_init_section(FILE *scenario_file) {
    char line[256];
    char type;
    int config;
    while (fgets(line, sizeof(line), scenario_file) && strncmp(line, "SCENARIO", 8) != 0) {
        if (sscanf(line, "%c %d", &type, &config) == 2) {
            components[component_count].type = type;
            components[component_count].config = config;
            component_count++;
        }
    }
}

void create_shared_memory() {
    // Create a shared memory segment for the simulator and components
    sharedMemory = mmap(NULL, sizeof(SharedMemory), PROT_READ | PROT_WRITE, MAP_SHARED | 0x20, -1, 0);
    // Initialize the shared memory structures here
}

void initialize_shared_memory() {
    sharedMemory->motor.speed = 0;
    sharedMemory->motor.direction = 0;
    sharedMemory->motor.status = 0;

    sharedMemory->sensor.temperature = 20.0; // Default to room temperature
    sharedMemory->sensor.pressure = 1000; // Default atmospheric pressure in hPa

    memset(sharedMemory->communication.message, 0, sizeof(sharedMemory->communication.message));
    sharedMemory->communication.message_length = 0;
}

pid_t pids[MAX_COMPONENTS];

void spawn_processes() {
    for (int i = 0; i < component_count; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork failed");
            exit(1);
        } else if (pid == 0) { // Child process
            char config_str[10];
            snprintf(config_str, sizeof(config_str), "%d", components[i].config);
            char *binary;
            switch (components[i].type) {
                case 'A': binary = "./A.out"; break;
                case 'B': binary = "./B.out"; break;
                case 'C': binary = "./C.out"; break;
                default:
                    fprintf(stderr, "Unknown component type\n");
                    exit(1);
            }
            execl(binary, binary, config_str, NULL);
            // If execl returns, there was an error
            perror("execl failed");
            exit(1);
        } else { // Parent process
            pids[i] = pid;
        }
    }
}

void simulate_scenario_section(FILE *scenario_file) {
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
}

void cleanup() {
    for (int i = 0; i < component_count; i++) {
        kill(pids[i], SIGTERM); // Send a termination signal
        waitpid(pids[i], NULL, 0); // Wait for child process to terminate
    }
    // Unmap shared memory
    munmap(sharedMemory, sizeof(SharedMemory));
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        printf("Usage: %s {scenario file}\n", argv[0]);
        return 1;
    }

    FILE *scenario_file = fopen(argv[1], "r");
    if (!scenario_file) {
        perror("Failed to open scenario file");
        return 1;
    }

    parse_init_section(scenario_file);
    create_shared_memory();
    initialize_shared_memory();
    spawn_processes();
    simulate_scenario_section(scenario_file);
    cleanup();

    fclose(scenario_file);
    return 0;
}
