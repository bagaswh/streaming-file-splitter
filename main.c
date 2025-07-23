#define _GNU_SOURCE

#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

struct program_flags {
    size_t split_size_in_bytes;
    char *file;
    char *part_prefix;
    bool overwrite_existing_part_files;
};

void
log_printf(char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "LOG: ");
    vfprintf(stderr, fmt, args);
    va_end(args);
}

void
log_fatalf(char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "FATAL: ");
    vfprintf(stderr, fmt, args);
    va_end(args);
    abort();
}

bool
strmatch(char *s1, char *s2)
{
    return strcmp(s1, s2) == 0;
}

enum parse_bytes_status { INVALID_FORMAT, INVALID_UNIT, OK };
struct parse_bytes_result {
    enum parse_bytes_status status;
    size_t bytes;
};

char *
parse_bytes_status_str(enum parse_bytes_status status)
{
    switch (status) {
    case OK:
        return "OK";
    case INVALID_FORMAT:
        return "Invalid format";
    case INVALID_UNIT:
        return "Invalid unit";
    }
}

struct parse_bytes_result
parse_bytes(char *bytes)
{
    struct parse_bytes_result res = { 0 };

    size_t multiplier = 1;
    size_t len = strlen(bytes);
    bool has_numeric = false;
    int digits_count = 0;
    for (size_t i = 0; i < len; i++) {
        char c = bytes[i];
        if (isdigit(c)) {
            digits_count++;
            has_numeric = true;
        } else if (!has_numeric && !isalnum(c)) {
            res.status = INVALID_FORMAT;
            return res;
        } else if (!isalnum(c)) {
            break;
        }
    }

    char numbers[digits_count + 1];
    strncpy(numbers, bytes, digits_count);
    size_t number = atoi(numbers);

    char *unit = bytes + digits_count;
    if (strmatch("B", unit))
        multiplier *= 1;
    else if (strmatch("KB", unit))
        multiplier *= 1000;
    else if (strmatch("KiB", unit) || strmatch("K", unit))
        multiplier *= 1024;
    else if (strmatch("MB", unit))
        multiplier *= 1000 * 1000;
    else if (strmatch("MiB", unit) || strmatch("M", unit))
        multiplier *= 1024 * 1024;
    else if (strmatch("GB", unit))
        multiplier *= 1000 * 1000 * 1000;
    else if (strmatch("GiB", unit) || strmatch("G", unit))
        multiplier *= 1024 * 1024 * 1024;
    else {
        res.status = INVALID_UNIT;
        return res;
    }

    res.bytes = number * multiplier;
    res.status = OK;
    return res;
}

struct program_flags
read_flags(int argc, char **argv)
{
    bool overwrite_existing_parts_file = false;
    size_t split_size_in_bytes = 0;

    int c;
    while ((c = getopt(argc, argv, "b:oh")) != -1) {
        switch (c) {
        case 'b':
            struct parse_bytes_result result = parse_bytes(optarg);
            if (result.status != OK)
                log_fatalf("failed to parse unit '%s' in option -b: %s\n",
                           optarg, parse_bytes_status_str(result.status));
            else
                split_size_in_bytes = result.bytes;
            break;
        case 'o':
            overwrite_existing_parts_file = true;
            break;
        case 'h':
            break;
        case '?':
            log_fatalf("Invalid option. Use -h for usage information.\n");
            break;
        default:
            log_fatalf("Unknown option: %c\n", c);
            break;
        }
    }

    char *file_path = NULL;
    if (optind < argc) {
        file_path = argv[optind];
        if (optind + 1 < argc) {
            log_fatalf("Too many arguments\n");
        }
    } else {
        log_fatalf("Missing file path argument\n");
    }

    return (struct program_flags) {
        .file = file_path,
        .split_size_in_bytes = split_size_in_bytes,
        .overwrite_existing_part_files = overwrite_existing_parts_file,
    };
}

int
num_digits(int n)
{
    int digits = 0;
    while (n) {
        digits++;
        n /= 10;
    }
    return digits;
}

