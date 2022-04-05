/**
 * @file mini_server.c
 * @author Tom Peng
 * @brief A minimal HTTP/1.1 server for learning purpose
 * A minimal HTTP/1.1 server for learning purpose
 * based on:
 *   Computer Systems: A Programmer's Perspective, 3rd Edition, tiny.c
 *       http://csapp.cs.cmu.edu/3e/ics3/code/netp/tiny/tiny.c
 *   Hacking: The Art of Exploitation, 2nd Edition, tinyweb.c
 *
 * implemented methods: GET, HEAD, POST
 * supported static file types: html, gif, png, jpg, mp4, plain text
 * concurrency supported
 * arguments parsing implemented for dynamic content using GET and POST
 * @version 0.1
 * @date 2022-04-04
 * 
 * @copyright Copyright (c) 2022
 * 
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>

#define DEFAULTFILE "index.html"
#define MAXSIZE 8192
#define MAXLISTEN 5
/* end-of-line */
#define EOL "\r\n"
#define EOLSIZE 2

extern char **environ;
int lisfd, confd;

int open_fd(int port);
void serve_content(struct sockaddr_in *ptr_caddr);
void serve_error(char *method, char *cause, 
    char *status_code, char *message);
void serve_static(char *method, 
    char *fname, int fsize);
void serve_dynamic(char *method, 
    char *fname, char *cgiargs);
void decode_uri(char *uri);
int parse_uri(char *uri, 
    char *fname, char *cgiargs);
void check_ftype(char *fname, char *ftype);
void cleanup(int signo);
void wait_child(int signo);
void handle_error(char *message);
int recv_line(int sockfd, char *dst, int size_max);
void recv_nbytes(int sockfd, char *dst, int n);
void send_nbytes(int sockfd, char *src, int n);

int main(int argc, char *argv[]) {
    struct sockaddr_in client_addr;
    socklen_t sin_size;
    pid_t pid;
    
    /* make sure port is assigned */
    if(argc != 2) {
        printf("Usage: %s <port>\n", argv[0]);
        return EXIT_SUCCESS;
    }
    
    /* open a listening socket fd on port */
    lisfd = open_fd(atoi(argv[1]));
    
    /* close lisfd and confd on Ctrl-C */
    signal(SIGINT, cleanup);
    
    /* handle SIGCHLD signal */
    signal(SIGCHLD, wait_child);
    
    while(1) {
        sin_size = sizeof(client_addr);
        if((confd = accept(lisfd, (struct sockaddr *)&client_addr, &sin_size)) == -1) {
            handle_error("accept failed");
        }
        
        /* handle request in child process */
        pid = fork();
        /* fork failed */
        if(pid < 0) {
            handle_error("Failed to fork");
        }
        /* child process */
        else if(pid == 0) {
            /* child process uses confd */
            close(lisfd);
            
            serve_content(&client_addr);
            
            close(confd);
            exit(EXIT_SUCCESS);
        }
        /* parent process */
        else {
            /* parent process uses lisfd */
            close(confd);
        }
    } 
    
    /* never reaches here */
    return EXIT_SUCCESS;
}

/* This function creates a socket, bind it to a IP 
 * address and port number, and use it to listen for 
 * incoming connections.
 */
int open_fd(int port) {
    int sockfd, optval;
    struct sockaddr_in host_addr;
    socklen_t sin_size;
    
    printf("Opening port %d for web services\n", port);
    
    /* create a SOCK_STREAM socket for IPv4 protocols */
    /* PF - Protocol Family, AF - Address Family
     * PF_INET is literally equivalent to AF_INET
     */
    if((sockfd = socket(PF_INET, SOCK_STREAM, 0)) == -1) {
        handle_error("socket failed");
    }
    
    /* allow to reuse port even in TIME_WAIT state */
    optval = 1;
    if(setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, 
        (const void *)&optval, sizeof(int)) == -1) {
        handle_error("setsockopt failed");
    }
    
    /* bind the socket to and IP address and port number */
    /* short host byte order: little endian for x86 */
    host_addr.sin_family = AF_INET;
    /* automatically fill with host IP */
    host_addr.sin_addr.s_addr = INADDR_ANY;
    /* short network byte order: big endian */
    host_addr.sin_port = htons(port);
    /* zero out the rest of host_addr */
    memset(&(host_addr.sin_zero), '\0', 8);
    if(bind(sockfd, (struct sockaddr *)&host_addr, 
        sizeof(struct sockaddr)) == -1) {
        handle_error("bind failed");
    }
    
    /* listen for incoming connections */
    /* allow to queue MAXLISTEN requests */
    if(listen(sockfd, MAXLISTEN) == -1) {
        handle_error("listen failed");
    }
    
    return sockfd;
}

