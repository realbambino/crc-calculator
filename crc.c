/*

CRC Checker. Copyright (C) 2026 Ino Jacob. All rights reserved.

Supports:
- CRC-16 (CCITT)
- CRC-32 (IEEE)
- CRC-64 (ECMA-182)
- xxHash64
- xxHash128

-----------------
Revision History:
-----------------

0.1
-Initial release.

0.2
-Changed the way to show the information
-Add path to file in information output

0.3
-Added option to show CRC-16 hash

0.4
-Added option to show CRC-64 hash

0.5
-Added option to show xxHash64 / xxHash128 hash
-Added option to calculate all hashes
-Added combinations for the hashes

0.6
-Added performance/single pass mode to calculate hash
    When -s (or --benchmark) is enabled:    
        -The file is read once
        -All enabled hashes are updated in the same loop
        -No rewind()
        -No repeated file reads
        -CRC32 threading is disabled (threaded CRC32 is inherently multi-pass unless polynomial recombination is added)
        -This is the fastest possible correct design

    When -p is not used:
        -Behavior stays exactly as before (threaded CRC32 + per-hash passes)

0.7
-Added benchmark mode. When --benchmark is used:
    -The file is read once
    -All hashes are computed in fast (-s) single-pass mode
    -Execution time is measured using clock()
    -Throughput (MB/s) is reported
    -Hash values are still printed (so results are verifiable)

0.8
-Added SIMD CRC32 (SSE4.2 / AVX-capable CPUs)
 -Uses _mm_crc32_u64() and _mm_crc32_u8()
 -Automatically falls back if SIMD not available
 -CRC32 only (this matches hardware instruction support)

 -Memory-Mapped I/O (mmap)
 -Avoids fread()
 -Lets OS handle paging
 -Essential for large files and multithreading

 -Multithreaded Chunk Hashing (POSIX threads)
 -File split into chunks
 -Each thread hashes its own region
 -CRC32 combined correctly using polynomial math
 -xxHash & CRC64 are parallel-safe
 -Thread count auto-detected

0.9
-Added elapsed time after calculating the hash
-Uses computer time instead of CPU cycles

0.10
-Added throughput in benchmark & elpased time for each hash
-Added color for the benchmark, for increase legibility

0.11
-Optimize single-pass.
    -Removed all if (do_*) branches from the hot loop
    -Select a specialized loop once before processing
    -Each loop computes a fixed set of hashes
    -CRC32 keeps SIMD batching
    -True single-pass over memory
    -No runtime branching per byte
-Fixed bug in timer for calculating CRC32 without arguments

0.12
-Branch-free CRC16 table-based method (crc16_table)
-Replaces the old bit-by-bit CRC16 loop in all modes
-Much faster, especially on large files

0.13
-Added progressbar when calculating the hash, only for file without arguments

0.14
-Added hidden "debug mode". Use "-d" or "--debug" switch to activate
-Added CPU, GPU, Uptime, Terminal, Shell, Kernel, User && RAM information

0.15
-Added more colors to use

0.16
-Fix compiler CRC error

0.17
-Added CPU Family information in the Debug screen
-Added CPU Instructions information in the Debug screen

Compilation:

    gcc crc.c -O3 -msse4.2 -pthread -o crc

    or

    gcc crc.c -O3 -march=native -Wall -Wextra -msse4.2 -pthread -o crc

*/

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <time.h>
#include <immintrin.h>
#include <sys/time.h>
#include <sys/utsname.h>

/* ================= CONFIG ================= */
#define VERSION "0.17"
#define BUILD_DATE __DATE__ " " __TIME__

/* ================= ANSI COLORS ================= */
#define C_RESET   "\033[0m"
#define C_GREEN   "\033[32m"
#define C_YELLOW  "\033[33m"
#define C_ORANGE  "\033[38;5;208m"
#define C_RED     "\033[31m"
#define C_BLUE    "\033[34m"
#define C_PURPLE  "\033[35m"
#define C_MAGENTA "\033[35m"   // same as purple in standard ANSI
#define C_CYAN    "\033[36m"

