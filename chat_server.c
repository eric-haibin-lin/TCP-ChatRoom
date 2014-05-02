#include "chat.h"
#include "chat_server.h"
#include <string.h>
#include <signal.h>
#include <assert.h> 
#include <pthread.h>
#include <semaphore.h>
  
//THE TAB SPACE OF THIS FILE IS SET TO 4 TO BE MORE READABLE
static char banner[] =
"\n\n\
/*****************************************************************/\n\
/*    CSIS0234_COMP3234 Computer and Communication Networks      */\n\
/*    Programming Assignment                                     */\n\
/*    Client/Server Application - Mutli-thread Chat Server       */\n\
/*                                                               */\n\
/*    USAGE:  ./chat_server    [port]                            */\n\
/*            Press <Ctrl + C> to terminate the server           */\n\
/*****************************************************************/\n\
\n\n";

/* 
 * Debug option 
 * In case you do not need debug information, just comment out it.
 */
//#define CSIS0234_COMP3234_DEBUG

/* 
 * Use DEBUG_PRINT to print debugging info
 */
#ifdef CSIS0234_COMP3234_DEBUG
#define DEBUG_PRINT(_f, _a...) \
 do { \
 	printf("[debug]<%s> " _f "\n", __func__, ## _a); \
 } while (0)
#else
#define DEBUG_PRINT(_f, _a...) do {} while (0)
#endif

#define MYPORT 50395    // the server port number

#define BACKLOG 10     // how many pending connections queue will hold
#define MAXDATASIZE 100 // max number of bytes we can get at once 

void server_init(int);
int check_duplicate_user(char*);
void server_run(void);
void *client_thread_fn(struct chat_client *);
void *broadcast_thread_fn(void *);
void create_client(struct chat_client *);
void shutdown_handler(int);

struct chat_server * chatserver;
struct client_queue * clients_queue;
struct chatmsg_queue * chatmsgs_queue;
pthread_t broadcast_thd;

int port = DEFAULT_LISTEN_PORT;

char buf[MAXDATASIZE]; 
int sockfd;
socklen_t sin_size;


/*
 * The main server process
 */
 int main(int argc, char **argv)
 {
 	printf("%s\n", banner);
	
 	if (argc > 1) {
 		port = atoi(argv[1]);
 	} else {
 		port = MYPORT;
 	}

 	printf("Starting chat server ...\n");

    // Register "Control + C" signal handler
 	signal(SIGINT, shutdown_handler);
 	signal(SIGTERM, shutdown_handler);

    // Initilize the server
 	server_init(port);

    // Run the server
 	server_run();

 	return 0;
 }


/*
 * Initilize the chatserver
 */
 void server_init(int port)
 {
    // TO DO:
    // Initilize all related data structures
    // 1. semaphores, mutex, pointers, etc.
    // 2. create the broadcast_thread

 	int i;

	//Create a socket for the server.
 	if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
 		perror("socket");
 		exit(1);
 	}

 	DEBUG_PRINT("\t Initialize chat server ...");
 	chatserver = malloc (sizeof (struct chat_server));

	/* Prepare the socket address structure of the server 		*/    
    (chatserver->address).sin_family = AF_INET;      		   	// host byte order
    (chatserver->address).sin_port = htons(port);     			// short, network byte order
    (chatserver->address).sin_addr.s_addr = htonl(INADDR_ANY); 	// automatically fill with my IP address
    memset(&((chatserver->address).sin_zero), '\0', 8); 			// zero the rest of the struct

	//Set the length of socket address	
    sin_size = sizeof(struct sockaddr_in);

	//Register the socket with server system.
    if (bind(sockfd, (struct sockaddr *) &(chatserver->address), sizeof(struct sockaddr)) == -1) {
    	perror("bind");
    	exit(1);
    }

    clients_queue = & (chatserver->room.clientQ);
    chatmsgs_queue = & (chatserver->room.chatmsgQ);

	//Set the number of online clients to 0.
    clients_queue->count = 0; 
    clients_queue->head = malloc (sizeof(struct chat_client));	
    clients_queue->head->prev = NULL;
    clients_queue->head->next = NULL;
	clients_queue->tail = clients_queue->head->next;
	
	
	// Get space more message slots
    for (i = 0; i < MAX_QUEUE_MSG; i++){
    	chatmsgs_queue->slots[i] = malloc (sizeof(char) * CONTENT_LENGTH);
    }

    chatmsgs_queue->tail = 0;
    chatmsgs_queue->head = 0;
	
	//init semaphore
	sem_init(&(clients_queue->cq_lock), 0, 1);
	sem_init(&(chatmsgs_queue->buffer_empty), 0, 0);
	sem_init(&(chatmsgs_queue->buffer_full), 0, MAX_QUEUE_MSG);
	sem_init(&(chatmsgs_queue->mq_lock), 0, 1);

	pthread_create(&broadcast_thd, NULL, (void *) &broadcast_thread_fn, (void *) (broadcast_thd));
	
	printf("Chat server is up and listening at port %d\n", port);
} 


