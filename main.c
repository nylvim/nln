#define _XOPEN_SOURCE 500

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <arpa/inet.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/random.h>
#include <sys/time.h>
#include <unistd.h>

/* CONFIG */
// allowed special characters: -._~:@!$&'()*+,;=
constexpr char LINK_CHARSET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
constexpr size_t MIN_LINK_LEN = 4;
constexpr size_t MAX_LINK_LEN = 8;
constexpr size_t DEFAULT_LINK_LEN = 6;
constexpr size_t MAX_LINK_GEN_RETRY = 10;

constexpr char DB_FILENAME[] = "nln.db";
constexpr unsigned int SAVE_DB_INTERVAL = 10;
constexpr bool USE_301 = true;
constexpr uint16_t DEFAULT_PORT = 6174;
constexpr time_t TRANSFER_TIMEOUT = 5;
constexpr size_t MAX_ACTIVE_THREADS = 64;
/* CONFIG END */

static_assert(MIN_LINK_LEN > 0);
static_assert(MAX_LINK_LEN < 32);
static_assert(DEFAULT_LINK_LEN >= MIN_LINK_LEN);
static_assert(DEFAULT_LINK_LEN <= MAX_LINK_LEN);

typedef struct {
    char key[32];
    char value[4096];
} Pair;

Pair pair_new(const char* key, const char* value) {
    Pair p = {};
    strncpy(p.key, key, 31);
    strncpy(p.value, value, 4095);
    return p;
}

typedef struct {
    Pair* pairs;
    size_t len;
    size_t size;
    bool is_modified;
} Db;

Db db_new(size_t size) {
    size = size > 4 ? size : 4;
    auto pairs = malloc(size * sizeof(Pair));
    return (Db){pairs, 0, size, false};
}

Db db_clone(const Db* db) {
    Pair* pairs = malloc(db->len * sizeof(Pair));
    memcpy(pairs, db->pairs, db->len * sizeof(Pair));
    return (Db){pairs, db->len, db->len, db->is_modified};
}

void db_insert_at(Db* db, size_t pos, Pair pair) {
    if (db->size <= db->len) {
        db->size *= 2;
        db->pairs = realloc(db->pairs, db->size * sizeof(Pair));
    }
    memmove(db->pairs + pos + 1, db->pairs + pos, (db->len - pos) * sizeof(Pair));
    db->pairs[pos] = pair;
    db->len++;
    db->is_modified = true;
}

size_t db_bsearch(const Db* db, const char* target, bool* is_present) {
    size_t low = 0;
    size_t high = db->len;
    while (low < high) {
        auto mid = (low + high) / 2;
        auto res = strcmp(db->pairs[mid].key, target);
        if (res == 0) {
            *is_present = true;
            return mid;
        } else if (res < 0) {
            low = mid + 1;
        } else if (res > 0) {
            high = mid;
        }
    }
    *is_present = false;
    return low;
}

char* db_remove_key(Db* db, const char* key) {
    bool is_present;
    auto pos = db_bsearch(db, key, &is_present);
    if (!is_present) { return nullptr; }
    auto value = strdup(db->pairs[pos].value);
    memmove(db->pairs + pos, db->pairs + pos + 1, (db->len - pos - 1) * sizeof(Pair));
    db->len--;
    db->is_modified = true;
    return value;
}

size_t db_remove_all_values(Db* db, const char* value) {
    size_t read = 0;
    size_t write = 0;
    for (; read < db->len; read++) {
        if (strcmp(value, db->pairs[read].value) != 0) {
            if (read != write) { db->pairs[write] = db->pairs[read]; }
            write++;
        }
    }
    db->len = write;
    db->is_modified |= read - write != 0;
    return read - write;
}

bool db_save(const Db* db, const char* filename) {
    char tmp_filename[strlen(filename) + 5];
    sprintf(tmp_filename, "%s.tmp", filename);
    auto file = fopen(tmp_filename, "w");
    if (!file) { return false; }

    for (size_t i = 0; i < db->len; i++) {
        auto pair = db->pairs[i];
        fprintf(file, "%s %s\n", pair.key, pair.value);
    }

    fclose(file);
    auto res = rename(tmp_filename, filename);
    return res == 0;
}

