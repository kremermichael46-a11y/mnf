// src/mnf.c
// Project: move-nested-files (mnf)
// Version: 1.0.0
//
// Extended, recursive tool to move all files from nested subdirectories of a
// source directory into a single destination directory.
//
// Build:
//   gcc -O2 -pthread -Wall -Wextra -o mnf src/mnf.c
//
// See the man page (man/mnf.1) or run: mnf --help

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/sendfile.h>
#include <sys/time.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <getopt.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#ifndef MNF_VERSION
#define MNF_VERSION "1.0.0"
#endif

// ------------------------------ Logging ------------------------------
static pthread_mutex_t log_mx = PTHREAD_MUTEX_INITIALIZER;
static int g_verbose = 1; // 0=quiet, 1=info, 2=debug

static void die(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    pthread_mutex_lock(&log_mx);
    vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    pthread_mutex_unlock(&log_mx);
    va_end(ap);
    exit(EXIT_FAILURE);
}

static void vlog_msg(int level, const char *fmt, va_list ap) {
    if (level > g_verbose) return;
    pthread_mutex_lock(&log_mx);
    vfprintf(stdout, fmt, ap); fputc('\n', stdout);
    fflush(stdout);
    pthread_mutex_unlock(&log_mx);
}
static void log_msg(int level, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vlog_msg(level, fmt, ap);
    va_end(ap);
}

// ------------------------------ Small utils ------------------------------
static void path_join(char *dst, size_t dstsz, const char *a, const char *b) {
    size_t alen = strlen(a);
    size_t blen = strlen(b);
    size_t needed = alen + 1 + blen + 1;
    if (needed > dstsz) die("Path too long: '%s' + '%s'", a, b);
    memcpy(dst, a, alen);
    dst[alen] = '/';
    memcpy(dst + alen + 1, b, blen);
    dst[alen + 1 + blen] = '\0';
}
static void str_copy_checked(char *dst, size_t dstsz, const char *src) {
    size_t slen = strlen(src) + 1;
    if (slen > dstsz) die("Path too long: '%s'", src);
    memcpy(dst, src, slen);
}
static const char *basename_const(const char *path) {
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}
static char *xstrdup(const char *s) {
    size_t n = strlen(s);
    char *p = (char *)malloc(n + 1);
    if (!p) die("Out of memory");
    memcpy(p, s, n + 1);
    return p;
}
static char **split_csv(const char *csv, size_t *out_count) {
    if (!csv || !*csv) { *out_count = 0; return NULL; }
    char *tmp = xstrdup(csv);
    size_t cap = 8, cnt = 0; char **arr = (char **)malloc(cap * sizeof(char *));
    if (!arr) die("Out of memory");
    char *saveptr = NULL; char *tok = strtok_r(tmp, ",", &saveptr);
    while (tok) {
        while (*tok == ' ' || *tok == '\t') tok++;
        size_t len = strlen(tok);
        while (len && (tok[len-1]==' ' || tok[len-1]=='\t' || tok[len-1]=='\n')) tok[--len] = '\0';
        if (len) {
            if (cnt == cap) { cap *= 2; arr = (char **)realloc(arr, cap * sizeof(char *)); if (!arr) die("OOM"); }
            arr[cnt++] = xstrdup(tok);
        }
        tok = strtok_r(NULL, ",", &saveptr);
    }
    free(tmp);
    *out_count = cnt; return arr;
}
static void free_strv(char **v, size_t n) { if (!v) return; for (size_t i=0;i<n;i++) free(v[i]); free(v); }

