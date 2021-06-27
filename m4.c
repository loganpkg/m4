/*
 * Copyright (c) 2021 Logan Ryan McLintock
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * m4 macro processor.
 * Assumes NULL pointers are zero.
 *
 * README:
 * By default the esyscmd and maketemp built-in macros are excluded.
 * Set ESYSCMD_MAKETEMP to 1 to include them.
 * To compile:
 * $ cc -ansi -g -O3 -Wall -Wextra -pedantic m4.c && mv a.out m4
 * or
 * > cl /Ot /Wall /wd4820 /wd4242 /wd4244 /wd4996 /wd4710 /wd5045 /wd4706 m4.c
 * and place the executable somewhere in your PATH.
 *
 * To use:
 * $ m4 [file...]
 *
 * These are the built-in macros, presented as a mini-tutorial:
 * changequote([, ])
 * define(cool, $1 and $2)
 * cool(goat, mice)
 * undefine([cool])
 * define(cool, wow)
 * dumpdef([cool], [y], [define])
 * hello dnl this will be removed
 * divnum
 * divert(2)
 * divnum
 * cool
 * divert(6)
 * divnum
 * y
 * undivert(2)
 * divert
 * undivert
 * incr(76)
 * len(goat)
 * index(elephant, ha)
 * substr(elephant, 2, 4)
 * translit(bananas, abcs, xyz)
 * ifdef([cool], yes defined, not defined)
 * define(y, 5)
 * ifelse(y, 5, true, false)
 * esyscmd(ifelse(dirsep, /, ls, dir))
 * esyscmd(echo hello > .test)
 * include(.test)
 * maketemp(XXXXXX)
 * errprint(oops there is an error)
 * htdist
 * add(8, 2, 4)
 * mult( , 5, , 3)
 * sub(80, 20, 5)
 * div(5, 2)
 * mod(5, 2)
 */

/* Set to 1 to enable the esyscmd and maketemp built-in macros */
#define ESYSCMD_MAKETEMP 0

#ifdef __linux__
#define _XOPEN_SOURCE 500
#endif

#include <sys/types.h>
#include <sys/stat.h>

#if ESYSCMD_MAKETEMP && !defined _WIN32
#include <sys/wait.h>
#endif

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <errno.h>

#if ESYSCMD_MAKETEMP && defined _WIN32
#define popen _popen
#define pclose _pclose
#endif

#define INIT_BUF_SIZE 512

#define HASH_TABLE_SIZE 16384

/* size_t Addition OverFlow test */
#define AOF(a, b) ((a) > SIZE_MAX - (b))
/* size_t Multiplication OverFlow test */
#define MOF(a, b) ((a) && (b) > SIZE_MAX / (a))

/* Minimum */
#define MIN(a, b) ((a) < (b) ? (a) : (b))

#define getch(b, read_stdin) (b->i ? *(b->a + --b->i) \
    : (read_stdin ? getchar() : EOF))

#define BUF_FREE_SIZE(b) (b->s - b->i)

struct buf {
    char *a;
    size_t i;
    size_t s;
};

/*
 * For hash table entries. Multpile entries at the same hash value link
 * together to form a singularly linked list.
 */
struct entry {
    struct entry *next;
    char *name;
    char *def;
};

/*
 * mcall used to stack on nested macro calls.
 * Only argument buffers 1 to 9 are used (index 0 will be NULL).
 */
struct mcall {
    struct mcall *next;
    char *name;                 /* Macro name */
    char *def;                  /* Macro definition before substitution */
    size_t bracket_depth;       /* Only unquoted brackets are counted */
    size_t act_arg;             /* The current argument being collected */
    struct buf *arg_buf[10];    /* For argument collection */
};

struct buf *init_buf(void)
{
    struct buf *b;
    if ((b = malloc(sizeof(struct buf))) == NULL)
        return NULL;
    if ((b->a = malloc(INIT_BUF_SIZE)) == NULL) {
        free(b);
        return NULL;
    }
    b->s = INIT_BUF_SIZE;
    b->i = 0;
    return b;
}

void free_buf(struct buf *b)
{
    if (b != NULL) {
        free(b->a);
        free(b);
    }
}

int grow_buf(struct buf *b, size_t will_use)
{
    char *t;
    size_t new_s;
    /* Gap is big enough, nothing to do */
    if (will_use <= BUF_FREE_SIZE(b))
        return 0;
    if (MOF(b->s, 2))
        return 1;
    new_s = b->s * 2;
    if (AOF(new_s, will_use))
        return 1;
    new_s += will_use;
    if ((t = realloc(b->a, new_s)) == NULL)
        return 1;
    b->a = t;
    b->s = new_s;
    return 0;
}

int ungetch(struct buf *b, int ch)
{
    if (b->i == b->s)
        if (grow_buf(b, 1))
            return EOF;
    return *(b->a + b->i++) = ch;
}