Db db_load(const char* filename) {
    auto file = fopen(filename, "r");
    if (!file) { return (Db){}; }

    auto db = db_new(4);
    int res;
    char key[32];
    char value[4096];
    while ((res = fscanf(file, "%31s %4095[^\n]\n", key, value)) != EOF) {
        if (res != 2) { continue; }
        db_insert_at(&db, db.len, pair_new(key, value));
    }

    fclose(file);
    db.is_modified = false;
    return db;
}

// server stuff

volatile bool KEEP_RUNNING = true;
sem_t THREADS;
const char* AUTH_TOKEN;
Db GLOBAL_DB;
pthread_rwlock_t DB_LOCK = PTHREAD_RWLOCK_INITIALIZER;

bool send_all(int socket, const void* buffer, size_t len) {
    while (len > 0) {
        auto len_sent = send(socket, buffer, len, 0);
        if (len_sent < 0) { return false; }
        buffer += len_sent;
        len -= len_sent;
    }
    return true;
}

bool auth(const char* header) {
    const char* line_break;
    while ((line_break = strstr(header, "\r\n"))) { // ignore first line
        header = line_break + 2;
        if (strncmp(header, "Authorization: Bearer ", 22) == 0) {
            header += 22;
            line_break = strstr(header, "\r\n");
            return line_break && strncmp(header, AUTH_TOKEN, line_break - header) == 0;
        }
    }
    return false;
}

bool query_arg(const char* query, const char* name) {
    auto delim = query - 1;
    char key[8];
    char val[8];

    do {
        query = delim + 1;
        auto res = sscanf(query, "%7[^=&]=%7[^=&]", key, val);
        if (res < 2) { return false; }
        if (strcmp(key, name) == 0) { return strcmp(val, "true") == 0; }
    } while ((delim = strchr(query, '&')));

    return false;
}

bool validate_url(const char* url) {
    if (strncmp(url, "https://", 8) == 0) {
        url += 8;
    } else if (strncmp(url, "http://", 7) == 0) {
        url += 7;
    } else {
        return false;
    }

    do { // reject empty URL
        if (*url <= 0x20 || *url >= 0x7F) { return false; }
        switch (*url) {
        case '"':
        case '\'':
        case '<':
        case '>':
        case '[':
        case '\\':
        case ']':
        case '^':
        case '`':
        case '{':
        case '|':
        case '}':
            return false;
        }
        url++;
    } while (*url);

    return true;
}

char* create_link(size_t len, const char* url) {
    constexpr unsigned char RAND_MAX_ALLOW = 255 - (256 % (sizeof(LINK_CHARSET) - 1));

    char* link = malloc(len + 1);
    for (size_t try = 0; try < MAX_LINK_GEN_RETRY; try++) {
        for (size_t i = 0; i < len; i++) {
            unsigned char rand;
            do { getrandom(&rand, 1, 0); } while (rand > RAND_MAX_ALLOW);
            link[i] = LINK_CHARSET[rand % (sizeof(LINK_CHARSET) - 1)];
        }
        link[len] = 0;

        bool is_present;
        auto pos = db_bsearch(&GLOBAL_DB, link, &is_present);
        if (!is_present) {
            db_insert_at(&GLOBAL_DB, pos, pair_new(link, url));
            return link;
        }
    }

    free(link);
    return nullptr;
}

#define OK "HTTP/1.1 200 OK\r\n"
#define NO_CONTENT "HTTP/1.1 204 No Content\r\n"
#define MOVED_PERMANENTLY "HTTP/1.1 301 Moved Permanently\r\n"
#define FOUND "HTTP/1.1 302 Found\r\n"
#define BAD_REQUEST "HTTP/1.1 400 Bad Request\r\n"
#define FORBIDDEN "HTTP/1.1 403 Forbidden\r\n"
#define NOT_FOUND "HTTP/1.1 404 Not Found\r\n"
#define METHOD_NOT_ALLOWED "HTTP/1.1 405 Method Not Allowed\r\n"
#define NOT_ACCEPTABLE "HTTP/1.1 406 Not Acceptable\r\n"
#define CONFLICT "HTTP/1.1 409 Conflict\r\n"
#define CONTENT_TOO_LARGE "413 Content Too Large\r\n"
#define UNPROCESSABLE_CONTENT "HTTP/1.1 422 Unprocessable Content\r\n"
#define INTERNAL_SERVER_ERROR "HTTP/1.1 500 Internal Server Error\r\n"

