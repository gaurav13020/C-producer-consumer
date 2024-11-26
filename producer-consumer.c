#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <semaphore.h>
#include <fcntl.h>

#define MAX_MESSAGE_LENGTH 1024
#define SOCKET_PATH "/tmp/producer_consumer_socket"

// Shared data structure for both socket and shared memory approaches
typedef struct {
    char message[MAX_MESSAGE_LENGTH];
    int is_empty;
} Queue;

// Command-line argument parsing structure
typedef struct {
    int is_producer;
    int is_consumer;
    char* message;
    int queue_depth;
    int use_socket;
    int use_shared_memory;
    int enable_echo;
} Arguments;

// Function prototypes
void parse_arguments(int argc, char* argv[], Arguments* args);
void producer_socket(Arguments* args);
void consumer_socket(Arguments* args);
void producer_shared_memory(Arguments* args);
void consumer_shared_memory(Arguments* args);

int main(int argc, char* argv[]) {
    Arguments args = {0};
    parse_arguments(argc, argv, &args);

    // Validate arguments
    if (!(args.is_producer ^ args.is_consumer)) {
        fprintf(stderr, "Error: Must specify exactly one of -p or -c\n");
        exit(1);
    }

    if (args.use_socket && args.use_shared_memory) {
        fprintf(stderr, "Error: Cannot use both socket and shared memory\n");
        exit(1);
    }

    if (!args.use_socket && !args.use_shared_memory) {
        fprintf(stderr, "Error: Must specify either -u or -s\n");
        exit(1);
    }

    // Execute based on mode
    if (args.use_socket) {
        if (args.is_producer) {
            producer_socket(&args);
        } else {
            consumer_socket(&args);
        }
    } else {
        if (args.is_producer) {
            producer_shared_memory(&args);
        } else {
            consumer_shared_memory(&args);
        }
    }

    return 0;
}

void parse_arguments(int argc, char* argv[], Arguments* args) {
    int opt;
    while ((opt = getopt(argc, argv, "pcm:q:use")) != -1) {
        switch (opt) {
            case 'p':
                args->is_producer = 1;
                break;
            case 'c':
                args->is_consumer = 1;
                break;
            case 'm':
                args->message = optarg;
                break;
            case 'q':
                args->queue_depth = atoi(optarg);
                break;
            case 'u':
                args->use_socket = 1;
                break;
            case 's':
                args->use_shared_memory = 1;
                break;
            case 'e':
                args->enable_echo = 1;
                break;
            default:
                fprintf(stderr, "Usage: %s -p|-c -m message -q depth -u|-s [-e]\n", argv[0]);
                exit(1);
        }
    }

    // Validate required arguments
    if (!args->message) {
        fprintf(stderr, "Error: Message (-m) is required\n");
        exit(1);
    }
    if (args->queue_depth <= 0) {
        fprintf(stderr, "Error: Queue depth (-q) must be positive\n");
        exit(1);
    }
}

void producer_socket(Arguments* args) {
    int sock;
    struct sockaddr_un server;

    // Create socket
    sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock == -1) {
        perror("socket");
        exit(1);
    }

    // Set up socket address
    server.sun_family = AF_UNIX;
    strcpy(server.sun_path, SOCKET_PATH);

    // Connect to the socket
    if (connect(sock, (struct sockaddr*)&server, sizeof(struct sockaddr_un)) == -1) {
        perror("connect");
        exit(1);
    }

    // Send message
    if (send(sock, args->message, strlen(args->message), 0) == -1) {
        perror("send");
        exit(1);
    }

    if (args->enable_echo) {
        printf("%s\n", args->message);
    }

    close(sock);
}