static bool parse_size(const char *s, off_t *out) {
    if (!s || !*s) return false;
    char *end = NULL; errno = 0;
    double val = strtod(s, &end);
    if (errno != 0) return false;
    double mult = 1.0;
    if (*end) {
        if (*end=='K'||*end=='k') mult = 1024.0;
        else if (*end=='M'||*end=='m') mult = 1024.0*1024.0;
        else if (*end=='G'||*end=='g') mult = 1024.0*1024.0*1024.0;
        else if (*end=='T'||*end=='t') mult = 1024.0*1024.0*1024.0*1024.0;
        else return false;
        end++;
        if (*end) return false;
    }
    if (val < 0) val = 0;
    *out = (off_t)(val * mult);
    return true;
}
static bool parse_time_spec(const char *s, time_t *out) {
    if (!s || !*s) return false;
    struct tm tmv; memset(&tmv, 0, sizeof(tmv));
    if (strlen(s) == 10 && s[4]=='-' && s[7]=='-') {
        if (sscanf(s, "%4d-%2d-%2d", &tmv.tm_year, &tmv.tm_mon, &tmv.tm_mday) == 3) {
            tmv.tm_year -= 1900; tmv.tm_mon -= 1; tmv.tm_isdst = -1;
            time_t t = mktime(&tmv);
            if (t != (time_t)-1) { *out = t; return true; }
        }
    }
    char *end = NULL; errno = 0;
    long long num = strtoll(s, &end, 10);
    if (errno != 0 || end==s) return false;
    long long seconds = 0;
    if (*end=='d'||*end=='D') seconds = num * 24LL * 3600LL;
    else if (*end=='h'||*end=='H') seconds = num * 3600LL;
    else if (*end=='m'||*end=='M') seconds = num * 60LL;
    else if (*end=='s'||*end=='S'||*end=='\0') seconds = num;
    else return false;
    *out = time(NULL) - (time_t)seconds;
    return true;
}

// ------------------------------ Options ------------------------------
typedef enum { MODE_RENAME=0, MODE_SKIP=1, MODE_OVERWRITE=2 } mode_tg;

typedef struct {
    char *src; char *dst;
    int threads;
    mode_tg mode;
    int min_depth;
    int max_depth;
    bool dry_run;
    bool progress;
    bool preserve_times;
    bool include_symlinks;
    bool prune_empty_dirs;

    off_t min_size; bool has_min_size;
    off_t max_size; bool has_max_size;
    time_t newer_than; bool has_newer;
    time_t older_than; bool has_older;

    char **includes; size_t n_includes;
    char **excludes; size_t n_excludes;
    char **allow_ext; size_t n_allow_ext;
    char **deny_ext;  size_t n_deny_ext;
} options_t;

static void print_usage_short(const char *prog) {
    fprintf(stderr, "Usage: %s SOURCE_DIR DEST_DIR [options]\n", prog);
    fprintf(stderr, "Try '%s --help' for a full description.\n", prog);
}

static void print_help(const char *prog) {
    printf(
"move-nested-files (mnf) %s\n"
"\n"
"Usage:\n"
"  %s SOURCE_DIR DEST_DIR [options]\n"
"\n"
"Description:\n"
"  Recursively move files from nested subdirectories under SOURCE_DIR into DEST_DIR.\n"
"  Files located directly in SOURCE_DIR are left in place by default (min-depth=1).\n"
"\n"
"Core options:\n"
"  --mode=rename|skip|overwrite   Collision handling (default: rename)\n"
"  -n, --dry-run                  Show actions without changing anything\n"
"  -t, --threads N                Number of worker threads (default: 1)\n"
"  -v, --verbose                  More output (repeat for debug)\n"
"  -q, --quiet                    Less output\n"
"      --progress                 Show per-file copy progress\n"
"      --no-preserve-times        Do not preserve atime/mtime when copying\n"
"      --include-symlinks         Move symlink files too (recreate links in DEST)\n"
"      --prune-empty-dirs         Remove empty directories in SOURCE afterwards\n"
"\n"
"Depth control:\n"
"      --min-depth N              Minimum depth to move (default: 1)\n"
"      --max-depth N              Maximum depth (default: unlimited)\n"
"\n"
"Filters:\n"
"      --include GLOBS            Comma list, e.g. '**/*.jpg,**/*.png'\n"
"      --exclude GLOBS            Comma list, e.g. '**/tmp/**,**/.cache/**'\n"
"      --allow-ext LIST           Whitelist extensions: 'jpg,png,gif'\n"
"      --deny-ext LIST            Blacklist extensions: 'tmp,part,~'\n"
"      --min-size SIZE            10K, 5M, 1G (base 1024)\n"
"      --max-size SIZE            Limit by size\n"
"      --newer-than SPEC          ISO date (YYYY-MM-DD) or relative (e.g. 7d)\n"
"      --older-than SPEC          ISO date or relative (e.g. 30d)\n"
"\n"
"Other:\n"
"  -h, --help                     Show this help and exit\n"
"  -V, --version                  Show version and exit\n"
"\n"
"Examples:\n"
"  %s ./src ./flat\n"
"  %s ./src ./flat --threads 4 --include \"**/*.jpg,**/*.png\" --min-size 1M --progress\n"
"  %s ./src ./flat --dry-run --exclude \"**/tmp/**\"\n"
"\n", MNF_VERSION, prog, prog, prog, prog);
}