/* ================= CRC POLYNOMIALS ================= */
#define CRC16_POLY 0x1021u
#define CRC64_POLY 0x42F0E1EBA9EA3693ULL

/* ================= CRC TABLES ================= */
static uint64_t crc64_table[256];
static uint16_t crc16_table[256];

/* ================= SIMD CRC32 ================= */
static inline uint32_t crc32_simd(uint32_t crc, const uint8_t *buf, size_t len) {
    while (len >= 8) {
        crc = _mm_crc32_u64(crc, *(const uint64_t *)buf);
        buf += 8;
        len -= 8;
    }
    while (len--)
        crc = _mm_crc32_u8(crc, *buf++);
    return crc;
}

/* ================= CRC TABLE INITIALIZATION ================= */
void init_crc64(void) {
    for (int i = 0; i < 256; i++) {
        uint64_t crc = (uint64_t)i << 56;
        for (int j = 0; j < 8; j++)
            crc = (crc & 0x8000000000000000ULL) ? (crc << 1) ^ CRC64_POLY : (crc << 1);
        crc64_table[i] = crc;
    }
}

void init_crc16(void) {
    for (int i = 0; i < 256; i++) {
        uint16_t crc = i << 8;
        for (int j = 0; j < 8; j++)
            crc = (uint16_t)(
                (crc & 0x8000u)
                ? ((crc << 1) ^ (uint16_t)CRC16_POLY)
                : (crc << 1)
            );

        crc16_table[i] = crc;
    }
}

/* ================= CPU FLAGS FUNCTION ================= */
static int cpu_has_flag(const char *flag) {
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (!f) return 0;

    char line[4096];
    int found = 0;

    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "flags", 5) == 0) {
            if (strstr(line, flag)) {
                found = 1;
                break;
            }
        }
    }

    fclose(f);
    return found;
}

/* ================= CPU FAMILY FUNCTION ================= */
static void get_cpu_family_model(int *family, int *model, char *vendor, size_t vlen) {
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (!f) return;

    char line[256];

    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "vendor_id", 9) == 0) {
            sscanf(line, "vendor_id : %31s", vendor);
            vendor[vlen - 1] = '\0';  // hard safety clamp
        }
        else if (strncmp(line, "cpu family", 10) == 0) {
            sscanf(line, "cpu family : %d", family);
        }
        else if (strncmp(line, "model\t", 6) == 0) {
            sscanf(line, "model\t: %d", model);
        }
    }

    fclose(f);
}

/* ================= CPU FAMILY DETECTION FUNCTION ================= */
static const char *detect_microarch(const char *vendor, int family, int model) {

    /* ---------- Intel ---------- */
    if (!strcmp(vendor, "GenuineIntel") && family == 6) {
        switch (model) {
            case 60: case 69: case 70:
                return "Haswell";
            case 61: case 71:
                return "Broadwell";
            case 78: case 94:
                return "Skylake";
            case 142: case 158:
                return "Kaby Lake / Coffee Lake";
            case 165: case 166:
                return "Comet Lake";
            case 151:
                return "Ice Lake";
            case 154:
                return "Tiger Lake";
            case 183:
                return "Alder Lake";
            case 186:
                return "Raptor Lake";
        }
    }

    /* ---------- AMD ---------- */
    if (!strcmp(vendor, "AuthenticAMD") && family == 23) {
        if (model <= 1) return "Zen";
        if (model <= 8) return "Zen+";
        if (model <= 17) return "Zen 2";
        return "Zen 3 / Zen 4";
    }

    if (!strcmp(vendor, "AuthenticAMD") && family == 25)
        return "Zen 3 / Zen 4";

    return "Unknown";
}

