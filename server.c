/*
	A minimal HTTP/1.1 server based on Tiny
	http://csapp.cs.cmu.edu/3e/ics3/code/netp/tiny/tiny.c
	* iterative
	* supported methods: GET, HEAD, POST
	* supported static file types: html, gif, png, jpg, mp4, plain text
	* SIGCHLD implemented: https://www.cnblogs.com/wuchanming/p/4020463.html
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
#include <netdb.h>

#define DEFAULTHOME "index.html"
#define	MAXSIZE 8192
#define MAXLISTEN 5

extern char **environ;

void waitChild(int signo);
int openFd(char *port);
void serveContent(int fdConnect, FILE *stream);
int analyzeRequest(char *uri, char *fname, char *cgiArgs);
void checkFileType(char *fname, char *ftype);
void serveError(char *method, FILE *stream, char *cause, char *errNum, char *shortMsg, char *longMsg);
void serveStatic(char *method, int fdConnect, FILE *stream, char *fname, int fsize);
void serveDynamic(char *method, int fdConnect, FILE *stream, char *fname, char *cgiArgs);

int main(int argc, char *argv[]) {
	int fdListen, fdConnect;
	char hostname[MAXSIZE], port[MAXSIZE];
	socklen_t clientLen;
	struct sockaddr_storage clientaddr;
	FILE *stream;

	if (argc != 2) {
		printf("Usage: %s <port>\n", argv[0]);
		return 0;
	}
	
	fdListen = openFd(argv[1]);
	clientLen = sizeof(clientaddr);
	while (1) {
		fdConnect = accept(fdListen, (struct sockaddr *)&clientaddr, &clientLen);
		getnameinfo((struct sockaddr *)&clientaddr, clientLen, hostname, MAXSIZE, port, MAXSIZE, 0);
		printf("Connected %s: %s\n", hostname, port);
		/* fdConnect as stream */
		stream = fdopen(fdConnect, "r+");
		serveContent(fdConnect, stream);
		fclose(stream);
		close(fdConnect);
	}
	
	return 0;
}

void waitChild(int signo) {  
    int status;
    while (waitpid(-1, &status, WNOHANG) > 0) {
		;
    }		
}

int openFd(char *port) {
    struct addrinfo hints, * listp, * p;
    int fd, rc, optval = 1;

    /* get a list of potential server addresses */
    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_socktype = SOCK_STREAM;                  // accept connection
    hints.ai_flags = AI_PASSIVE | AI_ADDRCONFIG;      // any IP address
    hints.ai_flags |= AI_NUMERICSERV;                 // port as number
    if ((rc = getaddrinfo(NULL, port, &hints, &listp)) != 0) {
        printf("getaddrinfo failed on (port %s): %s\n", port, gai_strerror(rc));
        return -2;
    }

    /* iterate list for a bindable fd */
    for (p = listp; p; p = p->ai_next) {
        /* creating descriptor failed, check next */
        if ((fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) < 0) {
            continue;
		}
        /* eliminate "Address already in use" error from bind */
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const void *)&optval , sizeof(int));
        /* bind descriptor to address */
        if (bind(fd, p->ai_addr, p->ai_addrlen) == 0) {
            break;
		}
        if (close(fd) < 0) {
            printf("openFd failed to close: %s\n", strerror(errno));
            return -1;
        }
    }

    freeaddrinfo(listp);
	/* no address worked */
    if (!p) {
        return -1;
	}
    /* listen to accept connection requests, allowing MAXLISTEN requests to queue */
    if (listen(fd, MAXLISTEN) < 0) {
        close(fd);
	    return -1;
    }
    return fd;
}

void serveContent(int fdConnect, FILE *stream) {
    int contentType;
    struct stat meta;                                      // metadata of requested file
    char buf[MAXSIZE], method[MAXSIZE], uri[MAXSIZE], version[MAXSIZE];
    char fname[MAXSIZE], cgiArgs[MAXSIZE];
	int payloadSize = -1;
	pid_t pid;

	signal(SIGCHLD, waitChild);
	pid = fork();
	if (pid < 0) {
		perror("Failed to fork");
	}
    else if (pid == 0) {
		/* extract Request-Line and ignore Request Headers */
		fgets(buf, MAXSIZE, stream);                       // read up to MAXSIZE-1 bytes, terminate with '\0'
		printf("%s", buf);
		sscanf(buf, "%s %s %s", method, uri, version);
		// printf("%s\n", method);
		while (strcmp(buf, "\r\n")) {
			fgets(buf, MAXSIZE, stream);
			// printf("%s", buf);
			/* get payloadSize for POST */
			if (!strcmp(method, "POST")) {
				buf[15] = '\0';                            // check up to first 15 chars
				if (strcasecmp(buf, "Content-Length:") == 0) {
					payloadSize = atoi(&(buf[16]));
				}
			}
		}

		/* unsupported Method */
		if (strcmp(method, "GET") && strcmp(method, "HEAD") && strcmp(method, "POST")) {
			serveError(method, stream, method, "501", "Not Implemented", "Not Implemented");
			printf("Not Implemented: %s\n", method);
			return;
		}

		/* analyze URI for content type, file name, and cgi arguments if any */
		contentType = analyzeRequest(uri, fname, cgiArgs);
		if (stat(fname, &meta) < 0) {
			serveError(method, stream, fname, "404", "Not found", "Not found");
			return;
		}

		/* not regular file, not readable for static, not executable for dynamic */
		if (!S_ISREG(meta.st_mode) || (!(meta.st_mode & S_IRUSR) && contentType == 0) 
			|| (!(meta.st_mode & S_IXUSR) && contentType == 1)) {
			serveError(method, stream, fname, "403", "Forbidden", "Forbidden");
			return;
		}

		/* static content */
		if (contentType == 0) {
			serveStatic(method, fdConnect, stream, fname, meta.st_size);
		}
		/* dynamic content */
		else if (contentType == 1) {
			/* extract payload for POST */
			if (!strcmp(method, "POST")) {
				/* POST request without Content-Length */
				if (payloadSize == -1) {
					serveError(method, stream, method, "411", "Length Required", "Length Required");
					return;
				}
				fgets(cgiArgs, payloadSize + 1, stream);
				printf("%s\n", cgiArgs);
			}
			serveDynamic(method, fdConnect, stream, fname, cgiArgs);
		}
	}
}

