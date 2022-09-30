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

/// @brief Fill a full line of data (terminated by \n) into a provided vector
///		   from a provided socket
/// @param client_fd the socket to read data from
/// @param line_vec the vector to put the data into
/// @return length of line if line is terminated, 0 if connection was terminated
///			-1 if there was a failure
int fill_line(int client_fd, vector *line_vec){
	ssize_t received = 0; 
	char recv_buf[CHUNK_SIZE];

	while(!strchr(line_vec->buf + line_vec->len - received, '\n')){

		// Perform non-blocking receive
		do {
			received = recv(client_fd, recv_buf, CHUNK_SIZE, 0);
		} while (received == -1 && errno == EAGAIN);

		if(received == -1) {
			syslog(LOG_ERR, "error on syscall: recv");
			return -1;
		}
		else if(received > 0){
			if(vector_append(line_vec, recv_buf, received)){
				return -1;
			}
		}
		else {
			return 0;
		}
	}

	return line_vec->len;
}

int main(int argc, char **argv) {
	int sockfd, new_fd;  // listen on sock_fd, new connection on new_fd
	struct addrinfo hints, *servinfo, *p;
	struct sockaddr_storage their_addr; // connector's address information
	struct sigaction sa;
	socklen_t sin_size;
	int yes=1;
	char s[INET6_ADDRSTRLEN];
	int rv;
	vector line_vec;
	char read_char;
	int flags;
	pid_t pid;

	openlog("server_log", LOG_CONS | LOG_NDELAY, LOG_USER);

	// install handler for SIGINT and SIGTERM
	sa.sa_handler = sig_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_RESTART;
	if (sigaction(SIGINT, &sa, NULL) == -1) {
		syslog(LOG_ERR, "error on syscall: sigaction");
		exit(-1);
	}
	if (sigaction(SIGTERM, &sa, NULL) == -1) {
		syslog(LOG_ERR, "error on syscall: sigaction");
		exit(-1);
	}

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE; // use my IP

	if ((rv = getaddrinfo(NULL, PORT, &hints, &servinfo)) != 0) {
		syslog(LOG_ERR, "getaddrinfo: %s\n", gai_strerror(rv));
		return -1;
	}

	// loop through all the results and bind to the first we can
	for(p = servinfo; p != NULL; p = p->ai_next) {
		if ((sockfd = socket(p->ai_family, p->ai_socktype,
				p->ai_protocol)) == -1) {
			continue;
		}

		if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes,
				sizeof(int)) == -1) {
			syslog(LOG_ERR, "error on syscall: setsockopt");
			exit(1);
		}

		if (bind(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
			close(sockfd);
			continue;
		}

		break;
	}

	if (p == NULL)  {
		syslog(LOG_ERR, "failed to bind\n");
		return -1;
	}

	// Close fd and exit if in daemon mode and this is the parent
	if(argc > 1 && !strcmp(argv[1], "-d")){
		pid = fork();
		if(pid == -1) {
			syslog(LOG_ERR, "error on syscall: fork");
			return -1;
		}
		// parent process
		else if(pid > 0) {
			close(sockfd);
			exit(0);
		}
		// child process
		else {

			// Create new session + process group so we don't get sigs from term
			if(setsid() == -1) {
				syslog(LOG_ERR, "error on syscall: setsid");
				return -1;
			}

			// Prevent working directory being unmounted by making it root
			if(chdir("/") == -1) {
				syslog(LOG_ERR, "error on syscall: chdir");
				return -1;
			}

			// Redirect stdin, stdout, and stderror to /dev/null
			open("/dev/null", O_RDWR);
			dup(0);
			dup(0);
		}

	}

	freeaddrinfo(servinfo); // all done with this structure

	flags = fcntl(sockfd, F_GETFL);
	if(flags == -1) {
		syslog(LOG_ERR, "error on syscall: fcntl");
		return -1;
	}
	if(fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) == -1) {
		syslog(LOG_ERR, "error on syscall: fcntl");
		return -1;		
	}

	// Start listening for new client connections
	if (listen(sockfd, BACKLOG) == -1) {
		syslog(LOG_ERR, "error on syscall: listen");
		return -1;
	}

	// Create/wipe data file
	int data_fd = open(DATA_FILE, O_CREAT | O_RDWR | O_TRUNC,0644);
	if(data_fd == -1) {
		syslog(LOG_ERR, "error creating data file");
		return -1;
	}

	syslog(LOG_DEBUG, "waiting for connections...\n");

	while(!sig_received) {  // main accept() loop
		// Accept new client connection
		sin_size = sizeof(their_addr);

		do {
			new_fd = accept(sockfd, (struct sockaddr *)&their_addr, &sin_size);
		} while (new_fd == -1 && errno == EAGAIN && !sig_received);
		if(new_fd == -1) {
			if(sig_received) {
				break;
			}
			else {
				syslog(LOG_ERR, "error on syscall: accept");
				return -1;				
			}
		}

		// Find client IP
		inet_ntop(their_addr.ss_family,
			get_in_addr((struct sockaddr *)&their_addr),
			s, sizeof(s));
		syslog(LOG_DEBUG, "Accepted connection from %s\n", s);
		
		// Loop until client closes connection
		while(!sig_received){
			// Clear line buffer to prepare for receive
			if(vector_init(&line_vec)){
				syslog(LOG_ERR, "vec_init fail\n");
				return -1;
			}

			// Fill linebuffer until new line
			rv = fill_line(new_fd, &line_vec);

			if(rv < 0){
				syslog(LOG_ERR, "fill_line fail\n");
				return -1;
			}
			else if(rv == 0){
				// Connection closed by client
				syslog(LOG_DEBUG, "Closed connection from %s\n", s);
				vector_close(&line_vec);
				break;
			}

			// Write received data to file
			if(write(data_fd, line_vec.buf, line_vec.len) == -1){
				syslog(LOG_ERR, "error writing data to file");
				return -1;				
			}

			// Clear line buff to prepare for data file read/response
			vector_close(&line_vec);
			if(vector_init(&line_vec)){
				syslog(LOG_ERR, "vec_init fail\n");
				return -1;
			}

			// Seek back to start of file for read
			if(lseek(data_fd, 0, SEEK_SET) == -1){
				syslog(LOG_ERR, "error seeking to start of file");
				return -1;
			}

			// Loop through chars in file
			while(read(data_fd, &read_char, 1) == 1){
				// Apeend each char to the line buffer
				if(vector_append(&line_vec, &read_char, 1)){
					syslog(LOG_ERR, "vec_append fail\n");
					return -1;
				}

				// If new line is found, send off and clear/re-init line buffer
				if( read_char == '\n'){
					// Perform non-blocking send
					do {
						rv = send(new_fd, line_vec.buf, line_vec.len, 0);
					} while (rv == -1 && errno == EAGAIN);
					if(rv == -1) {
						syslog(LOG_ERR, "error on syscall: send");
						return -1;
					}

					vector_close(&line_vec);
					if(vector_init(&line_vec)){
						syslog(LOG_ERR, "vec_init fail\n");
						return -1;
					}
					continue;
				}
			}

			// clear buffer to prepare for next receive
			vector_close(&line_vec);
		}

		// Close client socket
		close(new_fd);
	}

	if(sig_received){
		syslog(LOG_DEBUG, "Caught signal, exiting\n");
	}

	close(data_fd);
	closelog();
	return 0;
}

