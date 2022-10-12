// Simple system call socket server
// Usage:
// ./aesdsocket [-d]
//				daemon mode, forks and runs in background
// Author: James Bohn
// Adapted from Beej's guide (https://beej.us/guide/bgnet/html/)

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <signal.h>
#include <fcntl.h>
#include <syslog.h>
#include <stdbool.h>
#include <pthread.h>
#include <time.h>
#include "queue.h"
#include "vector.h"

#define PORT "9000"  // the port users will be connecting to
#define DATA_FILE "/var/tmp/aesdsocketdata"
#define TIMESTAMP_SIZE 100
#define TIMESTAMP_INTERVAL_S 10
#define CHUNK_SIZE 200

#define BACKLOG 10	 // how many pending connections queue will hold

// Struct to manage the data file across threads
struct shared_file {
	int fd;
	pthread_mutex_t mtx;
};

struct thread_data {
	pthread_t thread;
	int client_fd;
	bool complete;
};

// Struct holding fd's to close and pointers to memory/structs to free
struct cleanup_data {
	struct addrinfo *servinfo;
	int sock_fd;
	int new_fd;
	int data_fd;
};

// Struct holding fd's to close and pointers to memory/structs to free
struct thread_cleanup_data {
	struct thread_data *t_data;
	vector *recv_vec;
	vector *send_vec;
};

// SLIST.
typedef struct slist_data_s slist_data_t;
struct slist_data_s {
    struct thread_data td;
    SLIST_ENTRY(slist_data_s) entries;
};

static bool sig_received = false;
static struct shared_file data_file;

void sig_handler(int s) {
	sig_received = true;
}

// get sockaddr, IPv4 or IPv6:
void *get_in_addr(struct sockaddr *sa) {
	if (sa->sa_family == AF_INET) {
		return &(((struct sockaddr_in*)sa)->sin_addr);
	}

	return &(((struct sockaddr_in6*)sa)->sin6_addr);
}

/// @brief Close/free all open system resources for main thread
/// @param cd pointer to struct holding all the things to cleanup
void cleanup(struct cleanup_data *cd){
	freeaddrinfo(cd->servinfo);
	close(cd->sock_fd);
	close(cd->new_fd);

	close(data_file.fd);
	pthread_mutex_destroy(&data_file.mtx);
	closelog();
}

/// @brief Close/tree all open system resources for client threads
/// @param cd pointer to struct holding all the things to cleanup
void thread_cleanup(struct thread_cleanup_data *cd){
	vector_close(cd->recv_vec);
	vector_close(cd->send_vec);
	
	cd->t_data->complete = true;
}

/// @brief function to be called every N seconds by posix timer that writes the 
///        current timestamp to the data file
/// @param sigval way to pass data into the function, unused
void write_timestamp(union sigval sigval){
	char time_string[TIMESTAMP_SIZE] = "timestamp:";
	time_t now = time(0);
	struct tm now_tm = *localtime(&now);
	strftime(time_string + sizeof("timestamp:")-1, sizeof(time_string), "%a, %d %b %Y %T %z%n", &now_tm);


	pthread_mutex_lock(&data_file.mtx);
	if(write(data_file.fd, time_string, strlen(time_string)) == -1){
		pthread_mutex_unlock(&data_file.mtx);
		syslog(LOG_ERR, "error writing data to file");
		return;				
	}
	pthread_mutex_unlock(&data_file.mtx);
}