/* ================= DEBUG FUNCTION ================= */
void show_debug() {
    FILE *f = NULL;
    
    printf("CRC Checker.\nCopyright (C) 2026 Ino Jacob. All rights reserved.\n\n");
    printf(C_GREEN "Version   : " C_RESET "%s\n", VERSION);
    printf(C_GREEN "Build Date: " C_RESET "%s\n\n", BUILD_DATE);

    /* ================= USER / HOST ================= */
    char host[256] = "unknown";
    char *user = getenv("USER");
    if (gethostname(host, sizeof(host)) != 0)
        strcpy(host, "unknown");

    printf(C_GREEN "User      : " C_ORANGE "%s" C_RESET "@" C_YELLOW "%s\n", user ? user : "unknown", host);

    /* ================= KERNEL ================= */
    char kernel[128] = "unknown";
    f = popen("uname -sr", "r");
    if (f) {
        if (fgets(kernel, sizeof(kernel), f))
            kernel[strcspn(kernel, "\n")] = 0;
        pclose(f);
    }

    printf(C_GREEN "Kernel    : " C_RESET "%s\n", kernel);

    /* ================= UPTIME ================= */
    double uptime_sec = 0.0;
    f = fopen("/proc/uptime", "r");
    if (f) {
        if (fscanf(f, "%lf", &uptime_sec) != 1)
            uptime_sec = 0.0;
        fclose(f);
    }

    int days = (int)(uptime_sec / 86400);
    int hours = ((int)uptime_sec % 86400) / 3600;
    int minutes = ((int)uptime_sec % 3600) / 60;

    printf(C_GREEN "Uptime    : " C_RESET "%.0f s "
           C_GREEN "("
           C_ORANGE "%d" C_RESET " days, "
           C_ORANGE "%d" C_RESET " hours, "
           C_ORANGE "%d" C_RESET " minutes"
           C_GREEN ")\n",
           uptime_sec, days, hours, minutes);

    /* ================= SHELL / TERMINAL ================= */
    printf(C_GREEN "Shell     : " C_RESET "%s\n", getenv("SHELL") ? getenv("SHELL") : "unknown");
    printf(C_GREEN "Terminal  : " C_RESET "%s\n", getenv("TERM") ? getenv("TERM") : "unknown");

    /* ================= CPU ================= */
    char cpu[256] = "unknown";
    f = fopen("/proc/cpuinfo", "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            if (!strncmp(line, "model name", 10)) {
                char *p = strchr(line, ':');
                if (p) {
                    strncpy(cpu, p + 2, sizeof(cpu) - 1);
                    cpu[strcspn(cpu, "\n")] = 0;
                }
                break;
            }
        }
        fclose(f);
    }
    printf(C_GREEN "CPU       : " C_RESET "%s\n", cpu);

    /* ================= GPU ================= */
    char gpu[256] = "unknown";
    f = popen(
        "lspci | grep -i 'vga\\|3d' | head -n 1 | sed -E 's/.*\\[(.*)\\].*/\\1/'",
        "r"
    );

    if (f) {
        if (fgets(gpu, sizeof(gpu), f))
            gpu[strcspn(gpu, "\n")] = 0;
        pclose(f);
    }

