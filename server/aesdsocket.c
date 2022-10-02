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
#include "vector.h"

#define PORT "9000"  // the port users will be connecting to
#define DATA_FILE "/var/tmp/aesdsocketdata"
#define CHUNK_SIZE 200

#define BACKLOG 10	 // how many pending connections queue will hold

// Struct holding fd's to close and pointers to memory/structs to free
struct cleanup_data {
	struct addrinfo *servinfo;
	int sock_fd;
	int new_fd;
	int data_fd;

	vector *recv_vec;
	vector *send_vec;
};

static bool sig_received = false;

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

/// @brief Close/free all open system resources
/// @param cd pointer to struct holding all the things to cleanup
void cleanup(struct cleanup_data *cd){
	freeaddrinfo(cd->servinfo);
	close(cd->sock_fd);
	close(cd->new_fd);
	close(cd->data_fd);
	vector_close(cd->recv_vec);
	vector_close(cd->send_vec);
	closelog();
}

int main(int argc, char **argv) {
	int sock_fd=0; // listen on sock_fd
	int new_fd=0; // new connections on new_fd
	int data_fd=0; // data file
	struct addrinfo hints, *servinfo=NULL, *p;
	struct sockaddr_storage their_addr; // connector's address information
	struct sigaction sa;
	socklen_t sin_size;
	int yes=1;
	char s[INET6_ADDRSTRLEN];
	char recv_buf[CHUNK_SIZE];
	char *new_line;
	int rv;
	int received;
	int written;
	vector recv_vec, send_vec;
	char read_char;
	int flags;
	pid_t pid;

	// setup stuff to cleanup
	struct cleanup_data cd;
	recv_vec.buf = NULL;
	send_vec.buf = NULL;
	cd.servinfo = servinfo;
	cd.sock_fd = sock_fd;
	cd.new_fd = new_fd;
	cd.data_fd = data_fd;
	cd.recv_vec = &recv_vec;
	cd.send_vec= &send_vec;

	openlog("server_log", LOG_CONS | LOG_NDELAY, LOG_USER);

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
	data_fd = open(DATA_FILE, O_CREAT | O_RDWR | O_TRUNC,0644);
	if(data_fd == -1) {
		syslog(LOG_ERR, "error creating data file");
		cleanup(&cd);
		return -1;
	}

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

		// Clear receive buffer to prepare for receive
		if(vector_init(&recv_vec)){
			syslog(LOG_ERR, "vec_init fail\n");
			cleanup(&cd);
			return -1;
		}
		
		// Loop until client closes connection
		while(!sig_received){
			received = 0;

			// Call receive until we've received a newline
			while(!(new_line = vector_find(&recv_vec, recv_vec.len - received, '\n'))){

				// Perform non-blocking receive
				do {
					received = recv(new_fd, recv_buf, CHUNK_SIZE, 0);
				} while (received == -1 && errno == EAGAIN);

				if(received == -1) {
					syslog(LOG_ERR, "error on syscall: recv");
					cleanup(&cd);
					return -1;
				}
				else if(received > 0){
					if(vector_append(&recv_vec, recv_buf, received)){
						syslog(LOG_ERR, "vec_append fail\n");
						cleanup(&cd);
						return -1;
					}
				}
				else {
					// Connection was terminated, follow into the if below
					// since double break isn't well defined
					break;
				}
			}

			// Connection closed by client
			if(received == 0){
				syslog(LOG_DEBUG, "Closed connection from %s\n", s);
				break;
			}

			// Write from the receive buffer into the data file one line at a time
			written = 0;
			while((new_line = vector_find(&recv_vec, written, '\n'))){
				if((rv = write(data_fd, recv_vec.buf+written, new_line + 1 - (char *)(recv_vec.buf+written))) == -1){
					syslog(LOG_ERR, "error writing data to file");
					cleanup(&cd);
					return -1;				
				}
				written += rv;
			}

			// If we have have a extra data in the receive buffer, carry it thourgh
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
				cleanup(&cd);
				return -1;
			}

			// Seek back to start of file for read
			if(lseek(data_fd, 0, SEEK_SET) == -1){
				syslog(LOG_ERR, "error seeking to start of file");
				cleanup(&cd);
				return -1;
			}

			// Loop through chars in file
			while(read(data_fd, &read_char, 1) == 1){
				// Apeend each char to the line buffer
				if(vector_append(&send_vec, &read_char, 1)){
					syslog(LOG_ERR, "vec_append fail\n");
					cleanup(&cd);
					return -1;
				}

				// If new line is found, send off and clear/re-init line buffer
				if( read_char == '\n'){
					// Perform non-blocking send
					do {
						rv = send(new_fd, send_vec.buf, send_vec.len, 0);
					} while (rv == -1 && errno == EAGAIN);
					if(rv == -1) {
						syslog(LOG_ERR, "error on syscall: send");
						cleanup(&cd);
						return -1;
					}

					vector_close(&send_vec);
					if(vector_init(&send_vec)){
						syslog(LOG_ERR, "vec_init fail\n");
						cleanup(&cd);
						return -1;
					}
					continue;
				}
			}

			// clear buffer to prepare for next receive
			vector_close(&send_vec);
		}

		vector_close(&recv_vec);
		// Close client socket
		close(new_fd);
	}

	if(sig_received){
		syslog(LOG_DEBUG, "Caught signal, exiting\n");
	}

	if (unlink(DATA_FILE) == -1) {
		syslog(LOG_ERR, "error on syscall: unlink");
		cleanup(&cd);
		return -1;
	}

	cleanup(&cd);
	return 0;
}

