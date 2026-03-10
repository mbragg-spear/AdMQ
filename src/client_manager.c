#include "client_manager.h"
#include "auth.h"
#include "hash.h"
#include "pubsub.h"

#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static HashTable* clients_map;
static pthread_rwlock_t clients_rwlock;

void client_manager_init() {
    clients_map = create_table();
    pthread_rwlock_init(&clients_rwlock, NULL);
}

void client_add(int fd, int conn_type) {
    Client* c = malloc(sizeof(Client));
    c->fd = fd;
    c->state = STATE_IDLE;
    c->conn_type = conn_type;
    c->auth_status = AUTH_PENDING;
    c->ssl = NULL;
    c->hostname[0] = '\0';
    c->last_activity = time(NULL);
    c->buffer_len = 0;
    pthread_mutex_init(&c->lock, NULL);

    char fd_key[32];
    snprintf(fd_key, sizeof(fd_key), "fd:%d", fd);

    pthread_rwlock_wrlock(&clients_rwlock);
    set(clients_map, fd_key, c);
    pthread_rwlock_unlock(&clients_rwlock);
}

void client_remove(int fd) {
    char fd_key[32];
    snprintf(fd_key, sizeof(fd_key), "fd:%d", fd);

    Client *c = NULL;

    pthread_rwlock_wrlock(&clients_rwlock);
    c = (Client*)get(clients_map, fd_key);
    if (c) {
        del(clients_map, fd_key);
        // Only remove the hostname map if it actively points to THIS client (prevents breaking reconnects)
        if (strlen(c->hostname) > 0) {
            Client* current_c = (Client*)get(clients_map, c->hostname);
            if (current_c == c) {
                del(clients_map, c->hostname);
            }
        }
    }
    pthread_rwlock_unlock(&clients_rwlock);

    if (c) {
        // Safe destruction: wait for any active worker threads using c->lock to finish
        pthread_mutex_lock(&c->lock);

        if (c->ssl) {
            SSL_shutdown(c->ssl);
            SSL_free(c->ssl);
            c->ssl = NULL;
        }
        if (c->fd > 0) {
            close(c->fd);
            c->fd = -1;
        }
        c->state = STATE_DISCONNECTED;

        pthread_mutex_unlock(&c->lock);
        pthread_mutex_destroy(&c->lock);
        free(c);
    }
}

Client* client_get_and_lock_by_fd(int fd) {
    char key[32];
    snprintf(key, sizeof(key), "fd:%d", fd);

    pthread_rwlock_rdlock(&clients_rwlock);
    Client* c = (Client*)get(clients_map, key);
    if (c) {
        pthread_mutex_lock(&c->lock); // Acquire the individual mutex before releasing the overarching read lock
    }
    pthread_rwlock_unlock(&clients_rwlock);
    return c;
}

Client* client_get_and_lock_by_hostname(const char* hostname) {
    pthread_rwlock_rdlock(&clients_rwlock);
    Client* c = (Client*)get(clients_map, hostname);
    if (c) {
        pthread_mutex_lock(&c->lock);
    }
    pthread_rwlock_unlock(&clients_rwlock);
    return c;
}

void client_unlock(Client* c) {
    if (c) {
        pthread_mutex_unlock(&c->lock);
    }
}

void client_set_hostname(int fd, const char* hostname) {
    char fd_key[32];
    snprintf(fd_key, sizeof(fd_key), "fd:%d", fd);

    pthread_rwlock_wrlock(&clients_rwlock);
    Client *c = (Client*)get(clients_map, fd_key);
    if (c) {
        set(clients_map, hostname, c); // Implement secondary lookup using the hostname
    }
    pthread_rwlock_unlock(&clients_rwlock);

    if (c) {
        pthread_mutex_lock(&c->lock);
        strncpy(c->hostname, hostname, sizeof(c->hostname) - 1);
        pthread_mutex_unlock(&c->lock);
    }
}

void client_buffer_append(Client* c, const char* data, int len) {
    if (c->buffer_len + len >= sizeof(c->buffer)) {
        printf("Warning: Client %d buffer overflow. Dropping data.\n", c->fd);
        c->buffer_len = 0;
        return;
    }
    memcpy(&c->buffer[c->buffer_len], data, len);
    c->buffer_len += len;
    c->buffer[c->buffer_len] = '\0';
}

int client_buffer_extract_line(Client* c, char* out_message, int max_len) {
    if (c->buffer_len == 0) return 0;

    char* newline_ptr = strchr(c->buffer, '\n');
    if (newline_ptr == NULL) return 0;

    int msg_len = newline_ptr - c->buffer;
    int copy_len = (msg_len < max_len - 1) ? msg_len : max_len - 1;
    strncpy(out_message, c->buffer, copy_len);
    out_message[copy_len] = '\0';

    int bytes_to_remove = msg_len + 1;
    int remaining_bytes = c->buffer_len - bytes_to_remove;

    if (remaining_bytes > 0) {
        memmove(c->buffer, &c->buffer[bytes_to_remove], remaining_bytes);
    }
    c->buffer_len = remaining_bytes;
    c->buffer[remaining_bytes] = '\0';

    return 1;
}

void client_manager_sweep_inactive(int timeout_seconds) {
    time_t now = time(NULL);
    int fds_to_remove[100];
    int remove_count = 0;

    pthread_rwlock_rdlock(&clients_rwlock);
    for (int i = 0; i < TABLE_SIZE; i++) {
        Entry *entry = clients_map->buckets[i];
        while (entry != NULL) {
            // Check only the network fd mapping bounds to iterate unique devices
            if (strncmp(entry->key, "fd:", 3) == 0) {
                Client *c = (Client*)entry->value;
                pthread_mutex_lock(&c->lock);
                if (c->state == STATE_IDLE && (now - c->last_activity > timeout_seconds)) {
                    fds_to_remove[remove_count++] = c->fd;
                }
                pthread_mutex_unlock(&c->lock);
            }
            entry = entry->next;
        }
    }
    pthread_rwlock_unlock(&clients_rwlock);

    for (int i = 0; i < remove_count; i++) {
        printf("\n[Heartbeat] Sweeping disconnected FD %d...\nadmq> ", fds_to_remove[i]);
        fflush(stdout);
        pubsub_unsubscribe_all(fds_to_remove[i]);
        client_remove(fds_to_remove[i]);
    }
}

void client_manager_print_status() {
    printf("\n=== CONNECTED AGENTS ===\n");
    int count = 0;

    pthread_rwlock_rdlock(&clients_rwlock);
    for (int i = 0; i < TABLE_SIZE; i++) {
        Entry *entry = clients_map->buckets[i];
        while (entry != NULL) {
            if (strncmp(entry->key, "fd:", 3) == 0) {
                Client *c = (Client*)entry->value;
                pthread_mutex_lock(&c->lock);

                char* name = (strlen(c->hostname) > 0) ? c->hostname : "Unknown/Pending";
                printf("  [FD: %d] %s\n", c->fd, name);
                count++;

                pthread_mutex_unlock(&c->lock);
            }
            entry = entry->next;
        }
    }
    pthread_rwlock_unlock(&clients_rwlock);

    if (count == 0) printf("  No agents connected.\n");
    printf("========================\n");
}