#define RESPOND_WITH(code)                                                                         \
    constexpr char _MSG[] = code "Content-Length: 0\r\n\r\n";                                      \
    send_all(client, _MSG, sizeof(_MSG) - 1)

#define RESPOND_WITH_ERR(code)                                                                     \
    RESPOND_WITH(code);                                                                            \
    goto cleanup

#define RESPOND_WITH_BODY(code, body)                                                              \
    char _msg[8192];                                                                               \
    snprintf(_msg, 8192, code "Content-Length: %zu\r\n\r\n%s", strlen(body), body);                \
    send_all(client, _msg, strlen(_msg))

void* handle_request(void* arg) {
    auto client = (int)(uintptr_t)arg;

    char request[8192];
    size_t req_len = 0;
    char* header_end;
    while (req_len < 8191) {
        auto len_received = recv(client, request + req_len, 8191 - req_len, 0);
        if (len_received <= 0) { goto cleanup; }
        req_len += len_received;
        request[req_len] = 0;
        header_end = strstr(request + req_len - len_received, "\r\n\r\n");
        if (header_end) { break; }
    }
    if (req_len < sizeof("GET /x HTTP/1.1\r\n") - 1) { goto cleanup; }

    size_t body_len = 0;
    auto cl_str = strstr(request, "\r\nContent-Length: ");
    if (cl_str && cl_str < header_end) {
        if (sscanf(cl_str + 18, "%zu", &body_len) != 1) { goto cleanup; }
    }

    size_t header_size = header_end + 4 - request;
    size_t expected_total = header_size + body_len;
    ssize_t len_body_left = expected_total - req_len;

    if (body_len > 0) {
        if (body_len >= 4096 || expected_total > 8191 - req_len) {
            RESPOND_WITH_ERR(CONTENT_TOO_LARGE);
        }

        if (len_body_left > 0) {
            auto remaining = len_body_left;
            while (remaining > 0) {
                auto len_received = recv(client, request + req_len, remaining, 0);
                if (len_received <= 0) { goto cleanup; }
                req_len += len_received;
                remaining -= len_received;
            }
            request[expected_total] = 0;
        }
    }

    char method[8];
    char path[64];
    int path_pos;

    auto res = sscanf(request,
                      "%7s /%n%63["
                      "A-Za-z0-9._~" // unreserved (plus hyphen)
                      ":/?@"         // gen-delims (part)
                      "!$&'()*+,;="  // sub-delims
                      "%%-] HTTP/1.1\r\n",
                      method, &path_pos, path);

    if (res < 1) {
        RESPOND_WITH_ERR(BAD_REQUEST);
    } else if (res == 1) {
        if (strncmp(request + path_pos, " HTTP/1.1\r\n", 11) == 0) {
            *path = 0;
        } else {
            RESPOND_WITH_ERR(BAD_REQUEST);
        }
    } else if (strchr(path, '/')) {
        RESPOND_WITH_ERR(NOT_FOUND);
    }

    if (strcmp(method, "GET") == 0 || strcmp(method, "HEAD") == 0) {
        auto delim = strchr(path, '?');
        if (delim) { *delim = 0; }
        auto path_len = strlen(path);
        if (path_len < MIN_LINK_LEN || path_len > MAX_LINK_LEN) { RESPOND_WITH_ERR(NOT_FOUND); }

        bool read = delim && query_arg(delim + 1, "read");

        pthread_rwlock_rdlock(&DB_LOCK);
        bool is_present;
        auto pos = db_bsearch(&GLOBAL_DB, path, &is_present);
        if (is_present) {
            auto target = GLOBAL_DB.pairs[pos].value;
            if (read) {
                if (*method == 'H') {
                    char msg[8192];
                    snprintf(msg, 8192, OK "Content-Length: %zu\r\n\r\n", strlen(target));
                    pthread_rwlock_unlock(&DB_LOCK);
                    send_all(client, msg, strlen(msg));
                } else {
                    char tmp[4096];
                    strncpy(tmp, target, 4095);
                    pthread_rwlock_unlock(&DB_LOCK);
                    RESPOND_WITH_BODY(OK, tmp);
                }
            } else {
                char msg[8192];
                auto code = USE_301 ? MOVED_PERMANENTLY : FOUND;
                snprintf(msg, 8192, "%sLocation: %s\r\nContent-Length: 0\r\n\r\n", code, target);
                pthread_rwlock_unlock(&DB_LOCK);
                send_all(client, msg, strlen(msg));
            }
        } else {
            pthread_rwlock_unlock(&DB_LOCK);
            RESPOND_WITH(NOT_FOUND);
        }
    } else if (strcmp(method, "POST") == 0) {
        if (body_len == 0) { RESPOND_WITH_ERR(BAD_REQUEST); }
        if (!auth(request)) { RESPOND_WITH_ERR(FORBIDDEN); }

        auto delim = strchr(path, '?');
        if (delim) { *delim = 0; }
        if (*path != 0) { RESPOND_WITH_ERR(METHOD_NOT_ALLOWED); }
        auto body = request + header_size;
        if (!validate_url(body)) { RESPOND_WITH_ERR(UNPROCESSABLE_CONTENT); }

        bool delete = delim && query_arg(delim + 1, "delete");

        if (delete) {
            pthread_rwlock_wrlock(&DB_LOCK);
            auto res = db_remove_all_values(&GLOBAL_DB, body);
            pthread_rwlock_unlock(&DB_LOCK);

            char num[8];
            snprintf(num, 8, "%zu", res);
            RESPOND_WITH_BODY(OK, num);
        } else {
            pthread_rwlock_wrlock(&DB_LOCK);
            char* link;
            for (auto len = DEFAULT_LINK_LEN; len <= MAX_LINK_LEN; len++) {
                if ((link = create_link(len, body))) { break; }
            }
            pthread_rwlock_unlock(&DB_LOCK);
            if (!link) { RESPOND_WITH_ERR(INTERNAL_SERVER_ERROR); }
            RESPOND_WITH_BODY(OK, link);
            free(link);
        }
    } else if (strcmp(method, "PUT") == 0) {
        if (body_len == 0) { RESPOND_WITH_ERR(BAD_REQUEST); }
        if (!auth(request)) { RESPOND_WITH_ERR(FORBIDDEN); }

        auto delim = strchr(path, '?');
        if (delim) { *delim = 0; }
        auto path_len = strlen(path);
        if (path_len < MIN_LINK_LEN || path_len > MAX_LINK_LEN) {
            RESPOND_WITH_ERR(NOT_ACCEPTABLE);
        }

        auto body = request + header_size;
        if (!validate_url(body)) { RESPOND_WITH_ERR(UNPROCESSABLE_CONTENT); }

        bool force = delim && query_arg(delim + 1, "force");

        pthread_rwlock_wrlock(&DB_LOCK);
        bool is_present;
        auto pos = db_bsearch(&GLOBAL_DB, path, &is_present);

        if (is_present) {
            auto target = GLOBAL_DB.pairs[pos].value;
            char existing[4096];
            strncpy(existing, target, 4095);
            if (force) {
                strncpy(target, body, 4095);
                target[body_len] = 0;
                GLOBAL_DB.is_modified = true;
                pthread_rwlock_unlock(&DB_LOCK);
                RESPOND_WITH_BODY(OK, existing);
            } else {
                pthread_rwlock_unlock(&DB_LOCK);
                RESPOND_WITH_BODY(CONFLICT, existing);
            }
        } else {
            db_insert_at(&GLOBAL_DB, pos, pair_new(path, body));
            pthread_rwlock_unlock(&DB_LOCK);
            RESPOND_WITH(NO_CONTENT);
        }
    } else if (strcmp(method, "DELETE") == 0) {
        if (!auth(request)) { RESPOND_WITH_ERR(FORBIDDEN); }

        auto delim = strchr(path, '?');
        if (delim) { *delim = 0; }
        auto path_len = strlen(path);
        if (path_len < MIN_LINK_LEN || path_len > MAX_LINK_LEN) { RESPOND_WITH_ERR(NO_CONTENT); }

        pthread_rwlock_wrlock(&DB_LOCK);
        auto target = db_remove_key(&GLOBAL_DB, path);
        pthread_rwlock_unlock(&DB_LOCK);

        if (target) {
            RESPOND_WITH_BODY(OK, target);
            free(target);
        } else {
            RESPOND_WITH(NO_CONTENT);
        }
    } else {
        RESPOND_WITH(METHOD_NOT_ALLOWED);
    }

cleanup:
    close(client);
    sem_post(&THREADS);
    return nullptr;
}