int analyzeRequest(char *uri, char *fname, char *cgiArgs) {
	/* 0 - static content */
	int contentType = 0;
    char * p;

	strcpy(cgiArgs, "");                              // initiate cgiArgs

    /* 1 - dynamic content */
    if (strstr(uri, "cgi-bin")) {
		contentType = 1;
		p = index(uri, '?');
		/* '?' in uri */
		if (p) {
			strcpy(cgiArgs, p + 1);
			*p = '\0';                                // uri is fragmented
		}
    }

	strcpy(fname, ".");                               // initiate fname
	strcat(fname, uri);                               // uri is original or fragmented
	/* add DEFAULTHOME to file name if not specified in uri for static content */
	if (contentType == 0 && strcmp(uri, "/") == 0) {
		strcat(fname, DEFAULTHOME);
	}
	return contentType;
}

void checkFileType(char *fname, char *ftype) {
    if (strstr(fname, ".html")) {
		strcpy(ftype, "text/html");
	}
    else if (strstr(fname, ".gif")) {
		strcpy(ftype, "image/gif");
	}
    else if (strstr(fname, ".png")) {
		strcpy(ftype, "image/png");
	}
    else if (strstr(fname, ".jpg")) {
		strcpy(ftype, "image/jpeg");
	}
    else if (strstr(fname, ".mp4")) {
		strcpy(ftype, "video/mp4");
	}
    else {
		strcpy(ftype, "text/plain");
	}
}

void serveError(char *method, FILE *stream, char *cause, char *errNum, char *shortMsg, char *longMsg) {
	/* send Response Headers */
    fprintf(stream, "HTTP/1.1 %s %s\n", errNum, shortMsg);
	fprintf(stream, "Server: Minimal HTTP Server\n");
    fprintf(stream, "Content-type: text/html\n\r\n");

    if (strcmp(method, "HEAD") == 0) {
		return;
	}

	/* send Response Body */
    fprintf(stream, "<html>\n<head><title>Web Server Error</title></head>\n");
    fprintf(stream, "<body>\n<h1>%s: %s</h1>\n", errNum, shortMsg);
    fprintf(stream, "<p>%s: %s</p>\n</body>\n</html>\n", longMsg, cause);
}

void serveStatic(char *method, int fdConnect, FILE *stream, char *fname, int fsize) {
    int ffd;
    char *fp, ftype[100], buf[MAXSIZE];

	/* check file type */
	checkFileType(fname, ftype);

	/* send Response Headers */
	fprintf(stream, "HTTP/1.1 200 OK\n");
	fprintf(stream, "Server: Minimal HTTP Server\n");
	fprintf(stream, "Content-type: %s\n", ftype);
	fprintf(stream, "Content-length: %d\n\r\n", fsize);

	if (strcmp(method, "HEAD") == 0) {
		return;
	}

	/* send Response Body */
	fflush(stream);
	ffd = open(fname, O_RDONLY, 0);
	fp = mmap(0, fsize, PROT_READ, MAP_PRIVATE, ffd, 0);
	close(ffd);
	write(fdConnect, fp, fsize);
	munmap(fp, fsize);
}

void serveDynamic(char *method, int fdConnect, FILE *stream, char *fname, char *cgiArgs) {
    char *empList[] = {NULL};

	/* send Response Headers */
	fprintf(stream, "HTTP/1.1 200 OK\n");
	fprintf(stream, "Server: Minimal HTTP Server\n\r\n");

	if (strcmp(method, "HEAD") == 0) {
		return;
	}

	/* send Response Body */
	fflush(stream);
	setenv("QUERY_STRING", cgiArgs, 1);
	dup2(fdConnect, STDOUT_FILENO);
	if (execve(fname, empList, environ) < 0) {
		perror("Failed to execve");
	}
}
