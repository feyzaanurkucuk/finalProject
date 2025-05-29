#include<stdio.h>
#include<stdlib.h>
#include<signal.h>
#include<unistd.h>
#include<string.h>
#include<pthread.h>
#include<fcntl.h>
#include<errno.h>
#include<time.h>
#include<arpa/inet.h>
#include<semaphore.h>
#include<sys/stat.h>

#define MAX_CLIENTS 30
#define MAX_ROOMS 10
#define MAX_UPLOADS 5
#define MAX_QUEUE_SIZE 10
#define LOG_FILE "log.txt"
#define COLOR_GREEN   "\x1b[32m"
#define COLOR_RED     "\x1b[31m"
#define COLOR_RESET   "\x1b[0m"


char* userList[MAX_CLIENTS];
int user_count=0;


sem_t empty_slots;  
sem_t full_slots;   
sem_t upload_slots;
pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t user_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
volatile sig_atomic_t stop = 0;


typedef struct {
    char sender[16];
    char filename[128];
    char target[16];
    int client_fd;
    time_t enqueue_time;
} UploadTask;

UploadTask upload_queue[MAX_QUEUE_SIZE];
int queue_front = 0;
int queue_rear = 0;
int queue_count = 0;


typedef struct{
	int port;
	char *ip;
}ConnectionInfo;

typedef struct{
	char *username;
	char *room;
	int socket_fd;
	char *ip;
}Client;
Client clients[MAX_CLIENTS];
typedef struct{
    char name[32];
    char *users[MAX_CLIENTS];
    int user_count;
}Room;

Room roomList[MAX_ROOMS];
int room_count = 0;

void write_to_log(char *message){
	pthread_mutex_lock(&log_mutex);
	int fd = open(LOG_FILE,O_CREAT|O_APPEND|O_WRONLY,0644);

	if(fd == -1){
		perror("open");
		return;
	}
	time_t now;
	struct tm *local;
	char buffer[20];

	time(&now);
	local = localtime(&now);
	char finalMessage[256];
	strftime(buffer,sizeof(buffer),"%d-%m-%Y %H:%M:%S",local);
	snprintf(finalMessage,sizeof(finalMessage),"%s%s",buffer,message);
	write(fd,finalMessage,strlen(finalMessage));

	close(fd);
	pthread_mutex_unlock(&log_mutex);
}


void broadcast_shutdown_to_clients() {
    pthread_mutex_lock(&user_mutex);
    for (int i = 0; i < user_count; i++) {
        send(clients[i].socket_fd, "SERVER_SHUTDOWN", strlen("SERVER_SHUTDOWN"), 0);
        close(clients[i].socket_fd);
    }
    pthread_mutex_unlock(&user_mutex);
}
void signal_handler(int signo){
	if (signo == SIGINT) {
        printf("\n[SHUTDOWN] SIGINT received. Shutting down the server gracefully...\n");
        char msg_to_log[256];
        snprintf(msg_to_log,sizeof(msg_to_log),"[SHUTDOWN] SIGINT received. Disconnecting %d clients, saving logs.\n",user_count);
        write_to_log(msg_to_log);
        stop = 1;
        broadcast_shutdown_to_clients();
        close(STDIN_FILENO); 
    }

}
void func(ConnectionInfo connectionInfo){
	printf("(ip: %s), port: %d\n", connectionInfo.ip, connectionInfo.port);
}
void print_users() {
    printf("[User List] (%d total):\n", user_count);
    for (int i = 0; i < user_count; i++) {
        printf(" - %s\n", clients[i].username);
    }
}


void remove_from_room(char *username, char* current_room){
	for (int i = 0; i < room_count; i++) {
                    if (strcmp(roomList[i].name, current_room) == 0) {
                        Room *room = &roomList[i];
                        for (int j = 0; j < room->user_count; j++) {
                            if (strcmp(room->users[j], username) == 0) {
                                free(room->users[j]);
                                for (int k = j; k < room->user_count - 1; k++) {
                                    room->users[k] = room->users[k + 1];
                                }
                                room->user_count--;
                                break;
                            }
                        }
                        break;
                    }
                }

                for(int i=0; i<user_count;i++){
                	if(clients[i].room && strcmp(clients[i].username,username) == 0){
                		clients[i].room  =NULL;
                		break;
                	}
                }
}