void *
malloc_or_die(size_t size)
{
    void *ptr = malloc(size);
    if (ptr)
        return ptr;
    log_fatalf("cannot malloc of size %zu\n", size);
    return NULL;
}

void
swap_char(char *a, char *b)
{
    char temp = *b;
    *b = *a;
    *a = temp;
}

char *
reverse_string(char *s)
{
    for (size_t i = 0; i < (strlen(s) / 2); i++) {
        printf("left_i=%zu right_i=%zu c_left=%c c_right=%c\n", i,
               strlen(s) - 1 - i, s[i], s[strlen(s) - 1 - i]);
        swap_char(&s[i], &s[strlen(s) - 1 - i]);
    }
    return s;
}

void
itoa(int n, char s[])
{
    int i, sign;

    if ((sign = n) < 0) /* record sign */
        n = -n;         /* make n positive */
    i = 0;
    do {                       /* generate digits in reverse order */
        s[i++] = n % 10 + '0'; /* get next digit */
    } while ((n /= 10) > 0); /* delete it */
    if (sign < 0)
        s[i++] = '-';
    s[i] = '\0';
    reverse_string(s);
}

void
free_wrap(void *ptr)
{
    if (ptr != NULL)
        free(ptr);
}

#define subtract_saturated(a, b, diff) \
    do {                               \
        if (a < b) {                   \
            diff = a;                  \
            a = 0;                     \
        } else {                       \
            diff = b;                  \
            a -= b;                    \
        }                              \
    } while (0)

int
main(int argc, char **argv)
{
    struct program_flags flags = read_flags(argc, argv);

    int in_fd = open(flags.file, O_RDWR);
    if (in_fd == -1)
        log_fatalf("cannot open file '%s': %s\n", flags.file,
                   strerror(errno));

    struct stat in_stat = { 0 };
    if (fstat(in_fd, &in_stat) == -1)
        log_fatalf("cannot stat file '%s': %s\n", flags.file,
                   strerror(errno));

    char *in_mmap =
        mmap(NULL, in_stat.st_size, PROT_READ, MAP_PRIVATE, in_fd, 0);
    if (in_mmap == MAP_FAILED)
        log_fatalf("failed to mmap file '%s': %s\n", flags.file,
                   strerror(errno));

    int total_parts = in_stat.st_size / flags.split_size_in_bytes;
    if (in_stat.st_size % flags.split_size_in_bytes)
        total_parts++;
    int parts_max_digits = num_digits(total_parts);
    size_t in_file_name_length = strlen(flags.file);
    char *out_file_name =
        malloc_or_die(in_file_name_length + 1 + parts_max_digits);
    strcpy(out_file_name, flags.file);

    size_t in_file_offset = in_stat.st_size;
    size_t write_len = 0;

    while (total_parts) {
        subtract_saturated(in_file_offset, flags.split_size_in_bytes,
                           write_len);

        itoa(total_parts, out_file_name + in_file_name_length);

        struct stat out_file_stat = { 0 };
        if (stat(out_file_name, &out_file_stat) != -1
            && !flags.overwrite_existing_part_files)
            log_fatalf(
                "The file '%s' already exists, I won't touch it as I "
                "may corrupt it. If YOU want me to overwrite it, pass "
                "-o/--overwrite-existing option. Unti then, so long!\n",
                out_file_name);

        int out_fd_part = open(out_file_name, O_CREAT | O_WRONLY | O_TRUNC);
        if (out_fd_part == -1)
            log_fatalf("cannot open file part '%s': %s\n", out_file_name,
                       strerror(errno));

        if (write(out_fd_part, in_mmap + in_file_offset, write_len) == -1)
            log_fatalf("cannot write %zu bytes to file '%s': %s\n", write_len,
                       out_file_name, strerror(errno));

        if (ftruncate(in_fd, in_file_offset) != 0)
            log_fatalf("failed to ftruncate file '%s': %s\n", flags.file,
                       strerror(errno));

        printf("part filename: %s; write_len: %zu\n", out_file_name,
               write_len);
        total_parts--;
    }

    free_wrap(out_file_name);
    return 0;
}
