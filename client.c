#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <ctype.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <ifaddrs.h>
#include <pthread.h>

typedef struct {
    int port;
    char *ip;
} ConnectionInfo;

volatile sig_atomic_t stop = 0;



char *getIpAddress(){
    struct ifaddrs *ifaddr, *ifa;

    static char ip[INET_ADDRSTRLEN];

    if(getifaddrs(&ifaddr) == -1){
        perror("getifaddrs");
       return NULL;
    }

    for(ifa = ifaddr; ifa!=NULL; ifa = ifa-> ifa_next){
        if(ifa->ifa_addr == NULL)
            continue;

        if(ifa->ifa_addr->sa_family == AF_INET){
            void *addr = &((struct sockaddr_in*)ifa->ifa_addr)->sin_addr;
            inet_ntop(AF_INET,addr,ip,sizeof(ip));

            if(strcmp(ip,"127.0.0.1") != 0){
                freeifaddrs(ifaddr);
                return ip;
            }
           
        }
    }

     freeifaddrs(ifaddr);
            return NULL;
}

void signal_handler(int signo){
    if (signo == SIGINT) {
        printf("\n[INFO] SIGINT received. Shutting down the server gracefully...\n");
        stop = 1;

        close(STDIN_FILENO); 
    }
}

int is_alphanumeric(const char *str) {
    for (int i = 0; str[i] != '\0'; i++) {
        if (!isalnum(str[i])) {
            return 0;
        }
    }
    return 1;
}

// Thread function to receive messages from the server
void* receive_messages(void* arg) {
    int client_socket = *(int*)arg;
    char buffer[1024];

    while (!stop) {
        memset(buffer, 0, sizeof(buffer));
        ssize_t bytes_received = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
        if (bytes_received <= 0) {
            //printf("\n[INFO] Server closed the connection.\n");
            close(client_socket);
            exit(0);
        } else {
            buffer[bytes_received] = '\0';
            if (strcmp(buffer, "SERVER_SHUTDOWN") == 0) {
                printf("\n[INFO] Server is shutting down. Disconnecting...\n");
                close(client_socket);
                exit(0);
            } else {
                printf("%s\n> ", buffer);
                fflush(stdout);
            }
        }

    }

    return NULL;
}

int send_message(ConnectionInfo connectionInfo) {
    int client_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (client_socket == -1) {
        perror("socket");
        return 1;
    }

    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_port = htons(connectionInfo.port);
    if (inet_pton(AF_INET, connectionInfo.ip, &address.sin_addr) <= 0) {
        perror("inet_pton");
        close(client_socket);
        return 1;
    }

    if (connect(client_socket, (struct sockaddr *)&address, sizeof(address)) == -1) {
        perror("connect");
        close(client_socket);
        return 1;
    }

    // Ask for username
    char username[18] = {0}; 
    char message[64];
    ssize_t bytesRead;

    printf("Enter your username: ");
    fflush(stdout);
    bytesRead = read(0, username, sizeof(username) - 1);
    if (bytesRead < 0 && stop==0) {
        perror("read failed");
        return 1;
    }

    if (username[bytesRead - 1] == '\n') {
        username[bytesRead - 1] = '\0';
        bytesRead--;
    } else {
        char ch;
        while (read(0, &ch, 1) == 1 && ch != '\n');
    }

    if (bytesRead > 16) {
        printf("Username too long. Maximum allowed is 16 characters.\n");
        return 1;
    }

    if (!is_alphanumeric(username)) {
        printf("Invalid username. Only alphanumeric characters are allowed.\n");
        return 1;
    }
    char *ip = getIpAddress();
    sprintf(message, "CONNECT %s %s", username,ip);
    if (write(client_socket, message, strlen(message)) < 0) {
        perror("write");
        return 1;
    }

    // Start thread to listen for server messages
    pthread_t recv_thread;
    if (pthread_create(&recv_thread, NULL, receive_messages, &client_socket) != 0) {
        perror("pthread_create");
        close(client_socket);
        return 1;
    }

    // Main loop for user input
    while (!stop) {
        printf("> ");
        fflush(stdout);

        char buffer[1024] = {0};
        bytesRead = read(0, buffer, sizeof(buffer) - 1);
        if (bytesRead < 0 && stop ==0) {
            perror("read failed");
            return 1;
        }

        if (buffer[bytesRead - 1] == '\n') {
            buffer[bytesRead - 1] = '\0';
        }

        if (strlen(buffer) == 0) continue;

        if (buffer[0] == '/') {
            
            if (write(client_socket, buffer, strlen(buffer)) < 0) {
                perror("write");
                return 1;
            }
        } else {
            printf("Invalid input. Commands must start with '/'.\n");
        }
    }

    close(client_socket);
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        printf("Usage: %s <IP> <PORT>\n", argv[0]);
        return 1;
    }

    signal(SIGINT, signal_handler);

    ConnectionInfo connectionInfo;
    connectionInfo.ip = argv[1];
    connectionInfo.port = atoi(argv[2]);

    return send_message(connectionInfo);
}