static void print_version(void) {
    printf("mnf %s\n", MNF_VERSION);
}

static void add_patterns(char ***arr, size_t *cnt, const char *csv) {
    size_t n=0; char **v = split_csv(csv, &n);
    if (!v) return;
    size_t old = *cnt; *cnt += n;
    *arr = (char **)realloc(*arr, (*cnt) * sizeof(char *));
    if (!*arr) die("OOM");
    for (size_t i=0;i<n;i++) (*arr)[old+i] = v[i];
    free(v);
}
static void add_exts(char ***arr, size_t *cnt, const char *csv) { add_patterns(arr, cnt, csv); }

static void parse_options(int argc, char **argv, options_t *o) {
    memset(o, 0, sizeof(*o));
    o->threads = 1;
    o->mode = MODE_RENAME;
    o->min_depth = 1; o->max_depth = -1;
    o->preserve_times = true;

    static struct option longopts[] = {
        {"mode", required_argument, 0, 1000},
        {"dry-run", no_argument, 0, 'n'},
        {"threads", required_argument, 0, 't'},
        {"verbose", no_argument, 0, 'v'},
        {"quiet", no_argument, 0, 'q'},
        {"progress", no_argument, 0, 1001},
        {"no-preserve-times", no_argument, 0, 1002},
        {"include-symlinks", no_argument, 0, 1003},
        {"prune-empty-dirs", no_argument, 0, 1004},
        {"min-depth", required_argument, 0, 1005},
        {"max-depth", required_argument, 0, 1006},
        {"include", required_argument, 0, 1007},
        {"exclude", required_argument, 0, 1008},
        {"allow-ext", required_argument, 0, 1009},
        {"deny-ext", required_argument, 0, 1010},
        {"min-size", required_argument, 0, 1011},
        {"max-size", required_argument, 0, 1012},
        {"newer-than", required_argument, 0, 1013},
        {"older-than", required_argument, 0, 1014},
        {"help", no_argument, 0, 'h'},
        {"version", no_argument, 0, 'V'},
        {0,0,0,0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "hqnvt:V", longopts, NULL)) != -1) {
        switch (c) {
            case 'h': print_help(argv[0]); exit(0);
            case 'V': print_version(); exit(0);
            case 'q': g_verbose = 0; break;
            case 'v': g_verbose++; break;
            case 'n': o->dry_run = true; break;
            case 't': o->threads = atoi(optarg); if (o->threads < 1) o->threads = 1; break;
            case 1000:
                if (strcmp(optarg, "rename") == 0) o->mode = MODE_RENAME;
                else if (strcmp(optarg, "skip") == 0) o->mode = MODE_SKIP;
                else if (strcmp(optarg, "overwrite") == 0) o->mode = MODE_OVERWRITE;
                else die("Invalid --mode: %s", optarg);
                break;
            case 1001: o->progress = true; break;
            case 1002: o->preserve_times = false; break;
            case 1003: o->include_symlinks = true; break;
            case 1004: o->prune_empty_dirs = true; break;
            case 1005: o->min_depth = atoi(optarg); if (o->min_depth < 0) o->min_depth = 0; break;
            case 1006: o->max_depth = atoi(optarg); break;
            case 1007: add_patterns(&o->includes, &o->n_includes, optarg); break;
            case 1008: add_patterns(&o->excludes, &o->n_excludes, optarg); break;
            case 1009: add_exts(&o->allow_ext, &o->n_allow_ext, optarg); break;
            case 1010: add_exts(&o->deny_ext, &o->n_deny_ext, optarg); break;
            case 1011: o->has_min_size = parse_size(optarg, &o->min_size); if (!o->has_min_size) die("Invalid --min-size: %s", optarg); break;
            case 1012: o->has_max_size = parse_size(optarg, &o->max_size); if (!o->has_max_size) die("Invalid --max-size: %s", optarg); break;
            case 1013: o->has_newer = parse_time_spec(optarg, &o->newer_than); if (!o->has_newer) die("Invalid --newer-than: %s", optarg); break;
            case 1014: o->has_older = parse_time_spec(optarg, &o->older_than); if (!o->has_older) die("Invalid --older-than: %s", optarg); break;
            default: print_usage_short(argv[0]); exit(2);
        }
    }

    if (optind + 2 != argc) { print_usage_short(argv[0]); exit(2); }
    o->src = argv[optind];
    o->dst = argv[optind+1];
}

