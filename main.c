#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <unistd.h>
#include <termios.h>
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <stdarg.h>

#define EDITOR_VERSION "0.0.1"
#define EDITOR_TAB_STOP 4

#define CTRL_KEY(k) ((k) & 0x1f) /* define ctrl+key keypresses by stripping bits for bitwise ANDs */

enum editorKey {
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    DEL_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN
};

typedef struct editorRow {
    int size;
    int rsize;
    char *chars;
    char *render;
} editorRow;

struct editorConfig {
    int cx, cy;
    int rx;
    int rowoffset;
    int coloffset;
    int screenrows;
    int screencols;
    int numrows;
    editorRow *row;
    char *filename;
    char statusmsg[80];
    time_t statusmsg_time;
    struct termios old_termios;
};

struct editorConfig ECONFIG;

/* Handles errors and exits the program */
void die(const char *s) {
    write(STDOUT_FILENO, "\x1b[2J", 4); /* clear the screen */
    write(STDOUT_FILENO, "\x1b[H", 3); /* position cursor at top left */

    perror(s); /* read global errno value and print error message */
    exit(1);
}

/* Restore to old I/O settings */
void resetTermios() {
    if (tcsetattr(0, TCSAFLUSH, &ECONFIG.old_termios) == -1) {
	die("tcsetattr");
    }
}

void initTermios() {
    if (tcgetattr(0, &ECONFIG.old_termios) == -1) die("tcgetattr"); /* grab old settings */
    atexit(resetTermios); /* reset settings on exit */
	
    struct termios new = ECONFIG.old_termios;
    new.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG); /* disable echo mode, buffered I/O, ctrl-c, ctrl-z, and ctrl-v */
    new.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP); /* disable ctrl-s, ctrl-q, ctrl-m, break SIGINT signal, parity checking, stripping of 8th bit */
    new.c_oflag &= ~(OPOST); /* disable output processing */
    new.c_cflag |= CS8; /* bit mask to set character size to 8 bits per byte */
    new.c_cc[VMIN] = 0;
    new.c_cc[VTIME] = 1;

    if (tcsetattr(0, TCSAFLUSH, &new) == -1) die("tcsetattr"); /* use the new I/O settings */
}

int readKey() {
    int readReturnVal;
    char c;

    while ((readReturnVal = read(STDIN_FILENO, &c, 1)) != 1) {
	if (readReturnVal == -1 && errno != EAGAIN) die("read");
    }
    
    if (c == '\x1b') {
        char seq[3];
        
        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9') {
                if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
                if (seq[2] == '~') {
                    switch (seq[1]) {
                        case '1': return HOME_KEY;
                        case '3': return DEL_KEY;
                        case '4': return END_KEY;
                        case '5': return PAGE_UP;
                        case '6': return PAGE_DOWN;
                        case '7': return HOME_KEY;
                        case '8': return END_KEY;
                    }
                }
            }
            else {
                switch (seq[1]) {
                    case 'A': return ARROW_UP;
                    case 'B': return ARROW_DOWN;
                    case 'C': return ARROW_RIGHT;
                    case 'D': return ARROW_LEFT;
                    case 'H': return HOME_KEY;
                    case 'F': return END_KEY;
                }
            }
        }
        else if (seq[0] == 'O') {
            switch (seq[1]) {
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
            }
        }
    
        return '\x1b';
    }
    else {
        return c;
    }
}

int getCursorPosition(int *rows, int *cols) {
    char buf[32];
    unsigned int i = 0;

    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

    while (i < sizeof(buf) - 1) {
        if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
        if (buf[i] == 'R') break;
        i++;
    }
    buf[i] = '\0';

    if (buf[0] != '\x1b' || buf[1] != '[') return -1;
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;

    return 0;
}

int getWindowSize(int *rows, int *cols) {
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
        return getCursorPosition(rows, cols);
    }
    else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

int editorRowCxToRx(editorRow *row, int cx) {
    int rx = 0;
    int j;
    for (j = 0; j < cx; j++) {
        if (row->chars[j] == '\t') {
            rx += (EDITOR_TAB_STOP - 1) - (rx % EDITOR_TAB_STOP);
        }
        rx++;
    }
    return rx;
}

void editorUpdateRow(editorRow *row) {
    int tabs = 0;
    int j;
    for (j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t') tabs++;
    }

    free(row->render);
    row->render = malloc(row->size + tabs*(EDITOR_TAB_STOP - 1) + 1);

    int idx = 0;
    for (j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t') {
            row->render[idx++] = ' ';
            while (idx % EDITOR_TAB_STOP != 0) row->render[idx++] = ' ';
        }
        else {
            row->render[idx++] = row->chars[j];
        }
    }
    row->render[idx] = '\0';
    row->rsize = idx;
}

