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
        Compile: gcc -o ollama_manager ollama_manager.c -O2 -lncurses -lpthread
*/
/* ************************************************************************** */

#include <ctype.h>
#include <ncurses.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* -----------------------------------------------------------------------------
 * Constants
 * -------------------------------------------------------------------------- */
#define MAX_MODELS      256
#define MAX_NAME_LEN    128
#define MAX_ID_LEN      64
#define MAX_SIZE_LEN    32
#define MAX_DATE_LEN    64
#define MAX_PROC_LEN    64
#define MAX_CONTEXT_LEN 16
#define MAX_LINE_LEN    1024
#define MAX_LOG_LEN     256
#define MIN_COL_PAD     2
#define MAX_COL_WIDTH   40

/* -----------------------------------------------------------------------------
 * ncurses Color Pairs
 * -------------------------------------------------------------------------- */
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

/* -----------------------------------------------------------------------------
 * Data Structures
 * -------------------------------------------------------------------------- */
typedef struct {
    char name[MAX_NAME_LEN];
    char id[MAX_ID_LEN];
    char size[MAX_SIZE_LEN];
    char date[MAX_DATE_LEN];
} Model;

typedef struct {
    char name[MAX_NAME_LEN];
    char id[MAX_ID_LEN];
    char size[MAX_SIZE_LEN];
    char proc[MAX_PROC_LEN];
    char context[MAX_CONTEXT_LEN];
    char expires[MAX_DATE_LEN];
} Running;

/* Global application state */
static struct {
    Model           models[MAX_MODELS];
    Running         running[MAX_MODELS];
    char            filter[MAX_NAME_LEN];
    int             sel_model, sel_running;
    int             model_cnt, running_cnt;
    int             tab; /* 0 = installed, 1 = running */
    int             pulling;
    char            status[MAX_LOG_LEN];
    char            logmsg[MAX_LOG_LEN];
    int             need_refresh;
    int             show_dialog;    /* pull dialog active */
    int             show_info;      /* info dialog active */
    int             show_search;    /* search dialog active */
    int             confirm_active; /* confirmation dialog active */
    int             confirm_choice; /* 0 = Cancel, 1 = OK */
    char            confirm_msg[256];
    char            confirm_target[MAX_NAME_LEN]; /* model name for delete/stop */
    int             confirm_is_delete;            /* 1 = delete, 0 = stop */
    char            dialog_input[MAX_NAME_LEN];
    char            info_out[8192];
    pthread_mutex_t mutex;

    /* Dynamic column widths */
    int col_name, col_id, col_size, col_date;
    int rcol_name, rcol_id, rcol_size, rcol_proc, rcol_ctx, rcol_exp;
} st;

static int rows, cols; /* terminal dimensions */

/* -----------------------------------------------------------------------------
 * Helper Functions
 * -------------------------------------------------------------------------- */