void consumer_socket(Arguments* args) {
    int sock, client_sock;
    struct sockaddr_un server, client;
    socklen_t client_len;
    char buffer[MAX_MESSAGE_LENGTH];

    // Create socket
    sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock == -1) {
        perror("socket");
        exit(1);
    }

    // Remove existing socket file if it exists
    unlink(SOCKET_PATH);

    // Set up socket address
    server.sun_family = AF_UNIX;
    strcpy(server.sun_path, SOCKET_PATH);

    // Bind socket
    if (bind(sock, (struct sockaddr*)&server, sizeof(struct sockaddr_un)) == -1) {
        perror("bind");
        exit(1);
    }

    // Listen for connections
    if (listen(sock, 1) == -1) {
        perror("listen");
        exit(1);
    }

    // Accept connection
    client_len = sizeof(client);
    client_sock = accept(sock, (struct sockaddr*)&client, &client_len);
    if (client_sock == -1) {
        perror("accept");
        exit(1);
    }

    // Receive message
    ssize_t received = recv(client_sock, buffer, sizeof(buffer), 0);
    if (received == -1) {
        perror("recv");
        exit(1);
    }
    buffer[received] = '\0';

    if (args->enable_echo) {
        printf("%s\n", buffer);
    }

    close(client_sock);
    close(sock);
    unlink(SOCKET_PATH);
}

void producer_shared_memory(Arguments* args) {
    key_t key;
    int shmid;
    Queue *shared_queue;
    sem_t *mutex, *empty, *full;

    // Generate unique key
    key = ftok("/tmp", 'Q');
    if (key == -1) {
        perror("ftok");
        exit(1);
    }

    // Create shared memory segment
    shmid = shmget(key, sizeof(Queue), IPC_CREAT | 0666);
    if (shmid == -1) {
        perror("shmget");
        exit(1);
    }

    // Attach shared memory
    shared_queue = (Queue*)shmat(shmid, NULL, 0);
    if (shared_queue == (void*)-1) {
        perror("shmat");
        exit(1);
    }

    // Create semaphores
    mutex = sem_open("/mutex_sem", O_CREAT, 0666, 1);
    empty = sem_open("/empty_sem", O_CREAT, 0666, args->queue_depth);
    full = sem_open("/full_sem", O_CREAT, 0666, 0);

    // Wait for empty slot
    sem_wait(empty);
    sem_wait(mutex);

    // Produce message
    strcpy(shared_queue->message, args->message);
    shared_queue->is_empty = 0;

    if (args->enable_echo) {
        printf("%s\n", args->message);
    }

    // Signal semaphores
    sem_post(mutex);
    sem_post(full);

    // Detach shared memory
    shmdt(shared_queue);

    // Close semaphores
    sem_close(mutex);
    sem_close(empty);
    sem_close(full);
}

void consumer_shared_memory(Arguments* args) {
    key_t key;
    int shmid;
    Queue *shared_queue;
    sem_t *mutex, *empty, *full;
    char buffer[MAX_MESSAGE_LENGTH];

    // Generate unique key
    key = ftok("/tmp", 'Q');
    if (key == -1) {
        perror("ftok");
        exit(1);
    }

    // Access shared memory segment
    shmid = shmget(key, sizeof(Queue), 0666);
    if (shmid == -1) {
        perror("shmget");
        exit(1);
    }

    // Attach shared memory
    shared_queue = (Queue*)shmat(shmid, NULL, 0);
    if (shared_queue == (void*)-1) {
        perror("shmat");
        exit(1);
    }

    // Open semaphores
    mutex = sem_open("/mutex_sem", 0);
    empty = sem_open("/empty_sem", 0);
    full = sem_open("/full_sem", 0);

    // Wait for full slot
    sem_wait(full);
    sem_wait(mutex);

    // Consume message
    strcpy(buffer, shared_queue->message);
    shared_queue->is_empty = 1;

    if (args->enable_echo) {
        printf("%s\n", buffer);
    }

    // Signal semaphores
    sem_post(mutex);
    sem_post(empty);

    // Detach shared memory
    shmdt(shared_queue);

    // Close semaphores
    sem_close(mutex);
    sem_close(empty);
    sem_close(full);

    // Optional: Remove shared memory and semaphores
    shmctl(shmid, IPC_RMID, NULL);
    sem_unlink("/mutex_sem");
    sem_unlink("/empty_sem");
    sem_unlink("/full_sem");
}