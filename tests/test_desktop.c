#include "desktop.h"
#include "desktop_assets.h"
#include "draw.h"
#ifdef ZELL_RENDER_NEURONTE
#include "draw_neuronte.h"
#endif
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    zell_scene s;
    zell_controls c={0};
    zell_scene_init(&s,640,400);zell_desktop_init(&s);
    float x=s.x,y=s.y;
    zell_scene_resize(&s,320,240);
    assert(s.x==x&&s.y==y);
    int cx,cy;zell_desktop_camera(&s,&cx,&cy);
    assert(cx==(int)x-160&&cy==(int)y-120);
    zell_scene_resize(&s,1600,1000);zell_desktop_camera(&s,&cx,&cy);
    assert(cx==0&&cy>=0&&cy<=24);
    c.move_x=-1;
    for(int i=0;i<1000;i++)zell_scene_tick(&s,&c);
    assert(s.x>=ZELL_WORLD_W*.135f);
    c.move_x=0;c.move_y=-1;
    for(int i=0;i<1000;i++)zell_scene_tick(&s,&c);
    assert(s.y>=ZELL_WORLD_H*.32f);
    c=(zell_controls){.launcher=true};zell_scene_tick(&s,&c);
    assert(s.mode==ZELL_MODE_LAUNCHER);
    s.menu_index=6;c=(zell_controls){.confirm=true};zell_scene_tick(&s,&c);
    assert(s.mode==ZELL_MODE_DESK&&s.station==6);
    zell_scene_tick(&s,&c);assert(s.desktop_launch);
    s.desktop_launch=false;s.mode=ZELL_MODE_WALK;
    bool voice=s.voice_on;
    s.x=zell_stations[10].x*ZELL_WORLD_W+40;s.y=zell_stations[10].y*ZELL_WORLD_H;
    assert(zell_desktop_near(&s)==10);
    zell_scene_tick(&s,&c);assert(s.voice_on!=voice&&s.voice_changed);
    s.mode=ZELL_MODE_WALK;s.x=ZELL_WORLD_W*.5f;s.y=ZELL_WORLD_H*.98f;
    c=(zell_controls){0};assert(zell_scene_tick(&s,&c));
    for(int i=0;i<ZELL_STATIONS;i++)assert(zell_desktop_actions(i)>0);
    s.leave=false;s.mode=ZELL_MODE_SERVICE;s.service_phase=2;s.service_count=3;
    c=(zell_controls){.move_y=1};s.menu_hold=0;zell_scene_tick(&s,&c);
    assert(s.service_index==1);
    c=(zell_controls){.confirm=true};zell_scene_tick(&s,&c);
    assert(s.service_send&&strcmp(s.service_reply,"1\n")==0&&s.service_phase==0);
    s.service_send=false;s.service_phase=1;
    c=(zell_controls){.character='a'};zell_scene_tick(&s,&c);
    assert(strcmp(s.service_input,"a")==0);
    c=(zell_controls){.cancel=true};zell_scene_tick(&s,&c);
    assert(s.service_send&&s.service_reply[0]==27);
    s.service_send=false;s.service_phase=0;zell_scene_tick(&s,&c);assert(s.service_cancel);
    s.service_phase=4;zell_scene_tick(&s,&c);assert(s.mode==ZELL_MODE_DESK);
    s.mode=ZELL_MODE_WALK;
    /* The lever is behind the shell PC and reachable from the walkable edge. */
    s.x=ZELL_WORLD_W*.865f;s.y=zell_stations[12].y*ZELL_WORLD_H;
    s.mode=ZELL_MODE_WALK;s.desktop_launch=false;
    assert(zell_stations[12].y<zell_stations[7].y);
    assert(zell_desktop_near(&s)==12);
    c=(zell_controls){.confirm=true};zell_scene_tick(&s,&c);
    assert(s.station==12&&s.desktop_launch&&s.mode==ZELL_MODE_DESK);
    s.y=zell_stations[7].y*ZELL_WORLD_H;
    assert(zell_desktop_near(&s)==7);
    s.mode=ZELL_MODE_WALK;s.desktop_launch=false;
    const zell_art *hero=zell_art_get(1);assert(hero&&hero->width>0&&hero->height<=144);
    /* Every imported cutout obeys the compositors' premultiplied contract.
     * Straight PNG RGB at small alpha used to paint colored rectangles. */
    for (int id=1; id<ZELL_ART_COUNT; id++) {
        const zell_art *art=zell_art_get(id);
        assert(art);
        assert(art->height==(id>=5&&id<=10?112:84));
        for (int pixel=0; pixel<art->width*art->height; pixel++) {
            uint32_t rgba=art->pixels[pixel], alpha=rgba>>24;
            assert(((rgba>>16)&255u)<=alpha);
            assert(((rgba>>8)&255u)<=alpha);
            assert((rgba&255u)<=alpha);
        }
    }
    zell_scene_init(&s,640,400);zell_desktop_init(&s);
    for (int actor=-1; actor<6; actor++) {
        s.flame_tick=0;zell_actor_pose a=zell_desktop_pose(&s,actor);
        s.flame_tick=32;zell_actor_pose b=zell_desktop_pose(&s,actor);
        assert(a.art==b.art && a.offset_y!=b.offset_y);
    }
    c=(zell_controls){.move_x=1};
    for (int frame=0;frame<4;frame++) {
        for (int tick=0;tick<7;tick++) zell_scene_tick(&s,&c);
        assert(zell_desktop_pose(&s,-1).art==17+3*4+frame);
    }
    c=(zell_controls){0};zell_scene_tick(&s,&c);
    assert(s.step_ticks==0&&zell_desktop_pose(&s,-1).art==4);
    s.x=ZELL_WORLD_W*.865f;c=(zell_controls){.move_x=1};
    zell_scene_tick(&s,&c);assert(s.step_ticks==0);
    /* Boots-style charge: no movement until ready, no diagonal dash/steering. */
    zell_scene_init(&s,640,400);zell_desktop_init(&s);s.x=700;s.y=550;
    float dash_x=s.x,dash_y=s.y;
    c=(zell_controls){.run=true,.move_x=1,.move_y=1};
    for(int tick=0;tick<ZELL_RUN_CHARGE_TICKS-1;tick++)zell_scene_tick(&s,&c);
    assert(!s.running&&s.x==dash_x&&s.y==dash_y&&s.run_charge>0);
    zell_scene_tick(&s,&c);assert(s.running);
    assert(fabsf(s.x-dash_x-ZELL_RUN_SPEED)<.01f&&s.y==dash_y);
    c=(zell_controls){.run=true,.move_y=-1};dash_x=s.x;
    zell_scene_tick(&s,&c);assert(s.direction==ZELL_DIR_RIGHT&&s.y==dash_y);
    assert(fabsf(s.x-dash_x-ZELL_RUN_SPEED)<.01f);
    c=(zell_controls){0};dash_x=s.x;zell_scene_tick(&s,&c);
    assert(!s.running&&s.run_charge==0&&s.x==dash_x);
    /* Releasing during charge cancels it; Shift alone uses the facing. */
    c=(zell_controls){.run=true};zell_scene_tick(&s,&c);assert(s.run_charge==1);
    c=(zell_controls){0};zell_scene_tick(&s,&c);assert(s.run_charge==0);
    c=(zell_controls){.run=true};
    for(int tick=0;tick<ZELL_RUN_CHARGE_TICKS;tick++)zell_scene_tick(&s,&c);
    assert(s.running&&s.direction==ZELL_DIR_RIGHT);
    s.x=ZELL_WORLD_W*.865f-1;zell_scene_tick(&s,&c);
    assert(!s.running&&s.run_blocked&&s.run_bump>0&&s.x<=ZELL_WORLD_W*.865f);
    for(int tick=0;tick<40;tick++)zell_scene_tick(&s,&c);
    assert(!s.running&&s.run_charge==0); /* No automatic recharge into the wall. */
    c=(zell_controls){0};zell_scene_tick(&s,&c);assert(!s.run_blocked);
    /* Collision substeps stop before passing through an NPC. */
    s.x=zell_stations[1].x*ZELL_WORLD_W;s.y=zell_stations[1].y*ZELL_WORLD_H+30;
    s.direction=ZELL_DIR_UP;c=(zell_controls){.run=true};
    for(int tick=0;tick<ZELL_RUN_CHARGE_TICKS;tick++)zell_scene_tick(&s,&c);
    assert(s.run_blocked&&!s.running);
    assert(s.y-zell_stations[1].y*ZELL_WORLD_H>=26);
    c=(zell_controls){.launcher=true,.run=true};zell_scene_tick(&s,&c);
    assert(s.mode==ZELL_MODE_LAUNCHER&&!s.running&&s.run_charge==0);

    /* Trapdoors, room-local launcher, stairs, and resource slowdown. */
    zell_scene_init(&s,640,400);zell_desktop_init(&s);
    s.x=zell_stations[13].x*ZELL_WORLD_W;s.y=zell_stations[13].y*ZELL_WORLD_H;
    c=(zell_controls){.confirm=true};zell_scene_tick(&s,&c);
    assert(s.room==1&&s.mode==ZELL_MODE_WALK&&!s.desktop_launch);
    assert(zell_desktop_launcher_count(&s)==8);
    assert(zell_desktop_launcher_station(&s,0)==15);
    assert(zell_desktop_launcher_station(&s,6)==29);
    s.x=zell_stations[28].x*ZELL_WORLD_W;s.y=zell_stations[28].y*ZELL_WORLD_H;
    zell_scene_tick(&s,&c);assert(s.room==0);
    s.x=zell_stations[14].x*ZELL_WORLD_W;s.y=zell_stations[14].y*ZELL_WORLD_H;
    zell_scene_tick(&s,&c);assert(s.room==2);
    s.disk_count=3;assert(zell_desktop_launcher_count(&s)==4);
    assert(zell_desktop_launcher_station(&s,3)==28);
    assert(!zell_desktop_station_visible(&s,23));
    s.hp=10;s.fatigue=100;assert(zell_desktop_speed_factor(&s)<.5f);
    s.hp=100;s.fatigue=0;assert(zell_desktop_speed_factor(&s)==.35f);
    s.hp=-1;s.fatigue=-1;assert(zell_desktop_speed_factor(&s)==1.0f);

    /* Check visible motion in the actual renderer, with unmoving floor
     * pixels as a control. This catches a pose that is never drawn. */
    int render_ready=1;
