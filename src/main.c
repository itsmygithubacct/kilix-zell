#include "draw.h"
#ifdef ZELL_RENDER_NEURONTE
#include "draw_neuronte.h"
#endif
#include "router.h"
#include "scene.h"
#include "voice.h"

#include "kitty_framebuffer.h"
#include "kitty_keyboard_posix.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static kittyfb_session framebuffer;
static kittykb_terminal keyboard;
static volatile sig_atomic_t stop_requested;

static void on_signal(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

enum { ZELL_PHASE_ASK = 0, ZELL_PHASE_LLM = 1 };

typedef struct zell_proc {
    pid_t pid;
    int out_fd;
    int err_fd;
    int64_t started_ms;
    int limit_ms;
    bool active;
} zell_proc;

typedef struct zell_turn {
    zell_request request;
    char text[6000];
    size_t length;
    int stdout_bytes;
    char err_text[1200];
    size_t err_length;
    int phase;
    int slot;
    int slot_count;
    zell_router_slot slots[ZELL_ROUTER_SLOTS];
    bool pending;
} zell_turn;

static void copy_text(char *destination, size_t capacity, const char *source)
{
    size_t length;

    if (capacity == 0) {
        return;
    }
    if (source == NULL) {
        destination[0] = '\0';
        return;
    }
    length = strlen(source);
    if (length >= capacity) {
        length = capacity - 1;
    }
    memcpy(destination, source, length);
    destination[length] = '\0';
}

static void proc_close(zell_proc *proc)
{
    if (proc->out_fd >= 0) {
        close(proc->out_fd);
    }
    if (proc->err_fd >= 0) {
        close(proc->err_fd);
    }
    proc->out_fd = -1;
    proc->err_fd = -1;
    proc->pid = 0;
    proc->active = false;
}

static void proc_stop(zell_proc *proc)
{
    int spins;

    if (!proc->active) {
        proc_close(proc);
        return;
    }
    kill(-proc->pid, SIGTERM);
    for (spins = 0; spins < 20; spins++) {
        if (waitpid(proc->pid, NULL, WNOHANG) == proc->pid) {
            proc_close(proc);
            return;
        }
        struct timespec pause = { .tv_nsec = 10000000 };
        nanosleep(&pause, NULL);
    }
    kill(-proc->pid, SIGKILL);
    waitpid(proc->pid, NULL, 0);
    proc_close(proc);
}

typedef struct zell_speech {
    pid_t pid;
    bool active;
    char dir[64];
    char text[6000];
} zell_speech;

static void speech_clear_dir(zell_speech *speech)
{
    char wav[80];
    int wrote;

    if (speech->dir[0] == '\0') {
        return;
    }
    wrote = snprintf(wav, sizeof wav, "%s/answer.wav", speech->dir);
    if (wrote > 0 && (size_t)wrote < sizeof wav) {
        unlink(wav);
    }
    rmdir(speech->dir);
    speech->dir[0] = '\0';
}

static void speech_stop(zell_speech *speech)
{
    int spins;

    if (!speech->active) {
        speech_clear_dir(speech);
        return;
    }
    kill(-speech->pid, SIGTERM);
    for (spins = 0; spins < 20; spins++) {
        if (waitpid(speech->pid, NULL, WNOHANG) == speech->pid) {
            speech->pid = 0;
            speech->active = false;
            speech_clear_dir(speech);
            return;
        }
        struct timespec pause = { .tv_nsec = 10000000 };
        nanosleep(&pause, NULL);
    }
    kill(-speech->pid, SIGKILL);
    waitpid(speech->pid, NULL, 0);
    speech->pid = 0;
    speech->active = false;
    speech_clear_dir(speech);
}

static void speech_poll(zell_speech *speech)
{
    int status;
    pid_t got;

    if (!speech->active) {
        return;
    }
    got = waitpid(speech->pid, &status, WNOHANG);
    if (got == 0 || (got < 0 && errno == EINTR)) {
        return;
    }
    speech->pid = 0;
    speech->active = false;
    speech_clear_dir(speech);
}

static int player_is_aplay(const char *player)
{
    size_t length = strlen(player);

    if (length >= 6 && strcmp(player + length - 6, "paplay") == 0) {
        return 0;
    }
    return length >= 5 && strcmp(player + length - 5, "aplay") == 0;
}

static void silence_stdio(void)
{
    int fd = open("/dev/null", O_RDWR);

    if (fd < 0 ||
        dup2(fd, STDIN_FILENO) < 0 ||
        dup2(fd, STDOUT_FILENO) < 0 ||
        dup2(fd, STDERR_FILENO) < 0) {
        _exit(127);
    }
    if (fd > STDERR_FILENO) {
        close(fd);
    }
}

static void speech_child(const zell_speech *speech, const zell_voice_runtime *runtime)
{
    char wav[80];
    const char *argv[14];
    const char *play[4];
    pid_t synth;
    int status;
    int wrote;

    if (setpgid(0, 0) != 0) {
        _exit(127);
    }
    silence_stdio();
    wrote = snprintf(wav, sizeof wav, "%s/answer.wav", speech->dir);
    if (wrote < 0 || (size_t)wrote >= sizeof wav) {
        _exit(127);
    }
    if (zell_voice_synth_argv(runtime, speech->text, wav, argv, 14) < 0) {
        _exit(127);
    }
    synth = fork();
    if (synth < 0) {
        _exit(127);
    }
    if (synth == 0) {
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    if (waitpid(synth, &status, 0) != synth ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        unlink(wav);
        _exit(127);
    }
    play[0] = runtime->player;
    if (player_is_aplay(runtime->player)) {
        play[1] = "-q";
        play[2] = wav;
        play[3] = NULL;
    } else {
        play[1] = wav;
        play[2] = NULL;
    }
    execvp(play[0], (char *const *)play);
    unlink(wav);
    _exit(127);
}

static int speech_start(zell_speech *speech, const char *text)
{
    zell_voice_runtime runtime;
    char template[] = "/tmp/zell-voice-XXXXXX";
    char *dir;
    pid_t pid;

    if (!zell_voice_prepare(&runtime)) {
        return 0;
    }
    speech_stop(speech);
    copy_text(speech->text, sizeof speech->text, text);
    dir = mkdtemp(template);
    if (dir == NULL) {
        return 0;
    }
    copy_text(speech->dir, sizeof speech->dir, dir);
    pid = fork();
    if (pid < 0) {
        speech_clear_dir(speech);
        return 0;
    }
    if (pid == 0) {
        zell_voice_runtime owned = runtime;
        speech_child(speech, &owned);
        _exit(127);
    }
    setpgid(pid, pid);
    speech->pid = pid;
    speech->active = true;
    return 1;
}

static void sync_voice(zell_scene *scene, zell_speech *speech, unsigned *spoken)
{
    if (scene->voice_changed) {
        zell_voice_save(zell_voice_state_path(), scene->voice_on ? 1 : 0);
        scene->voice_changed = false;
        if (!scene->voice_on) {
            speech_stop(speech);
        }
    }
    if (scene->answer_serial != *spoken) {
        *spoken = scene->answer_serial;
        speech_stop(speech);
        if (zell_voice_should_speak(scene->voice_on ? 1 : 0, scene->answer)) {
            speech_start(speech, scene->answer);
        }
    }
    speech_poll(speech);
}

static void load_voice(zell_scene *scene)
{
    int enabled = 1;
    int loaded = zell_voice_load(zell_voice_state_path(), &enabled);

    scene->voice_on = enabled != 0;
    scene->voice_changed = false;
    if (loaded == 0) {
        zell_voice_save(zell_voice_state_path(), scene->voice_on ? 1 : 0);
    }
}

static bool set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static void append_out(zell_turn *turn, const char *bytes, size_t count)
{
    size_t room;

    if (count == 0) {
        return;
    }
    turn->stdout_bytes = 1;
    room = sizeof turn->text - turn->length - 1;
    if (count > room) {
        count = room;
    }
    if (count == 0) {
        return;
    }
    memcpy(turn->text + turn->length, bytes, count);
    turn->length += count;
    turn->text[turn->length] = '\0';
}

static void append_err(zell_turn *turn, const char *bytes, size_t count)
{
    size_t room = sizeof turn->err_text - 1;
    size_t drop;

    if (count > 512) {
        count = 512;
    }
    if (count == 0 || count > room) {
        return;
    }
    if (turn->err_length + count > room) {
        drop = turn->err_length + count - room;
        memmove(turn->err_text, turn->err_text + drop, turn->err_length - drop);
        turn->err_length -= drop;
    }
    memcpy(turn->err_text + turn->err_length, bytes, count);
    turn->err_length += count;
    turn->err_text[turn->err_length] = '\0';
}

static void drain(zell_proc *proc, zell_turn *turn)
{
    char buffer[512];

    if (proc->out_fd >= 0) {
        for (;;) {
            ssize_t count = read(proc->out_fd, buffer, sizeof buffer);
            if (count <= 0) {
                break;
            }
            append_out(turn, buffer, (size_t)count);
        }
    }
    if (proc->err_fd >= 0) {
        for (;;) {
            ssize_t count = read(proc->err_fd, buffer, sizeof buffer);
            if (count <= 0) {
                break;
            }
            append_err(turn, buffer, (size_t)count);
        }
    }
}

static bool proc_start(zell_proc *proc, const zell_request *request, int limit_ms)
{
    int out_pipe[2];
    int err_pipe[2];
    const char *argv[8];
    pid_t pid;
    int devnull;

    if (zell_request_argv(request, argv, 8) < 0) {
        return false;
    }
    if (pipe(out_pipe) != 0) {
        return false;
    }
    if (pipe(err_pipe) != 0) {
        close(out_pipe[0]);
        close(out_pipe[1]);
        return false;
    }
    pid = fork();
    if (pid < 0) {
        close(out_pipe[0]);
        close(out_pipe[1]);
        close(err_pipe[0]);
        close(err_pipe[1]);
        return false;
    }
    if (pid == 0) {
        if (setpgid(0, 0) != 0) {
            _exit(127);
        }
        devnull = open("/dev/null", O_RDONLY);
        if (devnull < 0 ||
            dup2(devnull, STDIN_FILENO) < 0 ||
            dup2(out_pipe[1], STDOUT_FILENO) < 0 ||
            dup2(err_pipe[1], STDERR_FILENO) < 0) {
            _exit(127);
        }
        if (devnull > STDERR_FILENO) {
            close(devnull);
        }
        close(out_pipe[0]);
        close(out_pipe[1]);
        close(err_pipe[0]);
        close(err_pipe[1]);
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    setpgid(pid, pid);
    close(out_pipe[1]);
    close(err_pipe[1]);
    if (!set_nonblock(out_pipe[0]) || !set_nonblock(err_pipe[0])) {
        close(out_pipe[0]);
        close(err_pipe[0]);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return false;
    }
    proc->pid = pid;
    proc->out_fd = out_pipe[0];
    proc->err_fd = err_pipe[0];
    proc->started_ms = now_ms();
    proc->limit_ms = limit_ms;
    proc->active = true;
    return true;
}

static void turn_clear_text(zell_turn *turn)
{
    turn->length = 0;
    turn->text[0] = '\0';
    turn->stdout_bytes = 0;
    turn->err_length = 0;
    turn->err_text[0] = '\0';
}

static long elapsed_ms(const zell_proc *proc)
{
    int64_t delta = now_ms() - proc->started_ms;

    if (delta < 0) {
        return 0;
    }
    if (delta > 86400000) {
        return 86400000;
    }
    return (long)delta;
}

static int more_slots(const zell_turn *turn)
{
    return turn->slot + 1 < turn->slot_count;
}

static bool start_llm(zell_proc *proc, zell_turn *turn, zell_scene *scene)
{
    const char *model_id;

    if (turn->slot < 0 || turn->slot >= turn->slot_count) {
        return false;
    }
    model_id = turn->slots[turn->slot].id;
    turn->phase = ZELL_PHASE_LLM;
    turn->request.route = ZELL_ROUTE_LLM;
    copy_text(turn->request.model, sizeof turn->request.model, model_id);
    scene->route = ZELL_ROUTE_LLM;
    zell_scene_set_answerer(scene, zell_model_label(model_id));
    return proc_start(proc, &turn->request, 180000);
}

static void fail_start(zell_turn *turn, zell_scene *scene)
{
    zell_scene_finish_request(scene, "Could not start the question.");
    turn->pending = false;
}

static void fall_through(zell_proc *proc, zell_turn *turn, zell_scene *scene)
{
    turn->slot_count = zell_router_prepare(zell_models_dir(), turn->slots,
                                           ZELL_ROUTER_SLOTS);
    turn->slot = 0;
    turn_clear_text(turn);
    if (!start_llm(proc, turn, scene)) {
        fail_start(turn, scene);
    }
}

static void begin_next_model(zell_proc *proc, zell_turn *turn, zell_scene *scene)
{
    turn->slot += 1;
    turn_clear_text(turn);
    if (!start_llm(proc, turn, scene)) {
        fail_start(turn, scene);
    }
}

static void finish_output(zell_turn *turn, zell_scene *scene, int status)
{
    char line[80];

    if (turn->length > 0) {
        zell_scene_finish_request(scene, turn->text);
    } else if (zell_ask_succeeded(0, turn->err_text)) {
        zell_scene_finish_request(scene, turn->err_text);
    } else if (status == 0) {
        zell_scene_finish_request(scene, "No answer.");
    } else {
        snprintf(line, sizeof line, "The question failed (status %d).", status);
        zell_scene_finish_request(scene, line);
    }
    turn->pending = false;
}

static void pump(zell_proc *proc, zell_turn *turn, zell_scene *scene)
{
    int status = 0;
    pid_t got;

    if (!proc->active) {
        return;
    }
    drain(proc, turn);
    if (turn->phase == ZELL_PHASE_LLM &&
        zell_router_should_switch(turn->slots[turn->slot].superior,
                                  turn->stdout_bytes, elapsed_ms(proc),
                                  more_slots(turn))) {
        proc_stop(proc);
        begin_next_model(proc, turn, scene);
        return;
    }
    if (now_ms() - proc->started_ms > proc->limit_ms) {
        proc_stop(proc);
        zell_scene_finish_request(scene, "The question took too long.");
        turn->pending = false;
        return;
    }
    got = waitpid(proc->pid, &status, WNOHANG);
    if (got == 0 || (got < 0 && errno == EINTR)) {
        return;
    }
    if (got == proc->pid) {
        drain(proc, turn);
        if (WIFEXITED(status)) {
            status = WEXITSTATUS(status);
        } else {
            status = 1;
        }
    } else {
        status = 1;
    }
    proc_close(proc);
    if (turn->phase == ZELL_PHASE_ASK) {
        if (zell_ask_succeeded(status, turn->text)) {
            zell_scene_finish_request(scene, turn->text);
            turn->pending = false;
            return;
        }
        fall_through(proc, turn, scene);
        return;
    }
    if (turn->stdout_bytes == 0 && turn->slots[turn->slot].superior &&
        more_slots(turn)) {
        begin_next_model(proc, turn, scene);
        return;
    }
    finish_output(turn, scene, status);
}

static unsigned typed_character(const kittykb_event *event)
{
    if (event->text_length > 0 && event->text[0] >= 32 && event->text[0] <= 126) {
        return event->text[0];
    }
    if ((event->modifiers & KITTYKB_MOD_SHIFT) != 0 &&
        event->shifted_key >= 32 && event->shifted_key <= 126) {
        return event->shifted_key;
    }
    if (event->key >= 32 && event->key <= 126) {
        return event->key;
    }
    return 0;
}

static void read_controls(zell_scene *scene, zell_controls *controls)
{
    kittykb_event event;

    memset(controls, 0, sizeof *controls);
    bool typing = scene->mode == ZELL_MODE_QUESTION || scene->mode == ZELL_MODE_TYPE;

    if (kittykb_input_key_down(&keyboard.input, KITTYKB_KEY_LEFT) ||
        (!typing && (kittykb_input_key_down(&keyboard.input, (uint32_t)'a') ||
                     kittykb_input_key_down(&keyboard.input, (uint32_t)'A')))) {
        controls->move_x -= 1;
    }
    if (kittykb_input_key_down(&keyboard.input, KITTYKB_KEY_RIGHT) ||
        (!typing && (kittykb_input_key_down(&keyboard.input, (uint32_t)'d') ||
                     kittykb_input_key_down(&keyboard.input, (uint32_t)'D')))) {
        controls->move_x += 1;
    }
    if (kittykb_input_key_down(&keyboard.input, KITTYKB_KEY_UP) ||
        (!typing && (kittykb_input_key_down(&keyboard.input, (uint32_t)'w') ||
                     kittykb_input_key_down(&keyboard.input, (uint32_t)'W')))) {
        controls->move_y -= 1;
    }
    if (kittykb_input_key_down(&keyboard.input, KITTYKB_KEY_DOWN) ||
        (!typing && (kittykb_input_key_down(&keyboard.input, (uint32_t)'s') ||
                     kittykb_input_key_down(&keyboard.input, (uint32_t)'S')))) {
        controls->move_y += 1;
    }
    while (kittykb_input_next(&keyboard.input, &event)) {
        if (event.action == KITTYKB_ACTION_RELEASE) {
            continue;
        }
        if (event.key == KITTYKB_KEY_ESCAPE && event.action == KITTYKB_ACTION_PRESS) {
            controls->cancel = true;
        } else if (event.key == KITTYKB_KEY_ENTER &&
                   event.action == KITTYKB_ACTION_PRESS) {
            controls->confirm = true;
        } else if (event.key == KITTYKB_KEY_BACKSPACE &&
                   (scene->mode == ZELL_MODE_TYPE ||
                    scene->mode == ZELL_MODE_QUESTION)) {
            controls->backspace = true;
        } else if (scene->mode == ZELL_MODE_TYPE ||
                   scene->mode == ZELL_MODE_QUESTION) {
            unsigned character = typed_character(&event);
            if (character != 0) {
                controls->character = character;
            }
        }
    }
}

static bool present_scene(zell_scene *scene, sr_canvas *canvas, uint8_t **rgba,
                          size_t *rgba_bytes)
{
    int width = kittyfb_width(&framebuffer);
    int height = kittyfb_height(&framebuffer);
    size_t bytes;
    int resized_w = 0;
    int resized_h = 0;

    if (kittyfb_check_resize(&framebuffer, &resized_w, &resized_h)) {
        width = resized_w;
        height = resized_h;
    }
    if (width < 1 || height < 1) {
        return false;
    }
    if (scene->width != width || scene->height != height) {
        zell_scene_resize(scene, width, height);
    }
    if (canvas->w != width || canvas->h != height) {
        sr_canvas_free(canvas);
        if (!sr_canvas_init(canvas, width, height)) {
            return false;
        }
    }
    bytes = (size_t)width * (size_t)height * 4u;
    if (*rgba_bytes != bytes) {
        uint8_t *grown = realloc(*rgba, bytes);
        if (grown == NULL) {
            return false;
        }
        *rgba = grown;
        *rgba_bytes = bytes;
    }
    zell_draw(scene, canvas);
    if (!sr_pack_rgba(canvas, *rgba, bytes)) {
        return false;
    }
    return kittyfb_present(&framebuffer, *rgba, width, height);
}

static int run_scene(void)
{
    kittyfb_options fb_options;
    kittykb_terminal_options key_options;
    zell_scene scene;
    zell_proc proc;
    zell_turn turn;
    zell_speech speech;
    unsigned spoken = 0;
    sr_canvas canvas;
    uint8_t *rgba = NULL;
    size_t rgba_bytes = 0;
    struct sigaction action;

    memset(&canvas, 0, sizeof canvas);
    memset(&proc, 0, sizeof proc);
    memset(&turn, 0, sizeof turn);
    memset(&speech, 0, sizeof speech);
    proc.out_fd = -1;
    proc.err_fd = -1;
    kittyfb_session_init(&framebuffer);
    kittyfb_options_init(&fb_options);
    fb_options.min_width = 160;
    fb_options.min_height = 120;
    fb_options.max_width = 1600;
    fb_options.max_height = 1000;
    if (kittyfb_start(&framebuffer, STDIN_FILENO, STDOUT_FILENO, &fb_options) != 0) {
        fprintf(stderr, "zell: this pane has no Kitty graphics (%s)\n",
                strerror(errno));
        return 1;
    }
    kittykb_terminal_init(&keyboard);
    kittykb_terminal_options_init(&key_options);
    key_options.flags = KITTYKB_FLAGS_KEY_STATE | KITTYKB_FLAG_REPORT_TEXT;
    key_options.make_raw = false;
    key_options.make_nonblocking = true;
    if (kittykb_terminal_start(&keyboard, STDIN_FILENO, STDOUT_FILENO,
                               &key_options) != 0) {
        kittyfb_stop(&framebuffer);
        fprintf(stderr, "zell: keyboard setup failed (%s)\n", strerror(errno));
        return 1;
    }
    memset(&action, 0, sizeof action);
    action.sa_handler = on_signal;
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    zell_scene_init(&scene, kittyfb_width(&framebuffer),
                    kittyfb_height(&framebuffer));
    load_voice(&scene);

    while (!stop_requested && !scene.leave) {
        struct pollfd poll_fd = { .fd = STDIN_FILENO, .events = POLLIN };
        zell_controls controls;

        poll(&poll_fd, 1, 16);
        if (kittykb_terminal_read(&keyboard) < 0 && errno != EINTR) {
            break;
        }
        read_controls(&scene, &controls);
        if (zell_scene_tick(&scene, &controls)) {
            break;
        }
        if (scene.mode != ZELL_MODE_WAIT) {
            if (proc.active) {
                proc_stop(&proc);
            }
            turn.pending = false;
        } else if (!proc.active && !turn.pending) {
            if (zell_scene_take_request(&scene, &turn.request)) {
                turn_clear_text(&turn);
                turn.phase = ZELL_PHASE_ASK;
                turn.slot = 0;
                turn.slot_count = 0;
                turn.pending = true;
                turn.request.route = ZELL_ROUTE_ASK;
                zell_scene_set_answerer(&scene, "Kilix Ask");
                if (!proc_start(&proc, &turn.request, 45000)) {
                    fail_start(&turn, &scene);
                }
            } else {
                zell_scene_finish_request(&scene, "No question.");
            }
        } else if (!proc.active && turn.pending) {
            fail_start(&turn, &scene);
        }
        if (proc.active) {
            pump(&proc, &turn, &scene);
        }
        sync_voice(&scene, &speech, &spoken);
        if (!present_scene(&scene, &canvas, &rgba, &rgba_bytes)) {
            break;
        }
    }

    speech_stop(&speech);
    proc_stop(&proc);
    free(rgba);
    sr_canvas_free(&canvas);
    kittykb_terminal_stop(&keyboard);
    kittyfb_stop(&framebuffer);
    return stop_requested ? 130 : 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--selftest") == 0) {
        fputs("zell: run make test\n", stdout);
        return 0;
    }
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        fputs("zell: run it inside a Kilix pane\n", stderr);
        return 1;
    }
#ifdef ZELL_RENDER_NEURONTE
    if (!zell_neuronte_open()) {
        fprintf(stderr, "zell: cannot load the Neuronte weights at %s\n"
                        "set ZELL_NEURONTE_WEIGHTS to a neuronte-split file\n",
                zell_neuronte_weights());
        return 1;
    }
#endif
    return run_scene();
}