/* Run a shell command and capture its stdout (optional). Returns exit status. */
static int run_cmd(const char *cmd, char *out, size_t sz) {
    FILE *fp = popen(cmd, "r");
    if (!fp) {
        if (out)
            out[0] = '\0';
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
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return -1;
}

/* -----------------------------------------------------------------------------
 * Parsing: `ollama list` and `ollama ps` (plain text)
 * -------------------------------------------------------------------------- */
static void parse_list(const char *out) {
    char work[16384];
    snprintf(work, sizeof(work), "%s", out);
    char *line = strtok(work, "\n");
    int   ln = 0, cnt = 0;
    while (line && cnt < MAX_MODELS) {
        if (ln++ == 0) {
            line = strtok(NULL, "\n");
            continue;
        }
        char *tok[20];
        int   n = 0;
        char *save;
        char *p = strtok_r(line, " \t", &save);
        while (p && n < 20) {
            tok[n++] = p;
            p        = strtok_r(NULL, " \t", &save);
        }
        if (n < 3) {
            line = strtok(NULL, "\n");
            continue;
        }

        snprintf(st.models[cnt].name, MAX_NAME_LEN, "%s", tok[0]);
        snprintf(st.models[cnt].id, MAX_ID_LEN, "%s", tok[1]);

        /* Locate the date field (contains "ago", "day", "hour", "minute") */
        int date_start = n;
        for (int i = 2; i < n; i++) {
            if (strstr(tok[i], "ago") ||  //
                strstr(tok[i], "day") ||  //
                strstr(tok[i], "hour") || //
                strstr(tok[i], "minute")) {
                date_start = i;
                break;
            }
        }
        if (date_start > 2 && date_start <= n) {
            /* Size = two tokens before the date */
            char size_buf[MAX_SIZE_LEN] = "";
            snprintf(size_buf, sizeof(size_buf), "%s %s", tok[date_start - 2], tok[date_start - 1]);
            snprintf(st.models[cnt].size, MAX_SIZE_LEN, "%s", size_buf);
            /* Date = remaining tokens */
            char date_buf[MAX_DATE_LEN] = "";
            for (int i = date_start; i < n; i++) {
                if (i > date_start)
                    strcat(date_buf, " ");
                strcat(date_buf, tok[i]);
            }
            snprintf(st.models[cnt].date, MAX_DATE_LEN, "%s", date_buf);
        } else {
            /* Fallback: size = tok[2], date = tok[3..] */
            snprintf(st.models[cnt].size, MAX_SIZE_LEN, "%s", tok[2]);
            char date_buf[MAX_DATE_LEN] = "";
            for (int i = 3; i < n; i++) {
                if (i > 3)
                    strcat(date_buf, " ");
                strcat(date_buf, tok[i]);
            }
            snprintf(st.models[cnt].date, MAX_DATE_LEN, "%s", date_buf);
        }
        cnt++;
        line = strtok(NULL, "\n");
    }
    st.model_cnt = cnt;
}

static void parse_ps(const char *out) {
    char work[16384];
    snprintf(work, sizeof(work), "%s", out);
    char *line = strtok(work, "\n");
    int   ln = 0, cnt = 0;
    while (line && cnt < MAX_MODELS) {
        if (ln++ == 0) {
            line = strtok(NULL, "\n");
            continue;
        }
        char *tok[20];
        int   n = 0;
        char *save;
        char *p = strtok_r(line, " \t", &save);
        while (p && n < 20) {
            tok[n++] = p;
            p        = strtok_r(NULL, " \t", &save);
        }
        if (n < 5) { /* NAME, ID, SIZE, PROCESSOR%, CONTEXT, EXPIRES... */
            line = strtok(NULL, "\n");
            continue;
        }

        snprintf(st.running[cnt].name, MAX_NAME_LEN, "%s", tok[0]);
        snprintf(st.running[cnt].id, MAX_ID_LEN, "%s", tok[1]);

        /* Find the processor field (contains '%') */
        int proc_idx = -1;
        for (int i = 2; i < n; i++) {
            if (strchr(tok[i], '%')) {
                proc_idx = i;
                break;
            }
        }

        if (proc_idx >= 3 && proc_idx + 2 < n) {
            /* Size = two tokens before processor */
            char size_buf[MAX_SIZE_LEN] = "";
            snprintf(size_buf, sizeof(size_buf), "%s %s", tok[proc_idx - 2], tok[proc_idx - 1]);
            snprintf(st.running[cnt].size, MAX_SIZE_LEN, "%s", size_buf);

            /* Processor: percentage + next token if it's CPU/GPU */
            char proc_buf[MAX_PROC_LEN] = "";
            snprintf(proc_buf, sizeof(proc_buf), "%s", tok[proc_idx]);
            if (proc_idx + 1 < n && (strcasecmp(tok[proc_idx + 1], "CPU") == 0 || strcasecmp(tok[proc_idx + 1], "GPU") == 0)) {
                strncat(proc_buf, " ", sizeof(proc_buf) - strlen(proc_buf) - 1);
                strncat(proc_buf, tok[proc_idx + 1], sizeof(proc_buf) - strlen(proc_buf) - 1);
                proc_idx++; /* skip the CPU/GPU token */
            }
            snprintf(st.running[cnt].proc, MAX_PROC_LEN, "%s", proc_buf);

            /* Next token after processor is the CONTEXT (numeric) */
            int ctx_idx = proc_idx + 1;
            if (ctx_idx < n) {
                /* Context is typically a number (e.g., 16384) */
                snprintf(st.running[cnt].context, MAX_CONTEXT_LEN, "%s", tok[ctx_idx]);
            } else {
                snprintf(st.running[cnt].context, MAX_CONTEXT_LEN, "?");
            }

            /* Expires = all remaining tokens after context */
            char exp_buf[MAX_DATE_LEN] = "";
            for (int i = ctx_idx + 1; i < n; i++) {
                if (i > ctx_idx + 1)
                    strcat(exp_buf, " ");
                strcat(exp_buf, tok[i]);
            }
            snprintf(st.running[cnt].expires, MAX_DATE_LEN, "%s", exp_buf);
        } else {
            /* Fallback for older ollama ps output (without CONTEXT) */
            snprintf(st.running[cnt].size, MAX_SIZE_LEN, "%s", tok[2]);
            if (n >= 4) {
                char proc_buf[MAX_PROC_LEN] = "";
                snprintf(proc_buf, sizeof(proc_buf), "%s", tok[3]);
                if (n >= 5 && (strcasecmp(tok[4], "CPU") == 0 || strcasecmp(tok[4], "GPU") == 0)) {
                    strncat(proc_buf, " ", sizeof(proc_buf) - strlen(proc_buf) - 1);
                    strncat(proc_buf, tok[4], sizeof(proc_buf) - strlen(proc_buf) - 1);
                    snprintf(st.running[cnt].proc, MAX_PROC_LEN, "%s", proc_buf);
                    /* No context in older output */
                    snprintf(st.running[cnt].context, MAX_CONTEXT_LEN, "N/A");
                    char exp_buf[MAX_DATE_LEN] = "";
                    for (int i = 5; i < n; i++) {
                        if (i > 5)
                            strcat(exp_buf, " ");
                        strcat(exp_buf, tok[i]);
                    }
                    snprintf(st.running[cnt].expires, MAX_DATE_LEN, "%s", exp_buf);
                } else {
                    snprintf(st.running[cnt].proc, MAX_PROC_LEN, "%s", tok[3]);
                    snprintf(st.running[cnt].context, MAX_CONTEXT_LEN, "N/A");
                    char exp_buf[MAX_DATE_LEN] = "";
                    for (int i = 4; i < n; i++) {
                        if (i > 4)
                            strcat(exp_buf, " ");
                        strcat(exp_buf, tok[i]);
                    }
                    snprintf(st.running[cnt].expires, MAX_DATE_LEN, "%s", exp_buf);
                }
            }
        }
        cnt++;
        line = strtok(NULL, "\n");
    }
    st.running_cnt = cnt;
}

/* -----------------------------------------------------------------------------
 * Dynamic Column Width Calculation
 * -------------------------------------------------------------------------- */
static void compute_widths(void) {
    int w_name = strlen("MODEL NAME");
    int w_id   = strlen("ID");
    int w_size = strlen("SIZE");
    int w_date = strlen("MODIFIED");
    for (int i = 0; i < st.model_cnt; i++) {
        int l = strlen(st.models[i].name);
        if (l > w_name)
            w_name = l;
        l = strlen(st.models[i].id);
        if (l > w_id)
            w_id = l;
        l = strlen(st.models[i].size);
        if (l > w_size)
            w_size = l;
        l = strlen(st.models[i].date);
        if (l > w_date)
            w_date = l;
    }
    w_name += 2; /* for the "* " prefix on running models */
    if (w_name > MAX_COL_WIDTH)
        w_name = MAX_COL_WIDTH;
    if (w_id > MAX_COL_WIDTH)
        w_id = MAX_COL_WIDTH;
    if (w_size > MAX_COL_WIDTH)
        w_size = MAX_COL_WIDTH;
    if (w_date > MAX_COL_WIDTH)
        w_date = MAX_COL_WIDTH;
    st.col_name = w_name + MIN_COL_PAD;
    st.col_id   = w_id + MIN_COL_PAD;
    st.col_size = w_size + MIN_COL_PAD;
    st.col_date = w_date + MIN_COL_PAD;

    /* Running tab columns */
    w_name     = strlen("MODEL NAME");
    w_id       = strlen("ID");
    w_size     = strlen("SIZE");
    int w_proc = strlen("PROCESSOR");
    int w_ctx  = strlen("CONTEXT");
    int w_exp  = strlen("EXPIRES");
    for (int i = 0; i < st.running_cnt; i++) {
        int l = strlen(st.running[i].name);
        if (l > w_name)
            w_name = l;
        l = strlen(st.running[i].id);
        if (l > w_id)
            w_id = l;
        l = strlen(st.running[i].size);
        if (l > w_size)
            w_size = l;
        l = strlen(st.running[i].proc);
        if (l > w_proc)
            w_proc = l;
        l = strlen(st.running[i].context);
        if (l > w_ctx)
            w_ctx = l;
        l = strlen(st.running[i].expires);
        if (l > w_exp)
            w_exp = l;
    }
    if (w_name > MAX_COL_WIDTH)
        w_name = MAX_COL_WIDTH;
    if (w_id > MAX_COL_WIDTH)
        w_id = MAX_COL_WIDTH;
    if (w_size > MAX_COL_WIDTH)
        w_size = MAX_COL_WIDTH;
    if (w_proc > MAX_COL_WIDTH)
        w_proc = MAX_COL_WIDTH;
    if (w_ctx > MAX_COL_WIDTH)
        w_ctx = MAX_COL_WIDTH;
    if (w_exp > MAX_COL_WIDTH)
        w_exp = MAX_COL_WIDTH;
    st.rcol_name = w_name + MIN_COL_PAD;
    st.rcol_id   = w_id + MIN_COL_PAD;
    st.rcol_size = w_size + MIN_COL_PAD;
    st.rcol_proc = w_proc + MIN_COL_PAD;
    st.rcol_ctx  = w_ctx + MIN_COL_PAD;
    st.rcol_exp  = w_exp + MIN_COL_PAD;
}

/* -----------------------------------------------------------------------------
 * Data Refresh (no ncurses calls – thread safe)
 * -------------------------------------------------------------------------- */
static void refresh_data(void) {
    char out[65536];
    run_cmd("ollama list 2>/dev/null", out, sizeof(out));
    pthread_mutex_lock(&st.mutex);
    parse_list(out);
    run_cmd("ollama ps 2>/dev/null", out, sizeof(out));
    parse_ps(out);
    compute_widths();
    pthread_mutex_unlock(&st.mutex);
}

/* -----------------------------------------------------------------------------
 * Model Operations (Delete, Stop, Show Info)
 * -------------------------------------------------------------------------- */
static void remove_model(const char *name) {
    char cmd[MAX_LINE_LEN];
    char output[512];
    snprintf(cmd, sizeof(cmd), "ollama rm %s 2>&1", name);
    int ret = run_cmd(cmd, output, sizeof(output));
    if (ret == 0) {
        snprintf(st.logmsg, sizeof(st.logmsg), "Removed model: %s", name);
        snprintf(st.status, sizeof(st.status), "Model removed");
    } else {
        size_t len = strlen(output);
        if (len > 0 && output[len - 1] == '\n')
            output[len - 1] = '\0';
        snprintf(st.logmsg, sizeof(st.logmsg), "Failed to remove %s: %s", name, output);
        snprintf(st.status, sizeof(st.status), "Delete failed");
    }
}

static void stop_model(const char *name) {
    char cmd[MAX_LINE_LEN];
    char output[512];
    snprintf(cmd, sizeof(cmd), "ollama stop %s 2>&1", name);
    int ret = run_cmd(cmd, output, sizeof(output));
    if (ret == 0) {
        snprintf(st.logmsg, sizeof(st.logmsg), "Stopped model: %s", name);
        snprintf(st.status, sizeof(st.status), "Model stopped");
    } else {
        size_t len = strlen(output);
        if (len > 0 && output[len - 1] == '\n')
            output[len - 1] = '\0';
        snprintf(st.logmsg, sizeof(st.logmsg), "Failed to stop %s: %s", name, output);
        snprintf(st.status, sizeof(st.status), "Stop failed");
    }
}

static void show_info(const char *name) {
    char cmd[MAX_LINE_LEN];
    snprintf(cmd, sizeof(cmd), "ollama show %s 2>&1", name);
    run_cmd(cmd, st.info_out, sizeof(st.info_out));
    st.show_info = 1;
}

/* -----------------------------------------------------------------------------
 * ncurses Initialization & Cleanup
 * -------------------------------------------------------------------------- */
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
        init_pair(CP_DEFAULT, COLOR_WHITE, -1);
        init_pair(CP_HEADER, COLOR_WHITE, COLOR_BLUE);
        init_pair(CP_ACCENT, COLOR_CYAN, -1);
        init_pair(CP_SUCCESS, COLOR_GREEN, -1);
        init_pair(CP_DANGER, COLOR_RED, -1);
        init_pair(CP_WARNING, COLOR_YELLOW, -1);
        init_pair(CP_RUNNING, COLOR_GREEN, -1);
        init_pair(CP_SELECTED, COLOR_BLACK, COLOR_CYAN);
        init_pair(CP_BORDER, COLOR_BLUE, -1);
        init_pair(CP_DIALOG, COLOR_WHITE, COLOR_BLUE);
        init_pair(CP_DIALOG_BORDER, COLOR_CYAN, COLOR_BLUE);
        init_pair(CP_INFO_TEXT, COLOR_YELLOW, COLOR_BLUE);
    }
    getmaxyx(stdscr, rows, cols);
}

