/* Write one PPM per screen, with whichever renderer this was linked to.
 *
 * usage: snap OUT_PREFIX [WIDTH HEIGHT]
 *
 * It opens no pane, launches no model and plays no audio. The two builds,
 * build/snap and build/snap-neuronte, give pictures to set side by side. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "draw.h"
#include "scene.h"
#include "soft_raster.h"

#ifdef ZELL_RENDER_NEURONTE
#include "draw_neuronte.h"
#endif

static int shot(const zell_scene *scene, sr_canvas *canvas, const char *prefix,
                const char *name)
{
    char path[512];

    zell_draw(scene, canvas);
    if (snprintf(path, sizeof path, "%s-%s.ppm", prefix, name) >= (int)sizeof path ||
        !sr_write_ppm(canvas, path)) {
        fprintf(stderr, "snap: cannot write %s-%s.ppm\n", prefix, name);
        return 1;
    }
    printf("%s\n", path);
    return 0;
}

int main(int argc, char **argv)
{
    int width = argc >= 4 ? atoi(argv[2]) : 640;
    int height = argc >= 4 ? atoi(argv[3]) : 400;
    zell_scene scene;
    sr_canvas canvas;
    int failed = 0;

    if (argc != 2 && argc != 4) {
        fprintf(stderr, "usage: snap OUT_PREFIX [WIDTH HEIGHT]\n");
        return 2;
    }
    if (width < 160 || height < 120 || width > 1600 || height > 1000 ||
        !sr_canvas_init(&canvas, width, height)) {
        fprintf(stderr, "snap: bad size\n");
        return 2;
    }
#ifdef ZELL_RENDER_NEURONTE
    if (!zell_neuronte_open()) {
        fprintf(stderr, "snap: cannot load %s\n", zell_neuronte_weights());
        return 2;
    }
#endif

    zell_scene_init(&scene, width, height);
    failed += shot(&scene, &canvas, argv[1], "walk");

    scene.mode = ZELL_MODE_QUESTION;
    scene.menu_index = 2;
    failed += shot(&scene, &canvas, argv[1], "question");

    scene.mode = ZELL_MODE_TYPE;
    scene.menu_index = 0;
    snprintf(scene.question, sizeof scene.question, "How do I split a pane?");
    scene.question_len = (int)strlen(scene.question);
    failed += shot(&scene, &canvas, argv[1], "type");

    scene.mode = ZELL_MODE_WAIT;
    failed += shot(&scene, &canvas, argv[1], "wait");

    scene.mode = ZELL_MODE_ANSWER;
    snprintf(scene.answerer, sizeof scene.answerer, "Kilix Ask");
    snprintf(scene.answer, sizeof scene.answer,
             "Press Ctrl+Shift+Enter to split the current pane. The new pane "
             "opens beside it with the same working directory. Use "
             "Ctrl+Shift+[ and ] to move between panes, and close one with "
             "Ctrl+Shift+W. 0123456789 {}[]()<>|/\\ ~!@#$%%^&*_+-=");
    failed += shot(&scene, &canvas, argv[1], "answer");

    sr_canvas_free(&canvas);
#ifdef ZELL_RENDER_NEURONTE
    zell_neuronte_close();
#endif
    return failed ? 1 : 0;
}
