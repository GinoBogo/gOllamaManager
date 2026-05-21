/* ************************************************************************** */
/*
    @file
        gollama_manager.c

    @date
        May, 2026

    @author
        Gino Francesco Bogo (ᛊᛟᚱᚱᛖ ᛗᛖᚨ ᛁᛊᛏᚨᛗᛁ ᚨcᚢᚱᛉᚢ)

    @license
        MIT

    @note
        Compile: gcc -o gollama_manager gollama_manager.c -O2 -lncursesw -lpthread
*/
/* ************************************************************************** */

#define _XOPEN_SOURCE 600 // POSIX.1-2001 (or use 700 for newer)
#include <ctype.h>        // isprint, tolower
#include <locale.h>       // LC_ALL, setlocale
#include <ncurses.h>      // ACS_HLINE, ACS_LLCORNER, ACS_LRCORNER, ACS_LTEE, ACS_RTEE ...
#include <pthread.h>      // pthread_create, pthread_detach, pthread_mutex_destroy, ...
#include <stdarg.h>       // va_list, va_start, va_end
#include <stdbool.h>      // bool, false, true
#include <stdio.h>        // FILE, fflush, fgets, fprintf, getchar, ...
#include <stdlib.h>       // WEXITSTATUS, WIFEXITED, exit, system
#include <string.h>       // memcpy, memset, strcmp, strlen, strtok, ...
#include <unistd.h>       // NULL, chdir, getcwd
#include <wchar.h>        // wchar_t, wcswidth

// -----------------------------------------------------------------------------
// Constants
// -----------------------------------------------------------------------------

#define MAX_MODELS      256
#define MAX_NAME_LEN    128
#define MAX_ID_LEN      64
#define MAX_SIZE_LEN    32
#define MAX_DATE_LEN    64
#define MAX_PROC_LEN    64
#define MAX_CONTEXT_LEN 16
#define MAX_LINE_LEN    1024
#define MAX_LOG_LEN     512
#define MIN_COL_PAD     2
#define MAX_COL_WIDTH   40
#define MAX_CMD_OUT     32768

// -----------------------------------------------------------------------------
// ncurses Color Pairs
// -----------------------------------------------------------------------------

enum {
    CP_DEFAULT = 1,
    CP_HEADER,
    CP_ACCENT,
    CP_SUCCESS,
    CP_DANGER,
    CP_WARNING,
    CP_RUNNING,
    CP_SELECTED,
    CP_BORDER,
    CP_DIALOG,
    CP_DIALOG_BORDER,
    CP_INFO_TEXT
};

// -----------------------------------------------------------------------------
// Data Structures
// -----------------------------------------------------------------------------

/**
 * @brief Represents an installed model.
 */
typedef struct {
    char name[MAX_NAME_LEN]; /**< Model name */
    char id[MAX_ID_LEN];     /**< Model identifier (hash) */
    char size[MAX_SIZE_LEN]; /**< Human-readable size */
    char date[MAX_DATE_LEN]; /**< Last modified date */
} Model;

/**
 * @brief Represents a running model instance.
 */
typedef struct {
    char name[MAX_NAME_LEN];       /**< Model name */
    char id[MAX_ID_LEN];           /**< Model identifier */
    char size[MAX_SIZE_LEN];       /**< Size of the model */
    char proc[MAX_PROC_LEN];       /**< Processor usage (percentage + CPU/GPU) */
    char context[MAX_CONTEXT_LEN]; /**< Context window size */
    char expires[MAX_DATE_LEN];    /**< Expiration time */
} Running;

/**
 * @brief Global application state.
 */
static struct {
    Model           models[MAX_MODELS];           /**< Array of installed models */
    Running         running[MAX_MODELS];          /**< Array of running models */
    char            filter[MAX_NAME_LEN];         /**< Search filter string */
    int             sel_model;                    /**< Selected index in installed list */
    int             sel_running;                  /**< Selected index in running list */
    int             model_cnt;                    /**< Number of installed models */
    int             running_cnt;                  /**< Number of running models */
    int             tab;                          /**< 0 = installed, 1 = running */
    int             pulling;                      /**< Non-zero if a pull operation is in progress */
    char            status[MAX_LOG_LEN];          /**< Status message displayed in header */
    char            logmsg[MAX_LOG_LEN];          /**< Log message displayed at bottom */
    int             need_refresh;                 /**< Set to 1 to request a UI refresh (protected by mutex) */
    int             refreshing;                   /**< Non-zero if a refresh thread is running (protected by mutex) */
    int             show_dialog;                  /**< Pull dialog active flag */
    int             show_info;                    /**< Info dialog active flag */
    int             show_search;                  /**< Search dialog active flag */
    int             confirm_active;               /**< Confirmation dialog active flag */
    int             confirm_choice;               /**< 0 = Cancel, 1 = OK */
    char            confirm_msg[256];             /**< Message to show in confirmation dialog */
    char            confirm_target[MAX_NAME_LEN]; /**< Model name for delete/stop */
    int             confirm_is_delete;            /**< 1 = delete, 0 = stop */
    char            dialog_input[MAX_NAME_LEN];   /**< Input buffer for dialogs */
    int             dialog_cursor;                /**< Current cursor position in dialog_input */
    int             dialog_insert;                /**< 1 = insert mode, 0 = overwrite mode */
    char            info_out[8192];               /**< Output buffer for `ollama show` */
    pthread_mutex_t mutex;                        /**< Mutex for thread-safe data access */

    /* Dynamic column widths for installed tab */
    int col_name, col_id, col_size, col_date;
    /* Dynamic column widths for running tab */
    int rcol_name, rcol_id, rcol_size, rcol_proc, rcol_ctx, rcol_exp;
} st;

/**< Current terminal dimensions */
static int rows;
static int cols;

// -----------------------------------------------------------------------------
// Helper Functions
// -----------------------------------------------------------------------------

/**
 * @brief Clear the terminal screen using ANSI escape codes.
 *
 * Sends the clear-screen and home-cursor sequence to stdout.
 */
static void clear_term(void) {
    printf("\033[2J\033[H");
    fflush(stdout);
}

/**
 * @brief Case‑insensitive substring search (portable replacement for
 * strcasestr).
 *
 * @param[in] haystack String to search in.
 * @param[in] needle Substring to search for.
 * @return Pointer to the first occurrence of needle within haystack, or NULL if
 * not found.
 */
static char *_strcasestr(const char *haystack, //
                         const char *needle) {
    if (!haystack || !needle || !*needle) {
        return !*needle ? (char *)haystack : NULL; // Empty needle matches at start
    }

    size_t n_len = strlen(needle);

    for (const char *p = haystack; *p; ++p) {
        // Check if needle matches starting at position p
        size_t i;
        for (i = 0; i < n_len; ++i) {
            if (tolower((unsigned char)p[i]) != tolower((unsigned char)needle[i])) {
                break; // Mismatch
            }
        }
        if (i == n_len) { // All characters matched
            return (char *)p;
        }
    }
    return NULL;
}

// -----------------------------------------------------------------------------
// Unicode Sanitizer for Fullwidth Characters (Info Dialog)
// -----------------------------------------------------------------------------

/**
 * @brief Convert a fullwidth character (U+FF00–U+FFEF) to its halfwidth ASCII
 * equivalent.
 *
 * @param cp Unicode codepoint.
 * @return Converted codepoint, or original if not in fullwidth range.
 */
static uint32_t fullwidth_to_halfwidth(uint32_t cp) {
    // Fullwidth ! " # $ % & ' ( ) * + , - . / 0-9 : ; < = > ? @ A-Z [ \ ] ^ _ ` a-z { | } ~
    if (cp >= 0xFF01 && cp <= 0xFF5E) {
        return cp - 0xFF01 + 0x21;
    }
    // Specific override for U+FF5C (fullwidth vertical bar) – ensure it becomes '|'
    if (cp == 0xFF5C) {
        return '|';
    }
    // Fullwidth cent, pound, etc. (optional)
    if (cp == 0xFFE0)
        return 0xA2; // cent
    if (cp == 0xFFE1)
        return 0xA3; // pound
    if (cp == 0xFFE5)
        return 0xA5; // yen
    // Everything else stays as is
    return cp;
}

/**
 * @brief Decode one UTF-8 character and return its Unicode codepoint.
 *
 * @param s Pointer to UTF-8 string.
 * @param len Number of bytes consumed (output).
 * @return Unicode codepoint, or 0xFFFD on error.
 */