int filesize(char *fn, size_t * fs)
{
    /* Gets the filesize of a filename */
    struct stat st;
    if (stat(fn, &st))
        return 1;

#ifndef S_ISREG
#define S_ISREG(m) ((m & S_IFMT) == S_IFREG)
#endif

    if (!S_ISREG(st.st_mode))
        return 1;
    if (st.st_size < 0)
        return 1;
    *fs = st.st_size;
    return 0;
}

int include(struct buf *b, char *fn)
{
    FILE *fp;
    int x;
    size_t fs;
    size_t back_i;

    if (filesize(fn, &fs))
        return 1;
    if (fs > BUF_FREE_SIZE(b) && grow_buf(b, fs))
        return 1;
    if ((fp = fopen(fn, "rb")) == NULL)
        return 1;
    back_i = b->i + fs;

    errno = 0;
    while ((x = getc(fp)) != EOF)
        *(b->a + --back_i) = x;

    if (errno) {
        fclose(fp);
        return 1;
    }
    if (fclose(fp))
        return 1;

    /* Check */
    if (back_i != b->i)
        return 1;

    /* Success */
    b->i += fs;

    return 0;
}

void delete_buf(struct buf *b)
{
    b->i = 0;
}

int getword(struct buf *token, struct buf *input, int read_stdin, int *err)
{
    int x;
    delete_buf(token);

    /* Always read at least one char */
    errno = 0;
    if ((x = getch(input, read_stdin)) == EOF) {
        if (errno)
            *err = 1;
        return 1;
    }
    /* EOF, no error */
    if (ungetch(token, x) == EOF)
        return 1;

    if (isalpha(x) || x == '_') {
        /* Could be the start of a macro name */
        while (1) {
            /* Read another char */
            errno = 0;
            if ((x = getch(input, read_stdin)) == EOF) {
                if (errno)
                    *err = 1;
                return 1;
            }
            if (!(isalnum(x) || x == '_')) {
                /* Read past the end of the token, so put the char back */
                if (ungetch(input, x) == EOF) {
                    *err = 1;
                    return 1;
                }
                break;
            } else {
                /* Store the char */
                if (ungetch(token, x) == EOF) {
                    *err = 1;
                    return 1;
                }
            }
        }
    }
    /* Null terminate token */
    if (ungetch(token, '\0') == EOF) {
        *err = 1;
        return 1;
    }
    return 0;
}

int ungetstr(struct buf *b, char *s)
{
    size_t len = strlen(s);
    while (len)
        if (ungetch(b, *(s + --len)) == EOF)
            return 1;
    return 0;
}

#if ESYSCMD_MAKETEMP
int esyscmd(struct buf *input, struct buf *tmp_buf, char *cmd)
{
    FILE *fp;
    int x, status;
    delete_buf(tmp_buf);

#ifdef _WIN32
#define R_STR "rb"
#else
#define R_STR "r"
#endif
    if ((fp = popen(cmd, R_STR)) == NULL)
        return 1;

    errno = 0;
    while ((x = getc(fp)) != EOF)
        if (x != '\0' && ungetch(tmp_buf, x) == EOF) {
            pclose(fp);
            return 1;
        }
    if (errno) {
        pclose(fp);
        return 1;
    }
    if ((status = pclose(fp)) == -1)
        return 1;
#ifdef _WIN32
    if (status)
        return 1;
#else
#define EXIT_OK (WIFEXITED(status) && !WEXITSTATUS(status))
    if (!EXIT_OK)
        return 1;
#endif
    if (ungetch(tmp_buf, '\0') == EOF)
        return 1;
    if (ungetstr(input, tmp_buf->a))
        return 1;
    return 0;
}
#endif

struct entry *init_entry(void)
{
    struct entry *e;
    if ((e = malloc(sizeof(struct entry))) == NULL)
        return NULL;
    e->next = NULL;
    e->name = NULL;
    e->def = NULL;
    return e;
}

size_t hash_str(char *s)
{
    /* djb2 */
    unsigned char c;
    size_t h = 5381;
    while ((c = *s++))
        h = h * 33 ^ c;
    return h % HASH_TABLE_SIZE;
}

void htdist(struct entry **ht)
{
    struct entry *e;
    size_t freq[101] = { 0 }, count, k;
    for (k = 0; k < HASH_TABLE_SIZE; ++k) {
        e = *(ht + k);
        count = 0;
        while (e != NULL) {
            e = e->next;
            ++count;
        }
        count < 100 ? ++*(freq + count) : ++*(freq + 100);
    }
    fprintf(stderr, "entries_per_bucket number_of_buckets\n");
    for (k = 0; k < 100; k++)
        if (*(freq + k))
            fprintf(stderr, "%lu %lu\n", (unsigned long) k,
                    (unsigned long) *(freq + k));
    if (*(freq + 100))
        fprintf(stderr, ">=100 %lu\n", (unsigned long) *(freq + 100));
}

struct entry *lookup_entry(struct entry **ht, char *name)
{
    size_t h = hash_str(name);
    struct entry *e = *(ht + h);
    while (e != NULL) {
        /* Found */
        if (!strcmp(name, e->name))
            return e;
        e = e->next;
    }
    /* Not found */
    return NULL;
}

