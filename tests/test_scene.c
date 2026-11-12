#include "draw.h"
#include "history.h"
#include "model.h"
#include "room.h"
#include "router.h"
#include "scene.h"
#include "sprites.h"
#include "voice.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures = 0;

static void expect(int condition, const char *label)
{
    if (!condition) {
        fprintf(stderr, "FAIL %s\n", label);
        failures += 1;
    }
}

static int warm_pixels(const sr_canvas *canvas, int x, int y, int width, int height);
static void copy_rect(const sr_canvas *canvas, int x, int y, int width, int height,
                      uint32_t *out);

static zell_controls controls_of(int x, int y, bool confirm, bool cancel)
{
    zell_controls controls;
    memset(&controls, 0, sizeof controls);
    controls.move_x = x;
    controls.move_y = y;
    controls.confirm = confirm;
    controls.cancel = cancel;
    return controls;
}

static void test_walk_off_the_bottom(void)
{
    zell_scene scene;
    zell_controls down = controls_of(0, 1, false, false);
    int guard = 0;

    zell_scene_init(&scene, 320, 240);
    while (!scene.leave && guard < 200) {
        zell_scene_tick(&scene, &down);
        guard += 1;
    }
    expect(scene.leave, "walking down leaves the pane");
    expect(guard < 200, "leaving does not take forever");
}

static void test_walls_block_the_walk(void)
{
    zell_scene scene;
    zell_controls left = controls_of(-1, 0, false, false);
    zell_controls up = controls_of(0, -1, false, false);
    zell_controls down = controls_of(0, 1, false, false);
    int guard;

    zell_scene_init(&scene, 320, 240);
    for (guard = 0; guard < 80; guard++) {
        zell_scene_tick(&scene, &left);
    }
    expect(scene.x > 32.0f, "the left wall stops Kilix");
    expect(!scene.leave, "a side wall is not an exit");

    zell_scene_init(&scene, 320, 240);
    scene.x = 80.0f;
    scene.y = 100.0f;
    for (guard = 0; guard < 40; guard++) {
        zell_scene_tick(&scene, &up);
    }
    expect(scene.y >= 60.0f, "the dark alcove stops Kilix");
    expect(!scene.leave, "the alcove is not an exit");

    zell_scene_init(&scene, 320, 240);
    scene.x = 48.0f;
    scene.y = 200.0f;
    for (guard = 0; guard < 40; guard++) {
        zell_scene_tick(&scene, &down);
    }
    expect(!scene.leave, "the bottom wall is not an exit");
    expect(scene.y < 240.0f, "Kilix stays inside the bottom wall");
}

static void test_fire_burns_and_respawns(void)
{
    zell_scene scene;
    zell_controls still = controls_of(0, 0, false, false);
    int guard;
    int hurt;

    zell_scene_init(&scene, 320, 240);
    expect(scene.hp == 6 && scene.max_hp == 6, "Kilix starts with six hearts");
    scene.x = 80.0f;
    scene.y = 120.0f;
    zell_scene_tick(&scene, &still);
    expect(scene.burn > 0, "touching a campfire sets Kilix on fire");
    expect(scene.hp == 5, "catching fire costs a heart");
    scene.x = 160.0f;
    scene.y = 180.0f;
    hurt = scene.hp;
    for (guard = 0; guard < 100 && scene.burn > 0; guard++) {
        zell_scene_tick(&scene, &still);
    }
    expect(scene.burn == 0, "the fire goes out after a bit");
    expect(scene.hp < hurt, "the burn keeps costing hearts");
    expect(scene.hp > 0, "one burn does not empty the hearts");

    zell_scene_init(&scene, 320, 240);
    scene.x = 80.0f;
    scene.y = 120.0f;
    for (guard = 0; guard < 200; guard++) {
        zell_scene_tick(&scene, &still);
    }
    expect(scene.hp == scene.max_hp, "zero hearts sends Kilix back at full");
    expect(scene.burn == 0, "the respawn puts the fire out");
    expect(!scene.leave, "burning out does not leave the pane");
    expect(scene.x > 140.0f && scene.x < 180.0f, "Kilix respawns in the middle");
    expect(scene.y > 160.0f, "Kilix respawns low in the cave");
}

static void test_burn_lifts_off_the_body(void)
{
    zell_scene scene;
    sr_canvas canvas;
    zell_controls still = {0};
    int head;
    int left;
    int above;
    int fire_x;
    int fire_y;
    int fire_w;
    int fire_h;
    int held_burn;
    uint32_t before[96 * 96];
    uint32_t after[96 * 96];

    zell_scene_init(&scene, 320, 240);
    expect(sr_canvas_init(&canvas, 320, 240), "burn canvas allocates");
    zell_draw(&scene, &canvas);
    head = (int)scene.y - zell_kilix_foot();
    left = (int)scene.x - 24;
    above = head - 28;
    if (above < 0) {
        above = 0;
    }
    expect(warm_pixels(&canvas, left, above, 48, head - above) < 5,
           "clear air above Kilix");
    scene.x = 80.0f;
    scene.y = 120.0f;
    zell_scene_tick(&scene, &still);
    expect(scene.burn > 0, "the body test stands in the fire");
    held_burn = scene.burn;
    scene.x = 160.0f;
    scene.y = (float)scene.height * 0.72f;
    scene.flame_tick = 7;
    scene.burn = 0;
    zell_draw(&scene, &canvas);
    zell_campfire_rect(canvas.w, canvas.h, 0, &fire_x, &fire_y, &fire_w, &fire_h);
    copy_rect(&canvas, fire_x, fire_y, fire_w, fire_h, before);
    scene.burn = held_burn;
    zell_draw(&scene, &canvas);
    copy_rect(&canvas, fire_x, fire_y, fire_w, fire_h, after);
    expect(memcmp(before, after, (size_t)fire_w * (size_t)fire_h * sizeof before[0]) == 0,
           "Kilix burning does not move the campfire");
    expect(warm_pixels(&canvas, left, above, 48, head - above) > 8,
           "flames rise off the body");
    sr_canvas_free(&canvas);
}

static void test_up_does_not_leave(void)
{
    zell_scene scene;
    zell_controls up = controls_of(0, -1, false, false);
    int guard;

    zell_scene_init(&scene, 320, 240);
    for (guard = 0; guard < 80; guard++) {
        zell_scene_tick(&scene, &up);
    }
    expect(!scene.leave, "walking up stays in the pane");
}