/*
 * Check if user_name is already used in the chat room. 
 */
 int check_duplicate_user(char* user_name){
 	struct chat_client * current_client;
 	int count = 0;

 	current_client = clients_queue->head;
 	while (count <= clients_queue->count){

		DEBUG_PRINT("\tNow checking: %s, %d",current_client->client_name, strcmp(current_client->client_name, user_name));
		//If the user name is equal, return false.
 		if (strcmp(current_client->client_name, user_name) == 0){
 			return 0;
		}
 		current_client = current_client->next;
 		count ++;
 	}
 	return 1;
 }
//This function creates a new client struct and save it in chatroom
 void create_client(struct chat_client * new_client){

	DEBUG_PRINT("\t enter create_client!");
 	struct chat_client * current_client;

	current_client = clients_queue->head;
	while (current_client->next != NULL){
		current_client = current_client->next;
	}
	//Init the client info structure.
 	new_client->prev = current_client;
	current_client->next = new_client;
	new_client->next = NULL;
	
	printf("A new client enters [%s %s:%d]\n", new_client->client_name,inet_ntoa((new_client->address).sin_addr), ((new_client->address).sin_port));
	
	(clients_queue->count) ++;
	sem_post(&(clients_queue->cq_lock));
	DEBUG_PRINT("\trelease clientQ lock!");
 	return;
 }