/* This is the main function to handle requests and 
 * send corresponding content, if any, to clients.
 */
void serve_content(struct sockaddr_in *ptr_caddr) {
    int ctype, length, size_payload;
    /* metadata of requested file */
    struct stat meta;
    char buffer[MAXSIZE + EOLSIZE], method[MAXSIZE + 1];
    char uri[MAXSIZE + 1], version[MAXSIZE + 1];
    char fname[MAXSIZE + 1], cgiargs[MAXSIZE + 1];
    
    /* process Request Headers */
    length = 1;
    size_payload = -1;
    method[0] = '\0';
    /* until a line with only "\r\n"
     * This strategy is learned from Computer 
     * Systems: A Programmer's Perspective. It 
     * assumes all requests are valid, i.e., each 
     * line is correctly spaced, terminated with 
     * EOL, etc. In case there might be invalid 
     * requests, one passible strategy is to receive 
     * all bytes using a relatively big buffer and 
     * then parse.
     */
    while(length) {
        length = recv_line(confd, buffer, MAXSIZE + EOLSIZE);
        if(length == -1) {
            serve_error(method, "Invalid Line", "400", "Bad Request");
            printf("Invalid Request-Line\n");
            return;
        }
        
        /* extract Request-Line */
        if(strlen(method) == 0) {
            /* convert formatted string */
            sscanf(buffer, "%s %s %s", method, uri, version);
            /* decode uri */
            decode_uri(uri);
            printf("%-5s%-25s%s\tfrom %s:%d\n", method, uri, 
                version, inet_ntoa(ptr_caddr->sin_addr), 
                ntohs(ptr_caddr->sin_port));
        }
        
        /* extract size_payload for POST */
        if(strcmp(method, "POST") == 0) {
            /* check up to first 15 chars */
            buffer[15] = '\0';
            if(strncmp(buffer, "Content-Length:", 15) == 0) {
                size_payload = atoi(&(buffer[16]));
            }
        }
    }
    
    /* unsupported Method */
    if(strcmp(method, "GET") && strcmp(method, "HEAD") 
        && strcmp(method, "POST")) {
        serve_error(method, method, "501", "Not Implemented");
        printf("Not Implemented: %s\n", method);
        return;
    }
    
    /* analyze URI for content type, file name, and 
     * cgi arguments if any 
     */
    ctype = parse_uri(uri, fname, cgiargs);
    if(stat(fname, &meta) == -1) {
        serve_error(method, fname, "404", "Not found");
        return;
    }
    
    /* not regular file, not readable for static, 
     * or not executable for dynamic 
     */
    if(!S_ISREG(meta.st_mode) 
        || (!(meta.st_mode & S_IRUSR) && ctype == 0) 
        || (!(meta.st_mode & S_IXUSR) && ctype == 1)) {
        serve_error(method, fname, "403", "Forbidden");
        return;
    }
    
    /* static content */
    if(ctype == 0) {
        serve_static(method, fname, meta.st_size);
    }
    
    /* dynamic content */
    else if(ctype == 1) {
        /* extract payload for POST */
        if(strcmp(method, "POST") == 0) {
            /* no Content-Length */
            if(size_payload == -1) {
                serve_error(method, method, "411", "Length Required");
                return;
            }
            
            /* assume Content-Length is correct */
            recv_nbytes(confd, cgiargs, size_payload);
            printf("     %s\n", cgiargs);
        }
        serve_dynamic(method, fname, cgiargs);
    }
}