static void test_wizard_question_routes(void)
{
    zell_scene scene;
    zell_controls talk = controls_of(0, 0, true, false);
    zell_request request;
    const char *argv[8];
    int argc;
    int index;

    zell_scene_init(&scene, 320, 240);
    scene.x = scene.wizard_x;
    scene.y = scene.wizard_y + 4.0f;
    expect(zell_near_wizard(&scene), "standing at the wizard is near enough");
    expect(!zell_near_switch(&scene), "the wizard is away from the voice switch");
    expect(strcmp(scene.answerer, "Kilix Ask") == 0, "Kilix Ask is asked first");
    zell_scene_tick(&scene, &talk);
    expect(scene.mode == ZELL_MODE_QUESTION, "enter opens the questions");
    expect(scene.question_len == 0, "the field under Ask starts blank");
    zell_scene_tick(&scene, &talk);
    expect(scene.mode == ZELL_MODE_WAIT, "a preset question is sent");
    expect(scene.route == ZELL_ROUTE_ASK, "the question starts with Kilix Ask");
    expect(zell_scene_take_request(&scene, &request), "the request is taken once");
    expect(!zell_scene_take_request(&scene, &request), "the request is not taken twice");
    argc = zell_request_argv(&request, argv, 8);
    expect(argc == 4, "Kilix Ask has four arguments");
    expect(strcmp(argv[0], "kilix") == 0 && strcmp(argv[1], "ask") == 0 &&
           strcmp(argv[2], "--") == 0, "Kilix Ask is invoked directly");
    expect(strcmp(argv[3], "What time is it?") == 0, "the question is one argument");
    for (index = 0; index < argc; index++) {
        expect(strcmp(argv[index], "--yes") != 0, "Kilix Ask is not confirmed");
    }

    memset(&request, 0, sizeof request);
    request.route = ZELL_ROUTE_LLM;
    memcpy(request.model, "qwen3.5-0.8b", sizeof "qwen3.5-0.8b");
    memcpy(request.question, "What time is it?", sizeof "What time is it?");
    argc = zell_request_argv(&request, argv, 8);
    expect(argc == 7, "the model command has seven arguments");
    expect(strcmp(argv[0], "kilix-llm") == 0 && strcmp(argv[1], "run") == 0 &&
           strcmp(argv[2], "--model-id") == 0, "the model command names an id");
    expect(strcmp(argv[3], "qwen3.5-0.8b") == 0, "the floor model is Qwen3.5 0.8B");
    expect(strcmp(argv[4], "-n") == 0 && strcmp(argv[5], "160") == 0,
           "the model answer is capped");
    expect(strstr(argv[6], "Answer in a few sentences: What time is it?") != NULL,
           "the preset is the prompt");
    expect(argv[7] == NULL, "the model command is terminated");
    for (index = 0; index < argc; index++) {
        expect(strcmp(argv[index], "--yes") != 0, "the model command has no --yes");
    }
    memcpy(request.device, "Vulkan1", sizeof "Vulkan1");
    expect(zell_request_argv(&request, argv, 8) < 0,
           "naming a GPU needs a longer argv");
    {
        const char *wide[10];
        char name[32];
        char reply[80];
        const char *transcript =
            "\n\nLoading model...\n\n"
            "banner\n"
            "\n> Answer in a few sentences: What time is it?\n"
            "Hello! How can I help you today?\n"
            "\n[ Prompt: 1 t/s | Generation: 1 t/s ]\n"
            "\nExiting...\n";

        argc = zell_request_argv(&request, wide, 10);
        expect(argc == 9, "the model command names the GPU");
        expect(strcmp(wide[6], "--device") == 0 && strcmp(wide[7], "Vulkan1") == 0,
               "the GPU is one argument");
        expect(strstr(wide[8], "Answer in a few sentences: What time is it?") != NULL,
               "the prompt stays after the GPU");
        expect(wide[9] == NULL, "the GPU command is terminated");
        expect(zell_llm_device_from_list(
                   "Vulkan0: Intel(R) UHD Graphics 630 (CFL GT2)\n"
                   "Vulkan1: Quadro RTX 3000 (6390 MiB, 5985 MiB free)\n",
                   name, sizeof name) == 1,
               "the Quadro is chosen over the integrated GPU");
        expect(strcmp(name, "Vulkan1") == 0, "the Quadro device is Vulkan1");
        expect(zell_llm_device_from_list("Vulkan0: Intel(R) UHD Graphics\n",
                                         name, sizeof name) == 1,
               "a single GPU is used");
        expect(strcmp(name, "Vulkan0") == 0, "the only device keeps its name");
        expect(zell_llm_device_from_list(
                   "Vulkan0: AMD One\nVulkan1: AMD Other\n",
                   name, sizeof name) == 0,
               "two unmatched GPUs are not guessed");
        expect(!zell_llm_reply_started("\n\nLoading model...\n"),
               "the load spinner is not a reply");
        expect(zell_llm_reply("\n> Answer in a few sentences: Hi\n\n",
                              reply, sizeof reply) == 0,
               "a prompt echo with no reply is not an answer");
        expect(zell_llm_reply_started(transcript), "a transcript reply has started");
        expect(zell_llm_reply(transcript, reply, sizeof reply) > 0,
               "the transcript has a reply");
        expect(strcmp(reply, "Hello! How can I help you today?") == 0,
               "the reply drops the banner and the timings");
    }
    request.model[0] = '\0';
    expect(zell_request_argv(&request, argv, 8) < 0, "an LLM needs a model id");

    zell_scene_init(&scene, 320, 240);
    scene.x = scene.wizard_x;
    scene.y = scene.wizard_y;
    zell_scene_tick(&scene, &talk);
    {
        zell_controls typed = controls_of(0, 0, false, false);
        const char *word = "where is the key";
        const char *cursor;

        for (cursor = word; *cursor != '\0'; cursor++) {
            typed.character = (unsigned char)*cursor;
            zell_scene_tick(&scene, &typed);
        }
        typed.character = 0;
        typed.confirm = true;
        zell_scene_tick(&scene, &typed);
    }
    expect(scene.mode == ZELL_MODE_WAIT, "a typed question is sent");
    expect(zell_scene_take_request(&scene, &request), "the field is the question");
    expect(strcmp(request.question, "where is the key") == 0,
           "the blank field keeps the typed words");
}

static void expect_slot(const zell_router_slot *slot, const char *model_id,
                        int superior, const char *label)
{
    expect(strcmp(slot->id, model_id) == 0, label);
    expect(slot->superior == superior, label);
}