char *get_def(struct entry **ht, char *name)
{
    struct entry *e;
    if ((e = lookup_entry(ht, name)) == NULL)
        return NULL;
    return e->def;
}

int upsert_entry(struct entry **ht, char *name, char *def)
{
    struct entry *e;
    size_t h;
    char *t = NULL;
    if ((e = lookup_entry(ht, name)) == NULL) {
        /* Insert entry: */
        if ((e = init_entry()) == NULL)
            return 1;
        /* Store data */
        if ((e->name = strdup(name)) == NULL) {
            free(e);
            return 1;
        }
        if (def != NULL && (e->def = strdup(def)) == NULL) {
            free(e->name);
            free(e);
            return 1;
        }
        h = hash_str(name);
        /* Link new entry in at head of list (existing head could be NULL) */
        e->next = *(ht + h);
        *(ht + h) = e;
    } else {
        /* Update entry: */
        if (def != NULL && (t = strdup(def)) == NULL)
            return 1;
        free(e->def);
        e->def = t;             /* Could be NULL */
    }
    return 0;
}

int delete_entry(struct entry **ht, char *name)
{
    size_t h = hash_str(name);
    struct entry *e = *(ht + h), *prev = NULL;
    while (e != NULL) {
        /* Found */
        if (!strcmp(name, e->name))
            break;
        prev = e;
        e = e->next;
    }
    if (e != NULL) {
        /* Found */
        /* Link around the entry */
        if (prev != NULL)
            prev->next = e->next;
        free(e->name);
        free(e->def);
        free(e);
        /* At head of list */
        if (prev == NULL)
            *(ht + h) = NULL;
        return 0;
    }

    /* Not found */
    return 1;
}

void free_hash_table(struct entry **ht)
{
    struct entry *e, *ne;
    size_t j;
    if (ht != NULL) {
        for (j = 0; j < HASH_TABLE_SIZE; ++j) {
            e = *(ht + j);
            while (e != NULL) {
                ne = e->next;
                free(e->name);
                free(e->def);
                free(e);
                e = ne;
            }
        }
        free(ht);
    }
}

void free_mcall(struct mcall *m)
{
    size_t j = 1;
    if (m != NULL) {
        free(m->name);
        free(m->def);
        while (*(m->arg_buf + j) != NULL && j < 10) {
            free_buf(*(m->arg_buf + j));
            ++j;
        }
        free(m);
    }
}

struct mcall *init_mcall(void)
{
    struct mcall *m;
    if ((m = calloc(1, sizeof(struct mcall))) == NULL)
        return NULL;
    /*
     * Arg 0 is not used. Only allocate the buffer for arg 1.
     * The others will be allocated on demand.
     */
    if ((*(m->arg_buf + 1) = init_buf()) == NULL) {
        free_mcall(m);
        return NULL;
    }
    m->act_arg = 1;
    return m;
}

int stack_on_mcall(struct mcall **stack)
{
    struct mcall *t;
    if ((t = init_mcall()) == NULL)
        return 1;

    /* Link new head on */
    if (*stack != NULL)
        t->next = *stack;

    *stack = t;
    return 0;
}

void delete_stack_head(struct mcall **stack)
{
    struct mcall *m;
    if (*stack == NULL)
        return;
    m = (*stack)->next;
    free_mcall(*stack);
    *stack = m;
}

void free_stack(struct mcall *stack)
{
    struct mcall *m = stack, *next;
    while (m != NULL) {
        next = m->next;
        free_mcall(m);
        m = next;
    }
}

int put_str(struct buf *b, char *s)
{
    size_t len = strlen(s);
    if (len > BUF_FREE_SIZE(b) && grow_buf(b, len))
        return 1;
    memcpy(b->a + b->i, s, len);
    b->i += len;
    return 0;
}

int sub_args(struct buf *result, struct mcall *stack)
{
    char *s = stack->def, ch, h;
    struct buf *b;
    delete_buf(result);
    while ((ch = *s++)) {
        if (ch == '$') {
            /* Look ahead */
            h = *s;
            if (isdigit(h) && h != '0') {
                /* Arg */
                if ((b = *(stack->arg_buf + (h - '0'))) != NULL) {
                    if (b->i > BUF_FREE_SIZE(result)
                        && grow_buf(result, b->i))
                        return 1;
                    memcpy(result->a + result->i, b->a, b->i);
                    result->i += b->i;
                }
                ++s;            /* Eat */
            } else {
                if (ungetch(result, ch) == EOF)
                    return 1;
            }
        } else {
            if (ungetch(result, ch) == EOF)
                return 1;
        }
    }
    if (ungetch(result, '\0') == EOF)
        return 1;

    return 0;
}