char* join_room_command(char *username, int client_fd,char *current_room,char *team){
	char msg_to_log[256];
	

            pthread_mutex_lock(&user_mutex);

            // Leave current room
            if (current_room) {
                remove_from_room(username,current_room);
                for(int i=0; i<user_count;i++){
                	if(strcmp(clients[i].username,username) ==0){
                		clients[i].room=NULL;
                		break;
                	}
                }
            }

            // Join new room
            int found = 0, room_index = -1;
            for (int i = 0; i < room_count; i++) {
                if (strcmp(roomList[i].name, team) == 0) {
                    found = 1;
                    room_index = i;
                    break;
                }
            }

            if (!found && room_count < MAX_ROOMS) {
                strcpy(roomList[room_count].name, team);
                room_index = room_count;
                room_count++;
            }

            if (room_index != -1) {
                Room *room = &roomList[room_index];
                if (room->user_count < MAX_CLIENTS) {
			    room->users[room->user_count++] = strdup(username);
			} else {
			    send(client_fd, "[SERVER]: Room is full.\n", 27, 0);
			    pthread_mutex_unlock(&user_mutex);
			    return NULL;
			}
                //current_room = room->name;
                for (int k = 0; k < user_count; ++k) {
				    if (strcmp(clients[k].username, username) == 0) {
				        clients[k].room = room->name;
				        break;
				    }
				}
				if(current_room){
					sprintf(msg_to_log,"-[ROOM] user '%s' left room '%s', joined room '%s'\n", username, current_room,room->name);

				}

				else{
					
					sprintf(msg_to_log,"-[JOIN] user '%s' joined room '%s'\n", username, room->name);
				}
				
				current_room=room->name;
				write_to_log(msg_to_log);
				
				char fullMessage[128];
				sprintf(fullMessage,COLOR_RED"[Server]: You joined the room '%s'\n"COLOR_RESET,current_room);
				if (send(client_fd, fullMessage, strlen(fullMessage), 0) < 0) {
				    perror("send");
				}
                printf("[COMMAND] %s joined room '%s'\n", username, current_room);
            }

            pthread_mutex_unlock(&user_mutex);
            return current_room;
}


void leave_room_command(char* username, int client_fd, char *current_room){
	char msg_to_log[256];
	pthread_mutex_lock(&user_mutex);
            if (current_room) {
                remove_from_room(username,current_room);
                for(int i=0; i<user_count;i++){
                	if(strcmp(clients[i].username,username) ==0){
                		clients[i].room="";
                		break;
                	}
                }
                sprintf(msg_to_log,"User %s left room: %s\n", username, current_room);
				
				write_to_log(msg_to_log);

				char fullMessage[128];
				sprintf(fullMessage,COLOR_RED"[Server]: You left the room '%s'\n"COLOR_RESET,current_room);
				if (send(client_fd, fullMessage, strlen(fullMessage), 0) < 0) {
				    perror("send");
				}
                printf("[COMMAND]: User %s left room: %s\n", username, current_room);
                current_room = NULL;
            }
            pthread_mutex_unlock(&user_mutex);
}

void broadcast_command(char *username, int client_fd, char* current_room, char*msg){
	char msg_to_log[256];
	char *senderRoom = NULL;
			    pthread_mutex_lock(&user_mutex);

			    // Find sender's room
			    for (int i = 0; i < user_count; ++i) {
			        if (strcmp(clients[i].username, username) == 0) {
			            senderRoom = clients[i].room;
			            //printf("sender room: %s\n",senderRoom);
			            break;
			        }
			    }

			    if (!senderRoom) {
			        pthread_mutex_unlock(&user_mutex);
			        return;
			    }

			    char fullMessage[512];
			    snprintf(fullMessage, sizeof(fullMessage), "[%s] %s: %s\n", senderRoom, username, msg);
			    
			    // Send to everyone else in same room
			    for (int i = 0; i < user_count; ++i) {
			        if (clients[i].room && strcmp(clients[i].room, senderRoom) == 0 &&
			            strcmp(clients[i].username, username) != 0) {
			            send(clients[i].socket_fd, fullMessage, strlen(fullMessage), 0);
			        }
			    }
			    sprintf(msg_to_log,"-[BROADCAST] user '%s': %s\n", username,msg);
				
				write_to_log(msg_to_log);
				sprintf(fullMessage,COLOR_RED"[SERVER]: Message sent to room '%s'"COLOR_RESET,senderRoom);
				send(client_fd,fullMessage,strlen(fullMessage),0);

				printf("[COMMAND] %s broadcast to '%s'\n", username, current_room);
			    pthread_mutex_unlock(&user_mutex);
}