static void test_router_prefers_a_fast_model(void)
{
    int ready[ZELL_ROUTER_SLOTS];
    zell_router_slot slots[ZELL_ROUTER_SLOTS];
    int count;
    char directory[] = "/tmp/zell-router-XXXXXX";

    memset(ready, 0, sizeof ready);
    count = zell_router_fill(ready, slots, ZELL_ROUTER_SLOTS);
    expect(count == 1, "no weights still names the stock 0.8B");
    expect_slot(&slots[0], "qwen3.5-0.8b", 0, "the floor model is not superior");

    ready[1] = 1;
    count = zell_router_fill(ready, slots, ZELL_ROUTER_SLOTS);
    expect(count == 2, "an installed 4B is tried before the 0.8B");
    expect_slot(&slots[0], "qwen3.5-4b", 1, "the 4B is superior");
    expect_slot(&slots[1], "qwen3.5-0.8b", 0, "the 0.8B remains the floor");

    ready[0] = 1;
    ready[2] = 1;
    ready[3] = 1;
    count = zell_router_fill(ready, slots, ZELL_ROUTER_SLOTS);
    expect(count == 3, "27B then 4B then one 0.8B");
    expect_slot(&slots[0], "qwen3.8-27b", 1, "27B is first");
    expect_slot(&slots[1], "qwen3.5-4b", 1, "4B is second");
    expect_slot(&slots[2], "qwen3.5-0.8b", 0, "stock 0.8B wins over no-loop");

    memset(ready, 0, sizeof ready);
    ready[3] = 1;
    count = zell_router_fill(ready, slots, ZELL_ROUTER_SLOTS);
    expect(count == 1, "no-loop is the floor only when it is the only 0.8B");
    expect_slot(&slots[0], "qwen-0.8b-no-loop-02", 0, "no-loop is not superior");

    ready[1] = 1;
    count = zell_router_fill(ready, slots, ZELL_ROUTER_SLOTS);
    expect(count == 2, "4B then the installed no-loop file");
    expect_slot(&slots[0], "qwen3.5-4b", 1, "4B stays ahead of no-loop");
    expect_slot(&slots[1], "qwen-0.8b-no-loop-02", 0, "no-loop is the floor");

    expect(zell_router_should_switch(1, 0, 10001, 1) == 1,
           "a silent superior yields after 10 seconds");
    expect(zell_router_should_switch(1, 0, 10000, 1) == 0,
           "10 seconds is still its window");
    expect(zell_router_should_switch(1, 1, 20000, 1) == 0,
           "a started answer keeps the model");
    expect(zell_router_should_switch(0, 0, 20000, 1) == 0,
           "the 0.8B is not on the 10 second clock");
    expect(zell_router_should_switch(1, 0, 20000, 0) == 0,
           "the last model is not replaced");

    expect(zell_ask_succeeded(0, "2026-11-10") == 1, "Ask text is an answer");
    expect(zell_ask_succeeded(0, " \n\t") == 0, "blank Ask output is not an answer");
    expect(zell_ask_succeeded(1, "no job") == 0, "a failed Ask is not an answer");
    expect(zell_ask_succeeded(0, NULL) == 0, "missing Ask output is not an answer");

    expect(strcmp(zell_model_label("qwen3.8-27b"), "Qwen3.8 27B") == 0,
           "27B has a short label");
    expect(strcmp(zell_model_label("qwen3.5-4b"), "Qwen3.5 4B") == 0,
           "4B has a short label");
    expect(strcmp(zell_model_label("qwen3.5-0.8b"), "Qwen3.5 0.8B") == 0,
           "stock 0.8B has a short label");
    expect(strcmp(zell_model_label("qwen-0.8b-no-loop-02"), "Qwen3.5 0.8B") == 0,
           "no-loop wears the 0.8B label");

    if (mkdtemp(directory) == NULL) {
        expect(0, "empty models directory");
        return;
    }
    count = zell_router_prepare(directory, slots, ZELL_ROUTER_SLOTS);
    expect(count == 1, "an empty directory still names the stock 0.8B");
    expect_slot(&slots[0], "qwen3.5-0.8b", 0, "the empty directory is not superior");
    rmdir(directory);
}

static void test_model_file_pins_the_router(void)
{
    int ready[ZELL_ROUTER_SLOTS];
    zell_router_slot slots[ZELL_ROUTER_SLOTS];
    char directory[] = "/tmp/zell-model-XXXXXX";
    char path[320];
    char device[32];
    char model[40];
    char previous[400];
    const char *saved;
    FILE *file;
    int count;
    int wrote;

    memset(ready, 0, sizeof ready);
    ready[0] = 1;
    ready[1] = 1;
    ready[2] = 1;
    count = zell_router_fill(ready, slots, ZELL_ROUTER_SLOTS);
    expect(zell_router_pin(slots, count, "auto") == 3, "auto keeps the router");
    count = zell_router_pin(slots, count, "qwen3.5-4b");
    expect(count == 1, "a chosen model is the only slot");
    expect_slot(&slots[0], "qwen3.5-4b", 0,
                "a chosen model is not on the 10 second clock");

    memset(ready, 0, sizeof ready);
    ready[1] = 1;
    count = zell_router_fill(ready, slots, ZELL_ROUTER_SLOTS);
    expect(zell_router_pin(slots, count, "qwen3.8-27b") == count,
           "a model that is not installed keeps the router");

    expect(zell_model_parse(
               "{\n  \"schema\": \"kilix.zell.model/v1\",\n"
               "  \"device\": \"Vulkan1\",\n"
               "  \"model\": \"qwen3.5-0.8b\"\n}\n",
               device, sizeof device, model, sizeof model) == 1,
           "a model file parses");
    expect(strcmp(device, "Vulkan1") == 0 && strcmp(model, "qwen3.5-0.8b") == 0,
           "the file names the GPU and the model");
    expect(zell_model_parse("{", device, sizeof device, model, sizeof model) < 0,
           "a file without the schema is unusable");
    expect(strcmp(device, "auto") == 0 && strcmp(model, "auto") == 0,
           "an unusable file means automatic");

    if (mkdtemp(directory) == NULL) {
        expect(0, "model state directory");
        return;
    }
    saved = getenv("KILIX_ZELL_STATE");
    previous[0] = '\0';
    if (saved != NULL) {
        wrote = snprintf(previous, sizeof previous, "%s", saved);
        expect(wrote > 0 && (size_t)wrote < sizeof previous,
               "the previous state directory fits");
    }
    setenv("KILIX_ZELL_STATE", directory, 1);
    wrote = snprintf(path, sizeof path, "%s/model.json", directory);
    expect(wrote > 0 && (size_t)wrote < sizeof path, "the model path fits");
    file = fopen(path, "w");
    expect(file != NULL, "model file can be written");
    if (file != NULL) {
        fputs("{\n  \"schema\": \"kilix.zell.model/v1\",\n"
              "  \"device\": \"Vulkan1\",\n"
              "  \"model\": \"qwen3.5-4b\"\n}\n",
              file);
        fclose(file);
    }
    expect(strcmp(zell_model_state_path(), path) == 0,
           "model state stays beside the voice file");
    expect(zell_model_load(device, sizeof device, model, sizeof model) == 1,
           "the model file loads");
    expect(strcmp(device, "Vulkan1") == 0, "the loaded GPU is Vulkan1");
    expect(strcmp(model, "qwen3.5-4b") == 0, "the loaded model is the 4B");
    unlink(path);
    if (previous[0] != '\0') {
        setenv("KILIX_ZELL_STATE", previous, 1);
    } else {
        unsetenv("KILIX_ZELL_STATE");
    }
    rmdir(directory);
}

