#include "csapp.h"

/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400

/* You won't lose style points for including this long line in your code */
static const char *user_agent_hdr = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n";

/* Cache structures and functions */
typedef struct {
    char *uri;              /* Key (URI) */
    char *content;          /* Web object content */
    int content_size;       /* Size of the content */
    int last_used;          /* Timestamp of last access */
    int is_valid;           /* Valid bit */
    pthread_rwlock_t lock;  /* Read-write lock for this cache entry */
} cache_entry_t;

typedef struct {
    cache_entry_t *entries; /* Array of cache entries */
    int num_entries;        /* Number of entries in cache */
    int timestamp;          /* Global timestamp counter */
    pthread_mutex_t mutex;  /* Mutex for accessing cache metadata */
} cache_t;

cache_t cache;              /* Global cache */

void cache_init();
void cache_deinit();
int cache_find(char *uri, char *buf);
void cache_insert(char *uri, char *buf, int size);
void cache_evict();
void sigpipe_handler(int sig);

void parse_uri(char *uri, char *hostname, char *path, int *port);
void build_http_header(char *http_header, char *hostname, char *path, rio_t *client_rio);
void *thread(void *vargp);
void doit(int fd);

int main(int argc, char **argv) 
{
    int listenfd, connfd;
    char hostname[MAXLINE], port[MAXLINE];
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;
    pthread_t tid;

    /* Check command line args */
    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(1);
    }

    /* Ignore SIGPIPE signals */
    Signal(SIGPIPE, sigpipe_handler);

    /* Initialize cache */
    cache_init();

    listenfd = Open_listenfd(argv[1]);
    while (1) {
        clientlen = sizeof(clientaddr);
        connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen);
        Getnameinfo((SA *) &clientaddr, clientlen, hostname, MAXLINE, 
                    port, MAXLINE, 0);
        
        /* Create a new thread to handle the client connection */
        int *connfdp = Malloc(sizeof(int));
        *connfdp = connfd;
        Pthread_create(&tid, NULL, thread, connfdp);
    }

    /* Clean up cache (though this code is never reached) */
    cache_deinit();
    return 0;
}

/* Thread routine */
void *thread(void *vargp) 
{
    int connfd = *((int *)vargp);
    Pthread_detach(pthread_self());
    Free(vargp);
    doit(connfd);
    Close(connfd);
    return NULL;
}

/* Handle SIGPIPE signals */
void sigpipe_handler(int sig) {
    return;
}

/* Handle HTTP transaction */
void doit(int fd) 
{
    char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
    char hostname[MAXLINE], path[MAXLINE];
    int port;
    
    rio_t rio, server_rio;
    
    /* Read request line and headers */
    Rio_readinitb(&rio, fd);
    if (!Rio_readlineb(&rio, buf, MAXLINE))
        return;
    
    sscanf(buf, "%s %s %s", method, uri, version);
    
    if (strcasecmp(method, "GET")) {
        // Return error for non-GET requests (would be better to send an HTTP error)
        return;
    }
    
    /* Check cache first */
    char cache_buf[MAX_OBJECT_SIZE];
    if (cache_find(uri, cache_buf)) {
        Rio_writen(fd, cache_buf, strlen(cache_buf));
        return;
    }
    
    /* Parse URI */
    parse_uri(uri, hostname, path, &port);
    
    /* Build HTTP header */
    char http_header[MAXLINE];
    build_http_header(http_header, hostname, path, &rio);
    
    /* Connect to server */
    char port_str[10];
    sprintf(port_str, "%d", port);
    int serverfd = Open_clientfd(hostname, port_str);
    if (serverfd < 0) {
        return;
    }
    
    /* Send HTTP request to server */
    Rio_readinitb(&server_rio, serverfd);
    Rio_writen(serverfd, http_header, strlen(http_header));
    
    /* Receive HTTP response from server and forward to client */
    char response_buf[MAX_OBJECT_SIZE];
    int response_size = 0;
    ssize_t n;
    while ((n = Rio_readnb(&server_rio, buf, MAXLINE)) > 0) {
        Rio_writen(fd, buf, n);
        
        /* Store in response buffer for caching if size permits */
        if (response_size + n <= MAX_OBJECT_SIZE) {
            memcpy(response_buf + response_size, buf, n);
            response_size += n;
        } else {
            /* Too big to cache */
            response_size = MAX_OBJECT_SIZE + 1; /* Mark as too big */
        }
    }
    
    /* Add to cache if size permits */
    if (response_size <= MAX_OBJECT_SIZE) {
        response_buf[response_size] = '\0'; /* Null-terminate for safety */
        cache_insert(uri, response_buf, response_size);
    }
    
    Close(serverfd);
}