// ------------------------------ Filters ------------------------------
static const char *ext_of(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot || dot==name) return NULL;
    return dot+1;
}
static bool list_contains_ci(char **list, size_t n, const char *needle) {
    if (!needle) return false;
    for (size_t i=0;i<n;i++) {
        const char *a = list[i];
        size_t la=strlen(a), ln=strlen(needle);
        if (la!=ln) continue;
        bool same=true;
        for (size_t j=0;j<la;j++) {
            char ca=a[j], cb=needle[j];
            if (ca>='A'&&ca<='Z') ca = (char)(ca-'A'+'a');
            if (cb>='A'&&cb<='Z') cb = (char)(cb-'A'+'a');
            if (ca!=cb) { same=false; break; }
        }
        if (same) return true;
    }
    return false;
}
static bool match_any_glob(const char *rel, char **globs, size_t n) {
    if (!globs || n==0) return true;
    for (size_t i=0;i<n;i++) {
        if (fnmatch(globs[i], rel, FNM_PATHNAME | FNM_PERIOD | FNM_CASEFOLD) == 0)
            return true;
    }
    return false;
}
static bool match_any_exclude(const char *rel, char **globs, size_t n) {
    if (!globs || n==0) return false;
    for (size_t i=0;i<n;i++) {
        if (fnmatch(globs[i], rel, FNM_PATHNAME | FNM_PERIOD | FNM_CASEFOLD) == 0)
            return true;
    }
    return false;
}
static bool file_passes_filters(const options_t *o, const char *rel, const struct stat *st, const char *name) {
    if (o->n_includes > 0 && !match_any_glob(rel, o->includes, o->n_includes)) return false;
    if (match_any_exclude(rel, o->excludes, o->n_excludes)) return false;
    const char *ext = ext_of(name);
    if (o->n_allow_ext > 0) {
        if (!ext || !list_contains_ci(o->allow_ext, o->n_allow_ext, ext)) return false;
    }
    if (ext && o->n_deny_ext > 0 && list_contains_ci(o->deny_ext, o->n_deny_ext, ext)) return false;
    if (o->has_min_size && st->st_size < o->min_size) return false;
    if (o->has_max_size && st->st_size > o->max_size) return false;
    if (o->has_newer && st->st_mtime < o->newer_than) return false;
    if (o->has_older && st->st_mtime > o->older_than) return false;
    return true;
}

// ------------------------------ Unique naming ------------------------------
static pthread_mutex_t name_mx = PTHREAD_MUTEX_INITIALIZER;
static void split_name(const char *name, char *base, size_t bsz, char *ext, size_t extsz) {
    const char *dot = strrchr(name, '.');
    if (name[0]=='.' && (!dot || dot==name)) {
        snprintf(base, bsz, "%s", name); ext[0] = '\0';
    } else if (dot && dot != name) {
        size_t blen = (size_t)(dot - name);
        snprintf(base, bsz, "%.*s", (int)blen, name);
        snprintf(ext, extsz, "%s", dot);
    } else {
        snprintf(base, bsz, "%s", name); ext[0] = '\0';
    }
}
static void unique_path(char *out, size_t outsz, const char *dest_dir, const char *name) {
    char base[PATH_MAX], ext[PATH_MAX];
    split_name(name, base, sizeof(base), ext, sizeof(ext));
    path_join(out, outsz, dest_dir, name);
    int n=1;
    while (access(out, F_OK) == 0) {
        char num[32];
        int numlen = snprintf(num, sizeof(num), "%d", n);
        if (numlen < 0 || (size_t)numlen >= sizeof(num)) die("Path suffix too long (unique_path)");
        size_t destlen = strlen(dest_dir);
        size_t baselen = strlen(base);
        size_t extlen = strlen(ext);
        size_t needed = destlen + 1 + baselen + 1 + (size_t)numlen + extlen + 1;
        if (needed > outsz) die("Path too long (unique_path)");
        memcpy(out, dest_dir, destlen);
        out[destlen] = '/';
        memcpy(out + destlen + 1, base, baselen);
        out[destlen + 1 + baselen] = '_';
        memcpy(out + destlen + 1 + baselen + 1, num, (size_t)numlen);
        memcpy(out + destlen + 1 + baselen + 1 + (size_t)numlen, ext, extlen);
        out[needed - 1] = '\0';
        n++;
    }
}

