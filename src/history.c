#include "history.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int usable_question(const char *text)
{
    size_t index;

    if (text == NULL || text[0] == '\0') {
        return 0;
    }
    for (index = 0; text[index] != '\0'; index++) {
        unsigned char byte = (unsigned char)text[index];

        if (byte < 32 || byte > 126 || index > 238) {
            return 0;
        }
    }
    return 1;
}

const char *zell_history_state_path(void)
{
    const char *env = getenv("KILIX_ZELL_STATE");
    const char *home;
    static char path[400];
    int wrote;

    if (env != NULL && env[0] != '\0') {
        wrote = snprintf(path, sizeof path, "%s/history.json", env);
    } else {
        home = getenv("GPU_TERMINAL_HOME");
        if (home != NULL && home[0] != '\0') {
            wrote = snprintf(path, sizeof path,
                             "%s/kilix-zell/state/history.json", home);
        } else {
            home = getenv("HOME");
            if (home == NULL || home[0] == '\0') {
                home = ".";
            }
            wrote = snprintf(path, sizeof path,
                             "%s/.local/gpu_terminal/kilix-zell/state/history.json",
                             home);
        }
    }
    if (wrote < 0 || (size_t)wrote >= sizeof path) {
        return "history.json";
    }
    return path;
}

static int read_json_string(const char **cursor, char *out, size_t cap)
{
    const char *text;
    size_t length = 0;

    if (cursor == NULL || *cursor == NULL || out == NULL || cap < 2) {
        return 0;
    }
    text = *cursor;
    if (*text != '"') {
        return 0;
    }
    text += 1;
    while (*text != '\0' && *text != '"') {
        char byte = *text;

        text += 1;
        if (byte == '\\') {
            if (*text == '\0') {
                return 0;
            }
            if (*text == 'n') {
                byte = '\n';
            } else if (*text == 't') {
                byte = '\t';
            } else {
                byte = *text;
            }
            text += 1;
        }
        if ((unsigned char)byte < 32 || length + 1 >= cap) {
            return 0;
        }
        out[length] = byte;
        length += 1;
    }
    if (*text != '"' || length == 0) {
        return 0;
    }
    out[length] = '\0';
    *cursor = text + 1;
    return 1;
}

static const char *after_key(const char *text, const char *key)
{
    char pattern[24];
    const char *found;
    int wrote;

    wrote = snprintf(pattern, sizeof pattern, "\"%s\"", key);
    if (wrote < 0 || (size_t)wrote >= sizeof pattern) {
        return NULL;
    }
    found = strstr(text, pattern);
    if (found == NULL) {
        return NULL;
    }
    found += strlen(pattern);
    while (*found == ' ' || *found == '\t' || *found == '\n' || *found == ':') {
        found += 1;
    }
    return found;
}

static void conversation_string(FILE *stream,const char *text)
{
    fputc('"',stream);
    for(const unsigned char *p=(const unsigned char *)text;*p;p++){
        if(*p=='"'||*p=='\\')fputc('\\',stream);
        if(*p<32)fprintf(stream,"\\u%04x",(unsigned)*p);
        else fputc(*p,stream);
    }
    fputc('"',stream);
}

int zell_conversation_save(const char *question,const char *answer)
{
    char path[512],settings[128];
    snprintf(path,sizeof path,"%s",zell_history_state_path());
    char *name=strrchr(path,'/');name=name?name+1:path;
    size_t left=sizeof path-(size_t)(name-path);
    snprintf(name,left,"conversation-settings.json");
    int fd=open(path,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    if(fd<0)return 0;
    ssize_t n=read(fd,settings,sizeof settings-1);close(fd);
    if(n<=0)return 0;
    settings[n]=0;const char *enabled=after_key(settings,"enabled");
    if(!enabled||strncmp(enabled,"true",4))return 0;
    snprintf(name,left,"wizard-conversations.jsonl");
    fd=open(path,O_WRONLY|O_CREAT|O_APPEND|O_NOFOLLOW|O_CLOEXEC,0600);
    if(fd<0)return -1;
    struct stat st;
    if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_uid!=getuid()||fchmod(fd,0600)){
        close(fd);return -1;
    }
    /* Bounded private journal. Retain the newest cycle at the size limit. */
    if(st.st_size>1024*1024&&ftruncate(fd,0)){close(fd);return -1;}
    FILE *stream=fdopen(fd,"a");if(!stream){close(fd);return -1;}
    fprintf(stream,"{\"station\":10,\"time\":%lld,\"role\":\"wizard\",\"question\":",(long long)time(NULL));
    conversation_string(stream,question?question:"");
    fputs(",\"text\":",stream);conversation_string(stream,answer?answer:"");
    fputs("}\n",stream);
    return fclose(stream)==0?1:-1;
}