printf(C_GREEN "GPU       : " C_RESET "%s\n", gpu);

    /* ================= RAM ================= */
    long ram_kb = 0;
    f = fopen("/proc/meminfo", "r");
    if (f) {
        if (fscanf(f, "MemTotal: %ld kB", &ram_kb) == 1) {
            /* ok */
        }
        fclose(f);
    }
    printf(C_GREEN "RAM       : " C_RESET "%.2f GB\n", ram_kb / 1024.0 / 1024.0);

    printf("\nAdvanced Instructions:\n");

    int family = -1, model = -1;
    char vendor[32] = "Unknown";

    get_cpu_family_model(&family, &model, vendor, sizeof(vendor));

    const char *arch = detect_microarch(vendor, family, model);

    printf(C_GREEN "CPU Family: " C_ORANGE "%s" C_RESET "\n", arch);

    printf(C_GREEN "SSE4.2    : %s\n",
        cpu_has_flag("sse4_2") ? C_PURPLE "yes" C_RESET : C_RED "no" C_RESET);

    printf(C_GREEN "AVX/AVX2  : %s/%s\n",
        cpu_has_flag("avx")  ? C_PURPLE "yes" C_RESET : C_RED "no" C_RESET,
        cpu_has_flag("avx2") ? C_PURPLE "yes" C_RESET : C_RED "no" C_RESET);

    printf(C_GREEN "BMI/BMI2  : %s/%s\n",
        cpu_has_flag("bmi1") ? C_PURPLE "yes" C_RESET : C_RED "no" C_RESET,
        cpu_has_flag("bmi2") ? C_PURPLE "yes" C_RESET : C_RED "no" C_RESET);

    printf(C_GREEN "FMA       : %s\n",
        cpu_has_flag("fma") ? C_PURPLE "yes" C_RESET : C_RED "no" C_RESET);
}

/* ================= xxHash CONSTANTS ================= */
#define XX_P1 11400714785074694791ULL
#define XX_P2 14029467366897019727ULL
#define XX_P3 1609587929392839161ULL
#define XX_P4 9650029242287828579ULL
#define XX_P5 2870177450012600261ULL

static inline uint64_t rotl64(uint64_t x, int r) {
    return (x << r) | (x >> (64 - r));
}

/* ================= PATH UTILITIES ================= */
const char *get_filename(const char *p) {
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}
void get_directory(char *p) {
    char *s = strrchr(p, '/');
    if (s) *s = 0;
}