#ifdef ZELL_RENDER_NEURONTE
    render_ready=zell_neuronte_open();
#endif
    if(render_ready) {
        sr_canvas before,after;
        assert(sr_canvas_init(&before,ZELL_WORLD_W,ZELL_WORLD_H));
        assert(sr_canvas_init(&after,ZELL_WORLD_W,ZELL_WORLD_H));
        zell_scene_init(&s,ZELL_WORLD_W,ZELL_WORLD_H);zell_desktop_init(&s);
        s.flame_tick=0;zell_draw(&s,&before);
        s.flame_tick=32;zell_draw(&s,&after);
        for(int actor=-1;actor<6;actor++) {
            int ax=actor<0?(int)s.x:(int)(zell_stations[actor].x*ZELL_WORLD_W);
            int ay=actor<0?(int)s.y:(int)(zell_stations[actor].y*ZELL_WORLD_H);
            int changes=0;
            for(int py=ay-110;py<ay;py++) for(int px=ax-40;px<ax+40;px++) {
                int at=py*ZELL_WORLD_W+px;
                if(before.px[at]!=after.px[at])changes++;
            }
            assert(changes>10);
        }
        assert(before.px[600*ZELL_WORLD_W+500]==after.px[600*ZELL_WORLD_W+500]);
        sr_canvas_free(&before);sr_canvas_free(&after);
    }
#ifdef ZELL_RENDER_NEURONTE
    zell_neuronte_close();
#endif
    puts("desktop world, camera, alpha, idle/walk animation, launcher and voice: ok");
    return 0;
}