static void test_wall_switch_toggles_voice(void)
{
    zell_scene scene;
    zell_controls talk = controls_of(0, 0, true, false);
    float x;
    float y;
    int voice;

    zell_scene_init(&scene, 320, 240);
    expect(scene.voice_on, "voice starts on");
    expect(!scene.voice_changed, "startup does not mark the switch");
    expect(scene.answer_serial == 0, "nothing has been answered yet");
    zell_switch_point(scene.width, scene.height, &x, &y);
    scene.x = x;
    scene.y = y + (float)scene.height / (float)ZELL_ROOM_ROWS;
    expect(zell_near_switch(&scene), "the floor in front of the switch reaches it");
    expect(!zell_near_wizard(&scene), "the switch is not the wizard");
    zell_scene_tick(&scene, &talk);
    expect(!scene.voice_on, "enter turns the voice off");
    expect(scene.voice_changed, "the switch asks to save");
    expect(scene.mode == ZELL_MODE_WALK, "the switch stays in the walk");
    expect(!scene.leave, "the switch does not leave");
    scene.voice_changed = false;
    zell_scene_tick(&scene, &talk);
    expect(scene.voice_on, "enter turns the voice back on");
    expect(scene.voice_changed, "turning the voice on is saved too");
    voice = scene.voice_on;
    zell_scene_resize(&scene, 640, 480);
    expect(scene.voice_on == voice, "resize keeps the voice switch");
    zell_scene_finish_request(&scene, "A short answer.");
    expect(scene.answer_serial == 1, "a model answer is counted");
    expect(strcmp(scene.answer, "A short answer.") == 0, "the answer text is kept");
}

static void test_voice_file_and_paradee_argv(void)
{
    char directory[] = "/tmp/zell-voice-state-XXXXXX";
    char path[320];
    char saved_env[400];
    char text[512];
    const char *previous;
    const char *argv[16];
    zell_voice_runtime runtime;
    struct stat info;
    int enabled = 0;
    int had_env = 0;
    int argc;
    int index;
    int wrote;
    FILE *file;
    size_t count;

    expect(zell_voice_should_speak(1, "The cave is quiet.") == 1,
           "an answer is spoken");
    expect(zell_voice_should_speak(0, "The cave is quiet.") == 0,
           "voice off is silent");
    expect(zell_voice_should_speak(1, "   ") == 0, "blank text is silent");
    expect(zell_voice_should_speak(1, NULL) == 0, "missing text is silent");
    expect(zell_voice_should_speak(1, "No answer.") == 0, "no answer is silent");
    expect(zell_voice_should_speak(1, "No question.") == 0, "no question is silent");
    expect(zell_voice_should_speak(1, "Could not start the question.") == 0,
           "a missed start is silent");
    expect(zell_voice_should_speak(1, "The question took too long.") == 0,
           "a timeout is silent");
    expect(zell_voice_should_speak(1, "The question failed (status 1).") == 0,
           "a failure is silent");
    enabled = 1;
    expect(zell_voice_parse("{\"schema\":\"kilix.zell.voice/v1\","
                            "\"enabled\":false,\"voice\":\"af_heart\"}",
                            &enabled) == 1,
           "the switch is the enabled field");
    expect(enabled == 0, "a different voice name does not turn it on");

    if (mkdtemp(directory) == NULL) {
        expect(0, "voice state directory");
        return;
    }
    previous = getenv("KILIX_ZELL_STATE");
    if (previous != NULL) {
        wrote = snprintf(saved_env, sizeof saved_env, "%s", previous);
        had_env = wrote > 0 && (size_t)wrote < sizeof saved_env;
    }
    if (setenv("KILIX_ZELL_STATE", directory, 1) != 0) {
        expect(0, "voice state override");
        rmdir(directory);
        return;
    }
    wrote = snprintf(path, sizeof path, "%s/voice.json", directory);
    expect(wrote > 0 && (size_t)wrote < sizeof path, "the voice path fits");
    expect(strcmp(zell_voice_state_path(), path) == 0,
           "state uses KILIX_ZELL_STATE");
    enabled = 0;
    expect(zell_voice_load(path, &enabled) == 0, "a missing file is the default");
    expect(enabled == 1, "the default voice is on");
    expect(zell_voice_save(path, 0) == 0, "the switch writes the file");
    expect(stat(path, &info) == 0 && (info.st_mode & 0777) == 0600,
           "the voice file is private");
    file = fopen(path, "r");
    expect(file != NULL, "the voice file opens");
    if (file != NULL) {
        count = fread(text, 1, sizeof text - 1, file);
        text[count] = '\0';
        fclose(file);
        expect(strstr(text, "kilix.zell.voice/v1") != NULL,
               "the voice file has its schema");
        expect(strstr(text, "bm_fable") != NULL, "the saved voice is bm_fable");
        expect(strstr(text, "paradee-8m-v1.2") != NULL,
               "the saved model is Paradee 8M");
        expect(strstr(text, "\"enabled\": false") != NULL, "voice off is in the file");
    }
    enabled = 1;
    expect(zell_voice_load(path, &enabled) == 1, "the file is read back");
    expect(enabled == 0, "voice off is saved");
    file = fopen(path, "w");
    if (file != NULL) {
        fputs("nope", file);
        fclose(file);
    }
    enabled = 0;
    expect(zell_voice_load(path, &enabled) == -1, "a broken file is unusable");
    expect(enabled == 1, "a broken file leaves voice on");

    memset(&runtime, 0, sizeof runtime);
    memcpy(runtime.python, "/usr/lib/kilix/pd/bin/python",
           sizeof "/usr/lib/kilix/pd/bin/python");
    memcpy(runtime.synth, "/usr/lib/kilix/zell/tools/paradee_speak.py",
           sizeof "/usr/lib/kilix/zell/tools/paradee_speak.py");
    memcpy(runtime.model, "/usr/lib/kilix/paradee/onnx/paradee_int8.onnx",
           sizeof "/usr/lib/kilix/paradee/onnx/paradee_int8.onnx");
    memcpy(runtime.config, "/usr/lib/kilix/paradee/config.json",
           sizeof "/usr/lib/kilix/paradee/config.json");
    memcpy(runtime.player, "/usr/bin/paplay", sizeof "/usr/bin/paplay");
    argc = zell_voice_synth_argv(&runtime, "hello", "/tmp/answer.wav", argv, 14);
    expect(argc == 13, "Paradee has thirteen arguments");
    expect(argv[13] == NULL, "the Paradee command is terminated");
    expect(strcmp(argv[0], runtime.python) == 0, "Paradee uses its python");
    expect(strcmp(argv[1], "-I") == 0, "Paradee isolates imports");
    expect(strcmp(argv[7], "--voice") == 0 && strcmp(argv[8], "bm_fable") == 0,
           "the voice is bm_fable");
    expect(strcmp(argv[9], "--text") == 0 && strcmp(argv[10], "hello") == 0,
           "the answer is the text");
    expect(strcmp(argv[11], "--wav") == 0 &&
           strcmp(argv[12], "/tmp/answer.wav") == 0,
           "audio goes to a wav");
    expect(strstr(argv[4], "paradee_int8.onnx") != NULL,
           "the model is the 8M onnx");
    for (index = 0; index < argc; index++) {
        expect(strcmp(argv[index], "sh") != 0 &&
               strcmp(argv[index], "bash") != 0 &&
               strcmp(argv[index], "/bin/sh") != 0,
               "Paradee is not a shell");
    }
    expect(zell_voice_synth_argv(&runtime, "", "/tmp/answer.wav", argv, 14) < 0,
           "empty text is not spoken");
    expect(zell_voice_synth_argv(&runtime, "hello", "/tmp/answer.wav", argv, 13) < 0,
           "a short argv is refused");

    if (had_env) {
        setenv("KILIX_ZELL_STATE", saved_env, 1);
    } else {
        unsetenv("KILIX_ZELL_STATE");
    }
    unlink(path);
    rmdir(directory);
}