/* Parse URI into hostname, path, and port */
void parse_uri(char *uri, char *hostname, char *path, int *port)
{
    *port = 80; /* Default port */
    
    /* Remove "http://" if present */
    char *ptr = strstr(uri, "http://");
    if (ptr) {
        ptr += 7; /* Move past "http://" */
    } else {
        ptr = uri; /* URI doesn't contain "http://" */
    }
    
    /* Extract hostname */
    char *host_end = strpbrk(ptr, ":/");
    if (!host_end) {
        /* No port or path specified */
        strcpy(hostname, ptr);
        strcpy(path, "/");
        return;
    }
    
    /* Copy hostname */
    strncpy(hostname, ptr, host_end - ptr);
    hostname[host_end - ptr] = '\0';
    
    /* Check if port is specified */
    if (*host_end == ':') {
        /* Port is specified */
        sscanf(host_end + 1, "%d", port);
        
        /* Find the start of the path */
        char *path_start = strchr(host_end, '/');
        if (path_start) {
            strcpy(path, path_start);
        } else {
            strcpy(path, "/");
        }
    } else {
        /* No port, just path */
        strcpy(path, host_end);
    }
}

/* Build HTTP header to send to the server */
void build_http_header(char *http_header, char *hostname, char *path, rio_t *client_rio)
{
    char buf[MAXLINE], request_hdr[MAXLINE], host_hdr[MAXLINE], other_hdr[MAXLINE];
    
    /* Initialize header buffers */
    sprintf(request_hdr, "GET %s HTTP/1.0\r\n", path);
    strcpy(host_hdr, "");
    strcpy(other_hdr, "");
    
    /* Read request headers from client */
    while (Rio_readlineb(client_rio, buf, MAXLINE) > 0) {
        /* Check for end of headers */
        if (!strcmp(buf, "\r\n")) break;
        
        /* Handle Host header */
        if (!strncasecmp(buf, "Host:", 5)) {
            strcpy(host_hdr, buf);
            continue;
        }
        
        /* Skip Connection, Proxy-Connection, and User-Agent headers */
        if (!strncasecmp(buf, "Connection:", 11) ||
            !strncasecmp(buf, "Proxy-Connection:", 17) ||
            !strncasecmp(buf, "User-Agent:", 11)) {
            continue;
        }
        
        /* Accumulate other headers */
        strcat(other_hdr, buf);
    }
    
    /* Create HTTP header */
    if (strlen(host_hdr) == 0) {
        sprintf(host_hdr, "Host: %s\r\n", hostname);
    }
    
    sprintf(http_header, "%s%s%s%s%s%s\r\n", 
            request_hdr,
            host_hdr,
            "Connection: close\r\n",
            "Proxy-Connection: close\r\n",
            user_agent_hdr,
            other_hdr);
}

/* Initialize cache */
void cache_init()
{
    cache.num_entries = 10; /* Number of cache entries */
    cache.entries = Calloc(cache.num_entries, sizeof(cache_entry_t));
    cache.timestamp = 0;
    
    pthread_mutex_init(&cache.mutex, NULL);
    
    for (int i = 0; i < cache.num_entries; i++) {
        cache.entries[i].is_valid = 0;
        pthread_rwlock_init(&cache.entries[i].lock, NULL);
    }
}

/* Clean up cache */
void cache_deinit()
{
    for (int i = 0; i < cache.num_entries; i++) {
        if (cache.entries[i].is_valid) {
            Free(cache.entries[i].uri);
            Free(cache.entries[i].content);
        }
        pthread_rwlock_destroy(&cache.entries[i].lock);
    }
    
    Free(cache.entries);
    pthread_mutex_destroy(&cache.mutex);
}