/*
 * Run the chat server 
 */
 void server_run(void)
 {
 	DEBUG_PRINT("\t enter server_run_thread!");
 	while (1) {
        // Listen for new connections
        // 1. if it is a CMD_CLIENT_JOIN, try to create a new client_thread
        //  1.1) check whether the room is full or not
        //  1.2) check whether the username has been used or not
        // 2. otherwise, return ERR_UNKNOWN_CMD
		
		// listen on sock_fd, new connection on new_fd
		int new_fd, numbytes;  
		// connector's address information
		struct sockaddr_in their_addr; 
		struct exchg_msg mbuf, reply_msg;
		memset(&mbuf, 0, sizeof(struct exchg_msg));
		memset(&reply_msg, 0, sizeof(struct exchg_msg));

		int received_instruction;
		//int error_code = 0;
		DEBUG_PRINT("\t server: listen connections.");
		//Listen for new connections
		if (listen(sockfd, BACKLOG) == -1) {
			perror("listen");
			exit(1);
		}
		
		DEBUG_PRINT("\t server: accept connections.");
		//Accept connect request from clients.
		if ((new_fd = accept(sockfd, (struct sockaddr *)&their_addr, &sin_size)) == -1) {
			perror("accept");
			exit(1);
		}
		DEBUG_PRINT("\t server: got connection from %s\n",inet_ntoa(their_addr.sin_addr));
		
		/* communicate with the client using new_fd */
		if ((numbytes=recv(new_fd, &mbuf, sizeof(mbuf), 0)) == -1) {
			perror("recv");
			exit(1);
		} 
		
		received_instruction = ntohl(mbuf.instruction);

		//If receives a join command.
		if (received_instruction == CMD_CLIENT_JOIN) {
			DEBUG_PRINT("\tJoin Command Received! ");
			sem_wait(&(clients_queue->cq_lock));
			DEBUG_PRINT("\t acquire clientQ lock! ");			
			//If the Capacity is already reached, report to client that the chat room is full.
			if ((clients_queue->count) >= MAX_ROOM_CLIENT) {
				reply_msg.instruction = htonl(CMD_SERVER_FAIL);
				reply_msg.private_data = htonl(ERR_JOIN_ROOM_FULL);
				
				/* echo the message back */
				if (send(new_fd, &reply_msg, sizeof(mbuf), 0) == -1)
					perror("send");
				DEBUG_PRINT("\tThe chat room is already full! ");
				sem_post(&(clients_queue->cq_lock));
				DEBUG_PRINT("\t release clientQ lock! ");	
			}
			else {
				char user_name[MAXDATASIZE];
				strcpy(user_name, mbuf.content);
				
				//If found duplicate users.				
				if (check_duplicate_user(user_name) == 0){
					reply_msg.instruction = htonl(CMD_SERVER_FAIL);
					reply_msg.private_data = htonl(ERR_JOIN_DUP_NAME);

					/* echo the message back */
					if (send(new_fd, &reply_msg, numbytes, 0) == -1)
						perror("send");
					DEBUG_PRINT("\tFound duplicate users! ");
					sem_post(&(clients_queue->cq_lock));
					DEBUG_PRINT("\t release clientQ lock! ");	
				}

				//Welcome the user to the chat room.
				else{
					//Create a client thread for this client
					pthread_t thrObj;					
					struct chat_client * new_client;
					new_client = malloc (sizeof(struct chat_client));
				 	new_client->socketfd = new_fd;
				 	new_client->address = their_addr;
					strcpy(new_client->client_name, user_name);	

					pthread_create(&thrObj, NULL, (void *) &client_thread_fn, (void *) (new_client));
					//DEBUG_PRINT("\t PTHREAD OBJ VALUE2 %lu", thrObj);
				}
			}
		}
		else{
			reply_msg.instruction = htonl(CMD_SERVER_FAIL);
			reply_msg.private_data = htonl(ERR_UNKNOWN_CMD);
			/* echo the message back */
			if (send(new_fd, &reply_msg, numbytes, 0) == -1)
				perror("send");
			DEBUG_PRINT("\tUnknown command error! ");
		}      
	}
}