/* This function sends error information to clients.
 */
void serve_error(char *method, char *cause, 
    char *status_code, char *message) {
    char buffer[128];
    
    /* send Response Headers */
    sprintf(buffer, "HTTP/1.1 %s %s\r\n", status_code, message);
    send_nbytes(confd, buffer, strlen(buffer));
    sprintf(buffer, "Server: Minimal HTTP Server\r\nContent-Type: text/html\r\n\r\n");
    send_nbytes(confd, buffer, strlen(buffer));
    
    /* no Response Body for "HEAD" request */
    if(strcmp(method, "HEAD") == 0) {
        return;
    }
    
    /* send Response Body */
    sprintf(buffer, "<html><head><title>Web Server Error</title></head><body><h1>");
    send_nbytes(confd, buffer, strlen(buffer));
    sprintf(buffer, "%s: %s</h1><p>%s</p></body></html>\r\n", status_code, message, cause);
    send_nbytes(confd, buffer, strlen(buffer));
}

/* This function reads static content and sends to 
 * clients.
 */
void serve_static(char *method, char *fname, int fsize) {
    int ffd;
    char * fp, ftype[50], buffer[128];
    
    /* check file type */
    check_ftype(fname, ftype);
    
    /* send Response Headers */
    sprintf(buffer, "HTTP/1.1 200 OK\r\nServer: Minimal HTTP Server\r\n");
    send_nbytes(confd, buffer, strlen(buffer));
    sprintf(buffer, "Content-Type: %s\r\nContent-Length: %d\r\n\r\n", ftype, fsize);
    send_nbytes(confd, buffer, strlen(buffer));
    
    /* no Response Body for "HEAD" request */
    if(strcmp(method, "HEAD") == 0) {
        return;
    }
    
    /* open file */
    ffd = open(fname, O_RDONLY, 0);
    if(ffd == -1) {
        serve_error(method, fname, "404", "Not found");
        return;
    }
    
    /* map contents into memory*/
    fp = mmap(0, fsize, PROT_READ, MAP_PRIVATE, ffd, 0);
    close(ffd);
    if(fp == MAP_FAILED) {
        serve_error(method, fname, "404", "Not found");
        return;
    }
    
    /* send Response Body */
    send_nbytes(confd, fp, fsize);
    if(munmap(fp, fsize) == -1) {
        handle_error("munmap failed");
    }
}

/* This function executes the cgi programs and sends 
 * the output to clients.
 */
void serve_dynamic(char *method, char *fname, char *cgiargs) {
    char buffer[128];
    char * empList[] = {NULL};
    
    /* send Response Headers */
    sprintf(buffer, "HTTP/1.1 200 OK\r\nServer: Minimal HTTP Server\r\n\r\n");
    send_nbytes(confd, buffer, strlen(buffer));
    
    /* no Response Body for "HEAD" request */
    if (strcmp(method, "HEAD") == 0) {
        return;
    }
    
    /* send Response Body */
    setenv("QUERY_STRING", cgiargs, 1);
    dup2(confd, STDOUT_FILENO);
    if(execve(fname, empList, environ) < 0) {
        handle_error("execve failed");
    }
}

/* This function decodes potential HTML URL encoding.
 */
void decode_uri(char *uri) {
    char buffer[MAXSIZE + 1];
	int i, j;
	char a, b;

	j = 0;
	for(i=0; i < strlen(uri); i++) {
		if(uri[i] == '%' && i < strlen(uri) - 2 && isxdigit(a = uri[i+1]) && isxdigit(b = uri[i+2])) {
			a = tolower(a);
			b = tolower(b);
			if(a >= 'a')
				a -= 'a' - 10;
			else
				a -= '0';
			if(b >= 'a')
				b -= 'a' - 10;
			else
				b -= '0';
			buffer[j] = 16 * a + b;
			i += 2;
		}
		else if(uri[i] == '+')
			buffer[j] = ' ';
		else
			buffer[j] = uri[i];
		j++;
	}
	buffer[j] = 0;
    
    /* refresh memory */
    strcpy(uri, buffer);
}

/* This function parses uri to extract information 
 * of requested resources, and cgi arguments if any.
 */