static void test_escape_steps_back(void)
{
    zell_scene scene;
    zell_controls talk = controls_of(0, 0, true, false);
    zell_controls back = controls_of(0, 0, false, true);

    zell_scene_init(&scene, 320, 240);
    scene.x = scene.wizard_x;
    scene.y = scene.wizard_y;
    zell_scene_tick(&scene, &talk);
    zell_scene_tick(&scene, &back);
    expect(scene.mode == ZELL_MODE_WALK, "escape leaves the menu");
    expect(!scene.leave, "escape does not leave the pane");
}

static int fringe_pixels(const uint32_t *pixels, int count)
{
    int found = 0;
    int index;

    for (index = 0; index < count; index++) {
        uint32_t pixel = pixels[index];
        int alpha = (int)(pixel >> 24);
        int red = (int)((pixel >> 16) & 255u);
        int green = (int)((pixel >> 8) & 255u);
        int blue = (int)(pixel & 255u);
        int spill;
        int gap;

        if (alpha < 128 || red < 24 || blue < 24) {
            continue;
        }
        spill = (red < blue ? red : blue) - green;
        gap = red > blue ? red - blue : blue - red;
        if ((spill > 55 && green < 90 && gap < 80) ||
            (green < 16 && spill > 24 && gap < 40)) {
            found += 1;
        }
    }
    return found;
}

static int pixels_where(const sr_canvas *canvas, int x, int y, int width, int height,
                        int (*match)(uint32_t pixel))
{
    int count = 0;
    int row;
    int col;

    for (row = y; row < y + height; row++) {
        if (row < 0 || row >= canvas->h) {
            continue;
        }
        for (col = x; col < x + width; col++) {
            if (col < 0 || col >= canvas->w) {
                continue;
            }
            if (match(canvas->px[row * canvas->w + col]) != 0) {
                count += 1;
            }
        }
    }
    return count;
}

static int is_flame_core(uint32_t pixel)
{
    int red = (int)((pixel >> 16) & 255u);
    int green = (int)((pixel >> 8) & 255u);
    int blue = (int)(pixel & 255u);

    return red > 240 && green > 190 && blue < 130 && green > blue + 40;
}

static int warm_pixels(const sr_canvas *canvas, int x, int y, int width, int height)
{
    return pixels_where(canvas, x, y, width, height, is_flame_core);
}

static void copy_rect(const sr_canvas *canvas, int x, int y, int width, int height,
                      uint32_t *out)
{
    int row;
    int col;
    int count = 0;

    for (row = y; row < y + height; row++) {
        for (col = x; col < x + width; col++) {
            out[count] = canvas->px[row * canvas->w + col];
            count += 1;
        }
    }
}

static int colored_in(const sr_canvas *canvas, int x, int y, int width, int height)
{
    int count = 0;
    int row;
    int col;

    for (row = y; row < y + height; row++) {
        if (row < 0 || row >= canvas->h) {
            continue;
        }
        for (col = x; col < x + width; col++) {
            if (col < 0 || col >= canvas->w) {
                continue;
            }
            if ((canvas->px[row * canvas->w + col] & 0x00ffffffu) != 0u) {
                count += 1;
            }
        }
    }
    return count;
}

static void test_no_magenta_fringe(void)
{
    int direction;
    int kilix_count = zell_kilix_width() * zell_kilix_height();
    int wizard_count = zell_wizard_width() * zell_wizard_height();
    int opaque = 0;
    int partial = 0;
    int index;
    const uint32_t *down = zell_kilix_sprite(ZELL_DIR_DOWN, 0);

    expect(zell_kilix_width() == 64, "Kilix's frame is 64 wide");
    expect(zell_kilix_height() == 64, "Kilix's frame is 64 tall");
    expect(zell_kilix_foot() == 60, "Kilix stands on source row 60");
    expect(wizard_count == 18 * 18, "the wizard stays 18 by 18");
    for (index = 0; index < kilix_count; index++) {
        if ((down[index] >> 24) != 0u) {
            opaque += 1;
        }
    }
    expect(opaque > 400, "Kilix's down frame has a body");
    /* Land's sheet is binary alpha. Its purple pixels are painted art. */
    for (direction = 0; direction < 4; direction++) {
        int step;
        for (step = 0; step < 2; step++) {
            const uint32_t *frame = zell_kilix_sprite(direction, step);
            for (index = 0; index < kilix_count; index++) {
                unsigned alpha = frame[index] >> 24;
                if (alpha != 0u && alpha != 255u) {
                    partial += 1;
                }
            }
        }
    }
    expect(partial == 0, "Kilix's alpha is fully on or off");
    expect(fringe_pixels(zell_wizard_sprite(), wizard_count) == 0,
           "the wizard has no magenta fringe");
}

