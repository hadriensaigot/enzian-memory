#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <hiredis/hiredis.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>

#define MAX_COLS 32

typedef struct {
    char name[64];
    int is_int;
    double mean;
} Column;

// ============================================================
// Enzian FPGA memory mapping + simple arena allocator (C version)
// Same pattern: open(devPath, O_RDWR|O_SYNC) + mmap(..., MAP_SHARED, fd, 0)
// ============================================================

static uint8_t* gArenaBase = NULL;
static uint8_t* gArenaCur  = NULL;
static uint8_t* gArenaEnd  = NULL;

static void* fpga_alloc(size_t n) {
    if (!gArenaBase) {
        // Fallback to normal heap if driver mapping isn't enabled/available
        void* p = malloc(n);
        if (!p) {
            fprintf(stderr, "ERROR: malloc failed (%zu bytes)\n", n);
            exit(1);
        }
        return p;
    }

    const size_t ALIGN = 64;
    uintptr_t cur = (uintptr_t)gArenaCur;
    uintptr_t aligned = (cur + (ALIGN - 1)) & ~(uintptr_t)(ALIGN - 1);

    if (aligned + n > (uintptr_t)gArenaEnd) {
        fprintf(stderr, "ERROR: FPGA arena out of space (requested %zu bytes)\n", n);
        exit(1);
    }

    gArenaCur = (uint8_t*)(aligned + n);
    return (void*)aligned;
}

static void SetupEnzianMemoryMapping(const char* devPath, size_t mapBytes) {
    int fd = open(devPath, O_RDWR | O_SYNC);
    if (fd < 0) {
        perror("open(enzian device) failed");
        fprintf(stderr, "Continuing WITHOUT driver mapping (using normal heap).\n");
        return;
    }

    void* addr = mmap(NULL, mapBytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);

    if (addr == MAP_FAILED) {
        perror("mmap(enzian device) failed");
        fprintf(stderr, "Continuing WITHOUT driver mapping (using normal heap).\n");
        return;
    }

    gArenaBase = (uint8_t*)addr;
    gArenaCur  = gArenaBase;
    gArenaEnd  = gArenaBase + mapBytes;

    // Optional: touch pages to reduce first-touch jitter
    const size_t page = 4096;
    for (size_t i = 0; i < mapBytes; i += (page * 1024)) {
        gArenaBase[i] = 0;
    }

    printf("Enzian FPGA memory mapped: %s (%zu bytes) at %p\n", devPath, mapBytes, addr);
}

// ============================================================
// Existing logic
// ============================================================

int gaussian_key(int rows) {
    static int seeded = 0;
    if (!seeded) {
        srand(42);
        seeded = 1;
    }

    double u1 = (rand() + 1.0) / (RAND_MAX + 1.0);
    double u2 = (rand() + 1.0) / (RAND_MAX + 1.0);
    double z  = sqrt(-2.0 * log(u1)) * cos(2 * M_PI * u2);

    int key = (int)(rows / 2.0 + (rows / 6.0) * z);
    if (key < 1) key = 1;
    if (key > rows) key = rows;
    return key;
}

long long now_us() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000000 + tv.tv_usec;
}

// ------------------------------------------------------------
// Read entire file into a buffer (FPGA-mapped if available)
// and parse CSV from that buffer.
// ------------------------------------------------------------
static char* read_file_into_buffer(const char* path, size_t* out_size) {
    struct stat st;
    if (stat(path, &st) != 0) {
        perror("stat(dataset) failed");
        return NULL;
    }

    size_t sz = (size_t)st.st_size;
    FILE* fp = fopen(path, "rb");
    if (!fp) {
        perror("Dataset open failed");
        return NULL;
    }

    // +1 so we can NUL-terminate for in-place tokenization
    char* buf = (char*)fpga_alloc(sz + 1);
    size_t got = fread(buf, 1, sz, fp);
    fclose(fp);

    if (got != sz) {
        fprintf(stderr, "ERROR: fread got %zu bytes, expected %zu\n", got, sz);
        return NULL;
    }
    buf[sz] = '\0';
    if (out_size) *out_size = sz;
    return buf;
}