int parse_uri(char *uri, char *fname, char *cgiargs) {
    int ctype;
    char * ptr;
    
    /* process cgi arguments */
    strcpy(cgiargs, "");
    /* default static content unless "cgi-bin" in uri*/
    ctype = 0;
    if(strstr(uri, "cgi-bin")) {
        ctype = 1;
        ptr = index(uri, '?');
        /* '?' in uri */
        if(ptr) {
            strcpy(cgiargs, ptr + 1);
            /* uri is fragmented */
            *ptr = '\0';
        }
    }

    /* process file name */
    strcpy(fname, ".");
    /* uri is original or fragmented */
    strcat(fname, uri);
    /* add DEFAULTFILE to file name if not 
     * specified in uri for static content
     */
    if (ctype == 0 && strcmp(uri, "/") == 0) {
        strcat(fname, DEFAULTFILE);
    }
    
    return ctype;
}

/* This function checks type of static contents 
 * to be sent to clients.
 */
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

/* This function closes confd and lisfd at exit.
 */
void cleanup(int signo) {
    /* shutdown before close? check here:
     * https://stackoverflow.com/questions
     * /4160347/close-vs-shutdown-socket
     */
    
    /*shutdown(confd, SHUT_RDWR);*/
    close(confd);
    
    /*shutdown(lisfd, SHUT_RDWR);*/
    close(lisfd);
    
    exit(EXIT_SUCCESS);
}

/* This function handles SIGCHLD signal.
 * https://www.cnblogs.com/wuchanming/p/4020463.html
 */
void wait_child(int signo) {  
    int status;
    while (waitpid(-1, &status, WNOHANG) > 0) {
        continue;
    }       
}

/* This function exits from current process after 
 * printing an error message and closing lisfd 
 * and confd.
 */
void handle_error(char *message) {
    char error_msg[101];
    int signo;
    
    strcpy(error_msg, "[!!] Error: ");
    strncat(error_msg, message, 88);
    perror(error_msg);
    
    close(confd);
    close(lisfd);
    
    exit(EXIT_FAILURE);
}

/* This function accepts a socket fd and a pointer 
 * to a destination buffer. It receives from the 
 * socket until the EOL shows, the socket is empty, 
 * or the destination buffer is full. The EOL 
 * bytes are read from the socket but nulled.
 * Returns the length of the read line (without 
 * the EOL bytes), or -1 if the read line is too 
 * long.
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
        /* error */
        if(received == -1) {
            handle_error("recv_nbytes failed");
        }
        /* socket is closed or half-closed */
        else if(received == 0) {
            break;
        }
        /* current byte matches a byte of EOL */
        if(*ptr == EOL[matched]) {
            matched++;
            /* EOL shows */
            if(matched == EOLSIZE) {
                *(ptr + 1 - EOLSIZE) = '\0';
                /* |a|a|\r|\n| -> |a|a|\0|\n| */
                return size_curr + 1 - EOLSIZE;
            }
        } else {
            matched = 0;
        }
        ptr++;
        size_curr++;
    }
    
    /* no EOL */
    return -1;
}

/* This function accepts a socket fd, a pointer to 
 * destination, and number of bytes to be received.
 */
void recv_nbytes(int sockfd, char *dst, int n) {
    char * ptr;
    int received;
    
    ptr = dst;
    /* receive a single byte every time */
    while(n > 0) {
        received = recv(sockfd, ptr, 1, 0);
        /* error */
        if(received == -1) {
            handle_error("recv_nbytes failed");
        }
        /* socket is closed or half-closed */
        else if(received == 0) {
            break;
        }
        ptr++;
        n--;
    }
}

/* This function accepts a socket fd, a pointer 
 * to source, and number of bytes to be sent.
 */
void send_nbytes(int sockfd, char *src, int n) {
    int sent;
    
    while(n > 0) {
        /* send not always send as long as required */
        sent = send(sockfd, src, n, MSG_NOSIGNAL);
        if(sent == -1) {
            handle_error("send_nbytes failed");
        }
        src += sent;
        n -= sent;
    }
}