void editorAppendRow(char *s, size_t len) {
    ECONFIG.row = realloc(ECONFIG.row, sizeof(editorRow) * (ECONFIG.numrows + 1));

    int at = ECONFIG.numrows;
    ECONFIG.row[at].size = len;
    ECONFIG.row[at].chars = malloc(len + 1);
    memcpy(ECONFIG.row[at].chars, s, len);
    ECONFIG.row[at].chars[len] = '\0';

    ECONFIG.row[at].rsize = 0;
    ECONFIG.row[at].render = NULL;
    editorUpdateRow(&ECONFIG.row[at]);

    ECONFIG.numrows++;
}

void editorOpen(char *filename) {
    free(ECONFIG.filename);
    ECONFIG.filename = strdup(filename);

    FILE *fp = fopen(filename, "r");
    if (!fp) die("fopen");

    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;
    while ((linelen = getline(&line, &linecap, fp)) != -1) {
        while (linelen > 0 && (line[linelen - 1] == '\n' || line[linelen -1] == '\r')) {
            linelen--;
        }

        editorAppendRow(line, linelen);
    }
    free(line);
    fclose(fp);
}

struct appendbuffer {
    char *b;
    int len;
};

#define APPENDBUFFER_INIT {NULL, 0}

void aBufferAppend(struct appendbuffer *ab, const char *s, int len) {
    char *new = realloc(ab->b, ab->len + len);

    if (new == NULL) return;
    memcpy(&new[ab->len], s, len);
    ab->b = new;
    ab->len += len;
}

void aBufferFree(struct appendbuffer *ab) {
    free(ab->b);
}

void editorScroll() {
    ECONFIG.rx = 0;
    if (ECONFIG.cy < ECONFIG.numrows) {
        ECONFIG.rx = editorRowCxToRx(&ECONFIG.row[ECONFIG.cy], ECONFIG.cx);
    }

    if (ECONFIG.cy < ECONFIG.rowoffset) {
        ECONFIG.rowoffset = ECONFIG.cy;
    }
    if (ECONFIG.cy >= ECONFIG.rowoffset + ECONFIG.screenrows) {
        ECONFIG.rowoffset = ECONFIG.cy - ECONFIG.screenrows + 1;
    }
    if (ECONFIG.rx < ECONFIG.coloffset) {
        ECONFIG.coloffset = ECONFIG.rx;
    }
    if (ECONFIG.rx >= ECONFIG.coloffset + ECONFIG.screencols) {
        ECONFIG.coloffset = ECONFIG.rx - ECONFIG.screencols + 1;
    }
}

void drawRows(struct appendbuffer *ab) {
    int y;
    for (y = 0; y < ECONFIG.screenrows; y++) {
        int filerow = y + ECONFIG.rowoffset;
        if (filerow >= ECONFIG.numrows) {
            if (ECONFIG.numrows == 0 && y == ECONFIG.screenrows /3) {
                char welcome[80];
                int welcomelen = snprintf(welcome, sizeof(welcome),
                    "Editor -- version %s", EDITOR_VERSION);
                if (welcomelen > ECONFIG.screencols) welcomelen = ECONFIG.screencols;
                int padding = (ECONFIG.screencols - welcomelen) / 2;
                if (padding) {
                    aBufferAppend(ab, "~", 1);
                    padding--;
                }
                while (padding--) aBufferAppend(ab, " ", 1);
                aBufferAppend(ab, welcome, welcomelen);
            }
            else {
                aBufferAppend(ab, "~", 1);
            }
        }
        else {
            int len = ECONFIG.row[filerow].rsize - ECONFIG.coloffset;
            if (len < 0) len = 0;
            if (len > ECONFIG.screencols) len = ECONFIG.screencols;
            aBufferAppend(ab, &ECONFIG.row[filerow].render[ECONFIG.coloffset], len);
        }
        
        aBufferAppend(ab, "\x1b[K", 3);
        aBufferAppend(ab, "\r\n", 2);
    }
}

void drawStatusBar(struct appendbuffer *ab) {
    aBufferAppend(ab, "\x1b[7m", 4);
    char status[80], rstatus[80];
    int len = snprintf(status, sizeof(status), "%.20s - %d lines",
        ECONFIG.filename ? ECONFIG.filename : "[No Name]", ECONFIG.numrows);
    int rlen = snprintf(rstatus, sizeof(rstatus), "%d / %d",
        ECONFIG.cy + 1, ECONFIG.numrows);
    if (len > ECONFIG.screencols) len = ECONFIG.screencols;
    aBufferAppend(ab, status, len);
    while (len < ECONFIG.screencols) {
        if (ECONFIG.screencols - len == rlen) {
            aBufferAppend(ab, rstatus, rlen);
            break;
        }
        else {
            aBufferAppend(ab, " ", 1);
            len++;
        }
    }
    aBufferAppend(ab, "\x1b[m", 3);
    aBufferAppend(ab, "\r\n", 2);
}