// Helper: get next line from an in-memory buffer (in-place, returns pointer to line)
static char* next_line(char** cursor) {
    if (!cursor || !*cursor || **cursor == '\0') return NULL;
    char* start = *cursor;
    char* nl = strchr(start, '\n');
    if (nl) {
        *nl = '\0';
        *cursor = nl + 1;
        // Trim possible CR
        size_t len = strlen(start);
        if (len && start[len - 1] == '\r') start[len - 1] = '\0';
        return start;
    } else {
        // last line
        *cursor = start + strlen(start);
        return start;
    }
}

int load_dataset_generic(
    redisContext *c,
    const char *path,
    Column *schema,
    int *ns
) {
    size_t fsz = 0;
    char* filebuf = read_file_into_buffer(path, &fsz);
    if (!filebuf) return 0;

    char* cursor = filebuf;
    char* line = NULL;

    char *tokens[MAX_COLS];
    int cols = 0;

    /* ---------- HEADER ---------- */
    line = next_line(&cursor);
    if (!line) {
        fprintf(stderr, "ERROR: Empty CSV\n");
        return 0;
    }

    char* saveptr = NULL;
    char* tok = strtok_r(line, ",", &saveptr);
    while (tok && cols < MAX_COLS) {
        strncpy(schema[cols].name, tok, 63);
        schema[cols].name[63] = '\0';
        schema[cols].is_int = 1;   // assume int first
        cols++;
        tok = strtok_r(NULL, ",", &saveptr);
    }
    *ns = cols;

    /* ---------- FIRST ROW (TYPE INFERENCE) ---------- */
    line = next_line(&cursor);
    if (!line) {
        fprintf(stderr, "ERROR: CSV has header but no data rows\n");
        return 0;
    }

    saveptr = NULL;
    tok = strtok_r(line, ",", &saveptr);
    for (int i = 0; i < cols && tok; i++) {
        tokens[i] = tok;
        for (char *p = tok; *p; p++) {
            if ((*p < '0' || *p > '9') && *p != '-') {
                schema[i].is_int = 0;
                break;
            }
        }
        tok = strtok_r(NULL, ",", &saveptr);
    }

    /* ---------- STORE SCHEMA IN REDIS ---------- */
    redisReply* rr = redisCommand(c, "DEL schema");
    if (rr) freeReplyObject(rr);

    for (int i = 0; i < cols; i++) {
        rr = redisCommand(
            c,
            "HSET schema %s %s",
            schema[i].name,
            schema[i].is_int ? "int" : "string"
        );
        if (rr) freeReplyObject(rr);
    }

    /* ---------- STORE FIRST ROW ---------- */
    int row = 1;
    char key[64];
    snprintf(key, sizeof(key), "row:%d", row);

    for (int i = 0; i < cols; i++) {
        rr = redisCommand(c, "HSET %s %s %s", key, schema[i].name, tokens[i]);
        if (rr) freeReplyObject(rr);
    }

    /* ---------- REMAINING ROWS ---------- */
    while ((line = next_line(&cursor)) != NULL) {
        // skip empty lines
        if (*line == '\0') continue;

        row++;
        snprintf(key, sizeof(key), "row:%d", row);

        saveptr = NULL;
        tok = strtok_r(line, ",", &saveptr);
        for (int i = 0; i < cols && tok; i++) {
            rr = redisCommand(c, "HSET %s %s %s", key, schema[i].name, tok);
            if (rr) freeReplyObject(rr);
            tok = strtok_r(NULL, ",", &saveptr);
        }
    }

    printf("Loaded %d rows, %d columns\n", row, cols);
    return row;
}