/* Find content in cache, return 1 if found, 0 if not found */
int cache_find(char *uri, char *buf)
{
    int found = 0;
    
    pthread_mutex_lock(&cache.mutex);
    cache.timestamp++; /* Increment global timestamp */
    int current_time = cache.timestamp;
    pthread_mutex_unlock(&cache.mutex);
    
    for (int i = 0; i < cache.num_entries; i++) {
        /* Try to get a read lock */
        pthread_rwlock_rdlock(&cache.entries[i].lock);
        
        if (cache.entries[i].is_valid && !strcmp(uri, cache.entries[i].uri)) {
            /* Found, copy content to buffer */
            memcpy(buf, cache.entries[i].content, cache.entries[i].content_size);
            buf[cache.entries[i].content_size] = '\0';
            found = 1;
            
            /* Update timestamp */
            pthread_mutex_lock(&cache.mutex);
            cache.entries[i].last_used = current_time;
            pthread_mutex_unlock(&cache.mutex);
        }
        
        pthread_rwlock_unlock(&cache.entries[i].lock);
        
        if (found) break;
    }
    
    return found;
}

/* Insert content into cache */
void cache_insert(char *uri, char *buf, int size)
{
    pthread_mutex_lock(&cache.mutex);
    
    /* Check if we need to evict first */
    int total_size = size;
    for (int i = 0; i < cache.num_entries; i++) {
        if (cache.entries[i].is_valid) {
            total_size += cache.entries[i].content_size;
        }
    }
    
    /* Evict entries until we have enough space */
    while (total_size > MAX_CACHE_SIZE) {
        cache_evict();
        
        /* Recalculate total size */
        total_size = size;
        for (int i = 0; i < cache.num_entries; i++) {
            if (cache.entries[i].is_valid) {
                total_size += cache.entries[i].content_size;
            }
        }
    }
    
    /* Find an empty slot or the least recently used one */
    int min_timestamp = cache.timestamp;
    int min_index = 0;
    
    for (int i = 0; i < cache.num_entries; i++) {
        if (!cache.entries[i].is_valid) {
            min_index = i;
            break;
        }
        
        if (cache.entries[i].last_used < min_timestamp) {
            min_timestamp = cache.entries[i].last_used;
            min_index = i;
        }
    }
    
    /* Get a write lock on the selected entry */
    pthread_rwlock_wrlock(&cache.entries[min_index].lock);
    
    /* If entry was valid, free old content */
    if (cache.entries[min_index].is_valid) {
        Free(cache.entries[min_index].uri);
        Free(cache.entries[min_index].content);
    }
    
    /* Insert new content */
    cache.entries[min_index].uri = Malloc(strlen(uri) + 1);
    strcpy(cache.entries[min_index].uri, uri);
    
    cache.entries[min_index].content = Malloc(size);
    memcpy(cache.entries[min_index].content, buf, size);
    
    cache.entries[min_index].content_size = size;
    cache.entries[min_index].last_used = cache.timestamp;
    cache.entries[min_index].is_valid = 1;
    
    pthread_rwlock_unlock(&cache.entries[min_index].lock);
    pthread_mutex_unlock(&cache.mutex);
}

/* Evict an entry from the cache using LRU policy */
void cache_evict()
{
    int min_timestamp = cache.timestamp;
    int min_index = 0;
    
    /* Find least recently used entry */
    for (int i = 0; i < cache.num_entries; i++) {
        if (cache.entries[i].is_valid && cache.entries[i].last_used < min_timestamp) {
            min_timestamp = cache.entries[i].last_used;
            min_index = i;
        }
    }
    
    /* Get a write lock on the selected entry */
    pthread_rwlock_wrlock(&cache.entries[min_index].lock);
    
    /* Free resources */
    if (cache.entries[min_index].is_valid) {
        Free(cache.entries[min_index].uri);
        Free(cache.entries[min_index].content);
        cache.entries[min_index].is_valid = 0;
    }
    
    pthread_rwlock_unlock(&cache.entries[min_index].lock);
}