// ------------------------------ Job queue ------------------------------
typedef struct job { char *src_path; char *rel_path; int depth; bool is_symlink; } job_t;
typedef struct node { job_t job; struct node *next; } node_t;
static struct {
    node_t *head, *tail;
    pthread_mutex_t mx;
    pthread_cond_t cv;
    bool done;
} q = { .head=NULL, .tail=NULL, .mx=PTHREAD_MUTEX_INITIALIZER, .cv=PTHREAD_COND_INITIALIZER, .done=false };

static void push_job(job_t *j) {
    node_t *n = (node_t *)malloc(sizeof(node_t)); if (!n) die("OOM");
    n->job = *j; n->next = NULL;
    pthread_mutex_lock(&q.mx);
    if (q.tail) q.tail->next = n; else q.head = n;
    q.tail = n;
    pthread_cond_signal(&q.cv);
    pthread_mutex_unlock(&q.mx);
}
static bool pop_job(job_t *out) {
    pthread_mutex_lock(&q.mx);
    for (;;) {
        if (q.head) {
            node_t *n = q.head; q.head = n->next; if (!q.head) q.tail = NULL;
            *out = n->job; free(n);
            pthread_mutex_unlock(&q.mx);
            return true;
        }
        if (q.done) { pthread_mutex_unlock(&q.mx); return false; }
        pthread_cond_wait(&q.cv, &q.mx);
    }
}
static void finish_jobs(void) {
    pthread_mutex_lock(&q.mx); q.done = true; pthread_cond_broadcast(&q.cv); pthread_mutex_unlock(&q.mx);
}

// ------------------------------ Stats ------------------------------
static struct {
    pthread_mutex_t mx;
    unsigned long moved, skipped, failed;
    unsigned long long bytes_copied;
} stats = { .mx = PTHREAD_MUTEX_INITIALIZER };

static void add_moved(void) { pthread_mutex_lock(&stats.mx); stats.moved++; pthread_mutex_unlock(&stats.mx); }
static void add_skipped(void) { pthread_mutex_lock(&stats.mx); stats.skipped++; pthread_mutex_unlock(&stats.mx); }
static void add_failed(void) { pthread_mutex_lock(&stats.mx); stats.failed++; pthread_mutex_unlock(&stats.mx); }
static void add_bytes(unsigned long long b) { pthread_mutex_lock(&stats.mx); stats.bytes_copied += b; pthread_mutex_unlock(&stats.mx); }

// ------------------------------ Move/Copy ------------------------------
static int copy_file_rw(const char *src, const char *dst, mode_t mode, bool preserve_times, bool progress) {
    int in = open(src, O_RDONLY);
    if (in < 0) return -1;
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, mode & 0777);
    if (out < 0) { close(in); return -1; }

    char buf[1<<20]; // 1 MiB
    ssize_t r; unsigned long long total = 0; off_t size = 0;
    struct stat st; if (fstat(in, &st)==0) size = st.st_size; else size = 0;

    while ((r = read(in, buf, sizeof(buf))) > 0) {
        ssize_t w = 0;
        while (w < r) {
            ssize_t k = write(out, buf + w, (size_t)(r - w));
            if (k < 0) { if (errno == EINTR) continue; close(in); close(out); return -1; }
            w += k;
        }
        total += (unsigned long long)r;
        add_bytes((unsigned long long)r);
        if (progress && size > 0) {
            pthread_mutex_lock(&log_mx);
            fprintf(stdout, "  copied %llu/%lld bytes (%.0f%%)\r",
                    total, (long long)size, (100.0*total)/((double)size));
            fflush(stdout);
            pthread_mutex_unlock(&log_mx);
        }
    }
    if (progress) { pthread_mutex_lock(&log_mx); fprintf(stdout, "\n"); fflush(stdout); pthread_mutex_unlock(&log_mx); }
    if (r < 0) { close(in); close(out); return -1; }