static void cleanup(void) {
    endwin();
}

/* -----------------------------------------------------------------------------
 * UI Drawing – Header, Footer, Tabs, Lists, Log, Dialogs
 * -------------------------------------------------------------------------- */
static void draw_header(void) {
    attron(COLOR_PAIR(CP_HEADER) | A_BOLD);
    for (int i = 0; i < cols; i++) mvaddch(0, i, ' ');
    for (int i = 0; i < cols; i++) mvaddch(3, i, ' ');
    mvprintw(1, 2, " OLLAMA MODEL MANAGER ");
    attroff(A_BOLD);
    if (strlen(st.status)) {
        attron(COLOR_PAIR(CP_SUCCESS));
        mvprintw(1, cols - strlen(st.status) - 3, " %s ", st.status);
        attroff(COLOR_PAIR(CP_SUCCESS));
    }
    attroff(COLOR_PAIR(CP_HEADER));
    attron(COLOR_PAIR(CP_BORDER));
    for (int i = 0; i < cols; i++) mvaddch(3, i, ACS_HLINE);
    mvaddch(3, 0, ACS_LTEE);
    mvaddch(3, cols - 1, ACS_RTEE);
    attroff(COLOR_PAIR(CP_BORDER));
}

static void draw_footer(void) {
    int y = rows - 5;
    attron(COLOR_PAIR(CP_BORDER));
    for (int i = 0; i < cols; i++) mvaddch(y, i, ACS_HLINE);
    mvaddch(y, 0, ACS_LTEE);
    mvaddch(y, cols - 1, ACS_RTEE);
    attroff(COLOR_PAIR(CP_BORDER));
    attron(COLOR_PAIR(CP_ACCENT));
    int x = 2;
    mvprintw(y + 1, x, "[UP/DOWN] Nav");
    x += 14;
    mvprintw(y + 1, x, "  ");
    x += 2;
    mvprintw(y + 1, x, "[LEFT/RIGHT] Tabs");
    x += 18;
    mvprintw(y + 1, x, "  ");
    x += 2;
    mvprintw(y + 1, x, "[I] Info");
    x += 9;
    mvprintw(y + 1, x, "  ");
    x += 2;
    mvprintw(y + 1, x, "[D] Delete");
    x += 11;
    mvprintw(y + 1, x, "  ");
    x += 2;
    mvprintw(y + 1, x, "[S] Stop");
    x += 9;
    mvprintw(y + 1, x, "  ");
    x += 2;
    mvprintw(y + 1, x, "[P] Pull");
    x += 9;
    mvprintw(y + 1, x, "  ");
    x += 2;
    mvprintw(y + 1, x, "[R] Refresh");
    x += 12;
    mvprintw(y + 1, x, "  ");
    x += 2;
    mvprintw(y + 1, x, "[/] Search");
    x += 11;
    mvprintw(y + 1, x, "  ");
    x += 2;
    mvprintw(y + 1, x, "[Q] Quit");
    attroff(COLOR_PAIR(CP_ACCENT));
    if (st.pulling) {
        attron(COLOR_PAIR(CP_WARNING));
        mvprintw(y + 1, cols - 22, " Pull in progress... ");
        attroff(COLOR_PAIR(CP_WARNING));
    }
}

