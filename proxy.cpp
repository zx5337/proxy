//
//  main.cpp
//  proxy
//
//  Created by Zhang Richar on 16/4/11.
//  Copyright © 2016年 Richar Zhang. All rights reserved.
//

#include <iostream>
#include <unistd.h>
#include <stdio.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <signal.h>
#include <errno.h>
#include <queue>
#include <mutex>
#include <pthread.h>
#include <string>
#include <unordered_map>
#include "csapp.h"

#define MAX_WORKER 30
#define MAX_LINE 1024
#define MAX_NAME 40
#define SLEEP_TIMER 1
#define MAX_UINT 32768
#define CACHE_LENGTH 1024


/*
 * read_requesthdrs - read and parse HTTP request headers
 */
void read_requesthdrs(rio_t *rp, char *method, char *uri, char *version)
{
    char buf[MAXLINE];
    
    Rio_readlineb(rp, buf, MAXLINE);
    
    sscanf(buf, "%s %s %s\n", method, uri, version);
    
    return;
}

/*
 * parse_uri - URI parser
 *
 * Given a URI from an HTTP proxy GET request (i.e., a URL), extract
 * the host name, path name, and port.  The memory for hostname and
 * pathname must already be allocated and should be at least MAXLINE
 * bytes. Return -1 if there are any problems.
 */
int parse_uri(char *uri, char *hostname, char *pathname, int *port)
{
    char *hostbegin;
    char *hostend;
    char *pathbegin;
    int len;
    
    if (strncasecmp(uri, "http://", 7) != 0) {
        hostname[0] = '\0';
        return -1;
    }
    
    /* Extract the host name */
    hostbegin = uri + 7;
    hostend = strpbrk(hostbegin, " :/\r\n\0");
    len = hostend - hostbegin;
    strncpy(hostname, hostbegin, len);
    hostname[len] = '\0';
    
    /* Extract the port number */
    *port = 80; /* default */
    if (*hostend == ':')
        *port = atoi(hostend + 1);
    
    /* Extract the path */
    pathbegin = strchr(hostbegin, '/');
    if (pathbegin == NULL) {
        pathname[0] = '\0';
    }
    else {
        //pathbegin++;
        strcpy(pathname, pathbegin);
    }
    
    return 0;
}

class proxy_cache{
public:
    int size;
    char** buf;
    std::mutex mu;
    proxy_cache(){
        size = 0;
        buf = (char **)malloc(sizeof(char*)*CACHE_LENGTH);
    }
    bool add_cache(char * cache){
        
        if(size >= CACHE_LENGTH)
            return false;
        
        buf[size++]=cache;
        return true;
    }
};

class RequestQueue{
private:
    std::queue<int> q;
    std::mutex mu;
public:
#ifdef WITH_CACHE
    std::unordered_map<std::string, proxy_cache *> cachemap;
    RequestQueue(){
        
    }
    std::unordered_map<std::string, proxy_cache *> * GetMap(){
        return &cachemap;
    }
#endif
    void lockmap(){
        mu.lock();
    }
    void unlockmap(){
        mu.unlock();
    }
    void Push(int v){
        mu.lock();
        q.push(v);
        mu.unlock();
    }
    int Pop(){
        mu.lock();
        int ret = -1;
        if (!q.empty()) {
            ret = q.front();
            q.pop();
        }
        mu.unlock();
        return ret;
    }
    bool isEmpty(){
        bool res = q.empty();
        return res;
    }
};