#ifdef __linux__
    if (preserve_times) {
        struct timespec ts[2];
        ts[0].tv_sec = st.st_atime; ts[0].tv_nsec = 0;
        ts[1].tv_sec = st.st_mtime; ts[1].tv_nsec = 0;
        futimens(out, ts);
    }
#endif
    fsync(out);
    close(in); close(out);
    return 0;
}
static int move_symlink(const char *src, const char *dst, bool overwrite) {
    char target[PATH_MAX]; ssize_t len = readlink(src, target, sizeof(target)-1);
    if (len < 0) return -1;
    target[len] = '\0';
    if (overwrite) unlink(dst);
    if (symlink(target, dst) != 0) return -1;
    if (unlink(src) != 0) return -1;
    return 0;
}
static int move_file_with_modes(const char *src, const char *dst, bool overwrite, bool preserve_times, bool progress) {
    if (overwrite) unlink(dst);
    if (rename(src, dst) == 0) return 0;
    if (errno != EXDEV) return -1;
    struct stat st;
    if (stat(src, &st) < 0) return -1;
    if (copy_file_rw(src, dst, st.st_mode, preserve_times, progress) < 0) return -1;
    if (unlink(src) < 0) return -1;
    return 0;
}

// ------------------------------ Traversal ------------------------------
static char SRC_CANON[PATH_MAX];
static char DST_CANON[PATH_MAX];

static bool is_under(const char *path, const char *prefix) {
    size_t n = strlen(prefix);
    if (strncmp(path, prefix, n) != 0) return false;
    return path[n] == '\0' || path[n] == '/';
}
static bool path_is_empty_dir(const char *dir) {
    DIR *d = opendir(dir); if (!d) return false;
    struct dirent *e; int count = 0;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".")==0 || strcmp(e->d_name, "..")==0) continue;
        count++; if (count>0) break;
    }
    closedir(d);
    return count == 0;
}
static void prune_empty(const char *dir) {
    DIR *d = opendir(dir); if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".")==0 || strcmp(e->d_name, "..")==0) continue;
        char path[PATH_MAX]; path_join(path, sizeof(path), dir, e->d_name);
        struct stat st; if (lstat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            char subcanon[PATH_MAX]; if (!realpath(path, subcanon)) continue;
            if (is_under(subcanon, DST_CANON)) continue;
            prune_empty(path);
            if (path_is_empty_dir(path)) { rmdir(path); }
        }
    }
    closedir(d);
}

typedef struct job job_t;
static bool file_passes_filters(const options_t *o, const char *rel, const struct stat *st, const char *name);

static void traverse_and_queue(const options_t *o, const char *dir, int depth, const char *relbase) {
    DIR *d = opendir(dir);
    if (!d) { log_msg(1, "Warning: cannot open '%s' (%s)", dir, strerror(errno)); return; }
    struct dirent *ent;

    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".")==0 || strcmp(ent->d_name, "..")==0) continue;
        char path[PATH_MAX]; path_join(path, sizeof(path), dir, ent->d_name);
        char rel[PATH_MAX];
        if (relbase && *relbase) path_join(rel, sizeof(rel), relbase, ent->d_name);
        else str_copy_checked(rel, sizeof(rel), ent->d_name);

        struct stat st; if (lstat(path, &st) < 0) { log_msg(1, "lstat failed for '%s' (%s)", path, strerror(errno)); continue; }

        if (S_ISLNK(st.st_mode)) {
            if (!o->include_symlinks) continue;
            if (o->max_depth >= 0 && depth > o->max_depth) continue;
            if (depth >= o->min_depth) {
                if (!file_passes_filters(o, rel, &st, ent->d_name)) continue;
                job_t j = { .src_path = xstrdup(path), .rel_path = xstrdup(rel), .depth = depth, .is_symlink = true };
                push_job(&j);
            }
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            char subcanon[PATH_MAX]; if (!realpath(path, subcanon)) { log_msg(1, "realpath failed for '%s' (%s)", path, strerror(errno)); continue; }
            if (is_under(subcanon, DST_CANON)) continue;
            if (o->max_depth >= 0 && depth >= o->max_depth) continue;
            traverse_and_queue(o, path, depth + 1, rel);
        } else if (S_ISREG(st.st_mode)) {
            if (o->max_depth >= 0 && depth > o->max_depth) continue;
            if (depth >= o->min_depth) {
                if (!file_passes_filters(o, rel, &st, ent->d_name)) continue;
                job_t j = { .src_path = xstrdup(path), .rel_path = xstrdup(rel), .depth = depth, .is_symlink = false };
                push_job(&j);
            }
        }
    }
    closedir(d);
}