static uint32_t utf8_decode(const char *s, int *len) {
    unsigned char c = (unsigned char)s[0];

    if (c < 0x80) {
        *len = 1;
        return c;
    }

    if ((c & 0xE0) == 0xC0 && //
        (s[1] & 0xC0) == 0x80) {
        *len = 2;
        return ((c & 0x1F) << 6) | (s[1] & 0x3F);
    }

    if ((c & 0xF0) == 0xE0 &&    //
        (s[1] & 0xC0) == 0x80 && //
        (s[2] & 0xC0) == 0x80) {
        *len = 3;
        return ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
    }

    if ((c & 0xF8) == 0xF0 &&    //
        (s[1] & 0xC0) == 0x80 && //
        (s[2] & 0xC0) == 0x80 && //
        (s[3] & 0xC0) == 0x80) {
        *len = 4;
        return ((c & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
    }

    *len = 1;
    return 0xFFFD; // replacement character
}

/**
 * @brief Encode a Unicode codepoint as UTF-8.
 *
 * @param cp Codepoint.
 * @param buf Output buffer (at least 4 bytes).
 * @return Number of bytes written.
 */
static int utf8_encode(uint32_t cp, char *buf) {
    if (cp < 0x80) {
        buf[0] = (char)cp;
        return 1;
    }

    if (cp < 0x800) {
        buf[0] = (char)(0xC0 | (cp >> 6));
        buf[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }

    if (cp < 0x10000) {
        buf[0] = (char)(0xE0 | (cp >> 12));
        buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }

    buf[0] = (char)(0xF0 | (cp >> 18));
    buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    buf[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/**
 * @brief Calculate the length (in terminal columns) of a UTF-8 string.
 *
 * @param str UTF-8 encoded string.
 * @return Number of UTF-8 characters, or fallback to byte count on error.
 */
static int utf8_strlen(const char *str) {
    if (!str || !*str)
        return 0;

    // Convert UTF-8 to wide characters for width calculation
    wchar_t wc[MAX_LINE_LEN];
    size_t  len = mbstowcs(wc, str, MAX_LINE_LEN - 1);

    if (len == (size_t)-1) {
        // Fallback: return byte count if conversion fails
        return (int)strlen(str);
    }
    wc[len] = L'\0';

    // wcswidth returns display columns (handles CJK, emojis, etc.)
    int width = wcswidth(wc, (int)len);
    return (width < 0) ? (int)strlen(str) : width;
}

/**
 * @brief Sanitize a UTF-8 string: convert fullwidth characters to halfwidth.
 *
 * @param dst Output buffer.
 * @param src Input string.
 * @param dst_size Size of output buffer.
 */
static void sanitize_fullwidth(char *dst, const char *src, size_t dst_size) {
    size_t di = 0;

    while (*src && di < dst_size - 4) { // room for worst-case 4 bytes
        int len;

        uint32_t cp = utf8_decode(src, &len);
        if (cp == 0xFFFD) {
            // invalid sequence: copy raw byte as '?'
            dst[di++] = '?';
            src++;
            continue;
        }

        uint32_t converted = fullwidth_to_halfwidth(cp);
        if (converted != cp) {
            char buf[4];
            int  n = utf8_encode(converted, buf);
            for (int i = 0; i < n && di < dst_size - 1; i++) {
                dst[di++] = buf[i];
            }
        } else {
            // keep original UTF-8 bytes
            for (int i = 0; i < len && di < dst_size - 1; i++) {
                dst[di++] = src[i];
            }
        }
        src += len;
    }
    dst[di] = '\0';
}

/**
 * @brief Change current working directory to root ("/")
 *
 * This function attempts to change the process's working directory to root. If
 * the operation fails, it is ignored (best effort).
 */
static void chdir2root(void) {
    /* Change current working directory to root */
    if (chdir("/") == -1) {
        /* ignore – best effort */
    }
}

/**
 * @brief Change current working directory to a specified path, ignoring errors.
 *
 * This function attempts to change the process's working directory to the given
 * path. If the operation fails, it is ignored (best effort). Useful for
 * restoring a previous working directory when we don't care about success.
 *
 * @param[in] path The directory to change to.
 */
static void chdir2prev(const char *path) {
    if (chdir(path) == -1) {
        /* ignore – best effort */
    }
}

/**
 * @brief Run a shell command and capture its standard output.
 *
 * @param[in] cmd The shell command to execute.
 * @param[out] out Buffer to store the command's stdout (may be NULL).
 * @param[in] sz Size of the output buffer (ignored if out is NULL).
 * @return Exit status of the command, or -1 if popen() fails.
 */
static int run_cmd(const char *cmd, //
                   char       *out,
                   size_t      sz) {
    char old_cwd[4096];
    int  have_old = 0;

    if (getcwd(old_cwd, sizeof(old_cwd)) != NULL) {
        have_old = 1;
    }
    chdir2root(); /* change to root for security */

    FILE *fp = popen(cmd, "r");
    if (!fp) {
        if (out)
            out[0] = '\0';
        if (have_old) {
            chdir2prev(old_cwd);
        }
        return -1;
    }

    if (out) {
        out[0] = '\0';
        char   buf[MAX_LINE_LEN];
        size_t len = 0;

        while (fgets(buf, sizeof(buf), fp) && len < sz - 1) {
            len += snprintf(out + len, sz - len, "%s", buf);
        }
    }

    int status = pclose(fp);
    if (have_old) {
        chdir2prev(old_cwd); /* restore original working directory */
    } else {
        chdir2root(); /* keep root if we couldn't get old cwd */
    }

    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }

    return -1;
}

/**
 * @brief Prints a styled hint at position (x,y) that may adjust the cursor
 * position based on the text length. The hint is displayed with either normal
 * or dimmed styling depending on the 'active' flag.
 *
 * @param[in] x Pointer to integer containing current X position.
 * @param[in] y Y coordinate for the print position (used by mvprintw).
 * @param[in] text Message to display
 * @param[in] active Boolean flag that enables or disables normal styling
 * movement.
 */
static void print_hint(int        *x, //
                       const int   y,
                       const char *text,
                       bool        active) {
    if (active) {
        attron(COLOR_PAIR(CP_ACCENT));
    } else {
        attron(COLOR_PAIR(CP_ACCENT) | A_DIM);
    }

    mvprintw(y, *x, "%s", text);
    attroff(COLOR_PAIR(CP_ACCENT) | A_DIM);

    *x += (int)utf8_strlen(text) + 2;
}

// -----------------------------------------------------------------------------
// Parsing: `ollama list` and `ollama ps` (plain text) into local buffers
// -----------------------------------------------------------------------------

/**
 * @brief Parse the output of `ollama list` into a local Model array.
 *
 * @param[in] out The raw text output from `ollama list`.
 * @param[out] dest Destination Model array.
 * @param[out] cnt Number of models parsed (set by the function).
 */
static void parse_list_into(const char *out, //
                            Model      *dest,
                            int        *cnt) {
    char work[MAX_CMD_OUT];
    snprintf(work, sizeof(work), "%s", out);

    char *line = strtok(work, "\n\r");
    int   ln   = 0;
    int   c    = 0;

    while (line && c < MAX_MODELS) {
        if (ln++ == 0) {
            line = strtok(NULL, "\n\r");
            continue; // Skip header row
        }
        if (line[0] == '\0') {
            line = strtok(NULL, "\n\r");
            continue;
        }

        char *tok[20];
        int   n = 0;
        char *save;
        char *p = strtok_r(line, " \t\r", &save);
        while (p && n < 20) {
            tok[n++] = p;
            p        = strtok_r(NULL, " \t\r", &save);
        }

        /* Ollama list v0.23.0 layout:
           [0]NAME [1]ID [2]SIZE_VAL [3]SIZE_UNIT [4+]MODIFIED_DATE
        */
        if (n < 4) {
            line = strtok(NULL, "\n\r");
            continue;
        }

        // 1. NAME
        snprintf(dest[c].name, MAX_NAME_LEN, "%s", tok[0]);

        // 2. ID
        snprintf(dest[c].id, MAX_ID_LEN, "%s", tok[1]);

        // 3. SIZE (always 2 tokens, e.g., "4.5 GB")
        snprintf(dest[c].size, MAX_SIZE_LEN, "%s %s", tok[2], tok[3]);

        // 4. MODIFIED DATE (all remaining tokens from index 4)
        char date_buf[MAX_DATE_LEN] = "";
        int  off                    = 0;
        for (int i = 4; i < n; i++) {
            if (i > 4) {
                off += snprintf(date_buf + off, MAX_DATE_LEN - off, " ");
            }
            off += snprintf(date_buf + off, MAX_DATE_LEN - off, "%s", tok[i]);
        }
        snprintf(dest[c].date, MAX_DATE_LEN, "%s", date_buf);

        c++;
        line = strtok(NULL, "\n\r");
    }
    *cnt = c;
}

/**
 * @brief Parse the output of `ollama ps` into a local Running array.
 *
 * @param[in] out The raw text output from `ollama ps`.
 * @param[out] dest Destination Running array.
 * @param[out] cnt Number of running models parsed (set by the function).
 */
static void parse_ps_into(const char *out, //
                          Running    *dest,
                          int        *cnt) {
    char work[MAX_CMD_OUT];
    snprintf(work, sizeof(work), "%s", out);

    char *line = strtok(work, "\n\r");
    int   ln   = 0;
    int   c    = 0;

    while (line && c < MAX_MODELS) {
        if (ln++ == 0) {
            line = strtok(NULL, "\n\r");
            continue; // Skip header row
        }
        if (line[0] == '\0') {
            line = strtok(NULL, "\n\r");
            continue;
        }

        char *tok[30];
        int   n = 0;
        char *save;
        char *p = strtok_r(line, " \t\r", &save);

        while (p && n < 30) {
            tok[n++] = p;
            p        = strtok_r(NULL, " \t\r", &save);
        }

        /* Ollama ps v0.23.0 layout:
           [0]NAME [1]ID [2]SIZE_VAL [3]SIZE_UNIT [4]LOAD(%) [5]PROC_TYPE [6]CONTEXT [7+]EXPIRES
        */
        if (n < 7) {
            line = strtok(NULL, "\n\r");
            continue;
        }

        // 1. NAME
        snprintf(dest[c].name, MAX_NAME_LEN, "%s", tok[0]);

        // 2. ID
        snprintf(dest[c].id, MAX_ID_LEN, "%s", tok[1]);

        // 3. SIZE (always 2 tokens)
        snprintf(dest[c].size, MAX_SIZE_LEN, "%s %s", tok[2], tok[3]);

        // 4. PROCESSOR (load % + type, e.g., "100% CPU")
        snprintf(dest[c].proc, MAX_PROC_LEN, "%s %s", tok[4], tok[5]);

        // 5. CONTEXT (numeric token)
        snprintf(dest[c].context, MAX_CONTEXT_LEN, "%s", tok[6]);

        // 6. EXPIRES (all remaining tokens)
        char exp_buf[MAX_DATE_LEN] = "";
        int  off                   = 0;
        for (int i = 7; i < n; i++) {
            if (i > 7) {
                off += snprintf(exp_buf + off, MAX_DATE_LEN - off, " ");
            }
            off += snprintf(exp_buf + off, MAX_DATE_LEN - off, "%s", tok[i]);
        }
        snprintf(dest[c].expires, MAX_DATE_LEN, "%s", exp_buf);

        c++;
        line = strtok(NULL, "\n\r");
    }
    *cnt = c;
}

// -----------------------------------------------------------------------------
// Dynamic Column Width Calculation (local buffers)
// -----------------------------------------------------------------------------

/**
 * @brief Compute optimal column widths from local arrays.
 *
 * @param[in] models      Array of installed models.
 * @param[in] model_cnt   Number of installed models.
 * @param[in] running     Array of running models.
 * @param[in] running_cnt Number of running models.
 * @param[out] col_name   Installed tab: width for model name.
 * @param[out] col_id     Installed tab: width for ID.
 * @param[out] col_size   Installed tab: width for size.
 * @param[out] col_date   Installed tab: width for date.
 * @param[out] rcol_name  Running tab: width for model name.
 * @param[out] rcol_id    Running tab: width for ID.
 * @param[out] rcol_size  Running tab: width for size.
 * @param[out] rcol_proc  Running tab: width for processor.
 * @param[out] rcol_ctx   Running tab: width for context.
 * @param[out] rcol_exp   Running tab: width for expires.
 */
static void compute_widths_from(const Model   *models, //
                                int            model_cnt,
                                const Running *running,
                                int            running_cnt,
                                int           *col_name,
                                int           *col_id,
                                int           *col_size,
                                int           *col_date,
                                int           *rcol_name,
                                int           *rcol_id,
                                int           *rcol_size,
                                int           *rcol_proc,
                                int           *rcol_ctx,
                                int           *rcol_exp) {
    // clang-format off
    int w_name = strlen("MODEL NAME");
    int w_id   = strlen("ID"        );
    int w_size = strlen("SIZE"      );
    int w_date = strlen("MODIFIED"  );

    for (int i = 0, l; i < model_cnt; i++) {
        l = strlen(models[i].name); if (l > w_name) { w_name = l; }
        l = strlen(models[i].id  ); if (l > w_id  ) { w_id   = l; }
        l = strlen(models[i].size); if (l > w_size) { w_size = l; }
        l = strlen(models[i].date); if (l > w_date) { w_date = l; }
    }
    w_name += 2; /* for the "* " prefix on running models */

    if (w_name > MAX_COL_WIDTH) { w_name = MAX_COL_WIDTH; }
    if (w_id   > MAX_COL_WIDTH) { w_id   = MAX_COL_WIDTH; }
    if (w_size > MAX_COL_WIDTH) { w_size = MAX_COL_WIDTH; }
    if (w_date > MAX_COL_WIDTH) { w_date = MAX_COL_WIDTH; }

    *col_name = w_name + MIN_COL_PAD;
    *col_id   = w_id   + MIN_COL_PAD;
    *col_size = w_size + MIN_COL_PAD;
    *col_date = w_date + MIN_COL_PAD;

    /* Running tab columns */
    w_name     = strlen("MODEL NAME");
    w_id       = strlen("ID"        );
    w_size     = strlen("SIZE"      );
    int w_proc = strlen("PROCESSOR" );
    int w_ctx  = strlen("CONTEXT"   );
    int w_exp  = strlen("EXPIRES"   );

    for (int i = 0, l; i < running_cnt; i++) {
        l = strlen(running[i].name   ); if (l > w_name) { w_name = l; }
        l = strlen(running[i].id     ); if (l > w_id  ) { w_id   = l; }
        l = strlen(running[i].size   ); if (l > w_size) { w_size = l; }
        l = strlen(running[i].proc   ); if (l > w_proc) { w_proc = l; }
        l = strlen(running[i].context); if (l > w_ctx ) { w_ctx  = l; }
        l = strlen(running[i].expires); if (l > w_exp ) { w_exp  = l; }
    }

    if (w_name > MAX_COL_WIDTH) { w_name = MAX_COL_WIDTH; }
    if (w_id   > MAX_COL_WIDTH) { w_id   = MAX_COL_WIDTH; }
    if (w_size > MAX_COL_WIDTH) { w_size = MAX_COL_WIDTH; }
    if (w_proc > MAX_COL_WIDTH) { w_proc = MAX_COL_WIDTH; }
    if (w_ctx  > MAX_COL_WIDTH) { w_ctx  = MAX_COL_WIDTH; }
    if (w_exp  > MAX_COL_WIDTH) { w_exp  = MAX_COL_WIDTH; }

    *rcol_name = w_name + MIN_COL_PAD;
    *rcol_id   = w_id   + MIN_COL_PAD;
    *rcol_size = w_size + MIN_COL_PAD;
    *rcol_proc = w_proc + MIN_COL_PAD;
    *rcol_ctx  = w_ctx  + MIN_COL_PAD;
    *rcol_exp  = w_exp  + MIN_COL_PAD;
    // clang-format on
}

// -----------------------------------------------------------------------------
// Model Operations (Delete, Stop, Show Info)
// -----------------------------------------------------------------------------

/**
 * @brief Remove a model from the Ollama server.
 *
 * @param[in] name The name of the model to remove.
 */
static void remove_model(const char *name) {
    char cmd[MAX_LINE_LEN];
    char out[MAX_LOG_LEN - 32];

    snprintf(cmd, sizeof(cmd), "ollama rm %s 2>&1", name);

    int ret = run_cmd(cmd, out, sizeof(out));
    if (ret == 0) {
        snprintf(st.logmsg, sizeof(st.logmsg), "Removed model: %s", name);
        snprintf(st.status, sizeof(st.status), "Model removed");
    } else {
        size_t len = strlen(out);
        if (len > 0 && out[len - 1] == '\n') {
            out[len - 1] = '\0';
        }
        snprintf(st.logmsg, sizeof(st.logmsg), "Failed to remove %s: %s", name, out);
        snprintf(st.status, sizeof(st.status), "Delete failed");
    }
}

/**
 * @brief Stop a running model.
 *
 * @param[in] name The name of the running model to stop.
 */
static void stop_model(const char *name) {
    char cmd[MAX_LINE_LEN];
    char out[MAX_LOG_LEN - 32];

    snprintf(cmd, sizeof(cmd), "ollama stop %s 2>&1", name);

    int ret = run_cmd(cmd, out, sizeof(out));
    if (ret == 0) {
        snprintf(st.logmsg, sizeof(st.logmsg), "Stopped model: %s", name);
        snprintf(st.status, sizeof(st.status), "Model stopped");
    } else {
        size_t len = strlen(out);
        if (len > 0 && out[len - 1] == '\n') {
            out[len - 1] = '\0';
        }
        snprintf(st.logmsg, sizeof(st.logmsg), "Failed to stop %s: %s", name, out);
        snprintf(st.status, sizeof(st.status), "Stop failed");
    }
}

/**
 * @brief Retrieve detailed information about a model and show the info dialog.
 *
 * @param[in] name The name of the model to inspect.
 */
static void show_info(const char *name) {
    char cmd[MAX_LINE_LEN];

    snprintf(cmd, sizeof(cmd), "ollama show %s 2>&1", name);

    run_cmd(cmd, st.info_out, sizeof(st.info_out));
    st.show_info = 1;
}

// -----------------------------------------------------------------------------
// Data Refresh (no ncurses calls – thread safe with brief lock)
// -----------------------------------------------------------------------------

/**
 * @brief Refresh model and running data by invoking ollama commands.
 *
 * This function fetches fresh data into local buffers, then locks the mutex
 * only to memcpy the results into the global state. This minimizes UI stutter.
 */
static void refresh_data(void) {
    char    out[MAX_CMD_OUT];
    Model   local_models[MAX_MODELS];
    Running local_running[MAX_MODELS];
    int     local_model_cnt   = 0;
    int     local_running_cnt = 0;
    int     local_col_name;
    int     local_col_id;
    int     local_col_size;
    int     local_col_date;
    int     local_rcol_name;
    int     local_rcol_id;
    int     local_rcol_size;
    int     local_rcol_proc;
    int     local_rcol_ctx;
    int     local_rcol_exp;

    run_cmd("ollama list 2>/dev/null", out, sizeof(out));
    parse_list_into(out, local_models, &local_model_cnt);

    run_cmd("ollama ps 2>/dev/null", out, sizeof(out));
    parse_ps_into(out, local_running, &local_running_cnt);

    compute_widths_from(local_models, //
                        local_model_cnt,
                        local_running,
                        local_running_cnt,
                        &local_col_name,
                        &local_col_id,
                        &local_col_size,
                        &local_col_date,
                        &local_rcol_name,
                        &local_rcol_id,
                        &local_rcol_size,
                        &local_rcol_proc,
                        &local_rcol_ctx,
                        &local_rcol_exp);

    pthread_mutex_lock(&st.mutex);
    // clang-format off
    memcpy(st.models , local_models , sizeof(Model  ) * local_model_cnt  );
    memcpy(st.running, local_running, sizeof(Running) * local_running_cnt);
    // clang-format on
    st.model_cnt    = local_model_cnt;
    st.running_cnt  = local_running_cnt;
    st.col_name     = local_col_name;
    st.col_id       = local_col_id;
    st.col_size     = local_col_size;
    st.col_date     = local_col_date;
    st.rcol_name    = local_rcol_name;
    st.rcol_id      = local_rcol_id;
    st.rcol_size    = local_rcol_size;
    st.rcol_proc    = local_rcol_proc;
    st.rcol_ctx     = local_rcol_ctx;
    st.rcol_exp     = local_rcol_exp;
    st.need_refresh = 1; /* signal main loop to redraw */
    st.refreshing   = 0; /* refresh thread finished */
    pthread_mutex_unlock(&st.mutex);
}

// -----------------------------------------------------------------------------
// ncurses Initialization & Cleanup
// -----------------------------------------------------------------------------

/**
 * @brief Initialize ncurses environment and color pairs.
 */
static void init_ncurses(void) {
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    timeout(100);
    curs_set(0);

    if (has_colors()) {
        start_color();
        use_default_colors();
        // clang-format off
        init_pair(CP_DEFAULT      , COLOR_WHITE , -1        );
        init_pair(CP_HEADER       , COLOR_WHITE , COLOR_BLUE);
        init_pair(CP_ACCENT       , COLOR_CYAN  , -1        );
        init_pair(CP_SUCCESS      , COLOR_GREEN , -1        );
        init_pair(CP_DANGER       , COLOR_RED   , -1        );
        init_pair(CP_WARNING      , COLOR_YELLOW, -1        );
        init_pair(CP_RUNNING      , COLOR_GREEN , -1        );
        init_pair(CP_SELECTED     , COLOR_BLACK , COLOR_CYAN);
        init_pair(CP_BORDER       , COLOR_BLUE  , -1        );
        init_pair(CP_DIALOG       , COLOR_WHITE , COLOR_BLUE);
        init_pair(CP_DIALOG_BORDER, COLOR_CYAN  , COLOR_BLUE);
        init_pair(CP_INFO_TEXT    , COLOR_YELLOW, COLOR_BLUE);
        // clang-format on
    }
    getmaxyx(stdscr, rows, cols);
}

/**
 * @brief Clean up ncurses and restore terminal state.
 */
static inline void cleanup(void) {
    endwin();
}

// -----------------------------------------------------------------------------
// UI Drawing – Box, Header, Footer, Tabs, Lists, Log, Dialogs
// -----------------------------------------------------------------------------

/**
 * @brief Draw a dialog box with background and border.
 *
 * @param[in] w Dialog width.
 * @param[in] h Dialog height.
 * @param[in] sy Starting Y position (row).
 * @param[in] sx Starting X position (column).
 * @param[in] title Null-terminated title string.
 */
static void draw_dialog_box(int         w, //
                            int         h,
                            int         sy,
                            int         sx,
                            const char *title) {
    // Background fill (solid block)
    attron(COLOR_PAIR(CP_DIALOG));
    for (int i = 0; i < h; i++) {
        mvhline(sy + i, sx, ' ', w);
    }
    attroff(COLOR_PAIR(CP_DIALOG));

    // Border
    attron(COLOR_PAIR(CP_DIALOG_BORDER));
    // clang-format off
    mvhline(sy        , sx        , ACS_HLINE, w);
    mvhline(sy + h - 1, sx        , ACS_HLINE, w);
    mvvline(sy        , sx        , ACS_VLINE, h);
    mvvline(sy        , sx + w - 1, ACS_VLINE, h);

    mvaddch(sy        , sx        , ACS_ULCORNER);
    mvaddch(sy        , sx + w - 1, ACS_URCORNER);
    mvaddch(sy + h - 1, sx        , ACS_LLCORNER);
    mvaddch(sy + h - 1, sx + w - 1, ACS_LRCORNER);
    // clang-format on
    attroff(COLOR_PAIR(CP_DIALOG_BORDER));

    // Title
    if (title != NULL && strlen(title) > 0) {
        int title_len = (int)strlen(title);
        attron(COLOR_PAIR(CP_HEADER) | A_BOLD);
        mvprintw(sy, sx + (w - title_len - 2) / 2, " %s ", title);
        attroff(A_BOLD | COLOR_PAIR(CP_HEADER));
    }
}

/**
 * @brief Draw the application header with title and status.
 */
static void draw_header(void) {
    attron(COLOR_PAIR(CP_HEADER));
    {
        attron(A_BOLD);
        // Clear row 0 and row 3 with spaces
        mvhline(0, 0, ' ', cols);
        mvhline(3, 0, ' ', cols);
        mvprintw(1, 2, " OLLAMA MODEL MANAGER ");
        attroff(A_BOLD);

        if (strlen(st.status)) {
            attron(COLOR_PAIR(CP_SUCCESS));
            mvprintw(1, cols - strlen(st.status) - 3, " %s ", st.status);
            attroff(COLOR_PAIR(CP_SUCCESS));
        }
    }
    attroff(COLOR_PAIR(CP_HEADER));

    attron(COLOR_PAIR(CP_BORDER));
    {
        mvhline(3, 0, ACS_HLINE, cols);
        mvaddch(3, 0, ACS_LTEE);
        mvaddch(3, cols - 1, ACS_RTEE);
    }
    attroff(COLOR_PAIR(CP_BORDER));
}

/**
 * @brief Draw the footer with navigation and command hints.
 */
static void draw_footer(void) {
    int x = 2;
    int y = rows - 5;

    attron(COLOR_PAIR(CP_BORDER));
    mvhline(y, 0, ACS_HLINE, cols);
    mvaddch(y, 0, ACS_LTEE);
    mvaddch(y, cols - 1, ACS_RTEE);
    attroff(COLOR_PAIR(CP_BORDER));

    y += 1;
    // Always available
    print_hint(&x, y, "[▲/▼] Nav", true);
    print_hint(&x, y, "[◀/▶] Tabs", true);
    // Tab 0 only (and requires installed models)
    print_hint(&x, y, "[I] Info", st.tab == 0 && st.model_cnt);
    print_hint(&x, y, "[D] Delete", st.tab == 0 && st.model_cnt);
    // Tab 1 only (and requires running models)
    print_hint(&x, y, "[S] Stop", st.tab == 1 && st.running_cnt);
    // Always available
    print_hint(&x, y, "[P] Pull", true);
    print_hint(&x, y, "[R] Refresh", true);
    print_hint(&x, y, "[/] Search", true);
    print_hint(&x, y, "[Q] Quit", true);

    if (st.pulling) {
        attron(COLOR_PAIR(CP_WARNING));
        mvprintw(y, cols - 22, " Pull in progress... ");
        attroff(COLOR_PAIR(CP_WARNING));
    }
}

/**
 * @brief Draw the tab bar (INSTALLED MODELS / RUNNING MODELS).
 */
static void draw_tabs(void) {
    int y = 5;

    if (st.tab == 0) {
        attron(COLOR_PAIR(CP_SELECTED) | A_BOLD);
    } else {
        attron(COLOR_PAIR(CP_ACCENT));
    }

    mvprintw(y, 2, " INSTALLED MODELS ");
    attroff(A_BOLD | COLOR_PAIR(CP_SELECTED) | COLOR_PAIR(CP_ACCENT));

    if (st.tab == 1) {
        attron(COLOR_PAIR(CP_SELECTED) | A_BOLD);
    } else {
        attron(COLOR_PAIR(CP_ACCENT));
    }

    mvprintw(y, 22, " RUNNING MODELS ");
    attroff(A_BOLD | COLOR_PAIR(CP_SELECTED) | COLOR_PAIR(CP_ACCENT));
}

/**
 * @brief Draw the search filter indicator.
 */
static void draw_search(void) {
    const int x = 60;
    const int y = 5;

    attron(COLOR_PAIR(CP_ACCENT));
    mvprintw(y, x, "Search:");
    attroff(COLOR_PAIR(CP_ACCENT));

    attron(COLOR_PAIR(CP_DEFAULT));
    if (strlen(st.filter)) {
        attron(COLOR_PAIR(CP_SUCCESS));
        mvprintw(y, x + 8, "%-30s", st.filter);
        attroff(COLOR_PAIR(CP_SUCCESS));
    } else {
        mvprintw(y, x + 8, "%-30s", "(none)");
    }
    attroff(COLOR_PAIR(CP_DEFAULT));
}

/**
 * @brief Draw the list of installed models with dynamic columns.
 */
static void draw_model_list(void) {
    int yh      = 7;
    int ylist   = yh + 2;
    int maxrows = rows - yh - 11;
    int x_name  = 2;
    int x_id    = x_name + st.col_name;
    int x_size  = x_id + st.col_id;
    int x_date  = x_size + st.col_size;

    for (int i = yh; i <= ylist + maxrows; i++) {
        move(i, 2);
        clrtoeol();
    }

    attron(COLOR_PAIR(CP_ACCENT) | A_BOLD);
    // clang-format off
    mvprintw(yh, x_name, "MODEL NAME");
    mvprintw(yh, x_id  , "ID"        );
    mvprintw(yh, x_size, "SIZE"      );
    mvprintw(yh, x_date, "MODIFIED"  );
    // clang-format on
    attroff(A_BOLD | COLOR_PAIR(CP_ACCENT));

    attron(COLOR_PAIR(CP_BORDER));
    mvhline(yh + 1, 2, ACS_HLINE, cols - 4);
    attroff(COLOR_PAIR(CP_BORDER));

    for (int i = 0; i < maxrows; i++) {
        mvprintw(ylist + i, 2, "%*s", cols - 4, "");
    }

    pthread_mutex_lock(&st.mutex);
    int disp = 0, row = ylist;

    for (int i = 0; i < st.model_cnt && row < rows - 9; i++) {
        int running = 0;

        for (int j = 0; j < st.running_cnt; j++)
            if (strcmp(st.models[i].name, st.running[j].name) == 0) {
                running = 1;
                break;
            }

        if (strlen(st.filter) && !_strcasestr(st.models[i].name, st.filter)) {
            continue;
        }

        if (disp == st.sel_model && st.tab == 0) {
            attron(COLOR_PAIR(CP_SELECTED));
        } else if (running) {
            attron(COLOR_PAIR(CP_RUNNING));
        } else {
            attron(COLOR_PAIR(CP_DEFAULT));
        }

        char disp_name[MAX_NAME_LEN + 3];
        snprintf(disp_name, sizeof(disp_name), "%s%s", running ? "* " : "  ", st.models[i].name);
        // clang-format off
        mvprintw(row, x_name, "%-*.*s", st.col_name - 1, st.col_name - 1, disp_name        );
        mvprintw(row, x_id  , "%-*.*s", st.col_id   - 1, st.col_id   - 1, st.models[i].id  );
        mvprintw(row, x_size, "%-*.*s", st.col_size - 1, st.col_size - 1, st.models[i].size);
        mvprintw(row, x_date, "%-*.*s", st.col_date - 1, st.col_date - 1, st.models[i].date);
        // clang-format on
        attroff(COLOR_PAIR(CP_SELECTED) | COLOR_PAIR(CP_RUNNING) | COLOR_PAIR(CP_DEFAULT));

        row++;
        disp++;
    }

    if (disp == 0) {
        attron(COLOR_PAIR(CP_WARNING));
        if (strlen(st.filter)) {
            mvprintw(ylist, 2, "No models match filter: %s", st.filter);
        } else {
            mvprintw(ylist, 2, "No models found. Press 'P' to pull.");
        }
        attroff(COLOR_PAIR(CP_WARNING));
    }
    pthread_mutex_unlock(&st.mutex);
}

/**
 * @brief Draw the list of running models with dynamic columns.
 */
static void draw_running_list(void) {
    int yh      = 7;
    int ylist   = yh + 2;
    int maxrows = rows - yh - 11;
    int x_name  = 2;
    int x_id    = x_name + st.rcol_name;
    int x_size  = x_id + st.rcol_id;
    int x_proc  = x_size + st.rcol_size;
    int x_ctx   = x_proc + st.rcol_proc;
    int x_exp   = x_ctx + st.rcol_ctx;

    for (int i = yh; i <= ylist + maxrows; i++) {
        move(i, 2);
        clrtoeol();
    }

    attron(COLOR_PAIR(CP_ACCENT) | A_BOLD);
    // clang-format off
    mvprintw(yh, x_name, "MODEL NAME");
    mvprintw(yh, x_id  , "ID"        );
    mvprintw(yh, x_size, "SIZE"      );
    mvprintw(yh, x_proc, "PROCESSOR" );
    mvprintw(yh, x_ctx , "CONTEXT"   );
    mvprintw(yh, x_exp , "EXPIRES"   );
    // clang-format on
    attroff(A_BOLD | COLOR_PAIR(CP_ACCENT));

    attron(COLOR_PAIR(CP_BORDER));
    mvhline(yh + 1, 2, ACS_HLINE, cols - 4);
    attroff(COLOR_PAIR(CP_BORDER));

    for (int i = 0; i < maxrows; i++) mvprintw(ylist + i, 2, "%*s", cols - 4, "");

    pthread_mutex_lock(&st.mutex);
    int row = ylist;

    for (int i = 0; i < st.running_cnt && row < rows - 9; i++) {
        if (i == st.sel_running && st.tab == 1) {
            attron(COLOR_PAIR(CP_SELECTED));
        } else {
            attron(COLOR_PAIR(CP_DEFAULT));
        }
        // clang-format off
        mvprintw(row, x_name, "%-*.*s", st.rcol_name - 1, st.rcol_name - 1, st.running[i].name   );
        mvprintw(row, x_id  , "%-*.*s", st.rcol_id   - 1, st.rcol_id   - 1, st.running[i].id     );
        mvprintw(row, x_size, "%-*.*s", st.rcol_size - 1, st.rcol_size - 1, st.running[i].size   );
        mvprintw(row, x_proc, "%-*.*s", st.rcol_proc - 1, st.rcol_proc - 1, st.running[i].proc   );
        mvprintw(row, x_ctx , "%-*.*s", st.rcol_ctx  - 1, st.rcol_ctx  - 1, st.running[i].context);
        mvprintw(row, x_exp , "%-*.*s", st.rcol_exp  - 1, st.rcol_exp  - 1, st.running[i].expires);
        // clang-format on
        attroff(COLOR_PAIR(CP_SELECTED) | COLOR_PAIR(CP_DEFAULT));
        row++;
    }
    if (st.running_cnt == 0) {
        attron(COLOR_PAIR(CP_WARNING));
        mvprintw(ylist, 2, "No running models.");
        attroff(COLOR_PAIR(CP_WARNING));
    }
    pthread_mutex_unlock(&st.mutex);
}

/**
 * @brief Draw the log area at the bottom of the screen.
 */
static void draw_log(void) {
    int y = rows - 3;

    attron(COLOR_PAIR(CP_BORDER));
    mvhline(y, 0, ACS_HLINE, cols);
    mvaddch(y, 0, ACS_LTEE);
    mvaddch(y, cols - 1, ACS_RTEE);
    attroff(COLOR_PAIR(CP_BORDER));

    attron(COLOR_PAIR(CP_ACCENT));
    mvprintw(y + 1, 2, "LOG:");
    attroff(COLOR_PAIR(CP_ACCENT));

    attron(COLOR_PAIR(CP_DEFAULT));
    mvprintw(y + 1, 7, "%-*s", cols - 10, st.logmsg);
    attroff(COLOR_PAIR(CP_DEFAULT));
}

/**
 * @brief Draw the main content area (tabs, search, and current list).
 */
static void draw_content(void) {
    draw_tabs();
    draw_search();

    if (st.tab == 0) {
        draw_model_list();
    } else {
        draw_running_list();
    }
}

/**
 * @brief Draw the model information dialog.
 *
 * This function now sanitizes each line to replace fullwidth characters
 * (e.g., U+FF5C '｜') with their ASCII equivalents.
 */
static void draw_info_dialog(void) {
    int w = cols - 4;
    int h = rows - 10;

    // clang-format off
    if (w > 90) { w = 90; }
    if (w < 40) { w = 40; }
    if (h > 30) { h = 30; }
    if (h < 10) { h = 10; }
    // clang-format on

    int sy = (rows - h) / 2;
    int sx = (cols - w) / 2;

    draw_dialog_box(w, h, sy, sx, "MODEL INFORMATION");

    attron(COLOR_PAIR(CP_INFO_TEXT) | A_BOLD);
    int   row  = sy + 2;
    char *line = strtok(st.info_out, "\n");
    while (line && row < sy + h - 2) {
        char disp[w - 4];
        char sanitized[w - 4];
        snprintf(disp, sizeof(disp), "%s", line);
        sanitize_fullwidth(sanitized, disp, sizeof(sanitized));
        mvprintw(row, sx + 2, "%s", sanitized);
        row++;
        line = strtok(NULL, "\n");
    }
    attroff(COLOR_PAIR(CP_INFO_TEXT) | A_BOLD);

    attron(COLOR_PAIR(CP_ACCENT));
    mvprintw(sy + h - 2, sx + (w - 26) / 2, " Press any key to close ");
    attroff(COLOR_PAIR(CP_ACCENT));
}

/**
 * @brief Draw a generic text-input dialog with a title and a footer hint.
 *
 * Renders the shared chrome (box, "Model:" label, input field, block cursor)
 * used by both the pull and search dialogs.  Only the title centred on row 1
 * and the hint centred on row 4 differ between the two callers.
 *
 * @param[in] title Null-terminated title string.
 * @param[in] hint  Null-terminated footer hint string.
 */
static void draw_input_dialog(const char *title, //
                              const char *hint) {
    int w  = 64;
    int h  = 6;
    int sy = (rows - h) / 2;
    int sx = (cols - w) / 2;

    draw_dialog_box(w, h, sy, sx, title);

    attron(COLOR_PAIR(CP_DIALOG));
    mvprintw(sy + 2, sx + 3, "Model:");
    attroff(COLOR_PAIR(CP_DIALOG));

    int field_xpad  = 10;
    int field_start = sx + field_xpad;
    int field_width = w - (2 * field_xpad);

    /* Clear input field and print current buffer */
    attron(COLOR_PAIR(CP_ACCENT));
    for (int i = 0; i < field_width; i++) {
        mvaddch(sy + 2, field_start + i, ' ');
    }
    mvprintw(sy + 2, field_start, "%-*.*s", field_width - 1, field_width - 1, st.dialog_input);
    attroff(COLOR_PAIR(CP_ACCENT));

    /* Draw block cursor at dialog_cursor */
    int cursor_pos = st.dialog_cursor;
    if (cursor_pos >= field_width - 1) {
        cursor_pos = field_width - 1;
    }
    attron(A_REVERSE);
    if (cursor_pos < (int)strlen(st.dialog_input)) {
        mvaddch(sy + 2, field_start + cursor_pos, st.dialog_input[cursor_pos]);
    } else {
        mvaddch(sy + 2, field_start + cursor_pos, ' ');
    }
    attroff(A_REVERSE);

    attron(COLOR_PAIR(CP_ACCENT));
    int hint_len = (int)strlen(hint);
    mvprintw(sy + 4, sx + (w - hint_len - 2) / 2, " %s ", hint);
    attroff(COLOR_PAIR(CP_ACCENT));
}

/**
 * @brief Draw the pull model dialog with text input.
 */
static void draw_pull_dialog(void) {
    draw_input_dialog("PULL MODEL", "Press ENTER to pull, ESC to cancel");
}

/**
 * @brief Draw the search dialog with text input.
 */
static void draw_search_dialog(void) {
    draw_input_dialog("SEARCH MODEL", "Press ENTER to search, ESC to cancel");
}

// -----------------------------------------------------------------------------
// Confirmation Dialog (OK / Cancel)
// -----------------------------------------------------------------------------

/**
 * @brief Draw a confirmation dialog with OK and Cancel buttons.
 */
static void draw_confirm_dialog(void) {
    int msg_len  = strlen(st.confirm_msg);
    int msg_xpad = 2;

    int w  = msg_xpad + (1 + msg_len + 1) + msg_xpad;
    int h  = 6;
    int sy = (rows - h) / 2;
    int sx = (cols - w) / 2;

    draw_dialog_box(w, h, sy, sx, "WARNING");

    int msg_x = sx + msg_xpad;

    attron(COLOR_PAIR(CP_DEFAULT));
    mvprintw(sy + 2, msg_x, " %s ", st.confirm_msg);
    attroff(COLOR_PAIR(CP_DEFAULT));

    int btn_ok_x     = sx + (w / 2) - 12;
    int btn_cancel_x = sx + (w / 2) + 2;

    attron(COLOR_PAIR(CP_ACCENT) | A_BOLD);
    if (st.confirm_choice == 1) {
        attron(A_REVERSE);
        mvprintw(sy + 4, btn_ok_x, "[   OK   ]");
        attroff(A_REVERSE);
        mvprintw(sy + 4, btn_cancel_x, "[ Cancel ]");
    } else {
        mvprintw(sy + 4, btn_ok_x, "[   OK   ]");
        attron(A_REVERSE);
        mvprintw(sy + 4, btn_cancel_x, "[ Cancel ]");
        attroff(A_REVERSE);
    }
    attroff(COLOR_PAIR(CP_ACCENT) | A_BOLD);
}

// -----------------------------------------------------------------------------
// Full Refresh and Logging
// -----------------------------------------------------------------------------

/**
 * @brief Perform a full screen refresh (redraw everything).
 *
 * Uses werase() to clear the logical screen without forcing a physical clear,
 * then batches all updates with wnoutrefresh() and doupdate() to eliminate
 * flicker.
 */
static void full_refresh(void) {
    werase(stdscr); // clear logical screen only
    draw_header();
    draw_content();
    draw_log();
    draw_footer();
    wnoutrefresh(stdscr); // queue updates
    doupdate();           // apply all updates at once
}

/**
 * @brief Update the log message area with a formatted message.
 *
 * @param[in] fmt printf-style format string.
 * @param[in] ... Variable arguments for the format string.
 */
static void log_msg(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(st.logmsg, sizeof(st.logmsg), fmt, ap);
    va_end(ap);

    draw_log();
    refresh(); // immediate refresh for log messages
}

// -----------------------------------------------------------------------------
// Background Refresh Thread
// -----------------------------------------------------------------------------

/**
 * @brief Background thread function to refresh data without blocking the UI.
 *
 * @param[in] arg Unused thread argument.
 * @return NULL.
 */
static void *refresh_thread(void *arg) {
    (void)arg;      /* unused */
    refresh_data(); /* this sets st.need_refresh and clears st.refreshing */
    return NULL;
}

// -----------------------------------------------------------------------------
// Helper Functions for Main Event Loop
// -----------------------------------------------------------------------------

/**
 * @brief Execute the ollama pull command for a model.
 *
 * @param[in] model_name The name of the model to pull.
 */
static void execute_pull_model(const char *model_name) {
    char cmd[MAX_LINE_LEN];
    snprintf(cmd, sizeof(cmd), "ollama pull %s", model_name);

    char old_cwd[4096];
    int  have_old = 0;

    if (getcwd(old_cwd, sizeof(old_cwd)) != NULL) {
        have_old = 1;
    }
    chdir2root();

    printf("\n");
    printf("┌─────────────────────────────────────────────────────────────────────┐\n");
    printf("│                            PULLING MODEL                            │\n");
    printf("└─────────────────────────────────────────────────────────────────────┘\n");
    printf("\n");
    printf("Model: %s\n", model_name);
    printf("───────────────────────────────────────────────────────────────────────\n");
    fflush(stdout);

    int ret = system(cmd);
    if (ret == 0)
        printf("[SUCCESS] Model '%s' pulled successfully.\n", model_name);
    else
        printf("[ERROR] Failed to pull model '%s'\n", model_name);

    printf("\nPress ENTER to continue...");
    fflush(stdout);
    getchar();

    if (have_old) {
        chdir2prev(old_cwd);
    } else {
        chdir2root();
    }

    reset_prog_mode();
    clearok(stdscr, TRUE);
    refresh_data();
    st.pulling = 0;
    memset(st.dialog_input, 0, sizeof(st.dialog_input));
    st.dialog_cursor = 0;
    st.dialog_insert = 1;
    snprintf(st.status, sizeof(st.status), "Pull complete");
    snprintf(st.logmsg, sizeof(st.logmsg), "Pulled model: %s", model_name);
    full_refresh();
}

/**
 * @brief Shared key-handler core for text-input dialogs.
 *
 * Processes one keystroke for any dialog that uses @c st.dialog_input. The
 * caller supplies two callbacks:
 *  - @p draw_fn redraws the dialog (called after BACKSPACE / printable key).
 *  - @p enter_fn called when ENTER is pressed with a non-empty input buffer;
 *    responsible for consuming the input and closing the dialog. ESC always
 *    clears the buffer and triggers a full_refresh().
 *
 * @param[in] active_flag Pointer to the st flag that keeps this dialog open
 * (set to 0 on ESC so the caller's loop exits cleanly).
 * @param[in] draw_fn Function that redraws the dialog.
 * @param[in] enter_fn Function that handles a confirmed (ENTER) submission.
 */
static void handle_input_dialog_keys(int *active_flag, //
                                     void (*draw_fn)(void),
                                     void (*enter_fn)(void)) {
    int ch = getch();
    if (ch == 27) { // ESCAPE
        *active_flag = 0;
        memset(st.dialog_input, 0, sizeof(st.dialog_input));
        st.dialog_cursor = 0;
        st.dialog_insert = 1;
        full_refresh();
        return;
    }
    if (ch == 10 || ch == 13) { // ENTER
        enter_fn();
        return;
    }

    int len = (int)strlen(st.dialog_input);

    switch (ch) {
        case KEY_LEFT:
            if (st.dialog_cursor > 0) {
                st.dialog_cursor--;
                draw_fn();
                refresh();
            }
            break;

        case KEY_RIGHT:
            if (st.dialog_cursor < len) {
                st.dialog_cursor++;
                draw_fn();
                refresh();
            }
            break;

        case KEY_HOME:
            st.dialog_cursor = 0;
            draw_fn();
            refresh();
            break;

        case KEY_END:
            st.dialog_cursor = len;
            draw_fn();
            refresh();
            break;

        case KEY_DC: // Delete key (forward delete)
            if (st.dialog_cursor < len) {
                for (int i = st.dialog_cursor; i < len; i++) {
                    st.dialog_input[i] = st.dialog_input[i + 1];
                }
                draw_fn();
                refresh();
            }
            break;

        case 127:
        case KEY_BACKSPACE:
        case 8: // Backspace
            if (st.dialog_cursor > 0) {
                for (int i = st.dialog_cursor; i <= len; i++) {
                    st.dialog_input[i - 1] = st.dialog_input[i];
                }
                st.dialog_cursor--;
                draw_fn();
                refresh();
            }
            break;

        case KEY_IC: // Insert key – toggle insert/overwrite mode
            st.dialog_insert = !st.dialog_insert;
            draw_fn();
            refresh();
            break;

        default:
            if (ch >= 32 && ch <= 126 && isprint(ch)) {
                if (st.dialog_insert) {
                    // Insert mode: make room for new character
                    if (len < MAX_NAME_LEN - 1) {
                        for (int i = len; i >= st.dialog_cursor; i--) {
                            st.dialog_input[i + 1] = st.dialog_input[i];
                        }
                        st.dialog_input[st.dialog_cursor] = (char)ch;
                        st.dialog_cursor++;
                        draw_fn();
                        refresh();
                    }
                } else {
                    // Overwrite mode
                    if (st.dialog_cursor < len) {
                        st.dialog_input[st.dialog_cursor] = (char)ch;
                        st.dialog_cursor++;
                        draw_fn();
                        refresh();
                    } else if (len < MAX_NAME_LEN - 1) {
                        // At end: append
                        st.dialog_input[len]     = (char)ch;
                        st.dialog_input[len + 1] = '\0';
                        st.dialog_cursor++;
                        draw_fn();
                        refresh();
                    }
                }
            }
            break;
    }
}

/**
 * @brief Handle the Enter key press in the pull dialog.
 */
static void pull_dialog_enter(void) {
    if (strlen(st.dialog_input)) {
        st.pulling     = 1;
        st.show_dialog = 0;
        full_refresh();

        def_prog_mode();
        endwin();
        clear_term();
        execute_pull_model(st.dialog_input);
    }
}

/**
 * @brief Handle keyboard input for the pull dialog.
 */
static void handle_pull_dialog_keys(void) {
    handle_input_dialog_keys(&st.show_dialog, draw_pull_dialog, pull_dialog_enter);
}

/**
 * @brief Handle the Enter key press in the search dialog.
 */
static void search_dialog_enter(void) {
    if (strlen(st.dialog_input)) {
        snprintf(st.filter, MAX_NAME_LEN, "%s", st.dialog_input);
        st.sel_model = 0;
    } else {
        st.filter[0] = '\0';
    }
    st.show_search = 0;
    memset(st.dialog_input, 0, sizeof(st.dialog_input));
    st.dialog_cursor = 0;
    st.dialog_insert = 1;
    full_refresh();
    log_msg("Search filter: %s", strlen(st.filter) ? st.filter : "(cleared)");
}

/**
 * @brief Handle keyboard input for the search dialog.
 */
static void handle_search_dialog_keys(void) {
    handle_input_dialog_keys(&st.show_search, draw_search_dialog, search_dialog_enter);
}

/**
 * @brief Handle keyboard input for the confirmation dialog.
 */
static void handle_confirm_dialog_keys(void) {
    int ch = getch();
    if (ch == 27) { // ESCAPE
        st.confirm_active = 0;
        full_refresh();
        return;
    }
    switch (ch) {
        case KEY_LEFT:
            st.confirm_choice = 1;
            draw_confirm_dialog();
            refresh();
            break;
        case KEY_RIGHT:
            st.confirm_choice = 0;
            draw_confirm_dialog();
            refresh();
            break;
        case '\t':
            st.confirm_choice = !st.confirm_choice;
            draw_confirm_dialog();
            refresh();
            break;
        case 10:
        case 13: // ENTER
            st.confirm_active = 0;
            full_refresh();
            if (st.confirm_choice == 1) {
                if (st.confirm_is_delete) {
                    remove_model(st.confirm_target);
                } else {
                    stop_model(st.confirm_target);
                }
                refresh_data();
                full_refresh();
            }
            break;
        default:
            break;
    }
}

/**
 * @brief Get the name of the currently selected model (filtered).
 *
 * @param[out] out_buf Destination buffer (must have at least MAX_NAME_LEN
 * bytes).
 * @return 1 if a name was copied, 0 if none selected.
 */
static int get_selected_model_name(char *out_buf) {
    int vis   = 0;
    int found = 0;

    pthread_mutex_lock(&st.mutex);
    for (int i = 0; i < st.model_cnt; i++) {
        if (!strlen(st.filter) || _strcasestr(st.models[i].name, st.filter)) {
            if (vis == st.sel_model) {
                snprintf(out_buf, MAX_NAME_LEN, "%s", st.models[i].name);
                found = 1;
                break;
            }
            vis++;
        }
    }
    pthread_mutex_unlock(&st.mutex);
    return found;
}

/**
 * @brief Get the count of visible models (after filtering).
 *
 * @return Number of visible models.
 */
static int get_visible_model_count(void) {
    int vis = 0;

    pthread_mutex_lock(&st.mutex);
    for (int i = 0; i < st.model_cnt; i++)
        if (!strlen(st.filter) || _strcasestr(st.models[i].name, st.filter))
            vis++;
    pthread_mutex_unlock(&st.mutex);
    return vis;
}

/**
 * @brief Handle main UI keyboard input.
 */
static void handle_main_keys(int ch) {
    switch (ch) {
        case 'q':
        case 'Q':
            cleanup();
            clear_term();
            pthread_mutex_destroy(&st.mutex);
            printf("\nGoodbye.\n");
            exit(0);

        case 'r':
        case 'R': {
            pthread_mutex_lock(&st.mutex);
            if (st.refreshing) {
                pthread_mutex_unlock(&st.mutex);
                log_msg("Refresh already in progress...");
                break;
            }
            st.refreshing = 1;
            pthread_mutex_unlock(&st.mutex);

            snprintf(st.status, sizeof(st.status), "Refreshing...");
            draw_header();
            refresh();

            pthread_t tid;
            pthread_create(&tid, NULL, refresh_thread, NULL);
            pthread_detach(tid);
        } break;

        case 'i':
        case 'I':
            if (st.tab == 0 && st.model_cnt) {
                char name_buf[MAX_NAME_LEN];
                if (get_selected_model_name(name_buf)) {
                    show_info(name_buf);
                    draw_info_dialog();
                    refresh();
                    log_msg("Info for %s", name_buf);
                }
            }
            break;

        case KEY_UP:
            if (st.tab == 0 && st.model_cnt) {
                int vis = get_visible_model_count();
                if (vis) {
                    st.sel_model = (st.sel_model - 1 + vis) % vis;
                    draw_model_list();
                    refresh();
                }
            } else if (st.tab == 1 && st.running_cnt) {
                st.sel_running = (st.sel_running - 1 + st.running_cnt) % st.running_cnt;
                draw_running_list();
                refresh();
            }
            break;

        case KEY_DOWN:
            if (st.tab == 0 && st.model_cnt) {
                int vis = get_visible_model_count();
                if (vis) {
                    st.sel_model = (st.sel_model + 1) % vis;
                    draw_model_list();
                    refresh();
                }
            } else if (st.tab == 1 && st.running_cnt) {
                st.sel_running = (st.sel_running + 1) % st.running_cnt;
                draw_running_list();
                refresh();
            }
            break;

        case KEY_LEFT:
            if (st.tab == 1) {
                st.tab       = 0;
                st.sel_model = 0;
                full_refresh();
            }
            break;

        case KEY_RIGHT:
            if (st.tab == 0) {
                st.tab         = 1;
                st.sel_running = 0;
                full_refresh();
            }
            break;

        case 'd':
        case 'D':
            if (st.tab == 0 && st.model_cnt) {
                char name_buf[MAX_NAME_LEN];
                if (get_selected_model_name(name_buf)) {
                    // clang-format off
                    snprintf(st.confirm_msg   , sizeof(st.confirm_msg), "Delete model '%s' ?", name_buf);
                    snprintf(st.confirm_target, MAX_NAME_LEN          , "%s"                 , name_buf);
                    // clang-format on
                    st.confirm_is_delete = 1;
                    st.confirm_choice    = 1;
                    st.confirm_active    = 1;
                    draw_confirm_dialog();
                    refresh();
                }
            }
            break;

        case 's':
        case 'S':
            if (st.tab == 1 && st.running_cnt) {
                /* Copy name under lock */
                char name_buf[MAX_NAME_LEN];

                pthread_mutex_lock(&st.mutex);
                if (st.sel_running < st.running_cnt) {
                    snprintf(name_buf, MAX_NAME_LEN, "%s", st.running[st.sel_running].name);
                } else {
                    name_buf[0] = '\0';
                }
                pthread_mutex_unlock(&st.mutex);

                if (name_buf[0]) {
                    // clang-format off
                    snprintf(st.confirm_msg   , sizeof(st.confirm_msg), "Stop model '%s' ?", name_buf);
                    snprintf(st.confirm_target, MAX_NAME_LEN          , "%s"               , name_buf);
                    // clang-format on
                    st.confirm_is_delete = 0;
                    st.confirm_choice    = 1;
                    st.confirm_active    = 1;
                    draw_confirm_dialog();
                    refresh();
                }
            }
            break;

        case 'p':
        case 'P':
            memset(st.dialog_input, 0, sizeof(st.dialog_input));
            st.dialog_cursor = 0;
            st.dialog_insert = 1;
            st.show_dialog   = 1;
            draw_pull_dialog();
            refresh();
            break;

        case '/':
            memset(st.dialog_input, 0, sizeof(st.dialog_input));
            st.dialog_cursor = 0;
            st.dialog_insert = 1;
            st.show_search   = 1;
            draw_search_dialog();
            refresh();
            break;

        case KEY_RESIZE:
            getmaxyx(stdscr, rows, cols);
            resize_term(rows, cols);
            full_refresh();
            break;

        default:
            break;
    }
}

/**
 * @brief Initialize the application.
 *
 * @return 0 on success, 1 on failure.
 */
static int initialize_app(void) {
    setlocale(LC_ALL, "");

    char test[256];
    run_cmd("ollama --version 2>/dev/null", test, sizeof(test));
    if (!strlen(test)) {
        fprintf(stderr, "Error: Ollama not found in PATH.\n");
        return 1;
    }

    memset(&st, 0, sizeof(st));
    st.dialog_insert = 1; // default to insert mode
    pthread_mutex_init(&st.mutex, NULL);
    init_ncurses();

    refresh_data();
    snprintf(st.status, sizeof(st.status), "Refreshed");
    snprintf(st.logmsg, sizeof(st.logmsg), "Loaded %d model(s), %d running", st.model_cnt, st.running_cnt);
    full_refresh();

    return 0;
}

// -----------------------------------------------------------------------------
// Entry point of the Ollama Model Manager
// -----------------------------------------------------------------------------

int main(void) {
    if (initialize_app() != 0) {
        return 1;
    }

    int ch;

    while (1) {
        pthread_mutex_lock(&st.mutex);
        int need_refresh = st.need_refresh;
        pthread_mutex_unlock(&st.mutex);

        if (need_refresh) {
            pthread_mutex_lock(&st.mutex);
            st.need_refresh = 0;
            pthread_mutex_unlock(&st.mutex);

            snprintf(st.status, sizeof(st.status), "Refreshed");
            snprintf(st.logmsg, sizeof(st.logmsg), "Loaded %d model(s), %d running", st.model_cnt, st.running_cnt);
            full_refresh();
        }

        if (st.show_info) {
            ch = getch();
            if (ch != ERR) {
                st.show_info = 0;
                full_refresh();
            }
            continue;
        }

        if (st.show_dialog) {
            handle_pull_dialog_keys();
            continue;
        }

        if (st.show_search) {
            handle_search_dialog_keys();
            continue;
        }

        if (st.confirm_active) {
            handle_confirm_dialog_keys();
            continue;
        }

        ch = getch();
        if (ch == ERR)
            continue;

        handle_main_keys(ch);
    }

    cleanup();
    pthread_mutex_destroy(&st.mutex);
    return 0;
}

/* *****************************************************************************
 End of File
 */