class WorkerThread{
private:
    int clientsock;
    int proxysock;
    int threadID;
    RequestQueue * queue;
#ifdef WITH_CACHE
    std::unordered_map<std::string, proxy_cache *> *pmap;
#endif
public:
    WorkerThread(RequestQueue * _queue){
        queue = _queue;
        static int id =1;
        static std::mutex lock;
        lock.lock();
        threadID = id++;
        lock.unlock();
        
#ifdef WITH_CACHE
        pmap = _queue->GetMap();
#endif
    }
    int open_targetfd(char* hostname, char* port){
        int clientfd = 0, rc;
        struct addrinfo hints, *listp, *p;
        
        /* Get a list of potential server addresses */
        memset(&hints, 0, sizeof(struct addrinfo));
        hints.ai_socktype = SOCK_STREAM;  /* Open a connection */
        hints.ai_flags = AI_NUMERICSERV;  /* ... using a numeric port arg. */
        hints.ai_flags |= AI_ADDRCONFIG;  /* Recommended for connections */

        if ((rc = getaddrinfo(hostname, port, &hints, &listp)) != 0) {
            fprintf(stderr, "TID(%d) getaddrinfo failed (%s:%s): %s\n", threadID, hostname, port, gai_strerror(rc));
            return -2;
        }
        
        /* Walk the list for one that we can successfully connect to */
        for (p = listp; p; p = p->ai_next) {
            /* Create a socket descriptor */
            if ((clientfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) < 0)
                continue; /* Socket failed, try the next */
            
            /* Connect to the server */
            if (connect(clientfd, p->ai_addr, p->ai_addrlen) != -1)
                break; /* Success */
            if (close(clientfd) < 0) { /* Connect failed, try another */  //line:netp:openclientfd:closefd
                fprintf(stderr, "TID(%d) open_clientfd: close failed: %s\n",threadID, strerror(errno));
                return -1;
            }
        }
        
        /* Clean up */
        freeaddrinfo(listp);
        if (!p) /* All connects failed */
            return -1;
        else    /* The last connect succeeded */
            return clientfd;
    }
    void mainloop(){
    
    rio_t rp_client, rp_server;
    int  hostport;
    char hostname[MAXLINE], pathname[MAXLINE];
    char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
    while (1) {
        char buffer[MAX_UINT];
        while (queue->isEmpty()) {
            sleep(1);
            continue;
        }
        clientsock = queue->Pop();
        if(clientsock == -1){
            //printf("Fake Client sockID:%d \n", clientsock);
            continue;
        }
        //printf("Get Client sockID:%d \n", clientsock);
        // Init read structure for client-proxy connection
        Rio_readinitb(&rp_client, clientsock);
        
        // Parse the HTTP request
        read_requesthdrs(&rp_client, method, uri, version);
        
        // Parse the URI of the request
        parse_uri(uri, hostname, pathname, &hostport);
        //printf("%s %s %d\n", hostname, pathname, hostport);
        /*****  If method is GET *****/
        if (strcmp(method, "GET")!=0) {
            char buf2[] = "500 'Internal Error' \r\n";
            Rio_writenb_w(clientsock, buf2, strlen(buf2));
            //printf("It is not GET method:%s socket:%d \n", method, clientsock);
            Rio_close(clientsock);
            continue;
        }
        
#ifdef WITH_CACHE
        //find url in cache and return [FIXME]
        if(pmap->find(uri)!=pmap->end()){ //not found the topic
            
            proxy_cache *pcache = (*pmap)[uri];
            
            for (int i =0; i<pcache->size; i++) {
                Rio_writen_w(clientsock, pcache->buf[i], strlen(pcache->buf[i]));
            }
            Rio_close(clientsock);
            continue;
        }
        // Make cache entry [FIXME]
        proxy_cache *pcache = new proxy_cache();
        std::string urlstr = *new std::string(uri);
        std::pair<std::string, proxy_cache *> newpair(urlstr,pcache);
        pmap->insert(newpair);
#endif
        // Open connection to requested web server
        char strport[10]={0};
        sprintf(strport, "%d",hostport);
        proxysock = open_targetfd(hostname, strport);
        
        if (proxysock < 0) {
            char buf2[] = "500 'Internal Error' \r\n";
            Rio_writenb_w(clientsock, buf2, strlen(buf2));
            Rio_close(clientsock);
            continue;
        }
        
        // Init read struct for proxy-webserver connection
        Rio_readinitb(&rp_server, proxysock);
        
        sprintf(buf, "%s %s %s\r\n", method, pathname, "HTTP/1.0");
        Rio_writenb_w(proxysock, buf, strlen(buf));
        //printf("%s", buf);
        sprintf(buf, "Host: %s\r\n", hostname);
        Rio_writenb_w(proxysock, buf, strlen(buf));
        //printf("%s", buf);

        // Read from client request
        // and write to web server
        while(strcmp(buf, "\r\n")) {
            Rio_readlineb_w(&rp_client, buf, MAXLINE);
            
            if (!strcmp(buf, "\r\n")) {
                char buf2[] = "Connection: close\r\n";
                Rio_writenb_w(proxysock, buf2, strlen(buf2));
                //printf("%s", buf2);
            }
            if (!strncmp(buf, "Connection: keep-alive", 22) ||
                !strncmp(buf, "Host:", 5)) {
                //printf("%s", buf);
                continue;
            }
            
            Rio_writenb_w(proxysock, buf, strlen(buf));
            //printf("%s", buf);
            
        }
        
        // Read the respons from webserver and
        // forward it to the requesting client
        ssize_t n = 0;
        while ((n = Rio_readnb_w(proxysock, buffer, MAX_UINT)) > 0) {
#ifdef WITH_CACHE
            char *cache = (char*)malloc(sizeof(char)*(n+1));
            if(cache == NULL){
                perror("Alloc memory failed, cache emited");
            }
            memset(cache, 0, n+1);
            strncpy(cache,buffer,n);
#endif
            Rio_writenb_w(clientsock, buffer, n);
            
        }
        
        Rio_close(clientsock);
        Rio_close(proxysock);
    }
}
    
};
void cleanExit(int arg){
    exit(0);
}
void *Thread_Func(void *arg)
{
    WorkerThread *worker = new WorkerThread((RequestQueue *)arg);
    signal(SIGTERM, cleanExit);
    signal(SIGINT, cleanExit);
    signal(SIGPIPE, SIG_IGN);
    worker->mainloop();
    
    pthread_exit(NULL);
}

