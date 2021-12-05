/*
	A minimal HTTP/1.1 server for static and dynamic contents
	* iterative
	* GET only
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
#define	MAXSIZE	 8192
#define MAXLISTEN 5

extern char **environ;

int openFd(char *port);
void serveContent(int fdConnect, FILE *stream);
int analyzeRequest(char *uri, char *fname, char *cgiArgs);
void checkFileType(char *fname, char *ftype);
void serveStatic(int fdConnect, FILE *stream, char *fname, int fsize);
void serveDynamic(int fdConnect, FILE *stream, char *fname, char *cgiArgs);
void serveError(FILE *stream, char *cause, char *errNum, char *shortMsg, char *longMsg);

int main(int argc, char *argv[])
{
	int fdListen, fdConnect;
	char hostname[MAXSIZE], port[MAXSIZE];
	socklen_t clientLen;
	struct sockaddr_storage clientaddr;
	FILE *stream;                                     // fdConnect as stream

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
		stream = fdopen(fdConnect, "r+");
		serveContent(fdConnect, stream);
		fclose(stream);
		close(fdConnect);
	}
	
	return 0;
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

void serveContent(int fdConnect, FILE *stream)
{
	/* content type */
    int contentType;
	/* metadata of requested file */
    struct stat meta;
    char buf[MAXSIZE], method[MAXSIZE], uri[MAXSIZE], version[MAXSIZE];
    char fname[MAXSIZE], cgiArgs[MAXSIZE];

    /* extract Request Line and ignore Request Headers */
    fgets(buf, MAXSIZE, stream);
    printf("%s", buf);
    sscanf(buf, "%s %s %s", method, uri, version);
    while (strcmp(buf, "\r\n")) {
        fgets(buf, MAXSIZE, stream);
        // printf("%s", buf);
    }

	/* disallowed Method */
    if (strcasecmp(method, "GET") != 0) {
        serveError(stream, method, "501", "Not Implemented", "Not Implemented");
        return;
    }

    /* analyze URI for content type, file name, and cgi arguments if any */
    contentType = analyzeRequest(uri, fname, cgiArgs);
    if (stat(fname, &meta) < 0) {
		serveError(stream, fname, "404", "Not found", "Not found");
		return;
    }

    /* static content */
	if (contentType == 0) {
		if (!S_ISREG(meta.st_mode) || !(meta.st_mode & S_IRUSR)) {
			serveError(stream, fname, "403", "Forbidden", "Forbidden");
			return;
		}
		serveStatic(fdConnect, stream, fname, meta.st_size);
    }
	/* dynamic content */
    else if (contentType == 1) {
		if (!S_ISREG(meta.st_mode) || !(meta.st_mode & S_IXUSR)) {
			serveError(stream, fname, "403", "Forbidden", "Forbidden");
			return;
		}
		serveDynamic(fdConnect, stream, fname, cgiArgs);
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
	/* add DEFAULTHOME to file name if not specified in uri */
	if (contentType == 0 && uri[strlen(uri) - 1] == '/') {
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

void serveStatic(int fdConnect, FILE *stream, char *fname, int fsize) {
    int ffd;
    char *fp, ftype[100], buf[MAXSIZE];

    /* check file type */
    checkFileType(fname, ftype);
	/* send Response Headers */
	fprintf(stream, "HTTP/1.1 200 OK\n");
	fprintf(stream, "Server: Minimal HTTP Server\n");
	fprintf(stream, "Content-type: %s\n", ftype);
	fprintf(stream, "Content-length: %d\n\r\n", fsize);
	fflush(stream);

    /* send Response Body */
    ffd = open(fname, O_RDONLY, 0);
    fp = mmap(0, fsize, PROT_READ, MAP_PRIVATE, ffd, 0);
    close(ffd);
    write(fdConnect, fp, fsize);
    munmap(fp, fsize);
}

void serveDynamic(int fdConnect, FILE *stream, char *fname, char *cgiArgs) {
	pid_t pid;
    char *empList[] = {NULL};

	pid = fork();
	if (pid < 0) {
		perror("Failed to fork");
	}
	/* parent waits for and reaps child */
    else if (pid > 0) {
		wait(NULL);
	}
    else {
		/* send Response Headers */
		fprintf(stream, "HTTP/1.1 200 OK\n");
		fprintf(stream, "Server: Minimal HTTP Server\n\r\n");
		fflush(stream);

		/* send Response Body */
		setenv("QUERY_STRING", cgiArgs, 1);
		dup2(fdConnect, STDOUT_FILENO);
		if (execve(fname, empList, environ) < 0) {
			perror("Failed to execve");
		}
    }
}

void serveError(FILE *stream, char *cause, char *errNum, char *shortMsg, char *longMsg)
{
	/* send Response Headers */
    fprintf(stream, "HTTP/1.1 %s %s\n", errNum, shortMsg);
	fprintf(stream, "Server: Minimal HTTP Server\n");
    fprintf(stream, "Content-type: text/html\n\r\n");

    /* send Response Body */
    fprintf(stream, "<html>\n<head><title>Web Server Error</title></head>\n");
    fprintf(stream, "<body>\n<h1>%s: %s</h1>\n", errNum, shortMsg);
    fprintf(stream, "<p>%s: %s</p>\n</body>\n</html>\n", longMsg, cause);
}
