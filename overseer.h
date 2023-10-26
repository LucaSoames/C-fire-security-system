#ifndef OVERSEER_H
#define OVERSEER_H

#include <netinet/in.h>
#include <pthread.h>
#include <arpa/inet.h>

#define MAX_DOORS 50
#define MAX_CARD_READERS 50
#define PORT 8080


typedef struct {
    char id[50];
    char address[50];
    int port;
    char type[15]; 
} Door;

typedef struct {
    char id[50];
    char address[50];
    int port;
} CardReader;

typedef struct {
    char id[50];
    char address[50];
    int port;
} FireAlarm;

typedef struct {
    char id[50];
    char address[50];
    int port;
} Simulator;
/**
 * Initialize the overseer listening on the specified port.
 * @param port The port to listen on.
 * @return The socket descriptor if successful, -1 otherwise.
 */
int init_overseer(int port);

/**
 * The main function for the TCP server thread. It listens for connections
 * and processes incoming messages.
 * @param arg The socket descriptor as void pointer.
 * @return NULL.
 */
void* tcp_server_thread(void* arg);

/**
 * Register a door with the provided message. The message should contain 
 * the necessary details about the door.
 * @param msg The message containing the details of the door.
 */
void register_door(char* msg);

/**
 * Find or add a door to the list of tracked doors. 
 * @param new_door The door details.
 * @return The index at which the door was added or found, -1 if an error occurred.
 */
int find_or_add_door(Door new_door);

void register_device(char* msg);

int find_or_add_cardReader(CardReader cardReader);

int find_or_add_fireAlarm(FireAlarm fireAlarm);

int find_or_add_simulator(Simulator simulator);

void* udp_server_thread(void* arg);

void cleanup_resources();

void process_udp_message(char* msg);

void command_line_interface();

void initialize_global_data();

void register_card_reader(char* msg);

int init_udp_server(char* address_port);

int init_tcp_server(char* address_port);

void register_fire_alarm(char* msg);

void send_all_saved_doors_to_firealarm();

void send_door_to_fire_alarm(Door door);

int is_fire_alarm_registered();
#endif // OVERSEER_H