static void draw_tabs(void) {
    int y = 5;
    if (st.tab == 0)
        attron(COLOR_PAIR(CP_SELECTED) | A_BOLD);
    else
        attron(COLOR_PAIR(CP_ACCENT));
    mvprintw(y, 2, " INSTALLED MODELS ");
    attroff(A_BOLD | COLOR_PAIR(CP_SELECTED) | COLOR_PAIR(CP_ACCENT));
    if (st.tab == 1)
        attron(COLOR_PAIR(CP_SELECTED) | A_BOLD);
    else
        attron(COLOR_PAIR(CP_ACCENT));
    mvprintw(y, 22, " RUNNING MODELS ");
    attroff(A_BOLD | COLOR_PAIR(CP_SELECTED) | COLOR_PAIR(CP_ACCENT));
}

static void draw_search(void) {
    attron(COLOR_PAIR(CP_ACCENT));
    mvprintw(6, 2, "SEARCH:");
    attroff(COLOR_PAIR(CP_ACCENT));
    attron(COLOR_PAIR(CP_DEFAULT));
    if (strlen(st.filter)) {
        attron(COLOR_PAIR(CP_SUCCESS));
        mvprintw(6, 10, " %-30s ", st.filter);
        attroff(COLOR_PAIR(CP_SUCCESS));
        attron(COLOR_PAIR(CP_ACCENT));
        mvprintw(6, 43, "[Press '/' to clear]");
        attroff(COLOR_PAIR(CP_ACCENT));
    } else {
        mvprintw(6, 10, " %-30s ", "(none)");
        attron(COLOR_PAIR(CP_ACCENT));
        mvprintw(6, 43, "[Press '/' to search]");
        attroff(COLOR_PAIR(CP_ACCENT));
    }
    attroff(COLOR_PAIR(CP_DEFAULT));
}

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
    mvprintw(yh, x_name, "MODEL NAME");
    mvprintw(yh, x_id, "ID");
    mvprintw(yh, x_size, "SIZE");
    mvprintw(yh, x_date, "MODIFIED");
    attroff(A_BOLD | COLOR_PAIR(CP_ACCENT));

    attron(COLOR_PAIR(CP_BORDER));
    for (int i = 2; i < cols - 2; i++) mvaddch(yh + 1, i, ACS_HLINE);
    attroff(COLOR_PAIR(CP_BORDER));

    for (int i = 0; i < maxrows; i++) mvprintw(ylist + i, 2, "%*s", cols - 4, "");

    pthread_mutex_lock(&st.mutex);
    int disp = 0, row = ylist;
    for (int i = 0; i < st.model_cnt && row < rows - 9; i++) {
        int running = 0;
        for (int j = 0; j < st.running_cnt; j++)
            if (strcmp(st.models[i].name, st.running[j].name) == 0) {
                running = 1;
                break;
            }
        if (strlen(st.filter) && !strcasestr(st.models[i].name, st.filter))
            continue;

        if (disp == st.sel_model && st.tab == 0)
            attron(COLOR_PAIR(CP_SELECTED));
        else if (running)
            attron(COLOR_PAIR(CP_RUNNING));
        else
            attron(COLOR_PAIR(CP_DEFAULT));

        char disp_name[MAX_NAME_LEN + 3];
        snprintf(disp_name, sizeof(disp_name), "%s%s", running ? "* " : "  ", st.models[i].name);
        mvprintw(row, x_name, "%-*.*s", st.col_name - 1, st.col_name - 1, disp_name);
        mvprintw(row, x_id, "%-*.*s", st.col_id - 1, st.col_id - 1, st.models[i].id);
        mvprintw(row, x_size, "%-*.*s", st.col_size - 1, st.col_size - 1, st.models[i].size);
        mvprintw(row, x_date, "%-*.*s", st.col_date - 1, st.col_date - 1, st.models[i].date);

        attroff(COLOR_PAIR(CP_SELECTED) | COLOR_PAIR(CP_RUNNING) | COLOR_PAIR(CP_DEFAULT));
        row++;
        disp++;
    }
    if (disp == 0) {
        attron(COLOR_PAIR(CP_WARNING));
        if (strlen(st.filter))
            mvprintw(ylist, 2, "No models match filter: %s", st.filter);
        else
            mvprintw(ylist, 2, "No models found. Press 'P' to pull.");
        attroff(COLOR_PAIR(CP_WARNING));
    }
    pthread_mutex_unlock(&st.mutex);
}

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
    mvprintw(yh, x_name, "MODEL NAME");
    mvprintw(yh, x_id, "ID");
    mvprintw(yh, x_size, "SIZE");
    mvprintw(yh, x_proc, "PROCESSOR");
    mvprintw(yh, x_ctx, "CONTEXT");
    mvprintw(yh, x_exp, "EXPIRES");
    attroff(A_BOLD | COLOR_PAIR(CP_ACCENT));

    attron(COLOR_PAIR(CP_BORDER));
    for (int i = 2; i < cols - 2; i++) mvaddch(yh + 1, i, ACS_HLINE);
    attroff(COLOR_PAIR(CP_BORDER));

    for (int i = 0; i < maxrows; i++) mvprintw(ylist + i, 2, "%*s", cols - 4, "");

    pthread_mutex_lock(&st.mutex);
    int row = ylist;
    for (int i = 0; i < st.running_cnt && row < rows - 9; i++) {
        if (i == st.sel_running && st.tab == 1)
            attron(COLOR_PAIR(CP_SELECTED));
        else
            attron(COLOR_PAIR(CP_DEFAULT));

        mvprintw(row, x_name, "%-*.*s", st.rcol_name - 1, st.rcol_name - 1, st.running[i].name);
        mvprintw(row, x_id, "%-*.*s", st.rcol_id - 1, st.rcol_id - 1, st.running[i].id);
        mvprintw(row, x_size, "%-*.*s", st.rcol_size - 1, st.rcol_size - 1, st.running[i].size);
        mvprintw(row, x_proc, "%-*.*s", st.rcol_proc - 1, st.rcol_proc - 1, st.running[i].proc);
        mvprintw(row, x_ctx, "%-*.*s", st.rcol_ctx - 1, st.rcol_ctx - 1, st.running[i].context);
        mvprintw(row, x_exp, "%-*.*s", st.rcol_exp - 1, st.rcol_exp - 1, st.running[i].expires);

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

static void draw_log(void) {
    int y = rows - 3;
    attron(COLOR_PAIR(CP_BORDER));
    for (int i = 0; i < cols; i++) mvaddch(y, i, ACS_HLINE);
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

static void draw_content(void) {
    draw_tabs();
    draw_search();
    if (st.tab == 0)
        draw_model_list();
    else
        draw_running_list();
}

static void draw_info_dialog(void) {
    int w = (cols - 4 < 90) ? cols - 4 : 90;
    if (w < 40)
        w = 40;
    int h = rows - 10;
    if (h > 30)
        h = 30;
    if (h < 10)
        h = 10;
    int sy = (rows - h) / 2, sx = (cols - w) / 2;

    attron(COLOR_PAIR(CP_DIALOG));
    for (int i = 0; i < h; i++)
        for (int j = 0; j < w; j++) mvaddch(sy + i, sx + j, ' ');
    attroff(COLOR_PAIR(CP_DIALOG));

    attron(COLOR_PAIR(CP_DIALOG_BORDER));
    for (int i = 0; i < w; i++) mvaddch(sy, sx + i, ACS_HLINE);
    for (int i = 0; i < w; i++) mvaddch(sy + h - 1, sx + i, ACS_HLINE);
    for (int i = 0; i < h; i++) mvaddch(sy + i, sx, ACS_VLINE);
    for (int i = 0; i < h; i++) mvaddch(sy + i, sx + w - 1, ACS_VLINE);
    mvaddch(sy, sx, ACS_ULCORNER);
    mvaddch(sy, sx + w - 1, ACS_URCORNER);
    mvaddch(sy + h - 1, sx, ACS_LLCORNER);
    mvaddch(sy + h - 1, sx + w - 1, ACS_LRCORNER);
    attroff(COLOR_PAIR(CP_DIALOG_BORDER));

    attron(COLOR_PAIR(CP_HEADER) | A_BOLD);
    mvprintw(sy + 1, sx + (w - 20) / 2, " MODEL INFORMATION ");
    attroff(A_BOLD | COLOR_PAIR(CP_HEADER));

    attron(COLOR_PAIR(CP_INFO_TEXT) | A_BOLD);
    int   row  = sy + 3;
    char *line = strtok(st.info_out, "\n");
    while (line && row < sy + h - 2) {
        char disp[w - 4];
        snprintf(disp, sizeof(disp), "%s", line);
        mvprintw(row, sx + 2, "%s", disp);
        row++;
        line = strtok(NULL, "\n");
    }
    attroff(COLOR_PAIR(CP_INFO_TEXT) | A_BOLD);

    attron(COLOR_PAIR(CP_ACCENT));
    mvprintw(sy + h - 2, sx + (w - 26) / 2, " Press any key to close ");
    attroff(COLOR_PAIR(CP_ACCENT));
}

static void draw_pull_dialog(void) {
    int w = 64, h = 6;
    int sy = (rows - h) / 2, sx = (cols - w) / 2;

    attron(COLOR_PAIR(CP_DIALOG));
    for (int i = 0; i < h; i++)
        for (int j = 0; j < w; j++) mvaddch(sy + i, sx + j, ' ');
    attroff(COLOR_PAIR(CP_DIALOG));

    attron(COLOR_PAIR(CP_DIALOG_BORDER));
    for (int i = 0; i < w; i++) mvaddch(sy, sx + i, ACS_HLINE);
    for (int i = 0; i < w; i++) mvaddch(sy + h - 1, sx + i, ACS_HLINE);
    for (int i = 0; i < h; i++) mvaddch(sy + i, sx, ACS_VLINE);
    for (int i = 0; i < h; i++) mvaddch(sy + i, sx + w - 1, ACS_VLINE);
    mvaddch(sy, sx, ACS_ULCORNER);
    mvaddch(sy, sx + w - 1, ACS_URCORNER);
    mvaddch(sy + h - 1, sx, ACS_LLCORNER);
    mvaddch(sy + h - 1, sx + w - 1, ACS_LRCORNER);
    attroff(COLOR_PAIR(CP_DIALOG_BORDER));

    attron(COLOR_PAIR(CP_HEADER) | A_BOLD);
    mvprintw(sy + 1, sx + (w - 12) / 2, " PULL MODEL ");
    attroff(A_BOLD | COLOR_PAIR(CP_HEADER));

    attron(COLOR_PAIR(CP_DIALOG));
    mvprintw(sy + 2, sx + 4, "Model name:");
    attroff(COLOR_PAIR(CP_DIALOG));

    int field_start = sx + 17;
    int field_width = 42;
    attron(COLOR_PAIR(CP_ACCENT));
    for (int i = 0; i < field_width; i++) {
        mvaddch(sy + 2, field_start + i, ' ');
    }
    mvprintw(sy + 2, field_start, "%-*.*s", field_width - 1, field_width - 1, st.dialog_input);
    int cursor_pos = strlen(st.dialog_input);
    if (cursor_pos >= field_width - 1)
        cursor_pos = field_width - 1;
    mvaddch(sy + 2, field_start + cursor_pos, '_');
    attroff(COLOR_PAIR(CP_ACCENT));

    attron(COLOR_PAIR(CP_ACCENT));
    const char *msg     = " Press ENTER to pull, ESC to cancel ";
    int         msg_len = strlen(msg);
    int         msg_x   = sx + (w - msg_len) / 2;
    mvprintw(sy + 4, msg_x, "%s", msg);
    attroff(COLOR_PAIR(CP_ACCENT));
}

static void draw_search_dialog(void) {
    int w = 64, h = 6;
    int sy = (rows - h) / 2, sx = (cols - w) / 2;

    attron(COLOR_PAIR(CP_DIALOG));
    for (int i = 0; i < h; i++)
        for (int j = 0; j < w; j++) mvaddch(sy + i, sx + j, ' ');
    attroff(COLOR_PAIR(CP_DIALOG));

    attron(COLOR_PAIR(CP_DIALOG_BORDER));
    for (int i = 0; i < w; i++) mvaddch(sy, sx + i, ACS_HLINE);
    for (int i = 0; i < w; i++) mvaddch(sy + h - 1, sx + i, ACS_HLINE);
    for (int i = 0; i < h; i++) mvaddch(sy + i, sx, ACS_VLINE);
    for (int i = 0; i < h; i++) mvaddch(sy + i, sx + w - 1, ACS_VLINE);
    mvaddch(sy, sx, ACS_ULCORNER);
    mvaddch(sy, sx + w - 1, ACS_URCORNER);
    mvaddch(sy + h - 1, sx, ACS_LLCORNER);
    mvaddch(sy + h - 1, sx + w - 1, ACS_LRCORNER);
    attroff(COLOR_PAIR(CP_DIALOG_BORDER));

    attron(COLOR_PAIR(CP_HEADER) | A_BOLD);
    mvprintw(sy + 1, sx + (w - 12) / 2, " SEARCH ");
    attroff(A_BOLD | COLOR_PAIR(CP_HEADER));

    attron(COLOR_PAIR(CP_DIALOG));
    mvprintw(sy + 2, sx + 4, "Search term:");
    attroff(COLOR_PAIR(CP_DIALOG));

    int field_start = sx + 17;
    int field_width = 42;
    attron(COLOR_PAIR(CP_ACCENT));
    for (int i = 0; i < field_width; i++) {
        mvaddch(sy + 2, field_start + i, ' ');
    }
    mvprintw(sy + 2, field_start, "%-*.*s", field_width - 1, field_width - 1, st.dialog_input);
    int cursor_pos = strlen(st.dialog_input);
    if (cursor_pos >= field_width - 1)
        cursor_pos = field_width - 1;
    mvaddch(sy + 2, field_start + cursor_pos, '_');
    attroff(COLOR_PAIR(CP_ACCENT));

    attron(COLOR_PAIR(CP_ACCENT));
    const char *msg     = " Press ENTER to search, ESC to cancel ";
    int         msg_len = strlen(msg);
    int         msg_x   = sx + (w - msg_len) / 2;
    mvprintw(sy + 4, msg_x, "%s", msg);
    attroff(COLOR_PAIR(CP_ACCENT));
}

/* -----------------------------------------------------------------------------
 * Confirmation Dialog (OK / Cancel)
 * -------------------------------------------------------------------------- */
static void draw_confirm_dialog(void) {
    int w = 50, h = 6;
    int sy = (rows - h) / 2, sx = (cols - w) / 2;

    attron(COLOR_PAIR(CP_DIALOG));
    for (int i = 0; i < h; i++)
        for (int j = 0; j < w; j++) mvaddch(sy + i, sx + j, ' ');
    attroff(COLOR_PAIR(CP_DIALOG));

    attron(COLOR_PAIR(CP_DIALOG_BORDER));
    for (int i = 0; i < w; i++) mvaddch(sy, sx + i, ACS_HLINE);
    for (int i = 0; i < w; i++) mvaddch(sy + h - 1, sx + i, ACS_HLINE);
    for (int i = 0; i < h; i++) mvaddch(sy + i, sx, ACS_VLINE);
    for (int i = 0; i < h; i++) mvaddch(sy + i, sx + w - 1, ACS_VLINE);
    mvaddch(sy, sx, ACS_ULCORNER);
    mvaddch(sy, sx + w - 1, ACS_URCORNER);
    mvaddch(sy + h - 1, sx, ACS_LLCORNER);
    mvaddch(sy + h - 1, sx + w - 1, ACS_LRCORNER);
    attroff(COLOR_PAIR(CP_DIALOG_BORDER));

    attron(COLOR_PAIR(CP_DEFAULT));
    int msg_len = strlen(st.confirm_msg);
    int msg_x   = sx + (w - msg_len) / 2;
    mvprintw(sy + 2, msg_x, "%s", st.confirm_msg);
    attroff(COLOR_PAIR(CP_DEFAULT));

    int btn_ok_x     = sx + w / 2 - 12;
    int btn_cancel_x = sx + w / 2 + 2;
    attron(COLOR_PAIR(CP_ACCENT) | A_BOLD);
    if (st.confirm_choice == 1) {
        attron(A_REVERSE);
        mvprintw(sy + 4, btn_ok_x, "[ OK ]");
        attroff(A_REVERSE);
        mvprintw(sy + 4, btn_cancel_x, "[ Cancel ]");
    } else {
        mvprintw(sy + 4, btn_ok_x, "[ OK ]");
        attron(A_REVERSE);
        mvprintw(sy + 4, btn_cancel_x, "[ Cancel ]");
        attroff(A_REVERSE);
    }
    attroff(COLOR_PAIR(CP_ACCENT) | A_BOLD);
}

static void full_refresh(void) {
    clear();
    draw_header();
    draw_content();
    draw_log();
    draw_footer();
    refresh();
}

static void log_msg(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(st.logmsg, sizeof(st.logmsg), fmt, ap);
    va_end(ap);
    draw_log();
    refresh();
}

/* -----------------------------------------------------------------------------
 * Background Refresh Thread
 * -------------------------------------------------------------------------- */
static void *refresh_thread(void *arg) {
    refresh_data();
    st.need_refresh = 1;
    return NULL;
}

/* -----------------------------------------------------------------------------
 * Main Program
 * -------------------------------------------------------------------------- */
int main(void) {
    char test[256];
    run_cmd("ollama --version 2>/dev/null", test, sizeof(test));
    if (!strlen(test)) {
        fprintf(stderr, "Error: Ollama not found in PATH.\n");
        return 1;
    }

    memset(&st, 0, sizeof(st));
    pthread_mutex_init(&st.mutex, NULL);
    init_ncurses();

    refresh_data();
    snprintf(st.status, sizeof(st.status), "Refreshed");
    snprintf(st.logmsg, sizeof(st.logmsg), "Loaded %d model(s), %d running", st.model_cnt, st.running_cnt);
    full_refresh();

    pthread_t tid;
    int       ch;

    while (1) {
        if (st.need_refresh) {
            snprintf(st.status, sizeof(st.status), "Refreshed");
            snprintf(st.logmsg, sizeof(st.logmsg), "Loaded %d model(s), %d running", st.model_cnt, st.running_cnt);
            full_refresh();
            st.need_refresh = 0;
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
            ch = getch();
            if (ch == 27) {
                st.show_dialog = 0;
                memset(st.dialog_input, 0, sizeof(st.dialog_input));
                full_refresh();
            } else if (ch == 10 || ch == 13) {
                if (strlen(st.dialog_input)) {
                    st.pulling     = 1;
                    st.show_dialog = 0;
                    full_refresh();

                    def_prog_mode();
                    endwin();
                    char cmd[MAX_LINE_LEN];
                    snprintf(cmd, sizeof(cmd), "ollama pull %s", st.dialog_input);
                    printf("\n");
                    printf("+----------------------------------------------------------------------+\n");
                    printf("|                         PULLING MODEL                                |\n");
                    printf("+----------------------------------------------------------------------+\n\n");
                    printf("Model: %s\n", st.dialog_input);
                    printf("------------------------------------------------------------------------\n\n");
                    fflush(stdout);

                    int ret = system(cmd);
                    printf("\n");
                    printf("------------------------------------------------------------------------\n");

                    if (ret == 0)
                        printf("[SUCCESS] Model '%s' pulled successfully.\n", st.dialog_input);
                    else
                        printf("[ERROR] Failed to pull model '%s'\n", st.dialog_input);

                    printf("\nPress ENTER to continue...");
                    fflush(stdout);
                    getchar();

                    reset_prog_mode();
                    init_ncurses();
                    refresh_data();
                    snprintf(st.status, sizeof(st.status), "Pull complete");
                    snprintf(st.logmsg, sizeof(st.logmsg), "Pulled model: %s", st.dialog_input);
                    full_refresh();
                    st.pulling = 0;
                    memset(st.dialog_input, 0, sizeof(st.dialog_input));
                }
            } else if (ch == 127 || ch == KEY_BACKSPACE || ch == 8) {
                size_t len = strlen(st.dialog_input);
                if (len) {
                    st.dialog_input[len - 1] = '\0';
                    draw_pull_dialog();
                    refresh();
                }
            } else if (ch >= 32 && ch <= 126 && isprint(ch)) {
                size_t len = strlen(st.dialog_input);
                if (len < MAX_NAME_LEN - 1) {
                    st.dialog_input[len]     = ch;
                    st.dialog_input[len + 1] = '\0';
                    draw_pull_dialog();
                    refresh();
                }
            }
            continue;
        }

        if (st.show_search) {
            ch = getch();
            if (ch == 27) {
                st.show_search = 0;
                memset(st.dialog_input, 0, sizeof(st.dialog_input));
                full_refresh();
            } else if (ch == 10 || ch == 13) {
                if (strlen(st.dialog_input)) {
                    snprintf(st.filter, MAX_NAME_LEN, "%s", st.dialog_input);
                    st.sel_model = 0;
                } else {
                    st.filter[0] = '\0';
                }
                st.show_search = 0;
                memset(st.dialog_input, 0, sizeof(st.dialog_input));
                full_refresh();
                log_msg("Search filter: %s", strlen(st.filter) ? st.filter : "(cleared)");
            } else if (ch == 127 || ch == KEY_BACKSPACE || ch == 8) {
                size_t len = strlen(st.dialog_input);
                if (len) {
                    st.dialog_input[len - 1] = '\0';
                    draw_search_dialog();
                    refresh();
                }
            } else if (ch >= 32 && ch <= 126 && isprint(ch)) {
                size_t len = strlen(st.dialog_input);
                if (len < MAX_NAME_LEN - 1) {
                    st.dialog_input[len]     = ch;
                    st.dialog_input[len + 1] = '\0';
                    draw_search_dialog();
                    refresh();
                }
            }
            continue;
        }

        if (st.confirm_active) {
            ch = getch();
            if (ch == 27) {
                st.confirm_active = 0;
                full_refresh();
                continue;
            }
            switch (ch) {
                case KEY_LEFT:
                    st.confirm_choice = 0;
                    draw_confirm_dialog();
                    refresh();
                    break;
                case KEY_RIGHT:
                    st.confirm_choice = 1;
                    draw_confirm_dialog();
                    refresh();
                    break;
                case '\t':
                    st.confirm_choice = !st.confirm_choice;
                    draw_confirm_dialog();
                    refresh();
                    break;
                case 10:
                case 13:
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
                    continue;
                default:
                    break;
            }
            continue;
        }

        ch = getch();
        if (ch == ERR)
            continue;

        switch (ch) {
            case 'q':
            case 'Q':
                cleanup();
                pthread_mutex_destroy(&st.mutex);
                printf("\nGoodbye.\n");
                return 0;

            case 'r':
            case 'R':
                snprintf(st.status, sizeof(st.status), "Refreshing...");
                draw_header();
                refresh();
                pthread_create(&tid, NULL, refresh_thread, NULL);
                pthread_detach(tid);
                break;

            case 'i':
            case 'I':
                if (st.tab == 0 && st.model_cnt) {
                    int   vis  = 0;
                    char *name = NULL;
                    pthread_mutex_lock(&st.mutex);
                    for (int i = 0; i < st.model_cnt; i++) {
                        if (!strlen(st.filter) || strcasestr(st.models[i].name, st.filter)) {
                            if (vis == st.sel_model) {
                                name = st.models[i].name;
                                break;
                            }
                            vis++;
                        }
                    }
                    pthread_mutex_unlock(&st.mutex);
                    if (name) {
                        show_info(name);
                        draw_info_dialog();
                        refresh();
                        log_msg("Info for %s", name);
                    }
                }
                break;

            case KEY_UP:
                if (st.tab == 0 && st.model_cnt) {
                    int vis = 0;
                    pthread_mutex_lock(&st.mutex);
                    for (int i = 0; i < st.model_cnt; i++)
                        if (!strlen(st.filter) || strcasestr(st.models[i].name, st.filter))
                            vis++;
                    pthread_mutex_unlock(&st.mutex);
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
                    int vis = 0;
                    pthread_mutex_lock(&st.mutex);
                    for (int i = 0; i < st.model_cnt; i++)
                        if (!strlen(st.filter) || strcasestr(st.models[i].name, st.filter))
                            vis++;
                    pthread_mutex_unlock(&st.mutex);
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
                    int   vis  = 0;
                    char *name = NULL;
                    pthread_mutex_lock(&st.mutex);
                    for (int i = 0; i < st.model_cnt; i++) {
                        if (!strlen(st.filter) || strcasestr(st.models[i].name, st.filter)) {
                            if (vis == st.sel_model) {
                                name = st.models[i].name;
                                break;
                            }
                            vis++;
                        }
                    }
                    pthread_mutex_unlock(&st.mutex);
                    if (name) {
                        snprintf(st.confirm_msg, sizeof(st.confirm_msg), "Remove model '%s' ?", name);
                        snprintf(st.confirm_target, MAX_NAME_LEN, "%s", name);
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
                    char *name = st.running[st.sel_running].name;
                    snprintf(st.confirm_msg, sizeof(st.confirm_msg), "Stop model '%s' ?", name);
                    snprintf(st.confirm_target, MAX_NAME_LEN, "%s", name);
                    st.confirm_is_delete = 0;
                    st.confirm_choice    = 1;
                    st.confirm_active    = 1;
                    draw_confirm_dialog();
                    refresh();
                }
                break;

            case 'p':
            case 'P':
                memset(st.dialog_input, 0, sizeof(st.dialog_input));
                st.show_dialog = 1;
                draw_pull_dialog();
                refresh();
                break;

            case '/':
                memset(st.dialog_input, 0, sizeof(st.dialog_input));
                st.show_search = 1;
                draw_search_dialog();
                refresh();
                break;

            case KEY_RESIZE:
                getmaxyx(stdscr, rows, cols);
                compute_widths();
                full_refresh();
                break;

            default:
                break;
        }
    }

    cleanup();
    return 0;
}