char *strip_def(char *def)
{
    char *d, *t, ch, h;
    if ((d = strdup(def)) == NULL)
        return NULL;
    t = d;
    while ((ch = *def++)) {
        if (ch == '$') {
            /* Look ahead */
            h = *def;
            if (isdigit(h) && h != '0')
                ++def;          /* Eat */
            else
                *t++ = ch;
        } else {
            *t++ = ch;
        }
    }
    *t = '\0';
    return d;
}

int terminate_args(struct mcall *stack)
{
    size_t j = 1;
    while (*(stack->arg_buf + j) != NULL && j < 10) {
        if (ungetch(*(stack->arg_buf + j), '\0'))
            return 1;
        ++j;
    }
    return 0;
}

int str_to_num(char *s, size_t * num)
{
    char ch;
    size_t n = 0, len = 0;
    if (s == NULL)
        return 1;
    while ((ch = *s++)) {
        ++len;
        if (isdigit(ch)) {
            if (MOF(n, 10))
                return 1;
            n *= 10;
            if (AOF(n, (ch - '0')))
                return 1;
            n += (ch - '0');
        } else {
            return 1;
        }
    }
    if (!len)
        return 1;
    *num = n;
    return 0;
}

int buf_dump_buf(struct buf *dst, struct buf *src)
{
    if (src->i > BUF_FREE_SIZE(dst) && grow_buf(dst, src->i))
        return 1;
    memcpy(dst->a + dst->i, src->a, src->i);
    dst->i += src->i;
    src->i = 0;
    return 0;
}

#define QUIT do { \
    ret = 1; \
    goto clean_up; \
} while (0)

int main(int argc, char **argv)
{
    int ret = 0, read_stdin = 1, err, j;
#if ESYSCMD_MAKETEMP && !defined _WIN32
    int fd;
#endif
    struct buf *input = NULL, *token = NULL, *next_token = NULL, *result =
        NULL, *tmp_buf = NULL;
    struct entry **ht = NULL, *e;
    int quote_on = 0;
    size_t quote_depth = 0, fs, total_fs = 0, act_div = 0, k, len, w, n;
    /* Diversion 10 is -1 */
    struct buf *diversion[11] = { NULL };
    struct buf *output;
    char left_quote[2] = { '`', '\0' };
    char right_quote[2] = { '\'', '\0' };
    struct mcall *stack = NULL;
#define NUM_SIZE 24
    char *sd = NULL, *tmp_str = NULL, num[NUM_SIZE], *p, *q;
    unsigned char uc, uc2;
    int map[UCHAR_MAX], x;

    if (argc < 1)
        return 1;

#ifdef _WIN32
    if (_setmode(_fileno(stdin), _O_BINARY) == -1)
        return 1;
    if (_setmode(_fileno(stdout), _O_BINARY) == -1)
        return 1;
    if (_setmode(_fileno(stderr), _O_BINARY) == -1)
        return 1;
#endif

    /* Setup buffers */
    if ((input = init_buf()) == NULL)
        QUIT;
    if ((token = init_buf()) == NULL)
        QUIT;
    if ((next_token = init_buf()) == NULL)
        QUIT;
    if ((result = init_buf()) == NULL)
        QUIT;
    if ((tmp_buf = init_buf()) == NULL)
        QUIT;

    /* Setup diversions */
    for (k = 0; k < 11; ++k)
        if ((*(diversion + k) = init_buf()) == NULL)
            QUIT;
    output = *diversion;

    if ((ht = calloc(HASH_TABLE_SIZE, sizeof(struct entry *))) == NULL)
        QUIT;

    /* Define built-in macros. They have a def of NULL. */
    if (upsert_entry(ht, "define", NULL))
        QUIT;
    if (upsert_entry(ht, "undefine", NULL))
        QUIT;
    if (upsert_entry(ht, "changequote", NULL))
        QUIT;
    if (upsert_entry(ht, "divert", NULL))
        QUIT;
    if (upsert_entry(ht, "dumpdef", NULL))
        QUIT;
    if (upsert_entry(ht, "errprint", NULL))
        QUIT;
    if (upsert_entry(ht, "ifdef", NULL))
        QUIT;
    if (upsert_entry(ht, "ifelse", NULL))
        QUIT;
    if (upsert_entry(ht, "include", NULL))
        QUIT;
    if (upsert_entry(ht, "len", NULL))
        QUIT;
    if (upsert_entry(ht, "index", NULL))
        QUIT;
    if (upsert_entry(ht, "translit", NULL))
        QUIT;
    if (upsert_entry(ht, "substr", NULL))
        QUIT;
    if (upsert_entry(ht, "dnl", NULL))
        QUIT;
    if (upsert_entry(ht, "divnum", NULL))
        QUIT;
    if (upsert_entry(ht, "undivert", NULL))
        QUIT;
#if ESYSCMD_MAKETEMP
    if (upsert_entry(ht, "esyscmd", NULL))
        QUIT;
    if (upsert_entry(ht, "maketemp", NULL))
        QUIT;
#endif
    if (upsert_entry(ht, "incr", NULL))
        QUIT;
    if (upsert_entry(ht, "htdist", NULL))
        QUIT;
    if (upsert_entry(ht, "dirsep", NULL))
        QUIT;
    if (upsert_entry(ht, "add", NULL))
        QUIT;
    if (upsert_entry(ht, "mult", NULL))
        QUIT;
    if (upsert_entry(ht, "sub", NULL))
        QUIT;
    if (upsert_entry(ht, "div", NULL))
        QUIT;
    if (upsert_entry(ht, "mod", NULL))
        QUIT;

    if (argc > 1) {
        /* Do not read stdin if there are command line files */
        read_stdin = 0;
        /* Get total size of command line files */
        for (j = argc - 1; j; --j) {
            if (filesize(*(argv + j), &fs))
                QUIT;
            total_fs += fs;
        }
        /* Make sure buffer is big enough to hold all files */
        if (grow_buf(input, total_fs))
            QUIT;
        /* Load command line files into buffer */
        for (j = argc - 1; j; --j)
            if (include(input, *(argv + j)))
                QUIT;
    }

/* Token string */
#define TS token->a

/* Next token string */
#define NTS next_token->a

/* End of argument collection */
#define ARG_END (stack != NULL && stack->bracket_depth == 1 \
    && !strcmp(TS, ")"))