// ------------------------------ Worker ------------------------------
static void *worker_main(void *arg) {
    const options_t *o = (const options_t *)arg;
    job_t j;
    while (pop_job(&j)) {
        const char *name = basename_const(j.rel_path);
        char target[PATH_MAX];
        bool skip=false, overwrite=false;

        if (o->mode == MODE_SKIP) {
            path_join(target, sizeof(target), DST_CANON, name);
            if (access(target, F_OK) == 0) { skip=true; }
        }
        if (o->mode == MODE_OVERWRITE) {
            path_join(target, sizeof(target), DST_CANON, name);
            overwrite = true;
        }
        if (o->mode == MODE_RENAME) {
            pthread_mutex_lock(&name_mx);
            unique_path(target, sizeof(target), DST_CANON, name);
            pthread_mutex_unlock(&name_mx);
        }

        if (skip) {
            log_msg(2, "Skip (exists): %s", name);
            add_skipped();
            free(j.src_path); free(j.rel_path);
            continue;
        }
        if (o->dry_run) {
            log_msg(1, "WOULD MOVE: '%s' -> '%s'", j.src_path, target);
            add_skipped();
            free(j.src_path); free(j.rel_path);
            continue;
        }

        int rc = 0;
        if (j.is_symlink) rc = move_symlink(j.src_path, target, overwrite);
        else rc = move_file_with_modes(j.src_path, target, overwrite, o->preserve_times, o->progress);

        if (rc == 0) { log_msg(2, "Moved: '%s' -> '%s'", j.src_path, target); add_moved(); }
        else { log_msg(1, "ERROR: cannot move '%s' (%s)", j.src_path, strerror(errno)); add_failed(); }

        free(j.src_path); free(j.rel_path);
    }
    return NULL;
}

// ------------------------------ main ------------------------------
int main(int argc, char **argv) {
    options_t opt; parse_options(argc, argv, &opt);

    if (!realpath(opt.src, SRC_CANON)) die("Source not found: %s", opt.src);
    if (access(opt.dst, F_OK) != 0) { if (mkdir(opt.dst, 0775) != 0) die("Cannot create destination: %s", opt.dst); }
    if (!realpath(opt.dst, DST_CANON)) die("Cannot resolve destination path: %s", opt.dst);
    if (access(DST_CANON, W_OK) != 0 && !opt.dry_run) die("No write permission in destination: %s", DST_CANON);

    log_msg(1, "Source: %s", SRC_CANON);
    log_msg(1, "Dest  : %s", DST_CANON);
    if (is_under(DST_CANON, SRC_CANON)) {
        log_msg(1, "Note: destination lies within source; that subtree will be excluded.");
    }

    int nth = opt.threads > 0 ? opt.threads : 1;
    pthread_t *ths = (pthread_t *)calloc((size_t)nth, sizeof(pthread_t)); if (!ths) die("OOM");
    for (int i=0;i<nth;i++) {
        if (pthread_create(&ths[i], NULL, worker_main, &opt) != 0) die("pthread_create failed");
    }

    traverse_and_queue(&opt, SRC_CANON, 0, "");

    finish_jobs();
    for (int i=0;i<nth;i++) pthread_join(ths[i], NULL);
    free(ths);

    if (opt.prune_empty_dirs && !opt.dry_run) prune_empty(SRC_CANON);

    pthread_mutex_lock(&stats.mx);
    unsigned long moved = stats.moved, skipped = stats.skipped, failed = stats.failed;
    unsigned long long bytes = stats.bytes_copied;
    pthread_mutex_unlock(&stats.mx);

    log_msg(1, "\nDone. Moved: %lu, Skipped: %lu, Failed: %lu, Bytes copied: %llu", moved, skipped, failed, bytes);

    free_strv(opt.includes, opt.n_includes);
    free_strv(opt.excludes, opt.n_excludes);
    free_strv(opt.allow_ext, opt.n_allow_ext);
    free_strv(opt.deny_ext, opt.n_deny_ext);

    return (failed > 0) ? EXIT_FAILURE : EXIT_SUCCESS;
}
