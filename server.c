/*
	A minimal HTTP/1.1 server for learning purpose
	* based on:
		Computer Systems: A Programmer's Perspective, 3rd Edition, tiny.c
			http://csapp.cs.cmu.edu/3e/ics3/code/netp/tiny/tiny.c
		Hacking: The Art of Exploitation, 2nd Edition, tinyweb.c
	* iterative
	* implemented methods: GET, HEAD, POST
	* supported static file types: html, gif, png, jpg, mp4, plain text
	* SIGCHLD implemented: https://www.cnblogs.com/wuchanming/p/4020463.html
	* arguments parsing implemented for dynamic content using GET and POST
*/
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <netdb.h>

#define DEFAULTFILE "index.html"
#define	MAXSIZE 8192
#define MAXLISTEN 5
#define EOL "\r\n"                                         // end-of-line byte sequence
#define EOLSIZE 2

extern char **environ;

void wait_child(int signo);
int open_fd(int port);
void serve_content(int sockfd, struct sockaddr_in *ptr_caddr);
int parse_uri(char *uri, char *fname, char *cgiargs);
void check_ftype(char *fname, char *ftype);
void serve_error(char *method, int sockfd, char *cause, char *status_code, char *message);
void serve_static(char *method, int sockfd, char *fname, int fsize);
void serve_dynamic(char *method, int sockfd, char *fname, char *cgiargs);
/* handle a fatal error */
void handle_error(char *message);
/* receive a line until EOL or size_max reaches */
int recv_line(int sockfd, char *dst, int size_max);
/* receive n bytes */
void recv_nbytes(int sockfd, char *dst, int n);
/* send n bytes */
void send_nbytes(int sockfd, char *src, int n);

int main(int argc, char *argv[]) {
	int lisfd, confd;
	char hname[MAXSIZE + 1], port[MAXSIZE + 1];
	struct sockaddr_in client_addr;
	socklen_t sin_size;
	
	if(argc != 2) {
		printf("Usage: %s <port>\n", argv[0]);
		return 0;
	}
	/* open a listening socket fd on port */
	lisfd = open_fd(atoi(argv[1]));
	while(1) {
		sin_size = sizeof(client_addr);
		if((confd = accept(lisfd, (struct sockaddr *)&client_addr, &sin_size)) == -1) {
			handle_error("accept failed");
		}
		serve_content(confd, &client_addr);
		close(confd);
	}
	
	return 0;
}

void wait_child(int signo) {  
    int status;
    while (waitpid(-1, &status, WNOHANG) > 0) {
		continue;
    }		
}

int open_fd(int port) {
	int sockfd, optval;
	struct sockaddr_in host_addr;
	socklen_t sin_size;
	
	printf("Opening port %d for web services\n", port);
	
	if((sockfd = socket(PF_INET, SOCK_STREAM, 0)) == -1) { // AF_INET works too
		handle_error("socket failed");
	}
	optval = 1;
	/* allow to reuse even in TIME_WAIT state */
	if(setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, (const void *)&optval, sizeof(int)) == -1) {
		handle_error("setsockopt failed");
	}
	host_addr.sin_family = AF_INET;                        // short host byte order: little endian for x86
	host_addr.sin_port = htons(port);                      // short network byte order: big endian
	host_addr.sin_addr.s_addr = INADDR_ANY;                // automatically fill with host IP
	memset(&(host_addr.sin_zero), '\0', 8);                // zero the rest of host_addr
	if(bind(sockfd, (struct sockaddr *)&host_addr, sizeof(struct sockaddr)) == -1) {
		handle_error("bind failed");
	}
    /* allow to queue MAXLISTEN requests */
	if(listen(sockfd, MAXLISTEN) == -1) {
		handle_error("listen failed");
	}
	return sockfd;
}