/* Nested close bracket (unquoted) */
#define NESTED_CB (stack != NULL && stack->bracket_depth > 1 \
    && !strcmp(TS, ")"))

/* Nested open bracket (unquoted) */
#define NESTED_OB (stack != NULL && !strcmp(TS, "("))

/* Argument comma in a macro call */
#define ARG_COMMA (stack != NULL && stack->bracket_depth == 1 \
    && !strcmp(TS, ","))

/* String s is a match of a macro name */
#define ISMACRO(s) ((isalpha(*s) || *s == '_') \
    && (e = lookup_entry(ht, s)) != NULL)

/* Set output to stack argument collection buffer or diversion buffer */
#define SET_OUTPUT output = (stack == NULL ? *(diversion + act_div) \
    : *(stack->arg_buf + stack->act_arg))

#define DIV(n) (*(diversion + n))

#define OUT_DIV(n) do { \
    if (DIV(n)->i && fwrite(DIV(n)->a, 1, DIV(n)->i, stdout) != DIV(n)->i) \
        QUIT; \
    DIV(n)->i = 0; \
} while (0)

#define UNDIVERT_ALL for (k = 0; k < 10; k++) \
    OUT_DIV(k)

/* Tests if a string consists of a single whitespace character */
#define WS(s) (!strcmp(s, " ") || !strcmp(s, "\t") || !strcmp(s, "\n") \
    || !strcmp(s, "\r"))

/* Remove stack head and set ouput */
#define REMOVE_SH do { \
    delete_stack_head(&stack); \
    SET_OUTPUT; \
} while (0)

/* Stack macro collected argument number n */
#define ARG(n) (*(stack->arg_buf + n) == NULL ? "" : (*(stack->arg_buf + n))->a)

#define READ_TOKEN(t) do { \
    err = 0; \
    if (getword(t, input, read_stdin, &err)) { \
        if (err) \
            QUIT; \
        else \
            goto end_of_input; \
    } \
} while(0)

/* Eat whitespace */
#define EAT_WS do { \
    do \
        READ_TOKEN(next_token); \
    while (WS(NTS)); \
    if (ungetstr(input, NTS)) \
        QUIT; \
} while (0)

/* Delete to new line (inclusive) */
#define DNL do { \
    do \
        READ_TOKEN(next_token); \
    while (strcmp(NTS, "\n")); \
} while (0)

#define DIVNUM do { \
    if (act_div == 10) \
        snprintf(num, NUM_SIZE, "%d", -1); \
    else \
        snprintf(num, NUM_SIZE, "%lu", (unsigned long) act_div); \
    if (ungetstr(input, num)) \
        QUIT; \
} while (0)

#define SN stack->name

#define EMSG(m) fprintf(stderr, m "\n")

#define EQUIT(m) do { \
    EMSG(m); \
    QUIT; \
} while (0)

#if ESYSCMD_MAKETEMP
#ifdef _WIN32
/* No integer overflow risk, as already a string. */
/* This function does not actually create the file */
#define MAKETEMP(s) if (_mktemp_s(s, strlen(s) + 1)) \
    EQUIT("maketemp: Failed")
#else
#define MAKETEMP(s) do { \
    if ((fd = mkstemp(s)) == -1) \
        EQUIT("maketemp: Failed"); \
    if (close(fd)) \
        EQUIT("maketemp: Failed to close temp file"); \
} while (0)
#endif
#endif

#ifdef _WIN32
#define DIRSEP "\\"
#else
#define DIRSEP "/"
#endif

