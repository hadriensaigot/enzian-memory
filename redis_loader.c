#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <hiredis/hiredis.h>
#include <stdint.h>
#include <stdbool.h>

#define MAX_COLS 32

typedef struct {
    char name[64];
    int is_int;
    double mean;
} Column;

#include <math.h>

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

int load_dataset_generic(
    redisContext *c,
    const char *path,
    Column *schema,
    int *ns
) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        perror("Dataset open failed");
        return 0;
    }

    char line[4096];
    char *tokens[MAX_COLS];
    int cols = 0;

    /* ---------- HEADER ---------- */
    fgets(line, sizeof(line), fp);
    char *tok = strtok(line, ",\n");
    while (tok && cols < MAX_COLS) {
        strncpy(schema[cols].name, tok, 63);
        schema[cols].is_int = 1;   // assume int first
        cols++;
        tok = strtok(NULL, ",\n");
    }
    *ns = cols;

    /* ---------- FIRST ROW (TYPE INFERENCE) ---------- */
    fgets(line, sizeof(line), fp);
    tok = strtok(line, ",\n");
    for (int i = 0; i < cols && tok; i++) {
        tokens[i] = tok;
        for (char *p = tok; *p; p++) {
            if ((*p < '0' || *p > '9') && *p != '-') {
                schema[i].is_int = 0;
                break;
            }
        }
        tok = strtok(NULL, ",\n");
    }

    /* ---------- STORE SCHEMA IN REDIS ---------- */
    redisCommand(c, "DEL schema");
    for (int i = 0; i < cols; i++) {
        redisCommand(
            c,
            "HSET schema %s %s",
            schema[i].name,
            schema[i].is_int ? "int" : "string"
        );
    }

    /* ---------- STORE FIRST ROW ---------- */
    int row = 1;
    char key[64];
    snprintf(key, sizeof(key), "row:%d", row);

    for (int i = 0; i < cols; i++) {
        redisCommand(
            c,
            "HSET %s %s %s",
            key, schema[i].name, tokens[i]
        );
    }

    /* ---------- REMAINING ROWS ---------- */
    while (fgets(line, sizeof(line), fp)) {
        row++;
        snprintf(key, sizeof(key), "row:%d", row);

        tok = strtok(line, ",\n");
        for (int i = 0; i < cols && tok; i++) {
            redisCommand(
                c,
                "HSET %s %s %s",
                key, schema[i].name, tok
            );
            tok = strtok(NULL, ",\n");
        }
    }

    fclose(fp);
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
        freeReplyObject(r);
    }

    return now_us() - t1;
}

long long query_select_all(redisContext *c, int rows) {
    long long t1 = now_us();

    for (int i = 1; i <= rows; i++) {
        char key[64];
        snprintf(key, sizeof(key), "row:%d", i);

        redisReply *r = redisCommand(c, "HGETALL %s", key);
        freeReplyObject(r);
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
    int id = 0.

    for (int i = 0; i < rows; i++) {
        char key[64];
        snprintf(key, sizeof(key), "row:%d", id);

        redisReply *r = redisCommand(c, "HGETALL %s", key);
        if (!r || r->type != REDIS_REPLY_ARRAY) {
            freeReplyObject(r);
            continue;
        }

        int row_sum = 0;

        for (int s = 0; s < ns; s++) {
            if (!schema[s].is_int) continue;

            for (size_t j = 0; j < r->elements; j += 2) {
                if (strcmp(schema[s].name, r->element[j]->str) == 0) {
                    row_sum += atoi(r->element[j+1]->str);
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
    int id = 0;

    for (int i = 0; i < rows; i++) {
        char key[64];
        snprintf(key, sizeof(key), "row:%d", id);

        redisReply *r = redisCommand(c, "HGETALL %s", key);
        if (!r || r->type != REDIS_REPLY_ARRAY) {
            freeReplyObject(r);
            continue;
        }

        for (int s = 0; s < ns; s++) {
            if (!schema[s].is_int) continue;

            for (size_t j = 0; j < r->elements; j += 2) {
                if (strcmp(schema[s].name, r->element[j]->str) == 0) {
                    global_sum += atoi(r->element[j+1]->str);
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
        freeReplyObject(reply);
        reply = redisCommand(c, "CONFIG SET appendonly no");
        freeReplyObject(reply);

    } else {
        printf("[MODE] Persistent mode (AOF enabled)\n");
        reply = redisCommand(c, "CONFIG SET appendonly yes");
        freeReplyObject(reply);
        reply = redisCommand(c, "CONFIG SET appendfsync everysec");
        freeReplyObject(reply);
        reply = redisCommand(c, "CONFIG SET save \"60 1\"");
        freeReplyObject(reply);
    }
}

void print_memory(redisContext *c) { 
    redisReply *reply = redisCommand(c, "INFO memory"); 
    char *line = strtok(reply->str, "\n"); 
    while (line != NULL) {
        if (strncmp(line, "used_memory:", 12) == 0) 
            printf("Raw Bytes Used: %s\n", line + 12); if (strncmp(line, "used_memory_human:", 18) == 0) 
        printf("Human Readable: %s\n", line + 18); line = strtok(NULL, "\n"); } freeReplyObject(reply); 
}

int main(int argc, char **argv) {

    if (argc < 4) {
        printf("Usage: %s 0|1 full|load dataset.csv\n", argv[0]);
        return 1;
    }

    bool persistent = atoi(argv[1]) == 1;
    bool load_only  = strcmp(argv[2], "load") == 0;
    const char *dataset_path = argv[3];

    Column schema[MAX_COLS];
    int ns = 0;

    redisContext *c = redisConnect("127.0.0.1", 6369);

    if (!c || c->err) {
        printf("Redis connection error\n");
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