void init_worker_threads(RequestQueue *queue){
    pthread_t threads[MAX_WORKER];
    for(int i=0; i <= MAX_WORKER; i++ ){
        int rc = pthread_create(&threads[i], NULL,
                                Thread_Func, (void *)queue);
        if (rc){
            std::cout << "Error:unable to create thread," << rc << std::endl;
            exit(-1);
        }
    }
}


// get sockaddr, IPv4 or IPv6:
void *get_in_addr(struct sockaddr *sa)
{

    if (sa->sa_family == AF_INET) {
        return &(((struct sockaddr_in*)sa)->sin_addr);
    }
    
    return &(((struct sockaddr_in6*)sa)->sin6_addr);
}


void sig_pipe(int sign)
{
    printf("Catch a SIGPIPE signal\n");
}
int main(int argc, const char * argv[]) {
    // insert code here...
    struct sockaddr_in servaddr, cliaddr;
    socklen_t cliaddr_len;
    int listenfd, connfd;
    int ret = 0;
    char s[INET6_ADDRSTRLEN];
    int serv_port;
    
    
    
    if (argc != 2) {
        fputs("usage: ./porxy port\n", stderr);
        exit(1);
    }
    sscanf(argv[1],"%d",&serv_port);
    signal(SIGTERM, cleanExit);
    signal(SIGINT, cleanExit);
    signal(SIGPIPE, SIG_IGN);
    
    listenfd = socket(AF_INET, SOCK_STREAM, 0);
    
    bzero(&servaddr, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port = htons(serv_port);
    
    ret = bind(listenfd, (struct sockaddr *)&servaddr, sizeof(servaddr));
    if(ret == -1){
        printf("Bind socket error: errno=%d port=%d\n",errno,serv_port);
        return 0;
    }
    
    ret = listen(listenfd, 20);
    if(ret == -1){
        printf("Listen socket error: errno=%d port=%d\n",errno,serv_port);
        return 0;
    }
    
    /*
    int n = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_NOSIGPIPE, &n, sizeof(n));
    */
    printf("Proxy Server started port:%d, accepting connections ...\n",serv_port);
    
    RequestQueue *queue = new RequestQueue();
    init_worker_threads(queue);
    while (1) {
        
        
        cliaddr_len = sizeof(cliaddr);
        connfd = accept(listenfd,(struct sockaddr *)&cliaddr, &cliaddr_len);
        if (connfd == -1) {
            if (errno == EINTR)//continue by system interupt
                continue;
            else {
                
                printf("Accept socket error: errno=%d port=%d\n",errno,serv_port);
                return 0;
            }
        }
        
        inet_ntop(cliaddr.sin_family,
                  get_in_addr((struct sockaddr *)&cliaddr),
                  s, sizeof s);
        //printf("Proxy Server accept connection from %s  sockID: %d\n", s ,connfd);
        
        //thread pool or request queue
        queue->Push(connfd);
        
        
        
    }
    return 0;
}