/* Process built-in macro with args */
#define PROCESS_BI_WITH_ARGS \
    if (!strcmp(SN, "define")) { \
        if (upsert_entry(ht, ARG(1), ARG(2))) \
            QUIT; \
    } else if (!strcmp(SN, "undefine")) { \
        if (delete_entry(ht, ARG(1))) \
            QUIT; \
    } else if (!strcmp(SN, "changequote")) { \
        if (strlen(ARG(1)) != 1 || strlen(ARG(2)) != 1 || *ARG(1) == *ARG(2) \
            || !isgraph(*ARG(1)) || !isgraph(*ARG(2)) \
            || *ARG(1) == '(' || *ARG(2) == '(' \
            || *ARG(1) == ')' || *ARG(2) == ')' \
            || *ARG(1) == ',' || *ARG(2) == ',') { \
            EQUIT("changequote: quotes must be different single graph chars" \
                " that cannot a comma or parentheses"); \
        } \
        *left_quote = *ARG(1); \
        *right_quote = *ARG(2); \
    } else if (!strcmp(SN, "divert")) { \
        if (strlen(ARG(1)) == 1 && isdigit(*ARG(1))) \
            act_div = *ARG(1) - '0'; \
        else if (!strcmp(ARG(1), "-1")) \
            act_div = 10; \
        else \
            EQUIT("divert: Diversion number must be 0 to 9 or -1"); \
        SET_OUTPUT; \
    } else if (!strcmp(SN, "dumpdef")) { \
        for (k = 1; k < 10; ++k) { \
            if (ISMACRO(ARG(k))) \
                fprintf(stderr, "%s: %s\n", ARG(k), \
                    e->def == NULL ? "built-in" : e->def); \
            else if (*ARG(k) != '\0') \
                fprintf(stderr, "%s: undefined\n", ARG(k)); \
        } \
    } else if (!strcmp(SN, "errprint")) { \
        for (k = 1; k < 10; ++k) \
            if (*ARG(k) != '\0') \
                fprintf(stderr, "%s\n", ARG(k)); \
    } else if (!strcmp(SN, "ifdef")) { \
        if (ungetstr(input, ISMACRO(ARG(1)) ? ARG(2) : ARG(3))) \
            QUIT; \
    } else if (!strcmp(SN, "ifelse")) { \
        if (ungetstr(input, !strcmp(ARG(1), ARG(2)) ? ARG(3) : ARG(4))) \
            QUIT; \
    } else if (!strcmp(SN, "include")) { \
        if (include(input, ARG(1))) { \
            fprintf(stderr, "include: Failed to include file: %s\n", ARG(1)); \
            QUIT; \
        } \
    } else if (!strcmp(SN, "len")) { \
        snprintf(num, NUM_SIZE, "%lu", (unsigned long) strlen(ARG(1))); \
        if (ungetstr(input, num)) \
            QUIT; \
    } else if (!strcmp(SN, "index")) { \
        p = strstr(ARG(1), ARG(2)); \
        if (p == NULL) \
            snprintf(num, NUM_SIZE, "%d", -1); \
        else \
            snprintf(num, NUM_SIZE, "%lu", (unsigned long) (p - ARG(1))); \
        if (ungetstr(input, num)) \
            QUIT; \
    } else if (!strcmp(SN, "translit")) { \
        /* Set mapping to pass through (-1) */ \
        for (k = 0; k < UCHAR_MAX; k++) \
            *(map + k) = -1; \
        p = ARG(2); \
        q = ARG(3); \
        /* Create mapping while strings are in parallel */ \
        while ((uc = *p++) && (uc2 = *q++)) \
            if (*(map + uc) == -1) /* Preference to first occurrence */ \
                *(map + uc) = uc2; \
        /* Continue first string, setting mapping to delete (\0) */ \
        while (uc != '\0') { \
            *(map + uc) = '\0'; \
            uc = *p++; \
        } \
        if ((tmp_str = strdup(ARG(1))) == NULL) \
            QUIT; \
        p = ARG(1); \
        q = tmp_str; \
        while ((uc = *p++)) { \
            x = *(map + uc); \
            if (x == -1) \
                *q++ = uc; \
            else if (x != '\0') \
                *q++ = x; \
        } \
        *q = '\0'; \
        if (ungetstr(input, tmp_str)) \
            QUIT; \
        free(tmp_str); \
        tmp_str = NULL; \
    } else if (!strcmp(SN, "substr")) { \
        if ((len = strlen(ARG(1)))) { \
            if (str_to_num(ARG(2), &w) || str_to_num(ARG(3), &n)) \
                EQUIT("substr: Invalid index or length"); \
            if (w < len) { \
                /* Do not need to check for overflow here */ \
                if ((tmp_str = malloc(len + 1)) == NULL) \
                    QUIT; \
                if (AOF(n, 1)) \
                    QUIT; \
                snprintf(tmp_str, MIN(len + 1, n + 1), "%s", ARG(1) + w); \
                if (ungetstr(input, tmp_str)) \
                    QUIT; \
                free(tmp_str); \
                tmp_str = NULL; \
            } \
        } \
    } else if (!strcmp(SN, "undivert")) { \
        if (!act_div) { \
            /* In diversion 0 */ \
            for (k = 1; k < 10; k++) \
                if (strlen(ARG(k)) == 1 && isdigit(*ARG(k)) && *ARG(k) != '0') \
                    OUT_DIV(*ARG(k) - '0'); \
        } else { \
            /* Cannot undivert division 0 or the active diversion */ \
            for (k = 1; k < 10; k++) \
                if (strlen(ARG(k)) == 1 && isdigit(*ARG(k)) && *ARG(k) != '0' \
                    && (size_t) (*ARG(k) - '0') != act_div \
                    && buf_dump_buf(DIV(act_div), DIV((*ARG(k) - '0')))) \
                        QUIT; \
        } \
    } else if (!strcmp(SN, "dnl")) { \
        DNL; \
    } else if (!strcmp(SN, "divnum")) { \
        DIVNUM; \
    } else if (!strcmp(SN, "incr")) { \
        if(str_to_num(ARG(1), &n)) \
            EQUIT("incr: Invalid number"); \
        if (AOF(n, 1)) \
            EQUIT("incr: Integer overflow"); \
        n += 1; \
        snprintf(num, NUM_SIZE, "%lu", (unsigned long) n); \
        if (ungetstr(input, num)) \
            QUIT; \
    } else if (!strcmp(SN, "htdist")) { \
        htdist(ht); \
    } else if (!strcmp(SN, "dirsep")) { \
        if (ungetstr(input, DIRSEP)) \
            QUIT; \
    } else if (!strcmp(SN, "add")) { \
        w = 0; \
        for (k = 1; k < 10; ++k) { \
            if (*ARG(k) != '\0') { \
                if (str_to_num(ARG(k), &n)) \
                    EQUIT("add: Invalid number"); \
                if (AOF(w, n)) \
                    EQUIT("add: Integer overflow"); \
                w += n; \
            } \
        } \
        snprintf(num, NUM_SIZE, "%lu", (unsigned long) w); \
        if (ungetstr(input, num)) \
            QUIT; \
    } else if (!strcmp(SN, "mult")) { \
        w = 1; \
        for (k = 1; k < 10; ++k) { \
            if (*ARG(k) != '\0') { \
                if (str_to_num(ARG(k), &n)) \
                    EQUIT("mult: Invalid number"); \
                if (MOF(w, n)) \
                    EQUIT("mult: Integer overflow"); \
                w *= n; \
            } \
        } \
        snprintf(num, NUM_SIZE, "%lu", (unsigned long) w); \
        if (ungetstr(input, num)) \
            QUIT; \
    } else if (!strcmp(SN, "sub")) { \
        if (*ARG(1) == '\0') \
            EQUIT("sub: Argument 1 must be used"); \
        if (str_to_num(ARG(1), &w)) \
            EQUIT("sub: Invalid number"); \
        for (k = 2; k < 10; ++k) { \
            if (*ARG(k) != '\0') { \
                if (str_to_num(ARG(k), &n)) \
                    EQUIT("sub: Invalid number"); \
                if (n > w) \
                    EQUIT("sub: Integer underflow"); \
                w -= n; \
            } \
        } \
        snprintf(num, NUM_SIZE, "%lu", (unsigned long) w); \
        if (ungetstr(input, num)) \
            QUIT; \
    } else if (!strcmp(SN, "div")) { \
        if (*ARG(1) == '\0') \
            EQUIT("div: Argument 1 must be used"); \
        if (str_to_num(ARG(1), &w)) \
            EQUIT("div: Invalid number"); \
        for (k = 2; k < 10; ++k) { \
            if (*ARG(k) != '\0') { \
                if (str_to_num(ARG(k), &n)) \
                    EQUIT("div: Invalid number"); \
                if (!n) \
                    EQUIT("div: Divide by zero"); \
                w /= n; \
            } \
        } \
        snprintf(num, NUM_SIZE, "%lu", (unsigned long) w); \
        if (ungetstr(input, num)) \
            QUIT; \
    } else if (!strcmp(SN, "mod")) { \
        if (*ARG(1) == '\0') \
            EQUIT("mod: Argument 1 must be used"); \
        if (str_to_num(ARG(1), &w)) \
            EQUIT("mod: Invalid number"); \
        for (k = 2; k < 10; ++k) { \
            if (*ARG(k) != '\0') { \
                if (str_to_num(ARG(k), &n)) \
                    EQUIT("mod: Invalid number"); \
                if (!n) \
                    EQUIT("mod: Modulo by zero"); \
                w %= n; \
            } \
        } \
        snprintf(num, NUM_SIZE, "%lu", (unsigned long) w); \
        if (ungetstr(input, num)) \
            QUIT; \
    }