int zell_history_parse(const char *text, int *enabled,
                       zell_history_call *calls, int cap, int *count)
{
    const char *cursor;
    const char *flag;

    if (enabled == NULL || count == NULL || (calls == NULL && cap > 0)) {
        return -1;
    }
    *enabled = 0;
    *count = 0;
    if (text == NULL || strstr(text, ZELL_HISTORY_SCHEMA) == NULL) {
        return -1;
    }
    flag = after_key(text, "enabled");
    if (flag == NULL) {
        return -1;
    }
    if (strncmp(flag, "true", 4) == 0) {
        *enabled = 1;
    } else if (strncmp(flag, "false", 5) == 0) {
        *enabled = 0;
    } else {
        return -1;
    }
    cursor = text;
    while (cursor != NULL && *count < cap) {
        const char *key = after_key(cursor, "text");
        const char *tail;
        const char *count_at;
        char question[240];
        int used = 0;

        if (key == NULL || *key == '\0') {
            break;
        }
        if (!read_json_string(&key, question, sizeof question)) {
            cursor = key + 1;
            continue;
        }
        if (!usable_question(question)) {
            cursor = key;
            continue;
        }
        tail = strstr(key, "\"text\"");
        count_at = after_key(key, "count");
        if (count_at == NULL || (tail != NULL && count_at > tail)) {
            cursor = key;
            continue;
        }
        if (*count_at == '-') {
            cursor = key;
            continue;
        }
        while (*count_at >= '0' && *count_at <= '9') {
            if (used > 100000) {
                used = 1000000;
                break;
            }
            used = used * 10 + (*count_at - '0');
            count_at += 1;
        }
        memset(calls[*count].text, 0, sizeof calls[*count].text);
        memcpy(calls[*count].text, question, strlen(question));
        calls[*count].count = used;
        *count += 1;
        cursor = key;
    }
    return 1;
}

int zell_history_load(const char *path, int *enabled,
                      zell_history_call *calls, int cap, int *count)
{
    FILE *file;
    char text[8192];
    size_t got;

    if (enabled == NULL || count == NULL) {
        return -1;
    }
    *enabled = 0;
    *count = 0;
    if (path == NULL || path[0] == '\0') {
        return -1;
    }
    file = fopen(path, "r");
    if (file == NULL) {
        return 0;
    }
    got = fread(text, 1, sizeof text - 1, file);
    fclose(file);
    text[got] = '\0';
    if (zell_history_parse(text, enabled, calls, cap, count) < 0) {
        *enabled = 0;
        *count = 0;
        return -1;
    }
    return 1;
}

static int mkdir_one(const char *path)
{
    if (mkdir(path, (mode_t)0700) == 0 || errno == EEXIST) {
        return 0;
    }
    return -1;
}

static int mkdir_parent(const char *file_path)
{
    char path[400];
    size_t length;
    size_t index;

    length = strlen(file_path);
    if (length == 0 || length >= sizeof path) {
        return -1;
    }
    memcpy(path, file_path, length + 1);
    for (index = 1; index < length; index++) {
        if (path[index] != '/') {
            continue;
        }
        path[index] = '\0';
        if (path[0] != '\0' && mkdir_one(path) != 0) {
            return -1;
        }
        path[index] = '/';
    }
    return 0;
}

static int write_escaped(FILE *file, const char *text)
{
    size_t index;

    if (fputc('"', file) == EOF) {
        return -1;
    }
    for (index = 0; text[index] != '\0'; index++) {
        char byte = text[index];

        if (byte == '"' || byte == '\\') {
            if (fprintf(file, "\\%c", byte) < 0) {
                return -1;
            }
        } else if (fprintf(file, "%c", byte) < 0) {
            return -1;
        }
    }
    if (fputc('"', file) == EOF) {
        return -1;
    }
    return 0;
}