void *client_thread_fn(struct chat_client * new_client)
{	
	struct chat_client client = *(new_client);
	free(new_client);	

	DEBUG_PRINT("\t enter client_thread_fn!");
	int numbytes, received_instruction; 
	struct exchg_msg mbuf;
	char * next_msg;
	memset(&mbuf, 0, sizeof(struct exchg_msg));
	client.client_thread = pthread_self();
	//DEBUG_PRINT("\t PTHREAD OBJ VALUE3 %lu", pthread_self());
	
	create_client(&client);
	
	/* echo the join success message back */
	mbuf.instruction = htonl(CMD_SERVER_JOIN_OK);
	mbuf.private_data = htonl(-1);
	if (send(client.socketfd, &mbuf, sizeof(struct exchg_msg), 0) == -1)
	DEBUG_PRINT("\t sent cmd_server_join_ok!");
	
	//Increase tail after putting the welcome message into msgQ	
    // Put one message into the bounded buffer "$client_name$ just joins, welcome!"
	sem_wait(&(chatmsgs_queue->buffer_full));
	sem_wait(&(chatmsgs_queue->mq_lock));	
	DEBUG_PRINT("\t acquire msgQ lock!");
	next_msg = chatmsgs_queue->slots[(chatmsgs_queue->tail)];
	strcpy(next_msg, client.client_name);
	strcat(next_msg, " just joins the chat room, welcome!");
	chatmsgs_queue->tail += 1;
	chatmsgs_queue->tail %= MAX_QUEUE_MSG;
	DEBUG_PRINT("\t add client message to msgQ!");
	sem_post(&(chatmsgs_queue->mq_lock));	
	sem_post(&(chatmsgs_queue->buffer_empty));	
	DEBUG_PRINT("\t release msgQ lock!");

    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);	
    // Wait for incomming messages from this client
    // 1. if it is CMD_CLIENT_SEND, put the message to the bounded buffer
    // 2. if it is CMD_CLIENT_DEPART: 
    //  2.1) send a message "$client_name$ leaves, goodbye!" to all other clients
    //  2.2) free/destroy the resources allocated to this client
    //  2.3) terminate this thread
	while (1){
		if ((numbytes=recv(client.socketfd, &mbuf, sizeof(mbuf), 0)) == -1) {
			perror("recv");
			exit(1);
		} 
		received_instruction = ntohl(mbuf.instruction);
		pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL);	
		if (received_instruction == CMD_CLIENT_SEND){
			DEBUG_PRINT("\t received a send command from client!");
			char * new_msg = malloc (sizeof(char ) * CONTENT_LENGTH);
			
			strcpy(new_msg, client.client_name);
			strcat(new_msg, ": ");
			strcat(new_msg, mbuf.content);

			//Increase tail after putting the welcome message into msgQ	
			sem_wait(&(chatmsgs_queue->buffer_full));
			sem_wait(&(chatmsgs_queue->mq_lock));	
			DEBUG_PRINT("\t acquire msgQ lock!");
			if (chatmsgs_queue->slots[(chatmsgs_queue->tail)] == NULL){
				DEBUG_PRINT("\t NULL pointer found in msgQ!");			
			}
			DEBUG_PRINT("\t try to add msg to msgQ ...");	
			DEBUG_PRINT("\t Tail: %d, %s", chatmsgs_queue->tail, new_msg);				
			strcpy(chatmsgs_queue->slots[(chatmsgs_queue->tail)], new_msg);

			chatmsgs_queue->tail += 1;
			chatmsgs_queue->tail %= MAX_QUEUE_MSG;
			DEBUG_PRINT("\t add client message to msgQ!");
			sem_post(&(chatmsgs_queue->mq_lock));	
			sem_post(&(chatmsgs_queue->buffer_empty));	
			DEBUG_PRINT("\t release msgQ lock!");
			// Free the space allocated for the message
			free(new_msg);
			pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);	
		}
		else if (received_instruction == CMD_CLIENT_DEPART){
			
			char * new_msg = malloc (sizeof(char ) * CONTENT_LENGTH);
			strcpy(new_msg, client.client_name);
			strcat(new_msg, " just leaves the chat room, goodbye!");
			
			sem_wait(&(chatmsgs_queue->buffer_full));
			sem_wait(&(chatmsgs_queue->mq_lock));	
			strcpy(chatmsgs_queue->slots[(chatmsgs_queue->tail)], new_msg);

			//Increase tail after putting the welcome message into msgQ	
			chatmsgs_queue->tail += 1;
			chatmsgs_queue->tail %= MAX_QUEUE_MSG;			
			sem_wait(&(clients_queue->cq_lock));
			free(new_msg);
			
			struct chat_client * current_client = clients_queue->head;
			while (current_client->next != &client){
				current_client = current_client->next;			
			}
			current_client->next = client.next;

			(clients_queue->count) --;
			printf("A client departs [%s %s:%d]\n", client.client_name,inet_ntoa((client.address).sin_addr), 	(client.address).sin_port);
			close(client.socketfd);
			sem_post(&(clients_queue->cq_lock));
			sem_post(&(chatmsgs_queue->mq_lock));	
			sem_post(&(chatmsgs_queue->buffer_empty));	
			//detach itself after clean the client information
			pthread_detach(pthread_self());
			pthread_exit(0);
		}
	
	}	
	return next_msg;
} 