static int rgb_box(const sr_canvas *canvas, uint32_t rgb,
                   int x0, int y0, int x1, int y1)
{
    int count = 0;
    int y;
    int x;

    if (x0 < 0) {
        x0 = 0;
    }
    if (y0 < 0) {
        y0 = 0;
    }
    if (x1 > canvas->w) {
        x1 = canvas->w;
    }
    if (y1 > canvas->h) {
        y1 = canvas->h;
    }
    for (y = y0; y < y1; y++) {
        for (x = x0; x < x1; x++) {
            if ((canvas->px[y * canvas->w + x] & 0x00ffffffu) == rgb) {
                count += 1;
            }
        }
    }
    return count;
}

static int rgb_count(const sr_canvas *canvas, uint32_t rgb, int above_y, int below)
{
    int count = 0;
    int index;

    for (index = 0; index < canvas->w * canvas->h; index++) {
        int y = index / canvas->w;

        if ((canvas->px[index] & 0x00ffffffu) != rgb) {
            continue;
        }
        if (below) {
            if (y >= above_y) {
                count += 1;
            }
        } else if (y < above_y) {
            count += 1;
        }
    }
    return count;
}

static unsigned frame_sum(const sr_canvas *canvas)
{
    unsigned sum = 2166136261u;
    int index;

    for (index = 0; index < canvas->w * canvas->h; index++) {
        sum ^= canvas->px[index];
        sum *= 16777619u;
    }
    return sum;
}

static void test_answer_scrolls_in_the_box(void)
{
    zell_scene scene;
    sr_canvas canvas;
    zell_controls down = controls_of(0, 1, false, false);
    zell_controls up = controls_of(0, -1, false, false);
    zell_controls still = controls_of(0, 0, false, false);
    zell_controls enter = controls_of(0, 0, true, false);
    char reply[1000];
    int used = 0;
    int limit;
    unsigned top;
    unsigned bottom;

    zell_scene_init(&scene, 320, 240);
    zell_scene_finish_request(&scene, "A short answer.");
    expect(scene.answer_scroll == 0, "a new answer starts at the top");
    expect(zell_answer_scroll_limit(320, 240, scene.answer) == 0,
           "a short answer fits in the box");
    zell_scene_tick(&scene, &down);
    expect(scene.mode == ZELL_MODE_ANSWER, "down leaves a short answer open");
    expect(scene.answer_scroll == 0, "a short answer does not scroll");

    reply[0] = '\0';
    while (used < 800) {
        int wrote = snprintf(reply + used, sizeof reply - (size_t)used,
                             "The cave keeps this sentence. ");

        if (wrote < 0 || used + wrote >= (int)sizeof reply) {
            break;
        }
        used += wrote;
    }
    zell_scene_finish_request(&scene, reply);
    limit = zell_answer_scroll_limit(scene.width, scene.height, scene.answer);
    expect(limit > 2, "a long answer is longer than the box");
    expect(scene.answer_scroll == 0, "the long answer starts at the top");
    zell_scene_tick(&scene, &down);
    expect(scene.answer_scroll == 1, "down scrolls the answer one line");
    expect(scene.mode == ZELL_MODE_ANSWER, "scrolling stays on the answer");
    zell_scene_tick(&scene, &still);
    zell_scene_tick(&scene, &up);
    expect(scene.answer_scroll == 0, "up scrolls back to the start");
    scene.answer_scroll = limit;
    scene.menu_hold = 0;
    zell_scene_tick(&scene, &down);
    expect(scene.answer_scroll == limit, "the answer stops at the last line");
    expect(sr_canvas_init(&canvas, 320, 240), "answer canvas allocates");
    scene.answer_scroll = 0;
    zell_draw(&scene, &canvas);
    top = frame_sum(&canvas);
    scene.answer_scroll = limit;
    zell_draw(&scene, &canvas);
    bottom = frame_sum(&canvas);
    expect(top != bottom, "scrolling shows the rest of the answer");
    sr_canvas_free(&canvas);
    zell_scene_tick(&scene, &enter);
    expect(scene.mode == ZELL_MODE_WALK, "enter closes the answer");
}

static void test_voice_sign_above_the_switch(void)
{
    zell_scene scene;
    sr_canvas canvas;
    float x;
    float y;
    int plate_top;

    zell_scene_init(&scene, 320, 240);
    expect(sr_canvas_init(&canvas, 320, 240), "sign canvas allocates");
    zell_draw(&scene, &canvas);
    zell_switch_point(scene.width, scene.height, &x, &y);
    plate_top = (int)y - 11;
    expect(rgb_count(&canvas, 0x8d5a32u, plate_top, 0) > 40,
           "the wooden voice sign is above the switch");
    expect(rgb_count(&canvas, 0x8d5a32u, plate_top, 1) == 0,
           "the sign stays off the switch plate");
#ifndef ZELL_RENDER_NEURONTE
    /* The bitmap font paints whole pixels in the ink colour. A net's
     * coverage is mostly partial at this size, so the Neuronte renderer
     * checks the sign's ink its own way, in test_neuronte.c. */
    expect(rgb_count(&canvas, 0x24160cu, plate_top, 0) > 20,
           "the sign reads voice");
#endif
    expect(rgb_count(&canvas, 0xf2c14eu, plate_top, 1) > 10,
           "the lever stays under the sign");
    sr_canvas_free(&canvas);
}