/// @brief Spawned thread to handle client connections
/// @param thread_param structure containing input and output data for the client
///		   connection
/// @return NULL
void *handle_connection(void *thread_param){
	int received, written, rv;
	vector recv_vec, send_vec;
	char *new_line;
	char read_char;
	char recv_buf[CHUNK_SIZE];
	struct thread_cleanup_data cd;

	// Cast paramater as correct struct
	struct thread_data *t_data = (struct thread_data *) thread_param;

	// Set up data for easy cleanup
	recv_vec.buf = NULL;
	send_vec.buf = NULL;
	cd.t_data = t_data;
	cd.recv_vec = &recv_vec;
	cd.send_vec = &send_vec;

	// Clear receive buffer to prepare for receive
	if(vector_init(&recv_vec)){
		syslog(LOG_ERR, "vec_init fail\n");
		thread_cleanup(&cd);
		return NULL;
	}
	
	// Loop until client closes connection
	while(!sig_received){
		received = 0;

		// Call receive until we've received a newline
		while(!(new_line = vector_find(&recv_vec, recv_vec.len - received, '\n'))){

			// Perform non-blocking receive
			do {
				received = recv(t_data->client_fd, recv_buf, CHUNK_SIZE, 0);
			} while (received == -1 && errno == EAGAIN);

			if(received < 0) {
				syslog(LOG_ERR, "error on syscall: recv");
				thread_cleanup(&cd);
				return NULL;
			}
			else if(received > 0){
				if(vector_append(&recv_vec, recv_buf, received)){
					syslog(LOG_ERR, "vec_append fail\n");
					thread_cleanup(&cd);
					return NULL;
				}
			}
			else {
				// Connection was terminated, follow into the if below
				// since double break isn't well defined
				break;
			}
		}

		// Will have fallen out of above loop
		if(received == 0){
			break;
		}

		// Write from the receive buffer into the data file one line at a time
		written = 0;
		while((new_line = vector_find(&recv_vec, written, '\n'))){
			pthread_mutex_lock(&data_file.mtx);
			if((rv = write(data_file.fd, recv_vec.buf+written, new_line + 1 - (char *)(recv_vec.buf+written))) == -1){
				pthread_mutex_unlock(&data_file.mtx);
				syslog(LOG_ERR, "error writing data to file");
				thread_cleanup(&cd);
				return NULL;				
			}
			pthread_mutex_unlock(&data_file.mtx);
			written += rv;
		}

		// If we have have any extra data in the receive buffer, carry it through
		// to the next packet, otherwise reset the buffer
		if(written < recv_vec.len){
			vector_carryover(&recv_vec, written);
		}
		else{
			vector_close(&recv_vec);
			vector_init(&recv_vec);
		}

		if(vector_init(&send_vec)){
			syslog(LOG_ERR, "vec_init fail\n");
			thread_cleanup(&cd);
			return NULL;
		}

		pthread_mutex_lock(&data_file.mtx);
		// Seek back to start of file for read
		if(lseek(data_file.fd, 0, SEEK_SET) == -1){
			pthread_mutex_unlock(&data_file.mtx);
			syslog(LOG_ERR, "error seeking to start of file");
			thread_cleanup(&cd);
			return NULL;
		}
		pthread_mutex_unlock(&data_file.mtx);

		// Loop through chars in file
		pthread_mutex_lock(&data_file.mtx);
		while(1){
			if((rv = read(data_file.fd, &read_char, 1)) != 1){
				break;
			}

			// Apeend each char to the line buffer
			if(vector_append(&send_vec, &read_char, 1)){
				syslog(LOG_ERR, "vec_append fail\n");
				thread_cleanup(&cd);
				return NULL;
			}

			// If new line is found, send off and clear/re-init line buffer
			if( read_char == '\n'){
				// Perform non-blocking send
				do {
					rv = send(t_data->client_fd, send_vec.buf, send_vec.len, 0);
				} while (rv == -1 && errno == EAGAIN);
				if(rv == -1) {
					syslog(LOG_ERR, "error on syscall: send");
					thread_cleanup(&cd);
					return NULL;
				}

				vector_close(&send_vec);
				if(vector_init(&send_vec)){
					syslog(LOG_ERR, "vec_init fail\n");
					thread_cleanup(&cd);
					return NULL;
				}
				continue;
			}
		}
		pthread_mutex_unlock(&data_file.mtx);

		// clear buffer to prepare for next receive
		vector_close(&send_vec);
	}

	thread_cleanup(&cd);
	return NULL;
}