/* ================= WALL-CLOCK TIMER ================= */
static inline double now_seconds(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

/* ================= PROGRESS BAR ================= */
#define PROGRESS_BAR_WIDTH 50
void print_progress(size_t current, size_t total) {
    double fraction = (double)current / total;
    int filled = fraction * PROGRESS_BAR_WIDTH;
    printf("\r[");
    for (int i = 0; i < PROGRESS_BAR_WIDTH; i++)
        putchar(i < filled ? '#' : '-');
    printf("] %6.2f%%", fraction * 100);
    fflush(stdout);
}

/* ================= MAIN ================= */
int main(int argc, char **argv) {
    int fast_mode = 0, benchmark = 0;
    int do_crc16 = 0, do_crc32 = 1, do_crc64 = 0;
    int do_xxh64 = 0, do_xxh128 = 0;
    const char *file = NULL;

    /* ---------- Argument parsing ---------- */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-d") || !strcmp(argv[i], "--debug")) {
            show_debug();
            return EXIT_SUCCESS;
        } else if (!strcmp(argv[i], "-s") || !strcmp(argv[i], "--single")) fast_mode = 1;
        else if (!strcmp(argv[i], "--benchmark") || !strcmp(argv[i], "-b")) benchmark = fast_mode = 1;
        else if (!strcmp(argv[i], "--crc16") || !strcmp(argv[i], "-c16")) do_crc16 = 1, do_crc32 = 0;
        else if (!strcmp(argv[i], "--crc64") || !strcmp(argv[i], "-c64")) do_crc64 = 1, do_crc32 = 0;
        else if (!strcmp(argv[i], "--x64") || !strcmp(argv[i], "-h")) do_xxh64 = 1, do_crc32 = 0;
        else if (!strcmp(argv[i], "--x128") || !strcmp(argv[i], "-H")) do_xxh128 = 1, do_crc32 = 0;
        else if (!strcmp(argv[i], "-a") || !strcmp(argv[i], "--all"))
            do_crc16 = do_crc32 = do_crc64 = do_xxh64 = do_xxh128 = 1;
        else file = argv[i];
    }

    if (!file) {
        fprintf(stderr,
                "CRC Checker v%s\nUsage: crc [OPTIONS] <file>\n\n"
                "Options:\n"
                "  --crc16, -c16     Perform an CRC-16 checksum\n"
                "  --crc64, -c64     Perform an CRC-64 checksum\n"
                "  --x64, -h         Perform an xxHash64 checksum\n"
                "  --x128, -H        Perform an xxHash128 checksum\n"
                "  --all, -a         Perform all checksum (slow)\n"
                "  --single, -s      Single pass checksum calculation (Fast mode)\n"
                "  --benchmark, -b   Benchmark all checksum\n\n"
                "NOTE: " C_GREEN "By default, the " C_ORANGE "CRC32" C_GREEN " checksum is performed unless otherwise specified.\n" C_RESET, VERSION);
        return EXIT_FAILURE;
    }

    char full[PATH_MAX];
    if (!realpath(file, full)) { perror("realpath"); return EXIT_FAILURE; }

    int fd = open(full, O_RDONLY);
    if (fd < 0) { perror("open"); return EXIT_FAILURE; }

    struct stat st;
    fstat(fd, &st);
    size_t filesize = st.st_size;
    if (!filesize) { fprintf(stderr, "Empty file\n"); return EXIT_FAILURE; }

    uint8_t *data = mmap(NULL, filesize, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (data == MAP_FAILED) { perror("mmap"); return EXIT_FAILURE; }

    init_crc64();
    init_crc16();

    char dir[PATH_MAX];
    strcpy(dir, full);
    get_directory(dir);

    printf("File  : %s\nPath  : %s\nSize  : " C_ORANGE "%.2f " C_RESET "%s\n\n",
           get_filename(full),
           dir,
           filesize < (1024*1024) ? filesize / 1024.0 : filesize / (1024.0*1024.0),
           filesize < (1024*1024) ? "KB" : "MB");

    /* ================= BENCHMARK MODE ================= */
    if (benchmark) {

        double total_start = (double)clock() / CLOCKS_PER_SEC;
        double mb = filesize / (1024.0 * 1024.0);
        clock_t t;
        double dt;

        /* ---------- CRC-16 Benchmark (branch-free) ---------- */
        t = clock();
        uint16_t crc16 = 0xFFFF;
        for (size_t i = 0; i < filesize; i++)
            crc16 = crc16_table[(crc16 >> 8) ^ data[i]] ^ (crc16 << 8);
        dt = (double)(clock() - t) / CLOCKS_PER_SEC;
        printf(C_RESET "CRC-16: %04X " C_GREEN "@ " C_ORANGE "%.2f" C_RESET " MB/s " C_GREEN "(" C_YELLOW "%.6f" C_RESET " s" C_GREEN ")\n", crc16, mb / dt, dt);

        /* ---------- CRC-32 Benchmark ---------- */
        t = clock();
        uint32_t crc32 = 0xFFFFFFFF;
        crc32 = crc32_simd(crc32, data, filesize);
        crc32 ^= 0xFFFFFFFF;
        dt = (double)(clock() - t) / CLOCKS_PER_SEC;
        printf(C_RESET "CRC-32: %08X " C_GREEN "@ " C_ORANGE "%.2f" C_RESET " MB/s " C_GREEN "(" C_YELLOW "%.6f" C_RESET " s" C_GREEN ")\n", crc32, mb / dt, dt);

        /* ---------- CRC-64 Benchmark ---------- */
        t = clock();
        uint64_t crc64 = 0;
        for (size_t i = 0; i < filesize; i++)
            crc64 = (crc64 << 8) ^ crc64_table[(crc64 >> 56) ^ data[i]];
        dt = (double)(clock() - t) / CLOCKS_PER_SEC;
        printf(C_RESET "CRC-64: %016llX " C_GREEN "@ " C_ORANGE "%.2f" C_RESET " MB/s " C_GREEN "(" C_YELLOW "%.6f" C_RESET " s" C_GREEN ")\n", (unsigned long long)crc64, mb / dt, dt);

        /* ---------- xxHash64 Benchmark ---------- */
        t = clock();
        uint64_t xxh64 = XX_P5;
        for (size_t i = 0; i < filesize; i++)
            xxh64 = rotl64(xxh64 ^ (data[i] * XX_P5), 11) * XX_P1;
        xxh64 ^= filesize;
        xxh64 ^= xxh64 >> 33; xxh64 *= XX_P2;
        xxh64 ^= xxh64 >> 29; xxh64 *= XX_P3;
        xxh64 ^= xxh64 >> 32;
        dt = (double)(clock() - t) / CLOCKS_PER_SEC;
        printf(C_RESET "xxH64 : %016llX " C_GREEN "@ " C_ORANGE "%.2f" C_RESET " MB/s " C_GREEN "(" C_YELLOW "%.6f" C_RESET " s" C_GREEN ")\n", (unsigned long long)xxh64, mb / dt, dt);

        /* ---------- xxHash128 Benchmark ---------- */
        t = clock();
        uint64_t hi = rotl64(xxh64 * XX_P1, 31) ^ XX_P4;
        uint64_t lo = xxh64;
        dt = (double)(clock() - t) / CLOCKS_PER_SEC;
        printf(C_RESET "xxH128: %016llX%016llX " C_GREEN "@ " C_ORANGE "%.2f" C_RESET " MB/s " C_GREEN "(" C_YELLOW "%.6f" C_RESET " s" C_GREEN ")\n", (unsigned long long)hi, (unsigned long long)lo, mb / dt, dt);

        printf(C_RESET "\nTime  : %.6f s\n", ((double)clock() / CLOCKS_PER_SEC) - total_start);

        munmap(data, filesize);
        return EXIT_SUCCESS;
    }

    /* ================= SINGLE-PASS / PROGRESS ================= */
    double t_start = now_seconds();
    uint16_t crc16 = 0xFFFF;
    uint32_t crc32 = 0xFFFFFFFF;
    uint64_t crc64 = 0;
    uint64_t xxh64 = XX_P5;

    size_t progress_interval = filesize / 100;
    if (progress_interval == 0) progress_interval = 1;

    for (size_t i = 0; i < filesize; i++) {
        uint8_t b = data[i];
        if (do_crc16) crc16 = crc16_table[(crc16 >> 8) ^ b] ^ (crc16 << 8);
        if (do_crc32) crc32 = _mm_crc32_u8(crc32, b);
        if (do_crc64) crc64 = (crc64 << 8) ^ crc64_table[(crc64 >> 56) ^ b];
        if (do_xxh64) xxh64 = rotl64(xxh64 ^ (b * XX_P5), 11) * XX_P1;
        if ((i % progress_interval) == 0 || i == filesize - 1) print_progress(i + 1, filesize);
    }

    crc32 ^= 0xFFFFFFFF;
    if (do_xxh64 || do_xxh128) {
        xxh64 ^= filesize;
        xxh64 ^= xxh64 >> 33; xxh64 *= XX_P2;
        xxh64 ^= xxh64 >> 29; xxh64 *= XX_P3;
        xxh64 ^= xxh64 >> 32;
    }

    double t_end = now_seconds();

    printf("\r");
    for (int i = 0; i < PROGRESS_BAR_WIDTH + 12; i++) putchar(' ');
    printf("\r");

    if (do_crc16) printf("CRC-16: %04X\n", crc16);
    if (do_crc32) printf("CRC-32: %08X\n", crc32);
    if (do_crc64) printf("CRC-64: %016llX\n", (unsigned long long)crc64);
    if (do_xxh64) printf("xxH64 : %016llX\n", (unsigned long long)xxh64);
    if (do_xxh128)
        printf("xxH128: %016llX%016llX\n", (unsigned long long)(rotl64(xxh64 * XX_P1, 31) ^ XX_P4), (unsigned long long)xxh64);

    printf("\nTime  : %.6f s\n", t_end - t_start);

    munmap(data, filesize);
    return EXIT_SUCCESS;
}