static void test_draw_places_both_sprites(void)
{
    zell_scene scene;
    sr_canvas canvas;
    int top = 0;
    int bottom = 0;
    int left = 0;
    int right = 0;
    int index;

    zell_scene_init(&scene, 320, 240);
    memset(&canvas, 0, sizeof canvas);
    expect(sr_canvas_init(&canvas, 320, 240), "canvas allocates");
    zell_draw(&scene, &canvas);
    for (index = 0; index < canvas.w * canvas.h; index++) {
        uint32_t pixel = canvas.px[index];
        if ((pixel & 0x00ffffffu) == 0) {
            continue;
        }
        if (index < canvas.w * (canvas.h / 2)) {
            top += 1;
        } else {
            bottom += 1;
        }
    }
    expect(top > 40, "the wizard half is drawn");
    expect(bottom > 40, "Kilix's half is drawn");
    expect(colored_in(&canvas, 0, 0, 16, 16) > 40, "rock fills the top left");
    expect(colored_in(&canvas, canvas.w - 16, 0, 16, 16) > 40,
           "rock fills the top right");
    expect(colored_in(&canvas, 40, (int)scene.wizard_y + 8, canvas.w - 80,
                      canvas.h - (int)scene.wizard_y - 28) > 400,
           "the cave has a stone floor");
    zell_campfire_rect(canvas.w, canvas.h, 0, &left, &top, &bottom, &right);
    expect(bottom > 8 && bottom <= 96 && right > 8 && right <= 96,
           "the left campfire fits the room");
    expect(warm_pixels(&canvas, left, top, bottom, right) > 20,
           "a campfire burns on the left");
    {
        int fire_x;
        int fire_y;
        int fire_w;
        int fire_h;
        uint32_t left_before[96 * 96];
        uint32_t right_before[96 * 96];
        uint32_t left_after[96 * 96];
        uint32_t right_after[96 * 96];

        fire_x = left;
        fire_y = top;
        fire_w = bottom;
        fire_h = right;
        copy_rect(&canvas, fire_x, fire_y, fire_w, fire_h, left_before);
        zell_campfire_rect(canvas.w, canvas.h, 1, &left, &top, &bottom, &right);
        expect(warm_pixels(&canvas, left, top, bottom, right) > 20,
               "a campfire burns on the right");
        copy_rect(&canvas, left, top, bottom, right, right_before);
        scene.x = 170.0f;
        scene.y = 220.0f;
        zell_draw(&scene, &canvas);
        copy_rect(&canvas, fire_x, fire_y, fire_w, fire_h, left_after);
        copy_rect(&canvas, left, top, bottom, right, right_after);
        expect(memcmp(left_before, left_after,
                      (size_t)fire_w * (size_t)fire_h * sizeof left_before[0]) == 0,
               "the left fire stays put when Kilix moves");
        expect(memcmp(right_before, right_after,
                      (size_t)bottom * (size_t)right * sizeof right_before[0]) == 0,
               "the right fire stays put when Kilix moves");
        scene.flame_tick = 5;
        zell_draw(&scene, &canvas);
        copy_rect(&canvas, fire_x, fire_y, fire_w, fire_h, left_after);
        copy_rect(&canvas, left, top, bottom, right, right_after);
        expect(memcmp(left_before, left_after,
                      (size_t)fire_w * (size_t)fire_h * sizeof left_before[0]) != 0,
               "the left fire changes frame");
        expect(memcmp(right_before, right_after,
                      (size_t)bottom * (size_t)right * sizeof right_before[0]) != 0,
               "the right fire changes frame");
        expect(warm_pixels(&canvas, fire_x, fire_y, fire_w, fire_h) > 20,
               "the next campfire frame still burns");
    }
    sr_canvas_free(&canvas);
}