int main(int argc, char **argv) {
	int sock_fd=0; // listen on sock_fd
	int new_fd=0; // new connections on new_fd
	struct addrinfo hints, *servinfo=NULL, *p;
	struct sockaddr_storage their_addr; // connector's address information
	struct sigaction sa;
	struct sigevent sev;
	socklen_t sin_size;
	timer_t timer;
	int yes=1;
	char s[INET6_ADDRSTRLEN];
	int rv;
	int flags;
	int active_conns = 0;
	pid_t pid;

	// setup stuff to cleanup
	struct cleanup_data cd;
	cd.servinfo = servinfo;
	cd.sock_fd = sock_fd;
	cd.new_fd = new_fd;

	// setup shared data file
	data_file.fd = 0;
	pthread_mutex_init(&data_file.mtx, NULL);

	openlog("server_log", LOG_CONS | LOG_NDELAY, LOG_USER);

	// setup SLIST
	slist_data_t *datap=NULL;
	slist_data_t *nextp=NULL;
    SLIST_HEAD(slisthead, slist_data_s) head;
    SLIST_INIT(&head);

	// install handler for SIGINT and SIGTERM
	sa.sa_handler = sig_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_RESTART;
	if (sigaction(SIGINT, &sa, NULL) == -1) {
		syslog(LOG_ERR, "error on syscall: sigaction");
		cleanup(&cd);
		return -1;
	}
	if (sigaction(SIGTERM, &sa, NULL) == -1) {
		syslog(LOG_ERR, "error on syscall: sigaction");
		cleanup(&cd);
		return -1;
	}

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE; // use my IP

	if ((rv = getaddrinfo(NULL, PORT, &hints, &servinfo)) != 0) {
		syslog(LOG_ERR, "getaddrinfo: %s\n", gai_strerror(rv));
		cleanup(&cd);
		return -1;
	}

	// loop through all the results and bind to the first we can
	for(p = servinfo; p != NULL; p = p->ai_next) {
		if ((sock_fd = socket(p->ai_family, p->ai_socktype,
				p->ai_protocol)) == -1) {
			continue;
		}

		if (setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, &yes,
				sizeof(int)) == -1) {
			syslog(LOG_ERR, "error on syscall: setsockopt");
			cleanup(&cd);
			return -1;
		}

		if (bind(sock_fd, p->ai_addr, p->ai_addrlen) == -1) {
			close(sock_fd);
			continue;
		}

		break;
	}

	if (p == NULL)  {
		syslog(LOG_ERR, "failed to bind\n");
		cleanup(&cd);
		return -1;
	}

	// Close fd and exit if in daemon mode
	if(argc > 1 && !strcmp(argv[1], "-d")){
		pid = fork();
		if(pid == -1) {
			syslog(LOG_ERR, "error on syscall: fork");
			cleanup(&cd);
			return -1;
		}
		// parent process
		else if(pid > 0) {
			cleanup(&cd);
			exit(0);
		}
		// child process
		else {

			// Create new session + process group so we don't get sigs from term
			if(setsid() == -1) {
				syslog(LOG_ERR, "error on syscall: setsid");
				cleanup(&cd);
				return -1;
			}

			// Prevent working directory being unmounted by making it root
			if(chdir("/") == -1) {
				syslog(LOG_ERR, "error on syscall: chdir");
				cleanup(&cd);
				return -1;
			}

			// Redirect stdin, stdout, and stderror to /dev/null
			open("/dev/null", O_RDWR);
			dup(0);
			dup(0);
		}

	}

	freeaddrinfo(servinfo); // all done with this structure
	servinfo = NULL;

	flags = fcntl(sock_fd, F_GETFL);
	if(flags == -1) {
		syslog(LOG_ERR, "error on syscall: fcntl");
		cleanup(&cd);
		return -1;
	}
	if(fcntl(sock_fd, F_SETFL, flags | O_NONBLOCK) == -1) {
		syslog(LOG_ERR, "error on syscall: fcntl");
		cleanup(&cd);
		return -1;		
	}

	// Start listening for new client connections
	if (listen(sock_fd, BACKLOG) == -1) {
		syslog(LOG_ERR, "error on syscall: listen");
		cleanup(&cd);
		return -1;
	}

	// Create/wipe data file
	data_file.fd = open(DATA_FILE, O_CREAT | O_RDWR | O_TRUNC,0644);
	if(data_file.fd == -1) {
		syslog(LOG_ERR, "error creating data file");
		cleanup(&cd);
		return -1;
	}

	// Intialize the timer to call write_timestamp every TIMESTAMP_INTERVAL_S seconds
	memset(&sev, 0, sizeof(struct sigevent));
	sev.sigev_notify = SIGEV_THREAD;
	sev.sigev_value.sival_ptr = &data_file;
	sev.sigev_notify_function = write_timestamp;
	timer_create(CLOCK_REALTIME, &sev, &timer);

	struct timespec sleep_time;
	sleep_time.tv_sec = TIMESTAMP_INTERVAL_S;
	sleep_time.tv_nsec = 0;
	struct itimerspec spec;
	spec.it_value = sleep_time;
	spec.it_interval = sleep_time;
	timer_settime(timer, 0, &spec, NULL);

	syslog(LOG_DEBUG, "waiting for connections...\n");

	while(!sig_received) {  // main accept() loop
		sin_size = sizeof(their_addr);

		// Handle non-blocking accept
		do {
			new_fd = accept(sock_fd, (struct sockaddr *)&their_addr, &sin_size);
		} while (new_fd == -1 && errno == EAGAIN && !sig_received);
		if(new_fd == -1) {
			if(sig_received) {
				break;
			}
			else {
				syslog(LOG_ERR, "error on syscall: accept");
				cleanup(&cd);
				return -1;				
			}
		}

		// Find client IP
		inet_ntop(their_addr.ss_family,
			get_in_addr((struct sockaddr *)&their_addr),
			s, sizeof(s));
		syslog(LOG_DEBUG, "Accepted connection from %s\n", s);

		// Allocate space for and add thread_data to LL
		datap = malloc(sizeof(slist_data_t));
		datap->td.client_fd = new_fd;
		datap->td.complete = false;
		SLIST_INSERT_HEAD(&head, datap, entries);

		// Spawn the thread for the client connection
		pthread_create(&datap->td.thread, NULL, handle_connection, &datap->td);
		active_conns += 1;

		// Check if any threads are complete and reap/clean them up
		SLIST_FOREACH_SAFE(datap, &head, entries, nextp){
			if(datap->td.complete){
				pthread_join(datap->td.thread, NULL);
				syslog(LOG_DEBUG, "Closed connection from %s\n", s);
				close(datap->td.client_fd);
				SLIST_REMOVE(&head, datap, slist_data_s, entries);
				free(datap);
				active_conns -= 1;
			}
		}
	}

	if(sig_received){
		syslog(LOG_DEBUG, "Caught signal, exiting\n");
	}

	// Reap remaining connections after they've finished their current packet
	while(active_conns > 0){
		SLIST_FOREACH_SAFE(datap, &head, entries, nextp){
		if(datap->td.complete){
				pthread_join(datap->td.thread, NULL);
				syslog(LOG_DEBUG, "Closed connection from %s\n", s);
				close(datap->td.client_fd);
				SLIST_REMOVE(&head, datap, slist_data_s, entries);
				free(datap);
				active_conns -= 1;
			}
		}
	}

	// Delete the data file
	if (unlink(DATA_FILE) == -1) {
		syslog(LOG_ERR, "error on syscall: unlink");
		cleanup(&cd);
		return -1;
	}

	timer_delete(timer);
	cleanup(&cd);
	return 0;
}