void serve_content(int sockfd, struct sockaddr_in *ptr_caddr) {
    int ctype, length, size_payload;
    struct stat meta;                                      // metadata of requested file
    char buffer[MAXSIZE + EOLSIZE], method[MAXSIZE + 1], uri[MAXSIZE + 1], version[MAXSIZE + 1];
    char fname[MAXSIZE + 1], cgiargs[MAXSIZE + 1];
	pid_t pid;
	
	signal(SIGCHLD, wait_child);
	pid = fork();
	if(pid < 0) {
		handle_error("Failed to fork");
	}
    else if(pid == 0) {
		/* process Request Headers */
		length = 1;
		size_payload = -1;
		method[0] = '\0';
		while(length) {                                    // not "\r\n"
			length = recv_line(sockfd, buffer, MAXSIZE + EOLSIZE);
			if(length == -1) {
				serve_error(method, sockfd, "Invalid Line", "400", "Bad Request");
				printf("Invalid Request-Line\n");
				return;
			}
			// printf("%s\n", buffer);
			/* extract Request-Line */
			if(strlen(method) == 0) {
				sscanf(buffer, "%s %s %s", method, uri, version);    // convert formatted string
				printf("%-5s%-25s%s\tfrom %s:%d\n", method, uri, version, inet_ntoa(ptr_caddr->sin_addr), ntohs(ptr_caddr->sin_port));
			}
			/* extract size_payload for POST */
			if(strcmp(method, "POST") == 0) {
				buffer[15] = '\0';                         // check up to first 15 chars
				if(strncmp(buffer, "Content-Length:", 15) == 0) {
					size_payload = atoi(&(buffer[16]));
				}
			}
		}
		/* unsupported Method */
		if(strcmp(method, "GET") && strcmp(method, "HEAD") && strcmp(method, "POST")) {
			serve_error(method, sockfd, method, "501", "Not Implemented");
			printf("Not Implemented: %s\n", method);
			return;
		}
		/* analyze URI for content type, file name, and cgi arguments if any */
		ctype = parse_uri(uri, fname, cgiargs);
		if(stat(fname, &meta) == -1) {
			serve_error(method, sockfd, fname, "404", "Not found");
			return;
		}
		/* not regular file, not readable for static, not executable for dynamic */
		if(!S_ISREG(meta.st_mode) || (!(meta.st_mode & S_IRUSR) && ctype == 0) 
			|| (!(meta.st_mode & S_IXUSR) && ctype == 1)) {
			serve_error(method, sockfd, fname, "403", "Forbidden");
			return;
		}
		/* static content */
		if(ctype == 0) {
			serve_static(method, sockfd, fname, meta.st_size);
		}
		/* dynamic content */
		else if(ctype == 1) {
			/* extract payload for POST */
			if(strcmp(method, "POST") == 0) {
				if(size_payload == -1) {                         // no Content-Length
					serve_error(method, sockfd, method, "411", "Length Required");
					return;
				}
				recv_nbytes(sockfd, cgiargs, size_payload);      // assuming Content-Length is correct
				printf("     %s\n", cgiargs);
			}
			serve_dynamic(method, sockfd, fname, cgiargs);
		}
	}
}

int parse_uri(char *uri, char *fname, char *cgiargs) {
	int ctype;
    char * ptr;

	strcpy(cgiargs, "");                                   // initiate cgiargs
	/* 0 - static content */
	ctype = 0;
    /* 1 - dynamic content */
    if(strstr(uri, "cgi-bin")) {
		ctype = 1;
		ptr = index(uri, '?');
		if(ptr) {                                            // '?' in uri
			strcpy(cgiargs, ptr + 1);
			*ptr = '\0';                                     // uri is fragmented
		}
    }

	strcpy(fname, ".");                                    // initiate fname
	strcat(fname, uri);                                    // uri is original or fragmented
	/* add DEFAULTFILE to file name if not specified in uri for static content */
	if (ctype == 0 && strcmp(uri, "/") == 0) {
		strcat(fname, DEFAULTFILE);
	}
	return ctype;
}

void check_ftype(char *fname, char *ftype) {
    if(strstr(fname, ".html")) {
		strcpy(ftype, "text/html");
	}
    else if(strstr(fname, ".gif")) {
		strcpy(ftype, "image/gif");
	}
    else if(strstr(fname, ".png")) {
		strcpy(ftype, "image/png");
	}
    else if(strstr(fname, ".jpg")) {
		strcpy(ftype, "image/jpeg");
	}
    else if(strstr(fname, ".mp4")) {
		strcpy(ftype, "video/mp4");
	}
    else {
		strcpy(ftype, "text/plain");
	}
}

void serve_error(char *method, int sockfd, char *cause, char *status_code, char *message) {
	char buffer[128];
	
	/* send Response Headers */
	sprintf(buffer, "HTTP/1.1 %s %s\r\n", status_code, message);
    send_nbytes(sockfd, buffer, strlen(buffer));
	sprintf(buffer, "Server: Minimal HTTP Server\r\nContent-Type: text/html\r\n\r\n");
    send_nbytes(sockfd, buffer, strlen(buffer));
	
    if(strcmp(method, "HEAD") == 0) {
		return;
	}
	
	/* send Response Body */
	sprintf(buffer, "<html><head><title>Web Server Error</title></head><body><h1>");
    send_nbytes(sockfd, buffer, strlen(buffer));
	sprintf(buffer, "%s: %s</h1><p>%s</p></body></html>\r\n", status_code, message, cause);
    send_nbytes(sockfd, buffer, strlen(buffer));
}

