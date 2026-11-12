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
#include "desktop.h"
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
#ifdef ZELL_RENDER_NEURONTE
    zell_desktop_init(&scene);
#endif
    failed += shot(&scene, &canvas, argv[1], "walk");
    scene.mode=ZELL_MODE_LAUNCHER;
    failed += shot(&scene, &canvas, argv[1], "launcher");

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

    if(scene.desktop){
        scene.mode=ZELL_MODE_WALK;scene.running=true;scene.direction=ZELL_DIR_RIGHT;scene.step_ticks=8;
        failed+=shot(&scene,&canvas,argv[1],"dash");
        scene.running=false;scene.step_ticks=0;
        scene.x=ZELL_WORLD_W*.865f;scene.y=zell_stations[12].y*ZELL_WORLD_H;
        failed+=shot(&scene,&canvas,argv[1],"shell-switch");
        scene.x=ZELL_WORLD_W*.5f;scene.y=ZELL_WORLD_H*.5f;
        scene.weather_state=1;scene.weather_icon=3;
        strcpy(scene.weather_temperature,"47 F");strcpy(scene.weather_place,"Example location");strcpy(scene.weather_condition,"Rain showers");
        failed+=shot(&scene,&canvas,argv[1],"weather-map");scene.weather_state=0;
        scene.mode=ZELL_MODE_WALK;scene.room=1;scene.x=ZELL_WORLD_W*.50f;scene.y=ZELL_WORLD_H*.55f;
        failed+=shot(&scene,&canvas,argv[1],"command-center");
        scene.room=2;scene.disk_count=3;scene.disks_ready=true;
        strcpy(scene.disk_labels[0],"nvme0n1p2 /system");scene.disk_used[0]=42;
        strcpy(scene.disk_labels[1],"nvme1n1p1 /work");scene.disk_used[1]=78;
        strcpy(scene.disk_labels[2],"sda1 /archive");scene.disk_used[2]=93;
        failed+=shot(&scene,&canvas,argv[1],"laundry");
        scene.room=0;scene.hp=57;scene.os_health=70;scene.dirty_files=13;scene.update_age_days=3;
        scene.mouse_valid=true;scene.mouse_x=32;scene.mouse_y=50;
        failed+=shot(&scene,&canvas,argv[1],"health-hover");scene.mouse_valid=false;
        {
            /* Floating terminal window, then minimized to the bottom bar. */
            int cols,rows;zell_terminal_cells(&scene,&cols,&rows);
            scene.terminal_width=cols*8;scene.terminal_height=rows*16;
            scene.terminal_pixels=calloc((size_t)scene.terminal_width*(size_t)scene.terminal_height,4);
            if(scene.terminal_pixels){
                for(int i=0;i<scene.terminal_width*scene.terminal_height;i++)scene.terminal_pixels[i]=0xff0b1418u;
                scene.mode=ZELL_MODE_SERVICE;scene.station=2;scene.service_phase=5;
                failed+=shot(&scene,&canvas,argv[1],"terminal-window");
                scene.terminal_minimized=true;scene.mode=ZELL_MODE_WALK;
                failed+=shot(&scene,&canvas,argv[1],"terminal-minimized");
                scene.terminal_minimized=false;free(scene.terminal_pixels);scene.terminal_pixels=NULL;
                scene.terminal_width=scene.terminal_height=0;
            }
        }
        scene.mode=ZELL_MODE_SERVICE;scene.station=4;scene.service_phase=2;
        strcpy(scene.service_title,"YouTube / playlists and creators");
        scene.service_count=5;scene.service_index=1;
        strcpy(scene.service_choices[0],"New content (12)");
        strcpy(scene.service_choices[1],"Refresh followed sources");
        strcpy(scene.service_choices[2],"All recent content");
        strcpy(scene.service_choices[3],"Follow a creator or playlist");
        strcpy(scene.service_choices[4],"Transcripts and jobs");
        failed+=shot(&scene,&canvas,argv[1],"station-menu");
        scene.station=3;scene.service_phase=3;
        strcpy(scene.service_text,"System health report\n\nMemory available: 21.4 GiB\nSystem disk: 186.2 GiB free\nUptime: 4.2 hours\n\nReports stay here in the cave.\nFull interactive tools open in a Kilix tab.");
        failed+=shot(&scene,&canvas,argv[1],"station-report");
        scene.station=9;scene.service_phase=1;
        strcpy(scene.service_title,"Add a note:");
        strcpy(scene.service_text,"Keep a reminder on the cave noticeboard.");
        strcpy(scene.service_input,"Read the new archive entries");
        failed+=shot(&scene,&canvas,argv[1],"station-input");
    }
    sr_canvas_free(&canvas);
#ifdef ZELL_RENDER_NEURONTE
    zell_neuronte_close();
#endif
    return failed ? 1 : 0;
}