void exit_command(char *username,int client_fd){
    char msg_to_log[256];
    
    pthread_mutex_lock(&user_mutex);
    for (int i = 0; i < user_count; i++) {
        if (strcmp(clients[i].username, username) == 0) {
            // Odayla ilişiği kesmeden önce kullanıcı adı lazım olabilir
            remove_from_room(username, clients[i].room);

            // Artık kullanıcı silinebilir
            free(clients[i].ip);
            free(clients[i].username);
            if (clients[i].room) free(clients[i].room); // Eğer strdup ile alınmışsa

            // Listeyi sola kaydır
            for (int j = i; j < user_count - 1; j++) {
                clients[j] = clients[j + 1];
            }
            break;
        }
    }
    user_count--; 
    pthread_mutex_unlock(&user_mutex);

    printf("[DISCONNECT] Client %s disconnected.\n", username);
    sprintf(msg_to_log, "-[DISCONNECT] Client %s disconnected.\n", username);
    write_to_log(msg_to_log);

    sprintf(msg_to_log, COLOR_RED "[SERVER]: Disconnected. Goodbye!\n" COLOR_RESET);
    send(client_fd, msg_to_log, strlen(msg_to_log), 0);
}

void whisper_command(char *username, char* target, char *msg, int client_fd){
	pthread_mutex_lock(&user_mutex);
			    int flag = 0;
			        for(int i=0; i< user_count; i++){
			            if(strcmp(clients[i].username,target) == 0){
			            	char buffmsg[256];
			            	sprintf(buffmsg,"[%s]: %s",username,msg);
			            	send(clients[i].socket_fd,buffmsg, strlen(buffmsg),0);
			            	flag = 1;
			            	char log_msg[256];
							sprintf(log_msg, "-[WHISPER] from '%s' to '%s': %s\n", username, target, msg);
							write_to_log(log_msg);
			            	printf("[COMMAND] %s sent whisper to %s\n", username, target);
			            		break;
			            }
			        }
			       	if(flag == 0){
			       		char msg[] =COLOR_RED "[ERROR]: Username is not found"COLOR_RESET;
			           	send(client_fd,msg,strlen(msg),0);
			         }
			         else{
			         	char fullMessage[512];
			         	sprintf(fullMessage, COLOR_RED"[SERVER]: Whisper sent to %s\n"COLOR_RESET, target);
			         	send(client_fd,fullMessage,strlen(fullMessage),0);
			         }
			            pthread_mutex_unlock(&user_mutex);
}

void print_room_list(char *current_room) {
    printf("Room list:\n");
    for (int i = 0; i < room_count; i++) {
        Room *room = &roomList[i];
        printf("- %s (%d users)\n", room->name, room->user_count);
        if (current_room && strcmp(room->name, current_room) == 0) {
            for (int j = 0; j < room->user_count; j++) {
                printf("  * %s\n", room->users[j]);
            }
        }
    }
}


void enqueue_upload(UploadTask task,int client_fd) {
    sem_wait(&empty_slots); // Kuyrukta boş yer var mı bekle
    pthread_mutex_lock(&queue_mutex);

    upload_queue[queue_rear] = task;
    queue_rear = (queue_rear + 1) % MAX_QUEUE_SIZE;
    queue_count++;

    pthread_mutex_unlock(&queue_mutex);
    sem_post(&full_slots); // Dolu eleman sayısını arttır

    char log_entry[256];
    sprintf(log_entry,COLOR_RED"[SERVER]: File upload added to queue.\n"COLOR_RESET);
    send(client_fd, log_entry,strlen(log_entry), 0);
    sprintf(log_entry, "upload %s from %s added to queue. queue size: %d\n",
            task.filename, task.sender, queue_count);
    write_to_log(log_entry);
}


int dequeue_upload(UploadTask *task) {
    sem_wait(&full_slots); 
    pthread_mutex_lock(&queue_mutex);

    if (queue_count == 0) {
      
        pthread_mutex_unlock(&queue_mutex);
        sem_post(&full_slots); 
        return 0;
    }

    *task = upload_queue[queue_front];
    queue_front = (queue_front + 1) % MAX_QUEUE_SIZE;
    queue_count--;

    pthread_mutex_unlock(&queue_mutex);
    sem_post(&empty_slots); 

    return 1;
}