void serve_static(char *method, int sockfd, char *fname, int fsize) {
    int ffd;
    char * fp, ftype[50], buffer[128];
	
	/* check file type */
	check_ftype(fname, ftype);
	/* send Response Headers */
	sprintf(buffer, "HTTP/1.1 200 OK\r\nServer: Minimal HTTP Server\r\n");
    send_nbytes(sockfd, buffer, strlen(buffer));
	sprintf(buffer, "Content-Type: %s\r\nContent-Length: %d\r\n\r\n", ftype, fsize);
    send_nbytes(sockfd, buffer, strlen(buffer));
	
	if(strcmp(method, "HEAD") == 0) {
		return;
	}
	
	/* open file */
	ffd = open(fname, O_RDONLY, 0);
	if(ffd == -1) {
		serve_error(method, sockfd, fname, "404", "Not found");
		return;
	}
	fp = mmap(0, fsize, PROT_READ, MAP_PRIVATE, ffd, 0);
	close(ffd);
	if(fp == MAP_FAILED) {
		serve_error(method, sockfd, fname, "404", "Not found");
		return;
	}
	/* send Response Body */
    send_nbytes(sockfd, fp, fsize);
	if(munmap(fp, fsize) == -1) {
		handle_error("munmap failed");
	}
}

void serve_dynamic(char *method, int sockfd, char *fname, char *cgiargs) {
	char buffer[128];
    char * empList[] = {NULL};
	
	/* send Response Headers */
	sprintf(buffer, "HTTP/1.1 200 OK\r\nServer: Minimal HTTP Server\r\n\r\n");
    send_nbytes(sockfd, buffer, strlen(buffer));
	
	if (strcmp(method, "HEAD") == 0) {
		return;
	}
	
	/* send Response Body */
	setenv("QUERY_STRING", cgiargs, 1);
	dup2(sockfd, STDOUT_FILENO);
	if(execve(fname, empList, environ) < 0) {
		handle_error("execve failed");
	}
}

/* This function exits after printing an error message.
 */
void handle_error(char *message) {
	char error_msg[101];
	
	strcpy(error_msg, "[!!] Fatal Error ");
	strncat(error_msg, message, 83);
	perror(error_msg);
	exit(-1);
}

/* This function accepts a socket fd and a pointer to a destination
 * buffer. It receives from the socket until the EOL shows, the
 * socket is empty, or the destination buffer is full.
 * The EOL bytes are read from the socket but nulled.
 * Returns the length of the read line (without the EOL bytes), or -1
 * if the read line is too long.
 * To-do: split recv when dst is full
 */
int recv_line(int sockfd, char *dst, int size_max) {
	char * ptr;
	int size_curr, matched, received;
	
	ptr = dst;
	size_curr = 0;
	matched = 0;
	/* receive a single byte every time */
	while(size_curr < size_max) {
		received = recv(sockfd, ptr, 1, 0);
		if(received == -1) {                               // error
			handle_error("recv_nbytes failed");
		}
		else if(received == 0) {                           // socket is closed or half-closed
			break;
		}
		// printf("%02x ", *ptr);
		if(*ptr == EOL[matched]) {                         // current byte matches a byte of EOL
			matched++;
			if(matched == EOLSIZE) {                       // EOL shows
				*(ptr + 1 - EOLSIZE) = '\0';
				return size_curr + 1 - EOLSIZE;            // |a|a|\r|\n| -> |a|a|\0|\n|
			}
		} else {
			matched = 0;
		}
		ptr++;
		size_curr++;
	}
	return -1;                                             // no EOL
}

/* This function accepts a socket fd, a pointer to destination, and number
 * of bytes to be received.
 */
void recv_nbytes(int sockfd, char *dst, int n) {
	char * ptr;
	int received;
	
	ptr = dst;
	/* receive a single byte every time */
	while(n > 0) {
		received = recv(sockfd, ptr, 1, 0);
		if(received == -1) {                               // error
			handle_error("recv_nbytes failed");
		}
		else if(received == 0) {                           // socket is closed or half-closed
			break;
		}
		ptr++;
		n--;
	}
}

/* This function accepts a socket fd, a pointer to source, and number
 * of bytes to be sent.
 */
void send_nbytes(int sockfd, char *src, int n) {
	int sent;
	
	while(n > 0) {
		/* send does not always send as long as needed */
		sent = send(sockfd, src, n, MSG_NOSIGNAL);
		if(sent == -1) {
			handle_error("send_nbytes failed");
		}
		src += sent;
		n -= sent;
	}
}