long long query_select_gaussian(redisContext *c, int rows) {
    long long t1 = now_us();

    for (int i = 0; i < rows; i++) {
        int id = gaussian_key(rows);
        char key[64];
        snprintf(key, sizeof(key), "row:%d", id);

        redisReply *r = redisCommand(c, "HGETALL %s", key);
        if (r) freeReplyObject(r);
    }

    return now_us() - t1;
}

long long query_select_all(redisContext *c, int rows) {
    long long t1 = now_us();

    for (int i = 1; i <= rows; i++) {
        char key[64];
        snprintf(key, sizeof(key), "row:%d", i);

        redisReply *r = redisCommand(c, "HGETALL %s", key);
        if (r) freeReplyObject(r);
    }

    return now_us() - t1;
}

long long query_projection_generic(
    redisContext *c,
    int rows,
    Column *schema,
    int ns
) {
    long long t1 = now_us();
    long long total_sum = 0;
    int count = 0;

    for (int i = 1; i <= rows; i++) {
        char key[64];
        snprintf(key, sizeof(key), "row:%d", i);

        redisReply *r = redisCommand(c, "HGETALL %s", key);
        if (!r || r->type != REDIS_REPLY_ARRAY) {
            if (r) freeReplyObject(r);
            continue;
        }

        long long row_sum = 0;

        for (int s = 0; s < ns; s++) {
            if (!schema[s].is_int) continue;

            for (size_t j = 0; j + 1 < r->elements; j += 2) {
                if (r->element[j] && r->element[j]->str &&
                    strcmp(schema[s].name, r->element[j]->str) == 0 &&
                    r->element[j+1] && r->element[j+1]->str) {
                    row_sum += atoll(r->element[j+1]->str);
                }
            }
        }

        total_sum += row_sum;
        count++;

        freeReplyObject(r);
    }

    double avg = count ? (double)total_sum / count : 0.0;
    printf("AVG(sum(int_columns)) = %.2f\n", avg);

    return now_us() - t1;
}

long long query_aggregation_generic(
    redisContext *c,
    int rows,
    Column *schema,
    int ns
) {
    long long t1 = now_us();
    long long global_sum = 0;

    for (int i = 1; i <= rows; i++) {
        char key[64];
        snprintf(key, sizeof(key), "row:%d", i);

        redisReply *r = redisCommand(c, "HGETALL %s", key);
        if (!r || r->type != REDIS_REPLY_ARRAY) {
            if (r) freeReplyObject(r);
            continue;
        }

        for (int s = 0; s < ns; s++) {
            if (!schema[s].is_int) continue;

            for (size_t j = 0; j + 1 < r->elements; j += 2) {
                if (r->element[j] && r->element[j]->str &&
                    strcmp(schema[s].name, r->element[j]->str) == 0 &&
                    r->element[j+1] && r->element[j+1]->str) {
                    global_sum += atoll(r->element[j+1]->str);
                }
            }
        }

        freeReplyObject(r);
    }

    printf("SUM(int_columns) = %lld\n", global_sum);
    return now_us() - t1;
}

/* ---------------- PERSISTENCE ---------------- */
void set_persistence(redisContext *c, bool persistent) {
    redisReply *reply;

    if (!persistent) {
        printf("[MODE] Non-persistent (RAM only)\n");
        reply = redisCommand(c, "CONFIG SET save \"\"");
        if (reply) freeReplyObject(reply);
        reply = redisCommand(c, "CONFIG SET appendonly no");
        if (reply) freeReplyObject(reply);

    } else {
        printf("[MODE] Persistent mode (AOF enabled)\n");
        reply = redisCommand(c, "CONFIG SET appendonly yes");
        if (reply) freeReplyObject(reply);
        reply = redisCommand(c, "CONFIG SET appendfsync everysec");
        if (reply) freeReplyObject(reply);
        reply = redisCommand(c, "CONFIG SET save \"60 1\"");
        if (reply) freeReplyObject(reply);
    }
}