void *broadcast_thread_fn(void * arg)
{	
	
	DEBUG_PRINT("\tenter broadcast_thread!");
	while (1) {
	    // Broadcast the messages in the bounded buffer to all clients, one by one
		sem_wait(&(chatmsgs_queue->buffer_empty));
		sem_wait(&(chatmsgs_queue->mq_lock));

		pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL);
		DEBUG_PRINT("\t chatmsgs_queue->buffer_NOT_empty!");
		char * next_msg = chatmsgs_queue->slots[(chatmsgs_queue->head)];
		int msg_len;
		struct exchg_msg mbuf;
		struct chat_client * current_client;

		memset(&mbuf, 0, sizeof(struct exchg_msg));

		mbuf.instruction = htonl(CMD_SERVER_BROADCAST);
		msg_len = strlen(next_msg)+1; 
		memcpy(mbuf.content, next_msg, msg_len);
		msg_len = (msg_len < CONTENT_LENGTH) ? msg_len : CONTENT_LENGTH;
		mbuf.private_data = htonl(msg_len);
	
		chatmsgs_queue->head += 1;
		chatmsgs_queue->head %= MAX_QUEUE_MSG;
		sem_wait(&(clients_queue->cq_lock));
		//Send broadcase message to every client
		if (clients_queue->head != NULL){
			current_client = clients_queue->head->next;
			
		 	while (current_client != NULL){
				DEBUG_PRINT("\t try to broadcast to client %s ... ", current_client->client_name);
				if (send(current_client->socketfd, &mbuf, sizeof(struct exchg_msg), 0) == -1){
					perror("send");
				}
				DEBUG_PRINT("\tbroadcast a message to client %s!", current_client->client_name);
		 		current_client = current_client->next;
			}
		}
		DEBUG_PRINT("\trelease the broadcast lock!");
		sem_post(&(clients_queue->cq_lock));
		sem_post(&(chatmsgs_queue->mq_lock));
		sem_post(&(chatmsgs_queue->buffer_full));
		pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
	}
	DEBUG_PRINT("\tleave broadcast_thread!");
	return arg;
}


/*
 * Signal handler (when "Ctrl + C" is pressed)
 */
 void shutdown_handler(int signum)
 {

	struct exchg_msg mbuf;
	struct chat_client * current_client;
	int i;

    // TO DO:
    // Implement server shutdown here
    // 1. send CMD_SERVER_CLOSE message to all clients
    // 2. terminates all threads: broadcast_thread, client_thread(s)
    // 3. free/destroy all dynamically allocated resources: memory, mutex, semaphore, whatever.
	DEBUG_PRINT("\tenter shutdown_handler!");
	printf("Kill by SIGKILL (kill -%d)\n", signum);
	printf("Shutdown server .....\n");

	pthread_cancel(broadcast_thd);	
	DEBUG_PRINT("\t try to cancel broadcast thread ...");
	pthread_join(broadcast_thd, NULL);
	//DEBUG_PRINT("\t cancelled broadcast thread!");

	// for client send depart, cancel join 
	memset(&mbuf, 0, sizeof(struct exchg_msg));
	mbuf.instruction = htonl(CMD_SERVER_CLOSE);
	mbuf.private_data = htonl(-1);

	sem_wait(&(clients_queue->cq_lock));
	DEBUG_PRINT("\t acquired clients_queue->cq_lock!");
	if (clients_queue->head != NULL){
		current_client = clients_queue->head->next;
		
	 	while (current_client != NULL){
			DEBUG_PRINT("\t try to broadcast CLOSE to client %s ... ", current_client->client_name);
			if (send(current_client->socketfd, &mbuf, sizeof(struct exchg_msg), 0) == -1){
				perror("send");
			}
			DEBUG_PRINT("\tbroadcast a CLOSE message to client %s!", current_client->client_name);
			close(current_client->socketfd);
			pthread_cancel(current_client->client_thread);
			DEBUG_PRINT("\tcancelling %s ... ", current_client->client_name);
			pthread_join(current_client->client_thread, NULL);
			//DEBUG_PRINT("\tcancelled %s!", current_client->client_name);
	 		current_client = current_client->next;
		}
	}
	sem_post(&(clients_queue->cq_lock));

	// Release semaphores
	sem_destroy(&(clients_queue->cq_lock));
	sem_destroy(&(chatmsgs_queue->buffer_full));
	sem_destroy(&(chatmsgs_queue->mq_lock));
	sem_destroy(&(chatmsgs_queue->buffer_empty));

	// Free chatserver and 0ther malloced resources
    free(clients_queue->head);
    for (i = 0; i < MAX_QUEUE_MSG; i++){
    	free(chatmsgs_queue->slots[i]);
	}
	free(chatserver);

	printf("Done\n");						
 	exit(0);
 }