int file_exists_for_user(const char *filename, const char *target, char *new_filename) {
    int counter = 1;
    char base_path[256];
    snprintf(base_path, sizeof(base_path), "uploads/%s", target); // uploads/<target> klasörü

    char temp[512];
    snprintf(temp, sizeof(temp), "%s/%s", base_path, filename);

    struct stat stat_buf;

    while (stat(temp, &stat_buf) == 0) {
        const char *dot = strrchr(filename, '.');
        if (!dot) dot = filename + strlen(filename); // uzantı yoksa en sona ekle
        int name_len = dot - filename;

        snprintf(temp, sizeof(temp), "%s/%.*s_%d%s",
                 base_path, name_len, filename,
                 counter++, dot);
    }

    const char *final_name = strrchr(temp, '/');
    if (final_name)
        strcpy(new_filename, final_name + 1);
    else
        strcpy(new_filename, temp);

    return counter > 1;
}

void* handle_upload(void *arg) {
    UploadTask task = *(UploadTask*)arg;
    free(arg);

    sem_wait(&upload_slots);

    time_t now = time(NULL);
    double wait_duration = difftime(now, task.enqueue_time);

    char log_entry[512];
    char final_filename[256];

    // Klasörü oluştur (varsa hata vermez)
    char user_dir[256];
    snprintf(user_dir, sizeof(user_dir), "uploads/%s", task.target);
    mkdir("uploads", 0755);
    mkdir(user_dir, 0755);

    int renamed = file_exists_for_user(task.filename, task.target, final_filename);

    if (renamed) {
        char log[128];
        sprintf(log, "[FILE] Conflict : '%s' received twice -> renamed '%s'\n",
                task.filename, final_filename);
        write_to_log(log);
        //strcpy(task.filename, final_filename);
    }

    sprintf(log_entry, "[UPLOAD START] %s from %s to %s after %.2f seconds in queue.\n",
            final_filename, task.sender, task.target, wait_duration);
    write_to_log(log_entry);

    // Gerçek dosya transferi (read/write ile)
    char filepath[512];
    snprintf(filepath, sizeof(filepath), "uploads/%s/%s", task.target, final_filename);

    int src_fd = open(task.filename, O_RDONLY);
    if (src_fd < 0) {
        perror("open source file");
        sem_post(&upload_slots);
        return NULL;
    }

    int dest_fd = open(filepath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (dest_fd < 0) {
        perror("open destination file");
        close(src_fd);
        sem_post(&upload_slots);
        return NULL;
    }

    char buffer[4096];
    ssize_t bytes;
    while ((bytes = read(src_fd, buffer, sizeof(buffer))) > 0) {
        write(dest_fd, buffer, bytes);
    }

    close(src_fd);
    close(dest_fd);

    sprintf(log_entry, "[UPLOAD COMPLETE] %s from %s to %s\n",
            task.filename, task.sender, task.target);
    write_to_log(log_entry);

    sem_post(&upload_slots);
    return NULL;
}

void* upload_queue_handler(void *arg) {
    while (!stop) {
        UploadTask task;
        if (dequeue_upload(&task)) {
            UploadTask *task_ptr = malloc(sizeof(UploadTask));
            *task_ptr = task;
            pthread_t upload_thread;
            pthread_create(&upload_thread, NULL, handle_upload, task_ptr);
            pthread_detach(upload_thread);
        } else {
            usleep(100000); 
        }
    }
    return NULL;
}
void* handle_client(void *arg) {
    int client_fd = *(int*)arg;
    free(arg);  

    char message[256];
    char username[16];
    char msg_to_log[256];
    char *current_room = NULL;
    ssize_t bytes_read = read(client_fd, message, sizeof(message) - 1);
    if (bytes_read <= 0) {
        printf(COLOR_RED "Client disconnected or read error.\n" COLOR_RESET);
        close(client_fd);
        pthread_exit(NULL);
    }

    message[bytes_read] = '\0';
    char* command = strtok(message, " \n");
    if (command && strcmp(command, "CONNECT") == 0) {
        char *token = strtok(NULL, " \n");
        if (token) {
            strcpy(username, token);
            pthread_mutex_lock(&user_mutex);
            for(int i=0; i<user_count; i++){
            	if(strcmp(clients[i].username,username) ==0 ){
            		//username already exists.
            		char msg[] = COLOR_RED "[ERROR]-Username already taken.\n"COLOR_RESET;
            		send(client_fd,msg,strlen(msg),0);
            		sprintf(msg_to_log, COLOR_RED "[REJECTED]-Duplicate username attempted: %s\n" COLOR_RESET,username);
            		write_to_log(msg_to_log);
            		pthread_mutex_unlock(&user_mutex);
            		close(client_fd);
            		pthread_exit(NULL);
            	}
            }
            token= strtok(NULL," \n");
            //printf("token: %s\n",token);
            clients[user_count].ip=strdup(token);
            clients[user_count].username = strdup(username);
			clients[user_count].socket_fd = client_fd;
			clients[user_count].room = NULL;
			user_count++;
			sprintf(msg_to_log,"-[LOGIN] user '%s' connected from '%s'\n", username, token);//ip yazmali
				
				write_to_log(msg_to_log);
			printf(COLOR_GREEN"[CONNECT] New client connected: %s from %s\n"COLOR_RESET,username,token);
            //print_users();
            pthread_mutex_unlock(&user_mutex);
        }
    }

    while (!stop) {
    	
    	strcpy(msg_to_log,"");
        bytes_read = read(client_fd, message, sizeof(message) - 1);
        if (bytes_read <= 0) {
           	printf("[DISCONNECT] Client %s disconnected.\n",username);
            pthread_mutex_lock(&user_mutex);
	            for(int i=0; i<user_count; i++){
	        	if(strcmp(clients[i].username,username) == 0){
	        		current_room = clients[i].room;
	        		remove_from_room(username,current_room);
	        		//print_room_list(current_room);
	        		free(clients[i].ip);
        			free(clients[i].username);
	        		for(int j=i; j<user_count-1; j++){
	        			clients[j] = clients[j+1];
	        		}
	        		user_count --;
			        pthread_mutex_unlock(&user_mutex);
			        close(client_fd);
			        pthread_exit(NULL);
	        		
	        	}
	        }

	        
        }
        message[bytes_read] = '\0';
        char* command = strtok(message, " \n");
        if (!command) continue;

        if (strcmp(command, "/join") == 0) {
        	char *team = strtok(NULL, " \n");
            if (!team) continue;
            current_room= join_room_command(username,client_fd,current_room,team);

            
        } else if (strcmp(command, "/leave") == 0) {
            leave_room_command(username,client_fd,current_room);
            //print_room_list(current_room);
            current_room =NULL;
        }
        else if (strcmp(command, "/broadcast") == 0) {
			    char *msg = strtok(NULL, "\n");
			    if (!msg) continue;
			    //printf("message: %s",msg);
			    broadcast_command(username,client_fd,current_room,msg);
			    
		}
		else if (strcmp(command, "/whisper") == 0) {
			    char *target = strtok(NULL, " ");
			    char *msg = strtok(NULL, "\n");
			    //printf("user: %s whisper to %s: %s\n", username, target, msg);
			    whisper_command(username,target,msg,client_fd);
			    
		} 
		else if (strcmp(command, "/sendfile") == 0) {
		    char *filename = strtok(NULL, " ");
		    char *to_user = strtok(NULL, " \n");
		    if (!filename || !to_user) continue;

		    struct stat st;
		if (stat(filename, &st) != 0) {
			char msg[] = COLOR_RED "[ERROR]: File does not exist.\n"COLOR_RESET;
		   	send(client_fd, msg, strlen(msg), 0);
		    continue;
		}

		if (st.st_size > 3 * 1024 * 1024) {
			char msg[] = COLOR_RED"[ERROR]: File size exceeds 3MB limit.\n"COLOR_RESET;
		    send(client_fd, msg, strlen(msg), 0);
		    sprintf(msg_to_log,COLOR_RED"[ERROR]: File '%s' from '%s' exceeds 3MB limit.\n"COLOR_RESET,filename,username);
		    write_to_log(msg_to_log);
		    continue;
		}
		int user_found = 0;
			pthread_mutex_lock(&user_mutex);
			for (int i = 0; i < user_count; ++i) {
			    if (strcmp(clients[i].username, to_user) == 0) {
			        user_found = 1;
			        break;
			    }
			}
			pthread_mutex_unlock(&user_mutex);

			if (!user_found) {
				char msg[] = COLOR_RED "[ERROR]: Target user not found.\n" COLOR_RESET;
				send(client_fd, msg, strlen(msg), 0);

			   
			    sprintf(msg_to_log,COLOR_RED"[ERROR]: Target user not found.\n"COLOR_RESET);
		    write_to_log(msg_to_log);
			    continue;
			}



		    UploadTask task;
		    strncpy(task.sender, username, sizeof(task.sender));
		    strncpy(task.filename, filename, sizeof(task.filename));
		    strncpy(task.target, to_user, sizeof(task.target));
		    task.client_fd = client_fd;
		    task.enqueue_time= time(NULL);
		    enqueue_upload(task,client_fd);

		    //send(client_fd, "[Server]: File added to upload queue.\n", 39, 0);
		}

		else if (strcmp(command, "/exit") == 0) {
				exit_command(username,client_fd);
			    //print_room_list(current_room);

			    break;
		}
	}

    close(client_fd);

    pthread_exit(NULL);
}
//MAIN FUNCTION

int main(int argc, char *argv[]){


	ConnectionInfo connectionInfo;
	
	connectionInfo.port = atoi(argv[1]);
	signal(SIGINT, signal_handler);

	int server_fd, client_fd;
	struct sockaddr_in address;
	socklen_t addrlen = sizeof(address);

	server_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (server_fd < 0) {
	    perror("Socket failed");
	    exit(EXIT_FAILURE);
	}
	int opt = 1;
	if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
	    perror("setsockopt");
	    exit(EXIT_FAILURE);
	}

	address.sin_family = AF_INET;
	address.sin_addr.s_addr = INADDR_ANY;
	address.sin_port = htons(connectionInfo.port);
        connectionInfo.ip = INADDR_ANY;
	if(bind(server_fd, (struct sockaddr *) &address, sizeof(address)) < 0){
		perror("Bind failed.");
		exit(EXIT_FAILURE);
	}

	if(listen(server_fd, 10 )< 0){
		perror("Listen failed");
		exit(EXIT_FAILURE);
	}
	printf("[INFO] Server listening on port: %d\n", connectionInfo.port);
	
	sem_init(&empty_slots, 0, MAX_QUEUE_SIZE); 
	sem_init(&full_slots, 0, 0);              
	sem_init(&upload_slots, 0, MAX_UPLOADS); 

    pthread_t queue_handler_thread;
    pthread_create(&queue_handler_thread, NULL, upload_queue_handler, NULL);
    pthread_detach(queue_handler_thread);
	int flags = fcntl(server_fd, F_GETFL, 0);
	fcntl(server_fd, F_SETFL, flags | O_NONBLOCK);
	while(!stop){
		
		char buffer[1024] = {0};
		 client_fd = accept(server_fd, (struct sockaddr*)&address, &addrlen);
        if (client_fd < 0) {
        	if (errno == EWOULDBLOCK || errno == EAGAIN) {
            
            usleep(100000); // 100 ms
            continue;
        } else if (stop) {
            break;
        } else {
            perror("accept");
            continue;
        }
    }
    int* new_socket = malloc(sizeof(int));
        if (!new_socket) {
            perror("malloc failed");
            close(client_fd);
            continue;
        }

        *new_socket = client_fd;

        pthread_t client_thread;
        if (pthread_create(&client_thread, NULL, handle_client, new_socket) != 0) {
            perror("pthread_create failed");
            close(client_fd);
            free(new_socket);
        } else {
            pthread_detach(client_thread); 
        }
       
       
	}

	 close(server_fd);


	pthread_mutex_lock(&user_mutex);
	for (int i = 0; i < user_count; ++i) {
	    free(clients[i].username);
	    free(clients[i].ip); 
	    close(clients[i].socket_fd);
	}
	for (int i = 0; i < room_count; i++) {
            Room *room = &roomList[i];
            for (int j = 0; j < room->user_count; j++) {
                free(room->users[j]);
            }
        }

	pthread_mutex_unlock(&user_mutex);
    pthread_mutex_destroy(&user_mutex);
    pthread_mutex_destroy(&log_mutex);

	return 0;
}