void print_memory(redisContext *c) {
    redisReply *reply = redisCommand(c, "INFO memory");
    if (!reply || !reply->str) {
        if (reply) freeReplyObject(reply);
        return;
    }

    char *dup = strdup(reply->str);
    freeReplyObject(reply);
    if (!dup) return;

    char *line = strtok(dup, "\n");
    while (line != NULL) {
        if (strncmp(line, "used_memory:", 12) == 0)
            printf("Raw Bytes Used: %s\n", line + 12);
        if (strncmp(line, "used_memory_human:", 18) == 0)
            printf("Human Readable: %s\n", line + 18);
        line = strtok(NULL, "\n");
    }
    free(dup);
}

int main(int argc, char **argv) {
    if (argc < 4) {
        printf("Usage:\n");
        printf("  %s 0|1 full|load dataset.csv [enzian_dev=/dev/enzian_memory] [map_gb=4]\n", argv[0]);
        return 1;
    }

    bool persistent = atoi(argv[1]) == 1;
    bool load_only  = strcmp(argv[2], "load") == 0;
    const char *dataset_path = argv[3];

    const char *dev = (argc >= 5) ? argv[4] : "/dev/enzian_memory";
    int mapGb       = (argc >= 6) ? atoi(argv[5]) : 4;

    if (mapGb > 0) {
        size_t mapBytes = (size_t)mapGb * 1024ULL * 1024ULL * 1024ULL;
        SetupEnzianMemoryMapping(dev, mapBytes);
    } else {
        printf("map_gb <= 0, skipping Enzian mapping (using normal heap).\n");
    }

    Column schema[MAX_COLS];
    int ns = 0;

    redisContext *c = redisConnect("127.0.0.1", 6369);
    if (!c || c->err) {
        printf("Redis connection error\n");
        if (c) redisFree(c);
        return 1;
    }

    printf("Connected to Redis\n");
    set_persistence(c, persistent);

    /* ---------------- LOAD DATASET ---------------- */
    int row_count = load_dataset_generic(c, dataset_path, schema, &ns);
    printf("\n--- DATASET SIZE: %d rows ---\n", row_count);

    if (load_only) {
        printf("[MODE] Load-only selected\n");
        redisFree(c);
        return 0;
    }

    /* ---------------- RUN QUERIES ---------------- */
    long long t_select_gauss = query_select_gaussian(c, row_count);
    long long t_select_all   = query_select_all(c, row_count);
    long long t_projection   = query_projection_generic(c, row_count, schema, ns);
    long long t_aggregation  = query_aggregation_generic(c, row_count, schema, ns);

    /* ---------------- LATENCY SUMMARY ---------------- */
    double avg_select_gauss = (double)t_select_gauss / row_count;
    double avg_select_all   = (double)t_select_all   / row_count;
    double avg_projection   = (double)t_projection   / row_count;
    double avg_aggregation  = (double)t_aggregation  / row_count;

    double base = (double)t_select_gauss;

    printf("\n=== LATENCY SUMMARY (microseconds) ===\n");
    printf("Workload        | Total Time           | Avg Latency        | Relative Cost\n");
    printf("--------------------------------------------------------------------------\n");

    printf("SELECT (Gauss)  | total = %10lld us | avg = %8.2f us/op | slowdown = %6.2fx\n",
           t_select_gauss, avg_select_gauss, 1.00);

    printf("SELECTION *     | total = %10lld us | avg = %8.2f us/op | slowdown = %6.2fx\n",
           t_select_all, avg_select_all, (double)t_select_all / base);

    printf("PROJECTION      | total = %10lld us | avg = %8.2f us/op | slowdown = %6.2fx\n",
           t_projection, avg_projection, (double)t_projection / base);

    printf("AGGREGATION     | total = %10lld us | avg = %8.2f us/op | slowdown = %6.2fx\n",
           t_aggregation, avg_aggregation, (double)t_aggregation / base);

    /* ---------------- MEMORY USAGE ---------------- */
    printf("\n--- MEMORY USAGE ---\n");
    print_memory(c);

    redisFree(c);
    return 0;
}