#if ESYSCMD_MAKETEMP
/* These tag onto the end of the list of built-in macros with args */
#define PROCESS_BI_WITH_ARGS_EXTRA \
    else if (!strcmp(SN, "maketemp")) { \
        /* ARG(1) is the template string which is modified in-place */ \
        MAKETEMP(ARG(1)); \
        if (ungetstr(input, ARG(1))) \
            QUIT; \
    } else if (!strcmp(SN, "esyscmd")) { \
        if (esyscmd(input, tmp_buf, ARG(1))) \
            EQUIT("esyscmd: Failed"); \
    }
#endif


/* Process built-in macro with no arguments */
#define PROCESS_BI_NO_ARGS do { \
    if (!strcmp(TS, "dnl")) { \
        DNL; \
    } else if (!strcmp(TS, "divnum")) { \
        DIVNUM; \
    } else if (!strcmp(TS, "undivert")) { \
        if (act_div) \
            EQUIT("undivert: Can only call from diversion 0" \
                " when called without arguments"); \
        UNDIVERT_ALL; \
    } else if (!strcmp(TS, "divert")) { \
        act_div = 0; \
        SET_OUTPUT; \
    } else if (!strcmp(TS, "htdist")) { \
        htdist(ht); \
    } else if (!strcmp(TS, "dirsep")) { \
        if (ungetstr(input, DIRSEP)) \
            QUIT; \
    } else { \
        /* The remaining macros must take arguments, so pass through */ \
        if (put_str(output, TS)) \
            QUIT; \
    } \
} while (0)


    /* m4 loop: read input word by word */
    while (1) {
        /* Write diversion 0 (for interactive use) */
        OUT_DIV(0);
        /* Read token */
        READ_TOKEN(token);

        if (!strcmp(TS, left_quote)) {
            if (!quote_on)
                quote_on = 1;
            if (quote_depth && put_str(output, TS))
                QUIT;
            ++quote_depth;
        } else if (!strcmp(TS, right_quote)) {
            if (quote_depth > 1 && put_str(output, TS))
                QUIT;
            if (!--quote_depth)
                quote_on = 0;
        } else if (quote_on) {
            if (put_str(output, TS))
                QUIT;
        } else if (ISMACRO(TS)) {
            /* Token match */
            READ_TOKEN(next_token);

            if (!strcmp(NTS, "(")) {
                /* Start of macro with arguments */
                /* Add macro call to stack */
                if (stack_on_mcall(&stack))
                    QUIT;
                /* Copy macro name */
                if ((stack->name = strdup(TS)) == NULL)
                    QUIT;
                /* Copy macro definition (built-ins will be NULL) */
                if (e->def != NULL
                    && (stack->def = strdup(e->def)) == NULL)
                    QUIT;
                /* Increment bracket depth for this first bracket */
                ++stack->bracket_depth;
                SET_OUTPUT;
                EAT_WS;
            } else {
                /* Macro call without arguments (stack is not used) */
                /* Put the next token back into the input */
                if (ungetstr(input, NTS))
                    QUIT;
                if (e->def == NULL) {
                    /* Built-in macro */
                    PROCESS_BI_NO_ARGS;
                } else {
                    /* User defined macro */
                    /* Strip dollar argument positions from definition */
                    if ((sd = strip_def(e->def)) == NULL)
                        QUIT;
                    if (ungetstr(input, sd))
                        QUIT;
                    free(sd);
                    sd = NULL;
                }
            }
        } else if (ARG_END) {
            /* End of argument collection */
            /* Decrement bracket depth for bracket just encountered */
            --stack->bracket_depth;
            if (stack->def == NULL) {
                /* Built-in macro */
                if (terminate_args(stack))
                    QUIT;
                /* Deliberately no semicolons after these macro calls */
                PROCESS_BI_WITH_ARGS
#if ESYSCMD_MAKETEMP
                    PROCESS_BI_WITH_ARGS_EXTRA
#endif
            } else {
                /* User defined macro */
                if (sub_args(result, stack))
                    QUIT;
                if (ungetstr(input, result->a))
                    QUIT;
            }
            REMOVE_SH;
        } else if (ARG_COMMA) {
            if (stack->act_arg == 9)
                EQUIT("Macro call has too many arguments");
            /* Allocate buffer for argument collection */
            if ((*(stack->arg_buf + ++stack->act_arg) =
                 init_buf()) == NULL)
                QUIT;
            SET_OUTPUT;
            EAT_WS;
        } else if (NESTED_CB) {
            if (put_str(output, TS))
                QUIT;
            --stack->bracket_depth;
        } else if (NESTED_OB) {
            if (put_str(output, TS))
                QUIT;
            ++stack->bracket_depth;
        } else {
            /* Pass through token */
            if (put_str(output, TS))
                QUIT;
        }
    }

  end_of_input:

    /* Checks */
    if (stack != NULL)
        EQUIT("Input finished without unwinding the stack");
    if (quote_on)
        EQUIT("Input finished without exiting quotes");

    UNDIVERT_ALL;

  clean_up:
    free_buf(input);
    free_buf(token);
    free_buf(next_token);
    free_buf(result);
    free_buf(tmp_buf);
    for (k = 0; k < 11; ++k)
        free_buf(*(diversion + k));
    free_hash_table(ht);
    free_stack(stack);
    free(sd);
    free(tmp_str);
    return ret;
}