void drawMessageBar(struct appendbuffer *ab) {
    aBufferAppend(ab, "\x1b[K", 3);
    int msglen = strlen(ECONFIG.statusmsg);
    if (msglen > ECONFIG.screencols) msglen = ECONFIG.screencols;
    if (msglen && time(NULL) - ECONFIG.statusmsg_time < 5) {
        aBufferAppend(ab, ECONFIG.statusmsg, msglen);
    }
}

void refreshScreen() {
    editorScroll();

    struct appendbuffer ab = APPENDBUFFER_INIT;
    aBufferAppend(&ab, "\x1b[?25l", 6); /* hide cursor */
    aBufferAppend(&ab, "\x1b[H", 3); /* position cursor at top left */
    
    drawRows(&ab);
    drawStatusBar(&ab);
    drawMessageBar(&ab);

    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (ECONFIG.cy - ECONFIG.rowoffset) + 1,
                                                (ECONFIG.rx - ECONFIG.coloffset) + 1);
    aBufferAppend(&ab, buf, strlen(buf)); /* position cursor back at top left */
    aBufferAppend(&ab, "\x1b[?25h", 6); /* show cursor */
    
    write(STDOUT_FILENO, ab.b, ab.len);
    aBufferFree(&ab);
}

void editorSetStatusMessage(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ECONFIG.statusmsg, sizeof(ECONFIG.statusmsg), fmt, ap);
    va_end(ap);
    ECONFIG.statusmsg_time = time(NULL);
}

void moveCursor(int key) {
    editorRow *row = (ECONFIG.cy >= ECONFIG.numrows) ? NULL : &ECONFIG.row[ECONFIG.cy];

    switch (key) {
        case ARROW_LEFT:
            if (ECONFIG.cx != 0) {
                ECONFIG.cx--;
            }
            else if (ECONFIG.cy > 0) {
                ECONFIG.cy--;
                ECONFIG.cx = ECONFIG.row[ECONFIG.cy].size;
            }
            break;
        case ARROW_RIGHT:
            if (row && ECONFIG.cx < row->size) {
                ECONFIG.cx++;
            }
            else if (row && ECONFIG.cx == row->size) {
                ECONFIG.cy++;
                ECONFIG.cx = 0;
            }
            break;
        case ARROW_UP:
            if (ECONFIG.cy != 0) {
                ECONFIG.cy--;
            }
            break;
        case ARROW_DOWN:
            if (ECONFIG.cy < ECONFIG.numrows) {
                ECONFIG.cy++;
            }
            break;
    }

    row = (ECONFIG.cy >= ECONFIG.numrows) ? NULL : &ECONFIG.row[ECONFIG.cy];
    int rowlen = row ? row->size : 0;
    if (ECONFIG.cx > rowlen) {
        ECONFIG.cx = rowlen;
    }
}

void processKeypress() {
    int c = readKey();

    switch (c) {
        case CTRL_KEY('q'):
            write(STDOUT_FILENO, "\x1b[2J", 4); /* clear the screen */
            write(STDOUT_FILENO, "\x1b[H", 3); /* position cursor at top left */
	    exit(0);
	    break;

        case HOME_KEY:
            ECONFIG.cx = 0;
            break;

        case END_KEY:
            if (ECONFIG.cy < ECONFIG.numrows) {
                ECONFIG.cx = ECONFIG.row[ECONFIG.cy].size;
            }
            break;
        
        case PAGE_UP:
        case PAGE_DOWN:
        {
            if (c == PAGE_UP) {
                ECONFIG.cy = ECONFIG.rowoffset;
            }
            else if (c == PAGE_DOWN) {
                ECONFIG.cy = ECONFIG.rowoffset + ECONFIG.screenrows - 1;
                if (ECONFIG.cy > ECONFIG.numrows) ECONFIG.cy = ECONFIG.numrows;
            }

            int times = ECONFIG.screenrows;
            while (times--)
                moveCursor(c == PAGE_UP ? ARROW_UP: ARROW_DOWN);
        }
        break;

        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            moveCursor(c);
            break;
    }
}

void initEditor() {
    ECONFIG.cx = 0;
    ECONFIG.cy = 0;
    ECONFIG.rx = 0;
    ECONFIG.rowoffset = 0;
    ECONFIG.coloffset = 0;
    ECONFIG.numrows = 0;
    ECONFIG.row = NULL;
    ECONFIG.filename = NULL;
    ECONFIG.statusmsg[0] = '\0';
    ECONFIG.statusmsg_time = 0;

    if (getWindowSize(&ECONFIG.screenrows, &ECONFIG.screencols) == -1) die("getWindowSize");
    ECONFIG.screenrows -= 2;
}

int main(int argc, char *argv[]) {
    initTermios();
    initEditor();
    if (argc >= 2) {
        editorOpen(argv[1]);
    }

    editorSetStatusMessage("HELP: Ctrl-Q = quit");

    while (1) {
        refreshScreen();
        processKeypress();
    }

    return 0;
}