static void test_history_switch(void)
{
    zell_scene scene;
    zell_controls talk = controls_of(0, 0, true, false);
    zell_controls down = controls_of(0, 1, false, false);
    zell_controls typed = controls_of(0, 0, false, false);
    sr_canvas canvas;
    zell_history_call stored[ZELL_HISTORY_CAP];
    char directory[] = "/tmp/zell-history-state-XXXXXX";
    char path[360];
    char saved_env[400];
    char text[1200];
    const char *previous;
    const char *word = "cave map";
    const char *cursor;
    struct stat info;
    float sx;
    float sy;
    int rows;
    int step;
    int enabled = 1;
    int count = 4;
    int had_env = 0;
    int wrote;
    int order[ZELL_HISTORY_CAP];
    FILE *file;
    size_t got;

    zell_scene_init(&scene, 320, 240);
    expect(!scene.history_on, "history starts off");
    expect(scene.call_count == ZELL_PRESET_COUNT, "presets are remembered at zero");
    expect(strcmp(zell_scene_choice(&scene, 0), "What time is it?") == 0,
           "history off keeps the first preset first");
    expect(strcmp(zell_scene_choice(&scene, 1), "How much disk is free?") == 0,
           "history off keeps the preset order");
    scene.x = scene.wizard_x;
    scene.y = scene.wizard_y;
    zell_scene_tick(&scene, &talk);
    expect(scene.mode == ZELL_MODE_QUESTION, "enter opens Ask for history");
    rows = zell_scene_choice_rows(&scene);
    expect(rows == 5, "five questions fit under the field");
    for (step = 0; step < rows; step++) {
        scene.menu_hold = 0;
        zell_scene_tick(&scene, &down);
    }
    expect(scene.menu_index == rows, "down reaches the history switch");
    expect(scene.mode == ZELL_MODE_QUESTION, "the switch stays on the Ask screen");
    zell_scene_tick(&scene, &talk);
    expect(scene.history_on, "enter turns history on");
    expect(scene.mode == ZELL_MODE_QUESTION, "the switch does not send a question");
    expect(scene.history_changed, "turning history on is marked to save");
    expect(zell_history_remember(scene.calls, &scene.call_count, ZELL_HISTORY_CAP,
                                 "How much disk is free?", 1) >= 0,
           "a preset use is counted");
    expect(zell_history_remember(scene.calls, &scene.call_count, ZELL_HISTORY_CAP,
                                 "What time is it?", 2) >= 0,
           "the busiest preset is counted");
    expect(strcmp(zell_scene_choice(&scene, 0), "What time is it?") == 0,
           "the most used call is first");
    expect(strcmp(zell_scene_choice(&scene, 1), "How much disk is free?") == 0,
           "the next most used call follows");
    scene.history_on = true;
    zell_scene_resize(&scene, 400, 300);
    expect(scene.history_on, "resize keeps history on");
    expect(strcmp(zell_scene_choice(&scene, 0), "What time is it?") == 0,
           "resize keeps the ranked calls");

    zell_scene_init(&scene, 320, 240);
    scene.x = scene.wizard_x;
    scene.y = scene.wizard_y;
    zell_scene_tick(&scene, &talk);
    for (cursor = word; *cursor != '\0'; cursor++) {
        typed.character = (unsigned char)*cursor;
        zell_scene_tick(&scene, &typed);
        typed.character = 0;
    }
    zell_scene_tick(&scene, &talk);
    expect(scene.mode == ZELL_MODE_WAIT, "a typed question is sent");
    expect(!scene.history_on, "sending a question leaves history off");
    expect(zell_history_find(scene.calls, scene.call_count, word) >= 0,
           "a typed question is kept while history is off");

    zell_scene_init(&scene, 320, 240);
    scene.mode = ZELL_MODE_QUESTION;
    rows = zell_scene_choice_rows(&scene);
    scene.menu_index = 0;
    expect(sr_canvas_init(&canvas, 320, 240), "history canvas allocates");
    zell_draw(&scene, &canvas);
    zell_history_switch_point(320, 240, rows, &sx, &sy);
    expect(rgb_box(&canvas, 0x6a6458u,
                   (int)sx - 12, (int)sy - 14, (int)sx + 12, (int)sy + 14) > 10,
           "history off draws the lever down");
    expect(rgb_box(&canvas, 0xf2c14eu,
                   (int)sx - 12, (int)sy - 14, (int)sx + 12, (int)sy + 14) == 0,
           "history off keeps the lever dim");
    scene.history_on = true;
    zell_draw(&scene, &canvas);
    expect(rgb_box(&canvas, 0xf2c14eu,
                   (int)sx - 12, (int)sy - 14, (int)sx + 12, (int)sy + 14) > 10,
           "history on draws the lever up");
    expect(rgb_box(&canvas, 0x6a6458u,
                   (int)sx - 12, (int)sy - 14, (int)sx + 12, (int)sy + 14) == 0,
           "history on lifts the dim lever");
#ifndef ZELL_RENDER_NEURONTE
    expect(rgb_box(&canvas, 0xf4f1e8u,
                   (int)sx + 10, (int)sy - 12, (int)sx + 80, (int)sy + 12) > 10,
           "the switch reads history");
#endif
    sr_canvas_free(&canvas);

    scene.history_on = true;
    scene.menu_index = rows;
    memcpy(scene.question, "a", 2);
    scene.question_len = 1;
    zell_scene_tick(&scene, &talk);
    expect(scene.mode == ZELL_MODE_WAIT, "typed text is asked ahead of the switch");
    expect(scene.history_on, "typing does not flip the switch");

    enabled = 1;
    count = 3;
    expect(zell_history_parse("{\"schema\":\"nope\",\"enabled\":true}",
                              &enabled, stored, ZELL_HISTORY_CAP, &count) == -1,
           "a bad history schema is refused");
    expect(enabled == 0 && count == 0, "a bad history file stays off and empty");
    expect(zell_history_parse("{\"schema\":\"kilix.zell.history/v1\",\"enabled\":true,"
                              "\"calls\":[{\"text\":\"\",\"count\":1}]}",
                              &enabled, stored, ZELL_HISTORY_CAP, &count) == 1,
           "an empty call is skipped");
    expect(count == 0, "an empty call is not stored");

    if (mkdtemp(directory) == NULL) {
        expect(0, "history state directory");
        return;
    }
    previous = getenv("KILIX_ZELL_STATE");
    if (previous != NULL) {
        wrote = snprintf(saved_env, sizeof saved_env, "%s", previous);
        had_env = wrote > 0 && (size_t)wrote < sizeof saved_env;
    }
    if (setenv("KILIX_ZELL_STATE", directory, 1) != 0) {
        expect(0, "history state override");
        rmdir(directory);
        return;
    }
    wrote = snprintf(path, sizeof path, "%s/history.json", directory);
    expect(wrote > 0 && (size_t)wrote < sizeof path, "the history path fits");
    expect(strcmp(zell_history_state_path(), path) == 0,
           "history uses KILIX_ZELL_STATE");
    enabled = 1;
    count = 4;
    expect(zell_history_load(path, &enabled, stored, ZELL_HISTORY_CAP, &count) == 0,
           "a missing history file is off");
    expect(enabled == 0 && count == 0, "a missing history file has no calls");
    memset(stored, 0, sizeof stored);
    memcpy(stored[0].text, "What time is it?", sizeof "What time is it?");
    stored[0].count = 3;
    stored[1].count = 9;
    memcpy(stored[2].text, "How much disk is free?", sizeof "How much disk is free?");
    stored[2].count = 1;
    expect(zell_history_save(path, 1, NULL, 1) == -1,
           "history refuses a missing call list");
    expect(zell_history_save(path, 1, stored, 3) == 0, "history writes the file");
    expect(stat(path, &info) == 0 && (info.st_mode & 0777) == 0600,
           "the history file is private");
    file = fopen(path, "r");
    expect(file != NULL, "the history file opens");
    if (file != NULL) {
        got = fread(text, 1, sizeof text - 1, file);
        text[got] = '\0';
        fclose(file);
        expect(strstr(text, "kilix.zell.history/v1") != NULL,
               "the history file has its schema");
        expect(strstr(text, "\"enabled\": true") != NULL, "history on is in the file");
        expect(strstr(text, ",\n  ]") == NULL, "a skipped call leaves no trailing comma");
    }
    enabled = 0;
    count = 0;
    expect(zell_history_load(path, &enabled, stored, ZELL_HISTORY_CAP, &count) == 1,
           "the history file is read back");
    expect(enabled == 1, "history on is saved");
    expect(count == 2, "the blank call is left out");
    expect(strcmp(stored[0].text, "What time is it?") == 0 && stored[0].count == 3,
           "the first saved call is kept");
    expect(strcmp(stored[1].text, "How much disk is free?") == 0 && stored[1].count == 1,
           "the second saved call is kept");
    zell_history_order(stored, count, order);
    expect(order[0] == 0 && order[1] == 1, "the busier saved call stays first");
    stored[1].count = 5;
    zell_history_order(stored, count, order);
    expect(order[0] == 1 && order[1] == 0, "the higher count moves first");

    {
        char settings[512],journal[512],journal_text[1024];
        snprintf(settings,sizeof settings,"%s/conversation-settings.json",directory);
        snprintf(journal,sizeof journal,"%s/wizard-conversations.jsonl",directory);
        expect(zell_conversation_save("Question","Answer")==0,"full replies default to off");
        expect(access(journal,F_OK)!=0,"disabled conversations leave no journal");
        FILE *config=fopen(settings,"w");
        if(config){fputs("{\"enabled\": true}",config);fclose(config);}
        expect(zell_conversation_save("Quote \"here\"","One\nTwo\\three")==1,"enabled conversations save");
        expect(stat(journal,&info)==0&&(info.st_mode&0777)==0600,"conversation journal is private");
        FILE *log=fopen(journal,"r");
        if(log){
            size_t len=fread(journal_text,1,sizeof journal_text-1,log);journal_text[len]=0;fclose(log);
            expect(strstr(journal_text,"One\\u000aTwo\\\\three")!=NULL,"journal escapes multiline replies");
        }
        unlink(journal);
        expect(symlink(path,journal)==0,"journal symlink test setup");
        expect(zell_conversation_save("q","a")==-1,"journal refuses symlink destination");
        unlink(journal);unlink(settings);
    }

    if (had_env) {
        setenv("KILIX_ZELL_STATE", saved_env, 1);
    } else {
        unsetenv("KILIX_ZELL_STATE");
    }
    unlink(path);
    rmdir(directory);
}

int main(void)
{
    test_walk_off_the_bottom();
    test_walls_block_the_walk();
    test_fire_burns_and_respawns();
    test_burn_lifts_off_the_body();
    test_up_does_not_leave();
    test_wizard_question_routes();
    test_router_prefers_a_fast_model();
    test_model_file_pins_the_router();
    test_wall_switch_toggles_voice();
    test_answer_scrolls_in_the_box();
    test_voice_sign_above_the_switch();
    test_history_switch();
    test_voice_file_and_paradee_argv();
    test_escape_steps_back();
    test_no_magenta_fringe();
    test_draw_places_both_sprites();
    if (failures != 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    puts("ok");
    return 0;
}