int zell_history_save(const char *path, int enabled,
                      const zell_history_call *calls, int count)
{
    char temporary[420];
    FILE *file;
    int descriptor;
    int wrote;
    int wrote_call = 0;
    int index;

    if (path == NULL || path[0] == '\0') {
        return -1;
    }
    if (count < 0) {
        count = 0;
    }
    if (count > ZELL_HISTORY_CAP) {
        count = ZELL_HISTORY_CAP;
    }
    if (count > 0 && calls == NULL) {
        return -1;
    }
    if (mkdir_parent(path) != 0) {
        return -1;
    }
    wrote = snprintf(temporary, sizeof temporary, "%s.tmp", path);
    if (wrote < 0 || (size_t)wrote >= sizeof temporary) {
        return -1;
    }
    descriptor = open(temporary, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW,
                      (mode_t)0600);
    if (descriptor < 0) {
        return -1;
    }
    file = fdopen(descriptor, "w");
    if (file == NULL) {
        close(descriptor);
        return -1;
    }
    if (fprintf(file,
                "{\n"
                "  \"schema\": \"%s\",\n"
                "  \"enabled\": %s,\n"
                "  \"calls\": [\n",
                ZELL_HISTORY_SCHEMA,
                enabled ? "true" : "false") < 0) {
        fclose(file);
        unlink(temporary);
        return -1;
    }
    for (index = 0; index < count; index++) {
        if (!usable_question(calls[index].text)) {
            continue;
        }
        if (wrote_call && fputs(",\n", file) == EOF) {
            fclose(file);
            unlink(temporary);
            return -1;
        }
        if (fprintf(file, "    {\"text\": ") < 0 ||
            write_escaped(file, calls[index].text) != 0 ||
            fprintf(file, ", \"count\": %d}", calls[index].count) < 0) {
            fclose(file);
            unlink(temporary);
            return -1;
        }
        wrote_call = 1;
    }
    if (wrote_call && fputc('\n', file) == EOF) {
        fclose(file);
        unlink(temporary);
        return -1;
    }
    if (fprintf(file, "  ]\n}\n") < 0 ||
        fflush(file) != 0 || fsync(fileno(file)) != 0 || fclose(file) != 0) {
        unlink(temporary);
        return -1;
    }
    if (rename(temporary, path) != 0) {
        unlink(temporary);
        return -1;
    }
    return 0;
}

int zell_history_find(const zell_history_call *calls, int count,
                      const char *text)
{
    int index;

    if (calls == NULL || text == NULL) {
        return -1;
    }
    for (index = 0; index < count; index++) {
        if (strcmp(calls[index].text, text) == 0) {
            return index;
        }
    }
    return -1;
}

int zell_history_remember(zell_history_call *calls, int *count, int cap,
                          const char *text, int amount)
{
    int slot;

    if (calls == NULL || count == NULL || cap < 1 || !usable_question(text)) {
        return -1;
    }
    if (*count < 0) {
        *count = 0;
    }
    if (*count > cap) {
        *count = cap;
    }
    if (amount < 0) {
        amount = 0;
    }
    slot = zell_history_find(calls, *count, text);
    if (slot >= 0) {
        if (amount > 0 && calls[slot].count < 1000000 - amount) {
            calls[slot].count += amount;
        }
        return slot;
    }
    if (*count >= cap) {
        return -1;
    }
    slot = *count;
    memset(calls[slot].text, 0, sizeof calls[slot].text);
    memcpy(calls[slot].text, text, strlen(text));
    calls[slot].count = amount;
    *count += 1;
    return slot;
}

void zell_history_order(const zell_history_call *calls, int count, int *order)
{
    int index;

    if (order == NULL || count < 1) {
        return;
    }
    if (count > ZELL_HISTORY_CAP) {
        count = ZELL_HISTORY_CAP;
    }
    for (index = 0; index < count; index++) {
        order[index] = index;
    }
    for (index = 1; index < count; index++) {
        int key = order[index];
        int place = index;

        while (place > 0) {
            int previous = order[place - 1];
            int earlier = 0;

            if (calls != NULL &&
                (calls[key].count > calls[previous].count ||
                 (calls[key].count == calls[previous].count && key < previous))) {
                earlier = 1;
            }
            if (!earlier) {
                break;
            }
            order[place] = previous;
            place -= 1;
        }
        order[place] = key;
    }
}