void* db_saver(void*) {
    while (KEEP_RUNNING) {
        sleep(SAVE_DB_INTERVAL);
        // no other threads read `is_modified`
        // nor are any writing to the database
        // so a read lock is safe
        pthread_rwlock_rdlock(&DB_LOCK);
        if (!GLOBAL_DB.is_modified) {
            pthread_rwlock_unlock(&DB_LOCK);
            continue;
        }
        auto current_db = db_clone(&GLOBAL_DB);
        GLOBAL_DB.is_modified = false;
        pthread_rwlock_unlock(&DB_LOCK);
        db_save(&current_db, DB_FILENAME);
        free(current_db.pairs);
    }
    return nullptr;
}

void handle_signal(int) { KEEP_RUNNING = false; }

int main(int argc, char** argv) {
    int port = argc >= 2 ? atoi(argv[1]) : DEFAULT_PORT;
    if (port == 0 || port > 65535) {
        fprintf(stderr, "port number must in range 1-65535\n");
        exit(EXIT_FAILURE);
    }

    AUTH_TOKEN = getenv("NLN_AUTH_TOKEN");
    if (!AUTH_TOKEN || *AUTH_TOKEN == 0) {
        fprintf(stderr, "auth token is not set\n");
        exit(EXIT_FAILURE);
    }

    GLOBAL_DB = db_load(DB_FILENAME);
    if (!GLOBAL_DB.pairs) {
        fprintf(stderr, "cannot load database file\n");
        exit(EXIT_FAILURE);
    }

    sigaction(SIGPIPE, &(struct sigaction){.sa_handler = SIG_IGN}, nullptr);
    sigaction(SIGINT, &(struct sigaction){.sa_handler = handle_signal}, nullptr);
    sigaction(SIGTERM, &(struct sigaction){.sa_handler = handle_signal}, nullptr);

    // begin serving

    auto server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port = htons(port),
    };

    int opt = 1;
    if (setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt failed");
        exit(EXIT_FAILURE);
    }

    if (bind(server, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind failed");
        exit(EXIT_FAILURE);
    }

    if (listen(server, 1024) < 0) {
        perror("listen failed");
        exit(EXIT_FAILURE);
    }

    pthread_t db_saver_thread;
    pthread_create(&db_saver_thread, nullptr, db_saver, nullptr);
    pthread_detach(db_saver_thread);

    sem_init(&THREADS, 0, MAX_ACTIVE_THREADS);

    while (KEEP_RUNNING) {
        sem_wait(&THREADS);

        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        auto client = accept(server, (struct sockaddr*)&client_addr, &addr_len);
        if (client < 0) {
            sem_post(&THREADS);
            if (!KEEP_RUNNING) { break; }
            perror("accept failed");
            continue;
        }

        struct timeval tv = {.tv_sec = TRANSFER_TIMEOUT};
        auto res = setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        if (res != 0) {
            close(client);
            sem_post(&THREADS);
            perror("setsockopt failed");
            continue;
        }
        res = setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        if (res != 0) {
            close(client);
            sem_post(&THREADS);
            perror("setsockopt failed");
            continue;
        }

        pthread_t thread;
        pthread_create(&thread, nullptr, handle_request, (void*)(uintptr_t)client);
        pthread_detach(thread);
    }

    for (size_t i = 0; i < MAX_ACTIVE_THREADS; i++) { sem_wait(&THREADS); }

    db_save(&GLOBAL_DB, DB_FILENAME);
}
