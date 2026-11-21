#include "desktop.h"
#include "desktop_assets.h"
#include "draw.h"
#ifdef ZELL_RENDER_NEURONTE
#include "draw_neuronte.h"
#endif
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

/* Largest per-channel difference between two pixels. */
static int pixel_gap(uint32_t a,uint32_t b){
    int gap=0;
    for(int shift=0;shift<24;shift+=8){int d=(int)((a>>shift)&255u)-(int)((b>>shift)&255u);if(d<0)d=-d;if(d>gap)gap=d;}
    return gap;
}
/* The Command Center has no stairs and no point that answers Enter: Kilix
 * leaves it by walking out of the bottom. */
static void walk_out_of_command_center(zell_scene *s){
    zell_controls down={.move_y=1};
    assert(s->room==1);
    s->x=zell_stations[28].x*(float)ZELL_WORLD_W;s->y=zell_stations[28].y*(float)ZELL_WORLD_H;
    for(int tick=0;tick<200&&s->room==1;tick++)zell_scene_tick(s,&down);
}
static void place_room(zell_scene *s,int room,float x,float y){
    zell_scene_init(s,960,600);
    zell_desktop_init(s);
    s->room=room;
    s->mode=ZELL_MODE_WALK;
    s->x=x;
    s->y=y;
    s->leave=false;
}
/* Same bodies the movement loop refuses, so an approach is not stopped early. */
static int body_near(const zell_scene *s,float x,float y){
    for(int i=0;i<ZELL_STATIONS;i++){
        if(!zell_desktop_station_visible(s,i)||i==28||i==ZELL_WORKSHOP_COMPUTER)continue;
        if(s->room==1&&(i<29||i>31))continue;
        if(s->room==0&&i>=6&&!(i>=32&&i<=41))continue;
        float ax=x-zell_stations[i].x*(float)ZELL_WORLD_W;
        float ay=y-zell_stations[i].y*(float)ZELL_WORLD_H;
        if(ax*ax+ay*ay<36.f*36.f)return 1;
    }
    return 0;
}
/* Step from outside one footprint until the foot stops on its near edge. */
static void walk_into_rect(zell_scene *s,float x0,float y0,float x1,float y1){
    float mid_x=(x0+x1)*0.5f,mid_y=(y0+y1)*0.5f;
    float goal_x=(float)ZELL_WORLD_W*0.50f,goal_y=(float)ZELL_WORLD_H*0.55f;
    if(s->room==1){goal_x=mid_x;goal_y=y1+24.f;}
    int along_x=fabsf(goal_x-mid_x)<fabsf(goal_y-mid_y);
    float span=along_x?(x1-x0):(y1-y0);
    int steps=(int)(span/20.f);if(steps<2)steps=2;
    int found=0,dir_x=0,dir_y=0;
    float start_x=0,start_y=0,edge=0;
    for(int i=1;i<steps&&!found;i++){
        float t=(float)i/(float)steps,px,py;
        int mx=0,my=0;
        if(along_x){
            px=x0+(x1-x0)*t;
            if(goal_y<mid_y){py=y0-24.f;my=1;edge=y0;}
            else {py=y1+24.f;my=-1;edge=y1;}
        }else{
            py=y0+(y1-y0)*t;
            if(goal_x<mid_x){px=x0-24.f;mx=1;edge=x0;}
            else {px=x1+24.f;mx=-1;edge=x1;}
        }
        if(px<(float)ZELL_WORLD_W*.14f||px>(float)ZELL_WORLD_W*.85f)continue;
        if(py<(float)ZELL_WORLD_H*.33f||py>(float)ZELL_WORLD_H*.90f)continue;
        if(zell_desktop_blocked(s,px,py)||body_near(s,px,py))continue;
        int clear=1;
        for(float d=3.5f;d<=20.f;d+=3.5f){
            float qx=px+(float)mx*d,qy=py+(float)my*d;
            if(zell_desktop_blocked(s,qx,qy)||body_near(s,qx,qy)){clear=0;break;}
        }
        if(!clear)continue;
        start_x=px;start_y=py;dir_x=mx;dir_y=my;found=1;
    }
    if(!found)fprintf(stderr,"no clear approach %.0f %.0f %.0f %.0f room %d\n",x0,y0,x1,y1,s->room);
    assert(found);
    s->x=start_x;s->y=start_y;s->leave=false;
    int room=s->room;
    zell_controls c={.move_x=dir_x,.move_y=dir_y};
    for(int tick=0;tick<40&&s->room==room&&!s->leave;tick++)zell_scene_tick(s,&c);
    if(s->room!=room||s->leave||zell_desktop_blocked(s,s->x,s->y))
        fprintf(stderr,"approach failed room %d at %.1f %.1f edge %.1f\n",room,s->x,s->y,edge);
    assert(s->room==room&&!s->leave&&!zell_desktop_blocked(s,s->x,s->y));
    if(dir_y>0){assert(s->y<edge&&s->y>edge-ZELL_WALK_SPEED-0.05f);assert(s->x==start_x);}
    if(dir_y<0){assert(s->y>=edge&&s->y<edge+ZELL_WALK_SPEED+0.05f);assert(s->x==start_x);}
    if(dir_x>0){assert(s->x<edge&&s->x>edge-ZELL_WALK_SPEED-0.05f);assert(s->y==start_y);}
    if(dir_x<0){assert(s->x>=edge&&s->x<edge+ZELL_WALK_SPEED+0.05f);assert(s->y==start_y);}
}
/* Deleting the tick's zell_desktop_blocked call lets this walk pass the screen. */
static void test_obstacles(void){
    zell_scene s;
    static const int expect_count[5]={2,1+ZELL_FLOOR_PANELS,0,4,7};
    for(int room=0;room<5;room++){
        float start_y=room==1?ZELL_COMMAND_LANDING_Y:(float)ZELL_WORLD_H*.80f;
        place_room(&s,room,(float)ZELL_WORLD_W*.50f,start_y);
        assert(!zell_desktop_blocked(&s,s.x,s.y));
        assert(zell_desktop_block_count(&s)==expect_count[room]);
        int blocks=zell_desktop_block_count(&s);
        for(int i=0;i<blocks;i++){
            float x0,y0,x1,y1;
            zell_desktop_block_rect(&s,i,&x0,&y0,&x1,&y1);
            float cx=(x0+x1)*0.5f,cy=(y0+y1)*0.5f;
            assert(zell_desktop_blocked(&s,cx,cy));
            s.room=2;
            assert(!zell_desktop_blocked(&s,cx,cy));
            s.room=room;
            walk_into_rect(&s,x0,y0,x1,y1);
        }
        /* Outside the corridor the south clamp still holds, including the cave. */
        place_room(&s,room,(float)ZELL_WORLD_W*.30f,start_y);
        zell_controls south={.move_y=1};
        for(int tick=0;tick<80&&s.room==room;tick++)zell_scene_tick(&s,&south);
        if(room==1||room==2)assert(s.room==0&&!s.leave);
        else assert(s.room==room&&!s.leave&&s.y<=(float)ZELL_WORLD_H*.91f+1.f);
        place_room(&s,room,(float)ZELL_WORLD_W*.50f,start_y);
        int ticks=0;
        while(s.room==room&&!s.leave&&ticks<200){zell_scene_tick(&s,&south);ticks++;}
        if(room==0)assert(s.leave&&s.room==0);
        else assert(s.room==0&&!s.leave);
    }
    /* Rooms 1 and 2 leave at the cave line, not at the old .87 cutout. */
    for(int room=1;room<=2;room++){
        place_room(&s,room,(float)ZELL_WORLD_W*.50f,0.92f*(float)ZELL_WORLD_H);
        zell_controls south={.move_y=1};
        zell_scene_tick(&s,&south);
        assert(s.room==room&&!s.leave);
        float last=s.y;
        for(int tick=0;tick<200&&s.room==room;tick++){last=s.y;zell_scene_tick(&s,&south);}
        assert(s.room==0&&!s.leave&&last>0.96f*(float)ZELL_WORLD_H);
    }
    /* Screen wall south edge: stop, then slide east without sticking. */
    zell_rect frame,display;
    zell_monitor_layout(3,&frame,&display);
    zell_rect wall=zell_command_wall_bounds();
    float edge=(float)(wall.y+wall.h);
    float foot_x=(float)frame.x+(float)frame.w*.5f;
    place_room(&s,1,foot_x,edge+20.f);
    assert(!zell_desktop_blocked(&s,s.x,s.y));
    assert(zell_desktop_blocked(&s,foot_x,edge-1.f));
    zell_controls north={.move_y=-1};
    for(int tick=0;tick<30;tick++)zell_scene_tick(&s,&north);
    assert(s.room==1&&s.y>=edge&&s.y<edge+ZELL_WALK_SPEED+0.05f&&s.y>400.f);
    place_room(&s,1,foot_x,edge);
    assert(!zell_desktop_blocked(&s,s.x,s.y));
    zell_controls east={.move_x=1};
    for(int tick=0;tick<8;tick++)zell_scene_tick(&s,&east);
    assert(s.y==edge&&s.x>foot_x+20.f&&s.step_ticks==8);
    place_room(&s,1,foot_x,edge+2.f);
    float held_x=s.x;
    zell_controls into={.move_x=1,.move_y=-1};
    for(int tick=0;tick<8;tick++)zell_scene_tick(&s,&into);
    assert(s.y>=edge&&s.y<=edge+2.f&&s.step_ticks==8);
    assert(fabsf(s.x-held_x-8.f*ZELL_WALK_SPEED/sqrtf(2.f))<.01f);
    /* A fitted view must not move world footprints. */
    s.room_view.scale=0.5f;s.room_view.offset_x=40;s.room_view.offset_y=-15;
    assert(zell_desktop_blocked(&s,foot_x,edge-1.f));
    assert(!zell_desktop_blocked(&s,(float)ZELL_WORLD_W*.50f,(float)ZELL_WORLD_H*.84f));
    /* A sprint from either side of each panel stops clear of the frame. */
    for(int i=0;i<ZELL_FLOOR_PANELS;i++){
        zell_rect panel;zell_floor_panel_layout(i,&panel,NULL);
        float cx=(float)panel.x+(float)panel.w*.5f;
        float front=(float)(panel.y+panel.h+ZELL_FOOT_RADIUS_Y);
        float back=(float)(panel.y-ZELL_FOOT_RADIUS_Y);
        place_room(&s,1,cx,ZELL_COMMAND_LANDING_Y);
        zell_controls up={.move_y=-1,.run=true};
        for(int tick=0;tick<25;tick++)zell_scene_tick(&s,&up);
        assert(s.room==1&&s.x==cx&&s.y>=front&&s.y<front+ZELL_WALK_SPEED&&!s.running&&s.step_ticks==0);
        zell_controls slide={.move_x=1,.move_y=-1,.run=true};
        for(int tick=0;tick<4;tick++)zell_scene_tick(&s,&slide);
        assert(fabsf(s.x-cx-4.f*ZELL_RUN_SPEED/sqrtf(2.f))<.01f);
        assert(s.y>=front&&s.y<front+ZELL_WALK_SPEED&&s.running&&s.step_ticks==4);
        if(i<2){
            zell_controls use={.confirm=true};zell_scene_tick(&s,&use);
            assert(s.station==46+i&&s.mode==ZELL_MODE_DESK&&s.desktop_launch);
        }
        place_room(&s,1,cx,back-12.f);
        zell_controls down={.move_y=1,.run=true};
        for(int tick=0;tick<25;tick++)zell_scene_tick(&s,&down);
        assert(s.room==1&&s.x==cx&&s.y<back&&s.y>=back-ZELL_WALK_SPEED&&!s.running);
    }
    /* The DJ, the librarian, and the workshop seat stay reachable. */
    place_room(&s,0,960.f,820.f);
    assert(!zell_desktop_blocked(&s,s.x,s.y));
    zell_controls toward={.move_x=1};
    for(int tick=0;tick<40&&s.x<zell_stations[4].x*(float)ZELL_WORLD_W;tick++)zell_scene_tick(&s,&toward);
    toward=(zell_controls){.move_y=1};
    for(int tick=0;tick<20;tick++)zell_scene_tick(&s,&toward);
    assert(zell_desktop_near(&s)==4&&!zell_desktop_blocked(&s,s.x,s.y));
    place_room(&s,0,1200.f,740.f);
    toward=(zell_controls){.move_x=1};
    for(int tick=0;tick<80;tick++)zell_scene_tick(&s,&toward);
    assert(zell_desktop_near(&s)==5&&!zell_desktop_blocked(&s,s.x,s.y));
    place_room(&s,4,790.f,520.f);
    assert(!zell_desktop_blocked(&s,790.f,420.f));
    toward=(zell_controls){.move_y=-1};
    for(int tick=0;tick<40&&s.y>421.f;tick++)zell_scene_tick(&s,&toward);
    assert(s.y<=421.f&&s.y>400.f&&!zell_desktop_blocked(&s,s.x,s.y));
    /* The grid tool prints the furniture table, not a second copy of it. */
    FILE *pipe=popen("python3 tools/room_grid.py --print","r");
    assert(pipe);
    char line[256];
    int index=0;
    while(fgets(line,sizeof line,pipe)){
        if(line[0]=='#'||line[0]=='\n')continue;
        int room;float x0,y0,x1,y1;
        assert(sscanf(line,"%d %f %f %f %f",&room,&x0,&y0,&x1,&y1)==5);
        int table_room;float tx0,ty0,tx1,ty1;
        assert(zell_desktop_block_table(index,&table_room,&tx0,&ty0,&tx1,&ty1));
        assert(room==table_room);
        assert(fabsf(x0-tx0)<0.01f&&fabsf(y0-ty0)<0.01f);
        assert(fabsf(x1-tx1)<0.01f&&fabsf(y1-ty1)<0.01f);
        index++;
    }
    assert(pclose(pipe)==0&&index>0);
    assert(!zell_desktop_block_table(index,&index,&edge,&edge,&edge,&edge));
    place_room(&s,1,768,820);
    float bx,by,bx1,by1;
    zell_desktop_block_rect(&s,0,&bx,&by,&bx1,&by1);
    assert(bx==(float)wall.x&&by==(float)wall.y);
    assert(bx1==(float)(wall.x+wall.w)&&by1==(float)(wall.y+wall.h));
}
static char *read_fixture(const char *path,char *buf,size_t cap){
    FILE *file=fopen(path,"r");
    assert(file);
    size_t n=fread(buf,1,cap-1,file);
    assert(!ferror(file));
    fclose(file);
    buf[n]=0;
    return buf;
}
static void click_world(zell_scene *s,float wx,float wy){
    float mx,my;
    zell_world_to_screen(s,wx,wy,&mx,&my);
    s->mouse_x=(int)floorf(mx);
    s->mouse_y=(int)floorf(my);
    s->mouse_click=true;
}
static int view_contains(const zell_scene *s,float wx,float wy){
    float sx,sy;
    zell_world_to_screen(s,wx,wy,&sx,&sy);
    return sx>=0.f&&sy>=(float)ZELL_ROOM_HUD_BOTTOM&&sx<(float)s->width&&sy<(float)(s->height-ZELL_ROOM_STATUS_STRIP);
}
static void test_room_fit(void){
    zell_scene s;
    float x,y,sx,sy,wx,wy,sx2,sy2;
    int room,cx,cy;
    const int fitted[2][2]={{960,600},{640,400}};
    zell_scene_init(&s,800,500);
    zell_desktop_init(&s);
    /* Zell starts with Kilix on the cave's entrance steps, in the stairs'
     * gap below the south wall, facing in; from there he walks in. */
    s.room=3;s.direction=ZELL_DIR_DOWN;
    zell_desktop_enter_hall(&s);
    assert(s.room==0&&s.direction==ZELL_DIR_UP&&s.x==(float)ZELL_WORLD_W*.5f);
    assert(s.y>(float)ZELL_WORLD_H*.91f&&!zell_desktop_blocked(&s,s.x,s.y)&&!zell_desktop_on_stairs(&s));
    {
        float start=s.y;zell_controls up={0};up.move_y=-1;
        for(int i=0;i<20;i++)zell_desktop_tick(&s,&up);
        assert(s.room==0&&s.y<start-20.f&&s.y<(float)ZELL_WORLD_H*.91f);
    }
    /* zell_desktop_start, which main() calls, is the desktop's set-up and
     * then that entrance, from wherever the scene was. */
    s.room=3;s.x=1.f;s.y=1.f;s.direction=ZELL_DIR_LEFT;s.desktop=false;s.hp=1;
    zell_desktop_start(&s);
    assert(s.desktop&&s.hp==100&&s.room==0&&s.direction==ZELL_DIR_UP);
    assert(s.x==(float)ZELL_WORLD_W*.5f&&s.y==(float)ZELL_WORLD_H*ZELL_HALL_ENTRY_Y);
    zell_desktop_init(&s);
    s.room=1;
    x=s.x;y=s.y;room=s.room;
    zell_scene_resize(&s,1536,1000);
    zell_room_view_identity(&s);
    assert(s.room_view.scale<1.f);
    assert(s.x==x&&s.y==y&&s.room==room);
    assert(s.room_view.logical_y==ZELL_ROOM_HUD_BOTTOM);
    assert(s.room_view.logical_h==ZELL_WORLD_H-ZELL_ROOM_HUD_BOTTOM-ZELL_ROOM_STATUS_STRIP);
    /* The Command Center is fitted across its whole width: its two bottom
     * corners hold the Command computer and the Key Master. */
    assert(s.room_view.logical_x==0);
    assert(s.room_view.logical_w==ZELL_WORLD_W);
    zell_world_to_screen(&s,100.f,200.f,&sx,&sy);
    zell_world_to_screen(&s,180.f,280.f,&sx2,&sy2);
    assert(fabsf((sx2-sx)-(sy2-sy))<0.001f);
    for(int i=0;i<2;i++){
        float top_x,top_y;
        zell_scene_resize(&s,fitted[i][0],fitted[i][1]);
        assert(s.x==x&&s.y==y&&s.room==room);
        assert(s.room_view.scale<1.f);
        zell_world_to_screen(&s,100.f,200.f,&sx,&sy);
        zell_world_to_screen(&s,180.f,200.f,&sx2,&sy2);
        assert(fabsf((sx2-sx)-80.f*s.room_view.scale)<0.001f);
        zell_world_to_screen(&s,100.f,280.f,&sx2,&sy2);
        assert(fabsf((sy2-sy)-80.f*s.room_view.scale)<0.001f);
        top_x=(float)s.room_view.logical_x+(float)s.room_view.logical_w*0.5f;
        top_y=(float)s.room_view.logical_y;
        zell_world_to_screen(&s,top_x,top_y,&sx,&sy);
        assert(sx>=0.f&&sx<(float)s.width);
        assert(sy>=(float)ZELL_ROOM_HUD_BOTTOM&&sy<(float)s.height);
        zell_screen_to_world(&s,sx,sy,&wx,&wy);
        assert(fabsf(wx-top_x)<1.f&&fabsf(wy-top_y)<1.f);
        zell_desktop_camera(&s,&cx,&cy);
        assert(cy>s.room_view.logical_y);
    }
#ifndef ZELL_RENDER_NEURONTE
    {
        sr_canvas big,small;
        zell_scene full,fitted_scene;
        int matched=0,differed=0;
        const float samples[4][2]={{1100.f,500.f},{900.f,800.f},{400.f,900.f},{250.f,200.f}};
        assert(sr_canvas_init(&big,ZELL_WORLD_W,ZELL_WORLD_H));
        assert(sr_canvas_init(&small,640,400));
        zell_scene_init(&full,ZELL_WORLD_W,ZELL_WORLD_H);zell_desktop_init(&full);
        full.room=1;full.flame_tick=0;
        zell_scene_init(&fitted_scene,640,400);zell_desktop_init(&fitted_scene);
        fitted_scene.room=1;fitted_scene.flame_tick=0;
        fitted_scene.x=full.x;fitted_scene.y=full.y;
        zell_draw(&full,&big);
        zell_draw(&fitted_scene,&small);
        zell_desktop_camera(&fitted_scene,&cx,&cy);
        for(int i=0;i<4;i++){
            float bx,by;int ix,iy,jx,jy,safe;
            zell_world_to_screen(&fitted_scene,samples[i][0],samples[i][1],&sx,&sy);
            ix=(int)floorf(sx);iy=(int)floorf(sy);
            safe=ix>=0&&ix<490&&iy>=ZELL_ROOM_HUD_BOTTOM&&iy<400-ZELL_ROOM_STATUS_STRIP;
            if(!safe)continue;
            zell_screen_to_world(&fitted_scene,(float)ix,(float)iy,&bx,&by);
            jx=(int)floorf(bx);jy=(int)floorf(by);
            assert(jx>=0&&jy>=0&&jx<ZELL_WORLD_W&&jy<ZELL_WORLD_H);
            assert(small.px[(size_t)iy*640u+(size_t)ix]==big.px[(size_t)jy*(size_t)ZELL_WORLD_W+(size_t)jx]);
            matched++;
            if(cx+ix>=0&&cy+iy>=0&&cx+ix<ZELL_WORLD_W&&cy+iy<ZELL_WORLD_H&&
               big.px[(size_t)(cy+iy)*(size_t)ZELL_WORLD_W+(size_t)(cx+ix)]!=small.px[(size_t)iy*640u+(size_t)ix])differed++;
        }
        assert(matched>=1&&differed>=1);
        sr_canvas_free(&big);sr_canvas_free(&small);
    }
    {
        sr_canvas big,small;
        zell_scene full,fitted_scene;
        const int rooms[2]={0,4};
        const float samples[2][2]={{119.f,330.f},{797.f,312.f}};
        assert(sr_canvas_init(&big,ZELL_WORLD_W,ZELL_WORLD_H));
        assert(sr_canvas_init(&small,960,600));
        for(int i=0;i<2;i++){
            float bx,by;int ix,iy,jx,jy;
            zell_scene_init(&full,ZELL_WORLD_W,ZELL_WORLD_H);zell_desktop_init(&full);
            full.room=rooms[i];full.flame_tick=0;full.x=700.f;full.y=700.f;
            zell_scene_init(&fitted_scene,960,600);zell_desktop_init(&fitted_scene);
            fitted_scene.room=rooms[i];fitted_scene.flame_tick=0;
            fitted_scene.x=full.x;fitted_scene.y=full.y;
            zell_room_view_identity(&full);zell_room_view_identity(&fitted_scene);
            zell_draw(&full,&big);zell_draw(&fitted_scene,&small);
            zell_world_to_screen(&fitted_scene,samples[i][0],samples[i][1],&sx,&sy);
            ix=(int)floorf(sx);iy=(int)floorf(sy);
            assert(ix>=0&&ix<960&&iy>=ZELL_ROOM_HUD_BOTTOM&&iy<600-ZELL_ROOM_STATUS_STRIP);
            zell_screen_to_world(&fitted_scene,(float)ix,(float)iy,&bx,&by);
            jx=(int)floorf(bx);jy=(int)floorf(by);
            assert(jx>=0&&jy>=0&&jx<ZELL_WORLD_W&&jy<ZELL_WORLD_H);
            assert(small.px[(size_t)iy*960u+(size_t)ix]==big.px[(size_t)jy*(size_t)ZELL_WORLD_W+(size_t)jx]);
        }
        sr_canvas_free(&big);sr_canvas_free(&small);
    }
#endif
    {
        const int views[3][2]={{960,600},{640,400},{820,1000}};
        const float cave_pts[3][2]={{119.f,330.f},{157.f,180.f},{768.f,1000.f}};
        const float down_pts[3][2]={{797.f,312.f},{796.f,364.f},{768.f,980.f}};
        for(int v=0;v<3;v++){
            zell_scene t;
            int ox,oy;
            zell_scene_init(&t,views[v][0],views[v][1]);
            zell_desktop_init(&t);
            t.room=0;t.mode=ZELL_MODE_WALK;t.mouse_valid=true;
            zell_room_view_identity(&t);
            assert(t.room_view.scale<1.f);
            assert(t.room_view.logical_x<=88&&t.room_view.logical_y<=156);
            assert(t.room_view.logical_x+t.room_view.logical_w>=ZELL_WORLD_W);
            assert(t.room_view.logical_y+t.room_view.logical_h>=ZELL_WORLD_H);
            for(int p=0;p<3;p++){
                assert(view_contains(&t,cave_pts[p][0],cave_pts[p][1]));
                zell_world_to_screen(&t,cave_pts[p][0],cave_pts[p][1],&sx,&sy);
                zell_screen_to_world(&t,sx,sy,&wx,&wy);
                assert(fabsf(wx-cave_pts[p][0])<1.f&&fabsf(wy-cave_pts[p][1])<1.f);
            }
            click_world(&t,119.f,330.f);
            assert(zell_desktop_click(&t)&&t.dictation_on);
            ox=t.room_view.offset_x;oy=t.room_view.offset_y;
            t.x=200.f;t.y=900.f;
            zell_room_view_identity(&t);
            assert(t.room_view.offset_x==ox&&t.room_view.offset_y==oy&&t.x==200.f&&t.y==900.f);
            for(int down=2;down<=4;down++){
                t.room=down;
                zell_room_view_identity(&t);
                assert(t.room_view.scale<1.f);
                assert(t.room_view.logical_x==(int)(0.135f*(float)ZELL_WORLD_W));
                assert(t.room_view.logical_y==64);
                assert(t.room_view.logical_y+t.room_view.logical_h==ZELL_WORLD_H);
                for(int p=0;p<3;p++){
                    assert(view_contains(&t,down_pts[p][0],down_pts[p][1]));
                    zell_world_to_screen(&t,down_pts[p][0],down_pts[p][1],&sx,&sy);
                    zell_screen_to_world(&t,sx,sy,&wx,&wy);
                    assert(fabsf(wx-down_pts[p][0])<1.f&&fabsf(wy-down_pts[p][1])<1.f);
                }
            }
        }
    }
}
static void test_workshop_hop(void){
    int peak=-1,peak_at=-1;
    for(int tick=0;tick<=ZELL_SEAT_HOP_TICKS;tick++){
        int lift=zell_seat_lift(tick);
        assert(lift>=0);
        if(tick==0||tick==ZELL_SEAT_HOP_TICKS)assert(lift==0);
        if(lift>peak){peak=lift;peak_at=tick;}
    }
    assert(peak_at==ZELL_SEAT_HOP_TICKS/2&&peak==ZELL_SEAT_LIFT_PEAK);
    for(int tick=0;tick<=ZELL_SEAT_HOP_TICKS;tick++)if(tick!=peak_at)assert(zell_seat_lift(tick)<peak);

    float floor_x,floor_y,seat_x,seat_y;
    zell_workshop_floor_point(&floor_x,&floor_y);
    zell_workshop_seat_point(&seat_x,&seat_y);
    /* Seat on the cushion; floor is south of the stool legs (opaque through y 383). */
    assert(floor_x==797.f&&floor_y==408.f&&seat_x==797.f&&seat_y==312.f);
    assert(seat_x>759.f&&seat_x<835.f&&seat_y>=291.f&&seat_y<=336.f&&floor_y>383.f);
    assert(fabsf(zell_stations[ZELL_WORKSHOP_COMPUTER].x*(float)ZELL_WORLD_W-floor_x)<0.5f);
    assert(fabsf(zell_stations[ZELL_WORKSHOP_COMPUTER].y*(float)ZELL_WORLD_H-floor_y)<0.5f);

    zell_scene s;
    zell_controls c;
    zell_scene_init(&s,640,400);zell_desktop_init(&s);
    for(int room=0;room<=3;room++){s.room=room;assert(!zell_desktop_station_visible(&s,ZELL_WORKSHOP_COMPUTER));}
    s.room=4;assert(zell_desktop_station_visible(&s,ZELL_WORKSHOP_COMPUTER));
    assert(!zell_desktop_station_visible(&s,ZELL_FLEET_BOARD));
    assert(!zell_desktop_station_visible(&s,ZELL_FLOOR_PANEL));
    s.x=floor_x;s.y=floor_y;s.mode=ZELL_MODE_WALK;
    assert(zell_desktop_near(&s)==ZELL_WORKSHOP_COMPUTER);
    {
        float kept=s.x;
        c=(zell_controls){.move_x=1};
        zell_scene_tick(&s,&c);
        assert(s.x>kept&&!s.seated&&s.seat_ticks==0);
        s.x=floor_x;s.y=floor_y;
    }
    snprintf(s.launcher_query,sizeof s.launcher_query,"Workshop computer");
    for(int i=0;i<zell_desktop_launcher_count(&s);i++)assert(zell_desktop_launcher_station(&s,i)!=ZELL_WORKSHOP_COMPUTER);
    s.launcher_query[0]=0;

    for(int i=0;i<ZELL_WINDOWS;i++){s.windows[i].used=true;snprintf(s.windows[i].title,sizeof s.windows[i].title,"Busy %d",i);}
    float blocked_x=s.x,blocked_y=s.y;
    c=(zell_controls){.confirm=true};
    zell_scene_tick(&s,&c);
    assert(!s.seated&&s.seat_ticks==0&&s.x==blocked_x&&s.y==blocked_y);
    assert(s.notice_count>0&&strstr(s.notice_text,"No window slot is free."));
    memset(s.windows,0,sizeof s.windows);
    s.notice_count=0;s.notice_text[0]=0;

    c=(zell_controls){.confirm=true};
    int launches=0;
    for(int tick=1;tick<=ZELL_SEAT_HOP_TICKS;tick++){
        bool before=s.desktop_launch;
        zell_scene_tick(&s,&c);
        c.confirm=false;
        float along=(float)tick/(float)ZELL_SEAT_HOP_TICKS;
        assert(s.seat_ticks==tick);
        assert(s.x==floor_x+(seat_x-floor_x)*along);
        assert(s.y==floor_y+(seat_y-floor_y)*along);
        assert(s.direction==ZELL_DIR_UP);
        if(tick<ZELL_SEAT_HOP_TICKS){
            assert(!s.seated&&!s.desktop_launch);
            assert(zell_seat_draw_dy(&s)==-zell_seat_lift(tick));
        }
        if(!before&&s.desktop_launch)launches++;
    }
    assert(launches==1&&s.seated&&s.desktop_launch);
    assert(s.x==seat_x&&s.y==seat_y);
    assert(zell_seat_lift(s.seat_ticks)==0&&zell_seat_draw_dy(&s)==ZELL_SEAT_SIT_DROP);
    assert(zell_desktop_pose(&s,-1).art==1+ZELL_DIR_UP);
    int slot=-1;
    for(int i=0;i<ZELL_WINDOWS;i++)if(s.windows[i].used&&!strcmp(s.windows[i].title,"Workshop computer"))slot=i;
    assert(slot>=0&&s.windows[slot].generation>0);

    s.desktop_launch=false;
    c=(zell_controls){0};
    zell_scene_tick(&s,&c);
    assert(!s.desktop_launch&&s.seated&&s.seat_ticks==ZELL_SEAT_HOP_TICKS);

    snprintf(s.terminal_title,sizeof s.terminal_title,"Workshop computer");
    s.mode=ZELL_MODE_SERVICE;s.service_phase=5;s.terminal_minimized=false;
    c=(zell_controls){.move_x=1};
    zell_scene_tick(&s,&c);
    assert(s.seated&&s.seat_ticks==ZELL_SEAT_HOP_TICKS&&!s.terminal_minimized&&s.mode==ZELL_MODE_SERVICE);

    s.terminal_width=80;s.terminal_height=40;s.width=640;s.height=400;
    zell_terminal_layout layout;zell_terminal_layout_for(&s,s.width,s.height,&layout);
    s.mouse_valid=true;s.mouse_click=true;s.mouse_x=layout.minimize.x+2;s.mouse_y=layout.minimize.y+2;
    assert(zell_terminal_click(&s));
    assert(s.terminal_minimized&&s.mode==ZELL_MODE_WALK&&s.seated&&s.seat_ticks==ZELL_SEAT_HOP_TICKS);

    c=(zell_controls){.move_x=1};
    zell_scene_tick(&s,&c);
    assert(s.seated&&s.seat_ticks==ZELL_SEAT_HOP_TICKS-1&&s.terminal_minimized&&!s.desktop_launch);
    c=(zell_controls){0};
    for(int expect=ZELL_SEAT_HOP_TICKS-1;expect>=1;expect--){
        assert(s.seat_ticks==expect&&s.seated);
        zell_scene_tick(&s,&c);
    }
    assert(!s.seated&&s.seat_ticks==0&&s.x==floor_x&&s.y==floor_y&&!s.desktop_launch);

    s.service_phase=5;
    snprintf(s.terminal_title,sizeof s.terminal_title,"Workshop computer");
    c=(zell_controls){.confirm=true};
    for(int tick=1;tick<=ZELL_SEAT_HOP_TICKS;tick++){
        bool before=s.desktop_launch;
        zell_scene_tick(&s,&c);
        c.confirm=false;
        if(!before&&s.desktop_launch)launches++;
    }
    assert(launches==1&&s.seated&&s.seat_ticks==ZELL_SEAT_HOP_TICKS&&!s.desktop_launch);
    assert(s.mode==ZELL_MODE_SERVICE&&!s.terminal_minimized);

    slot=-1;
    for(int i=0;i<ZELL_WINDOWS;i++)if(s.windows[i].used&&!strcmp(s.windows[i].title,"Workshop computer"))slot=i;
    assert(slot>=0);
    s.windows[slot].close_requested=true;
    c=(zell_controls){0};
    zell_scene_tick(&s,&c);
    assert(s.seated&&s.seat_ticks==ZELL_SEAT_HOP_TICKS&&!s.windows[slot].used);

    s.mode=ZELL_MODE_SERVICE;s.service_phase=4;s.station=ZELL_WORKSHOP_COMPUTER;
    snprintf(s.service_text,sizeof s.service_text,"Could not start this station's helper.");
    zell_scene_tick(&s,&c);
    assert(s.mode==ZELL_MODE_WALK&&s.seated&&s.seat_ticks==ZELL_SEAT_HOP_TICKS);
    assert(strstr(s.notice_text,"Could not start"));

    sr_canvas canvas;
    assert(sr_canvas_init(&canvas,320,240));
    zell_draw(&s,&canvas);
    sr_canvas_free(&canvas);
}
static void test_contract_fixtures(void){
    char text[4096];
    int rows=0,width,height,scale,offset_x,offset_y;
    FILE *geometry=fopen("tests/fixtures/contracts/geometry.txt","r");
    assert(geometry);
    while(fscanf(geometry,"%d %d %d %d %d",&width,&height,&scale,&offset_x,&offset_y)==5){
        zell_scene view;
        float sx,sy,wx,wy,x,y;
        int view_room;
        /* geometry.txt records the pre-fit adapter. The live view is the fit. */
        assert(scale==1&&offset_x==0&&offset_y==0);
        zell_scene_init(&view,width,height);
        zell_desktop_init(&view);
        x=view.x;y=view.y;view_room=view.room;
        zell_room_view_identity(&view);
        assert(view.x==x&&view.y==y&&view.room==view_room);
        zell_scene_resize(&view,width,height);
        assert(view.x==x&&view.y==y&&view.room==view_room);
        zell_world_to_screen(&view,(float)view.room_view.logical_x+25.f,(float)view.room_view.logical_y+25.f,&sx,&sy);
        zell_screen_to_world(&view,sx,sy,&wx,&wy);
        assert(fabsf(wx-((float)view.room_view.logical_x+25.f))<1.f);
        assert(fabsf(wy-((float)view.room_view.logical_y+25.f))<1.f);
        rows++;
    }
    fclose(geometry);
    assert(rows==2);
    read_fixture("tests/fixtures/contracts/launch-settings.json",text,sizeof text);
    assert(strstr(text,"\"prompt_ref\""));
    assert(!strstr(text,"\"prompt\""));
    read_fixture("tests/fixtures/contracts/provider-rows-v1.txt",text,sizeof text);
    {
        char *reject=strstr(text,"\nREJECT\n");
        zell_scene parsed;
        assert(reject);
        *reject=0;
        reject+=strlen("\nREJECT\n");
        zell_scene_init(&parsed,320,240);
        assert(zell_provider_apply(&parsed,text));
        assert(parsed.provider_count==2&&parsed.provider_generation==9);
        assert(!strcmp(parsed.provider_rows[0].id,"codex")&&parsed.provider_rows[0].percent==40);
        assert(!strcmp(parsed.provider_rows[1].id,"claude")&&parsed.provider_rows[1].percent==70);
        assert(parsed.provider_mana_ready);
        assert(parsed.provider_mana[0]==40&&parsed.provider_mana[1]==70&&parsed.provider_mana[2]==-1);
        assert(!zell_provider_apply(&parsed,reject));
        assert(parsed.provider_count==2&&parsed.provider_generation==9);
        assert(parsed.provider_mana[0]==40&&parsed.provider_mana[1]==70&&parsed.provider_mana[2]==-1);
    }
    {
        zell_scene slots;
        zell_scene_init(&slots,320,240);
        assert(zell_window_accepts_frame(&slots,0,0));
        assert(!zell_window_accepts_frame(&slots,1,0));
        assert(!zell_window_accepts_frame(&slots,3,0));
        assert(!zell_window_accepts_frame(&slots,-1,1));
        assert(!zell_window_accepts_frame(&slots,4,1));
        slots.windows[0].generation=3;
        slots.windows[2].generation=9;
        assert(zell_window_accepts_frame(&slots,0,0));
        assert(zell_window_accepts_frame(&slots,0,3));
        assert(!zell_window_accepts_frame(&slots,2,0));
        assert(zell_window_accepts_frame(&slots,2,9));
        assert(!zell_window_accepts_frame(&slots,2,3));
        assert(!zell_window_accepts_frame(&slots,1,9));
    }
}
static void test_fleet_board(void){
    zell_scene s;
    zell_rect board,frame,display;
    char line[160];
    sr_canvas canvas;
    zell_scene_init(&s,ZELL_WORLD_W,ZELL_WORLD_H);
    zell_desktop_init(&s);
    s.room=1;
    zell_fleet_board_layout(&board);
    zell_monitor_layout(4,&frame,&display);
    zell_rect panel;zell_floor_panel_layout(0,&panel,NULL);
    assert(board.y==panel.y&&board.w==panel.w&&board.h==panel.h);
    assert(board.y>frame.y+frame.h&&board.x>panel.x+panel.w);
    assert(zell_fleet_apply(&s,"1 3 1 0 1\nfleet-1\tNight shift\tLedger\t2\t1"));
    assert(zell_fleet_board_line(&s,0,line,sizeof line));
    assert(strstr(line,"Night shift"));
    assert(strstr(line,"Ledger"));
    assert(strstr(line,"2 workers"));
    assert(strstr(line,"1 live"));
    assert(!strstr(line,"/home"));
    assert(!strstr(line,"prompt"));
    assert(!zell_fleet_board_line(&s,1,line,sizeof line));
    for(int id=ZELL_WORKSHOP_COMPUTER;id<=ZELL_FLOOR_PANEL;id++){
        assert(zell_desktop_actions(id)==0);
        assert(zell_desktop_near(&s)!=id);
    }
    assert(!zell_desktop_station_visible(&s,ZELL_WORKSHOP_COMPUTER));
    assert(zell_desktop_station_visible(&s,ZELL_FLEET_BOARD));
    assert(zell_desktop_station_visible(&s,ZELL_FLOOR_PANEL));
    assert(sr_canvas_init(&canvas,ZELL_WORLD_W,ZELL_WORLD_H));
    zell_draw(&s,&canvas);
    assert((canvas.px[(board.y+100)*ZELL_WORLD_W+board.x+10]&0xffffffu)==0x050809u);
    sr_canvas_free(&canvas);
    s.mode=ZELL_MODE_WALK;s.mouse_valid=true;s.mouse_click=true;
    s.mouse_x=board.x+4;s.mouse_y=board.y+4;
    zell_desktop_click(&s);
    assert(s.station!=ZELL_FLEET_BOARD&&!s.desktop_launch);
}

static int region_changes(const sr_canvas *a,const sr_canvas *b,int x0,int x1,int y0,int y1){
    int changes=0;
    if(x0<0)x0=0;
    if(y0<0)y0=0;
    if(x1>a->w)x1=a->w;
    if(y1>a->h)y1=a->h;
    for(int y=y0;y<y1;y++)for(int x=x0;x<x1;x++){
        size_t at=(size_t)y*(size_t)a->w+(size_t)x;
        if(a->px[at]!=b->px[at])changes++;
    }
    return changes;
}
static void walk_notice_scene(zell_scene *s){
    zell_scene_init(s,640,400);
    zell_desktop_init(s);
    s->mode=ZELL_MODE_WALK;
    s->x=40;s->y=40;
    s->running=false;s->flame_tick=0;s->room=0;
    s->mouse_valid=false;s->dictation_on=false;
    assert(zell_desktop_near(s)<0);
    assert(s->width==640&&s->height==400&&s->room_view.scale<1.f);
}
static void test_notice_deadlines(void){
    zell_scene s;
    char key[64];
    zell_scene_init(&s,640,400);
    zell_notice_set_clock(5000);
    zell_notice_apply(&s,"Job finished");
    assert(!strcmp(s.notice_text,"Job finished"));
    assert(s.notice_until_ms==13000&&s.notice_key[0]);
    memcpy(key,s.notice_key,sizeof key);
    zell_notice_set_clock(9000);
    zell_notice_apply(&s,"Job finished");
    assert(s.notice_until_ms==13000&&!memcmp(s.notice_key,key,sizeof key));
    zell_notice_set_clock(10000);
    zell_notice_apply(&s,"Other notice");
    assert(!strcmp(s.notice_text,"Other notice")&&s.notice_until_ms==18000);
    assert(memcmp(s.notice_key,key,sizeof key)!=0);
    zell_notice_set_clock(18000);
    zell_notice_apply(&s,"Other notice");
    assert(s.notice_until_ms==18000);
    zell_notice_apply(&s,"");
    assert(!s.notice_until_ms&&!s.notice_key[0]&&!s.notice_text[0]);
    zell_notice_set_clock(20000);
    zell_notice_apply(&s,"Job finished");
    assert(!strcmp(s.notice_text,"Job finished")&&s.notice_until_ms==28000&&s.notice_key[0]);
}
static void test_notice_pixels(void){
    zell_scene s;
    sr_canvas plain,shown,later,expired,again,clipped;
    const char *hint="WASD walk | Hold Shift run | Tab stations";
    const char *toast="Notice";
    int hint_w=(int)strlen(hint)*8,left=12+hint_w+16,right=640-12;
    int text_x=right-(int)strlen(toast)*8;
    int strip=400-30;
    char long_text[301];
    char key[64];
    walk_notice_scene(&s);
    assert(sr_canvas_init(&plain,640,400)&&sr_canvas_init(&shown,640,400));
    assert(sr_canvas_init(&later,640,400)&&sr_canvas_init(&expired,640,400));
    assert(sr_canvas_init(&again,640,400)&&sr_canvas_init(&clipped,640,400));
    zell_notice_set_clock(5000);
    zell_draw(&s,&plain);
    zell_notice_apply(&s,toast);
    assert(s.notice_until_ms==13000);
    memcpy(key,s.notice_key,sizeof key);
    zell_draw(&s,&shown);
    /* Room image, including the old (12, 104) spot, stays put. Hints stay put. */
    assert(region_changes(&plain,&shown,0,640,30,strip)==0);
    assert(region_changes(&plain,&shown,0,text_x,strip,400)==0);
    assert(region_changes(&plain,&shown,text_x,right,strip,400)>0);
    assert(region_changes(&plain,&shown,right,640,strip,400)==0);
    zell_notice_set_clock(9000);
    zell_notice_apply(&s,toast);
    assert(s.notice_until_ms==13000&&!memcmp(s.notice_key,key,sizeof key));
    /* Hundreds of frames do not expire it. The clock does, on the millisecond. */
    s.flame_tick=400;
    zell_notice_set_clock(12999);
    zell_draw(&s,&later);
    zell_notice_set_clock(13000);
    zell_draw(&s,&expired);
    assert(region_changes(&later,&expired,0,640,30,strip)==0);
    assert(region_changes(&later,&expired,text_x,right,strip,400)>0);
    zell_notice_apply(&s,"");
    assert(!s.notice_until_ms&&!s.notice_key[0]);
    zell_notice_set_clock(20000);
    zell_notice_apply(&s,toast);
    assert(s.notice_until_ms==28000&&s.notice_key[0]);
    s.flame_tick=400;
    zell_draw(&s,&again);
    assert(region_changes(&expired,&again,text_x,right,strip,400)>0);
    memset(long_text,'M',300);long_text[300]=0;
    zell_notice_set_clock(30000);
    zell_notice_apply(&s,long_text);
    assert(strlen(s.notice_text)==300&&s.notice_until_ms==38000);
    s.flame_tick=0;
    zell_draw(&s,&clipped);
    zell_notice_apply(&s,"");
    s.flame_tick=0;
    zell_draw(&s,&plain);
    assert(region_changes(&plain,&clipped,0,640,30,strip)==0);
    assert(region_changes(&plain,&clipped,0,left,strip,400)==0);
    assert(region_changes(&plain,&clipped,left,right,strip,400)>0);
    assert(region_changes(&plain,&clipped,right,640,strip,400)==0);
    sr_canvas_free(&plain);sr_canvas_free(&shown);sr_canvas_free(&later);
    sr_canvas_free(&expired);sr_canvas_free(&again);sr_canvas_free(&clipped);
}
static void panel_scene(zell_scene *s){
    zell_scene_init(s,ZELL_WORLD_W,ZELL_WORLD_H);
    zell_desktop_init(s);
    s->room=1;s->x=768;s->y=400;s->mode=ZELL_MODE_WALK;
    zell_room_view_identity(s);
}
static uint32_t canvas_px(const sr_canvas *c,int x,int y){return c->px[(size_t)y*(size_t)c->w+(size_t)x];}
static int rect_diffs(const sr_canvas *a,const sr_canvas *b,zell_rect r){
    int n=0;
    for(int y=r.y;y<r.y+r.h;y++)for(int x=r.x;x<r.x+r.w;x++)
        if(canvas_px(a,x,y)!=canvas_px(b,x,y))n++;
    return n;
}
#ifndef ZELL_RENDER_NEURONTE
static int rim_diffs(const sr_canvas *a,const sr_canvas *b,zell_rect frame,zell_rect glass){
    int n=0;
    for(int y=frame.y;y<frame.y+frame.h;y++)for(int x=frame.x;x<frame.x+frame.w;x++){
        if(x>=glass.x-2&&x<glass.x+glass.w+2&&y>=glass.y-2&&y<glass.y+glass.h+2)continue;
        if(canvas_px(a,x,y)!=canvas_px(b,x,y))n++;
    }
    return n;
}
static bool block_same(const sr_canvas *a,int ax,int ay,const sr_canvas *b,int bx,int by){
    for(int y=0;y<8;y++)for(int x=0;x<8;x++)
        if(canvas_px(a,ax+x,ay+y)!=canvas_px(b,bx+x,by+y))return false;
    return true;
}
/* A hit is the whole glyph, one new color, and a change at every ink pixel. */
static void glyph_places(const sr_canvas *before,const sr_canvas *after,zell_rect search,zell_rect glass,const char *text,int *inside,int *outside){
    int ox[4096],oy[4096],ink=0,chars=(int)strlen(text),tw=chars*8,th=16;
    sr_canvas mark;
    *inside=*outside=0;
    assert(chars>0&&chars<40);
    assert(sr_canvas_init(&mark,tw,th));
    sr_text(&mark,0.0f,0.0f,text,0xffffffu,1.0f,1);
    for(int y=0;y<th;y++)for(int x=0;x<tw;x++){
        if((mark.px[(size_t)y*(size_t)tw+(size_t)x]>>24)<128)continue;
        assert(ink<4096);
        ox[ink]=x;oy[ink]=y;ink++;
    }
    sr_canvas_free(&mark);
    assert(ink>12);
    for(int y=search.y;y+th<=search.y+search.h;y++)for(int x=search.x;x+tw<=search.x+search.w;x++){
        uint32_t color=0;int matched=0;bool same=true;
        for(int i=0;i<ink;i++){
            int px=x+ox[i],py=y+oy[i];
            uint32_t now,was,rgb;
            if(px<0||py<0||px>=after->w||py>=after->h){same=false;break;}
            now=canvas_px(after,px,py);was=canvas_px(before,px,py);
            if(now==was){same=false;break;}
            rgb=now&0xffffffu;
            if(matched==0)color=rgb;else if(rgb!=color){same=false;break;}
            matched++;
        }
        if(!same||matched<=12)continue;
        if(x>=glass.x&&y>=glass.y&&x+tw<=glass.x+glass.w&&y+th<=glass.y+glass.h)(*inside)++;
        else (*outside)++;
    }
}
#endif
static void test_floor_panels(void){
    zell_scene s;
    zell_rect yolo,yolo_glass,agents,agents_glass,spare,spare_glass,sw;
    zell_controls c;
    panel_scene(&s);
    zell_floor_panel_layout(0,&yolo,&yolo_glass);
    zell_floor_panel_layout(1,&agents,&agents_glass);
    zell_floor_panel_layout(2,&spare,&spare_glass);
    zell_command_switch_layout(0,&sw);
    assert(yolo.x==sw.x&&yolo.y==sw.y&&yolo.w==sw.w&&yolo.h==sw.h);
    zell_command_switch_layout(1,&sw);
    assert(agents.x==sw.x&&agents.y==sw.y&&agents.w==sw.w&&agents.h==sw.h);
    assert(yolo_glass.y>yolo.y&&yolo_glass.x>yolo.x&&yolo_glass.x+yolo_glass.w<yolo.x+yolo.w);
    assert(yolo_glass.y+yolo_glass.h<yolo.y+yolo.h&&agents_glass.y>agents.y);
    assert(!zell_desktop_station_in_front(&s,46)&&!zell_desktop_station_in_front(&s,ZELL_FLOOR_PANEL));
    s.coding_yolo=true;s.coding_yolo_ready=true;s.coding_yolo_refresh=false;
    walk_out_of_command_center(&s);
    c=(zell_controls){.confirm=true};
    assert(s.room==0&&s.coding_yolo&&s.coding_yolo_ready);
    s.x=zell_stations[13].x*(float)ZELL_WORLD_W;s.y=zell_stations[13].y*(float)ZELL_WORLD_H;
    zell_scene_tick(&s,&c);
    assert(s.room==1&&s.coding_yolo_refresh&&!s.coding_yolo_ready&&s.coding_yolo);
    panel_scene(&s);
    s.mouse_valid=true;s.mouse_click=true;
    s.mouse_x=yolo.x+yolo.w/2;s.mouse_y=yolo.y+4;
    assert(s.mouse_y<yolo_glass.y);
    assert(zell_desktop_click(&s)&&s.station==46&&s.desktop_launch&&s.mode==ZELL_MODE_DESK);
    assert(!s.coding_yolo&&!s.coding_yolo_ready);
    panel_scene(&s);
    s.room_view.offset_x+=80;s.mouse_valid=true;s.mouse_click=true;
    s.mouse_x=yolo.x+2;s.mouse_y=yolo.y+2;
    assert(!zell_desktop_click(&s)&&s.mouse_click);
    s.mouse_x=yolo.x+2+80;s.mouse_click=true;
    assert(zell_desktop_click(&s)&&s.station==46&&s.desktop_launch);
    panel_scene(&s);
    s.station=3;s.mouse_valid=true;s.mouse_click=true;
    s.mouse_x=spare.x+spare.w/2;s.mouse_y=spare.y+4;
    assert(zell_desktop_click(&s)&&s.station==3&&s.mode==ZELL_MODE_WALK&&!s.desktop_launch&&!s.mouse_click);
    s.x=zell_stations[ZELL_FLOOR_PANEL].x*(float)ZELL_WORLD_W;
    s.y=zell_stations[ZELL_FLOOR_PANEL].y*(float)ZELL_WORLD_H+ZELL_FOOT_RADIUS_Y;
    assert(zell_desktop_near(&s)==ZELL_FLOOR_PANEL);
    c=(zell_controls){.confirm=true};zell_scene_tick(&s,&c);
    assert(s.station==ZELL_FLOOR_PANEL&&s.mode==ZELL_MODE_WALK&&!s.desktop_launch);
#ifndef ZELL_RENDER_NEURONTE
    {
        sr_canvas reading,other;
        zell_rect inner={yolo_glass.x+4,yolo_glass.y+4,yolo_glass.w-8,yolo_glass.h-8};
        int sx=yolo_glass.x+yolo_glass.w/2-4,sy=yolo_glass.y+yolo_glass.h-14;
        uint32_t read_px,on_px,off_px;
        int inside,outside;
        assert(sr_canvas_init(&reading,ZELL_WORLD_W,ZELL_WORLD_H));
        assert(sr_canvas_init(&other,ZELL_WORLD_W,ZELL_WORLD_H));
        panel_scene(&s);zell_draw(&s,&reading);
        assert(canvas_px(&reading,yolo.x+yolo.w/2,yolo.y+8)!=canvas_px(&reading,yolo.x+yolo.w/2,yolo.y-6));
        panel_scene(&s);s.coding_yolo=true;zell_draw(&s,&other);
        assert(rect_diffs(&reading,&other,yolo_glass)==0);
        assert(rim_diffs(&reading,&other,yolo,yolo_glass)==0);
        panel_scene(&s);s.coding_yolo_ready=true;s.coding_yolo=true;zell_draw(&s,&other);
        on_px=canvas_px(&other,sx+4,sy+4);
        assert(rect_diffs(&reading,&other,inner)>40);
        assert(rim_diffs(&reading,&other,yolo,yolo_glass)==0);
        panel_scene(&s);s.coding_yolo_ready=true;zell_draw(&s,&other);
        off_px=canvas_px(&other,sx+4,sy+4);
        read_px=canvas_px(&reading,sx+4,sy+4);
        assert((read_px&0xffffffu)==0x050809u&&on_px==read_px&&off_px==read_px);
        assert(rect_diffs(&reading,&other,inner)>40);
        assert(rim_diffs(&reading,&other,yolo,yolo_glass)==0);
        panel_scene(&s);s.coding_yolo=true;s.coding_yolo_ready=true;
        walk_out_of_command_center(&s);assert(s.room==0);
        c=(zell_controls){.confirm=true};
        s.x=zell_stations[13].x*(float)ZELL_WORLD_W;s.y=zell_stations[13].y*(float)ZELL_WORLD_H;
        zell_scene_tick(&s,&c);
        s.x=768;s.y=400;s.flame_tick=0;s.mode=ZELL_MODE_WALK;
        zell_draw(&s,&other);
        assert(rect_diffs(&reading,&other,yolo_glass)==0);
        panel_scene(&s);zell_draw(&s,&reading);
        s.update_age_days=80;zell_draw(&s,&other);
        assert(rect_diffs(&reading,&other,agents_glass)==0);
        glyph_places(&reading,&other,agents,agents_glass,"UPDATE AVAILABLE",&inside,&outside);
        assert(inside==0&&outside==0);
        glyph_places(&reading,&other,agents,agents_glass,"1/3",&inside,&outside);
        assert(inside==0&&outside==0);
        assert(zell_update_apply(&s,"1 4 1 3 running"));
        zell_draw(&s,&other);
        assert(rect_diffs(&reading,&other,agents_glass)>0);
        glyph_places(&reading,&other,agents,agents_glass,"1/3",&inside,&outside);
        assert(inside>=1&&outside==0);
        glyph_places(&reading,&other,agents,agents_glass,"UPDATE AVAILABLE",&inside,&outside);
        assert(inside==0&&outside==0);
        assert(!zell_update_apply(&s,"1 9 0 1 running"));
        zell_draw(&s,&reading);
        assert(rect_diffs(&reading,&other,agents_glass)==0);
        panel_scene(&s);zell_draw(&s,&reading);
        assert(!zell_update_apply(&s,"1 4 4 3 running"));
        assert(!s.update_job_active);
        zell_draw(&s,&other);
        assert(rect_diffs(&reading,&other,agents_glass)==0);
        glyph_places(&reading,&other,agents,agents_glass,"1/3",&inside,&outside);
        assert(inside==0&&outside==0);
        panel_scene(&s);zell_draw(&s,&reading);
        s.room_view.offset_x+=40;zell_draw(&s,&other);
        assert(block_same(&reading,yolo.x+2,sy,&other,yolo.x+2+40,sy));
        assert(!block_same(&reading,yolo.x+2,sy,&reading,yolo.x+2+40,sy));
        panel_scene(&s);zell_draw(&s,&reading);
        s.x=(float)(yolo_glass.x+yolo_glass.w/2);
        s.y=(float)(yolo_glass.y+yolo_glass.h-6);
        zell_draw(&s,&other);
        assert(rect_diffs(&reading,&other,inner)>20);
        sr_canvas_free(&reading);sr_canvas_free(&other);
    }
#endif
}
/* Hatches draw from their bottom edge. Measure the stone rim above it. */
static int hatch_rim_gap(const zell_art *art){
    for(int gap=0;gap<art->height;gap++){
        const uint32_t *row=art->pixels+(art->height-1-gap)*art->width;
        for(int x=0;x<art->width;x++)if((row[x]>>24)>=128)return gap;
    }
    return -1;
}

/* These two doors use the Workshop's stonework and hinges. In either pose
 * the only pixels that change are inside the room-specific lid panel. */
static void check_hatch_panel(int art,int frame_art,zell_rect panel){
    const zell_art *door=zell_art_get(art),*frame=zell_art_get(frame_art);
    assert(door&&frame&&door->width==frame->width&&door->height==frame->height);
    int changed=0;
    for(int y=0;y<door->height;y++){
        for(int x=0;x<door->width;x++){
            int pixel=y*door->width+x;
            if(door->pixels[pixel]==frame->pixels[pixel])continue;
            assert(x>=panel.x&&x<panel.x+panel.w&&y>=panel.y&&y<panel.y+panel.h);
            changed++;
        }
    }
    assert(changed>panel.w*panel.h/2);
}

static void test_lower_hatches(void){
    const int stations[]={ZELL_COMMAND_HATCH,ZELL_LAUNDRY_HATCH,ZELL_VAULT_HATCH,ZELL_WORKSHOP_HATCH};
    for(int room=1;room<=2;room++){
        const zell_cave_hatch *hatch=&zell_cave_hatches[room];
        check_hatch_panel(zell_stations[hatch->station].art,ZELL_ART_WORKSHOP_HATCH,(zell_rect){20,21,96,52});
        check_hatch_panel(hatch->open_art,ZELL_ART_WORKSHOP_HATCH_OPEN,(zell_rect){30,14,76,34});
    }
    zell_scene s;
    sr_canvas away,near;
    zell_scene_init(&s,ZELL_WORLD_W,ZELL_WORLD_H);
    zell_desktop_init(&s);
    assert(sr_canvas_init(&away,s.width,s.height)&&sr_canvas_init(&near,s.width,s.height));
    s.x=768;s.y=940;
    zell_room_view_identity(&s);
    zell_draw(&s,&away);
    for(int i=0;i<4;i++){
        int station=stations[i];
        const zell_station *p=&zell_stations[station];
        const zell_art *closed=zell_art_get(p->art);
        assert(closed);
        int x=(int)(p->x*ZELL_WORLD_W),y=(int)(p->y*ZELL_WORLD_H);
        int foot=y+ZELL_HATCH_FOOT_OFFSET,top=foot-closed->height;
        zell_rect lid={x-25,top-17,50,14},well={x-25,top+20,50,25};
        s.x=(float)(x+84);s.y=(float)y;
        assert(zell_desktop_near(&s)==station);
        zell_draw(&s,&near);
        assert(rect_diffs(&away,&near,lid)>300);
        assert(rect_diffs(&away,&near,well)>600);
        /* The opaque front sill stays on the same floor pixels. */
        int checked=0;
        for(int row=closed->height-6;row<closed->height;row++){
            for(int col=15;col<closed->width-15;col++){
                if((closed->pixels[row*closed->width+col]>>24)<248)continue;
                int px=x-closed->width/2+col,py=top+row;
                assert(pixel_gap(canvas_px(&away,px,py),canvas_px(&near,px,py))<=8);
                checked++;
            }
        }
        assert(checked>100);
        s.x=768;s.y=940;
        zell_draw(&s,&near);
        assert(rect_diffs(&away,&near,lid)==0&&rect_diffs(&away,&near,well)==0);
        /* The floor above a closed hatch becomes clickable as the lid rises. */
        s.mouse_valid=true;
        click_world(&s,(float)x,(float)(top-10));
        assert(!zell_desktop_click(&s));
        for(int opened=0;opened<=1;opened++){
            s.x=opened?(float)(x+84):768.f;s.y=opened?(float)y:940.f;
            click_world(&s,(float)x,(float)(opened?top-10:y-16));
            assert(zell_desktop_click(&s)&&s.room==i+1);
            s.room=0;s.mode=ZELL_MODE_WALK;
            zell_room_view_identity(&s);
        }
    }
    sr_canvas_free(&away);sr_canvas_free(&near);
}

static void test_workshop_puzzllm_hatch(void){
    zell_scene s;
    zell_scene_init(&s,ZELL_WORLD_W,ZELL_WORLD_H);
    zell_desktop_init(&s);
    int station=ZELL_PUZZLLM_HATCH;
    for(int room=0;room<=ZELL_DOWNSTAIRS_ROOMS;room++){
        s.room=room;
        assert(zell_desktop_station_visible(&s,station)==(room==4));
    }
    s.room=4;s.mouse_valid=true;
    zell_room_view_identity(&s);
    const zell_station *p=&zell_stations[station];
    int x=(int)(p->x*ZELL_WORLD_W),y=(int)(p->y*ZELL_WORLD_H);
    int top=y+ZELL_HATCH_FOOT_OFFSET-zell_art_get(p->art)->height;
    s.x=768;s.y=860;
    sr_canvas away,near;
    assert(sr_canvas_init(&away,s.width,s.height)&&sr_canvas_init(&near,s.width,s.height));
    zell_draw(&s,&away);
    click_world(&s,(float)x,(float)top-10);
    assert(!zell_desktop_click(&s));
    click_world(&s,(float)x,(float)y-20);
    assert(zell_desktop_click(&s)&&s.desktop_launch&&s.room==4&&s.station==station);

    s.mode=ZELL_MODE_WALK;s.desktop_launch=false;s.x=(float)x+84;s.y=(float)y;
    assert(zell_desktop_near(&s)==station);
    zell_draw(&s,&near);
    assert(rect_diffs(&away,&near,(zell_rect){x-24,top-14,48,12})>250);
    click_world(&s,(float)x,(float)top-10);
    assert(zell_desktop_click(&s)&&s.desktop_launch&&s.station==station);

    s.mode=ZELL_MODE_WALK;s.desktop_launch=false;
    zell_controls enter={.confirm=true};
    zell_scene_tick(&s,&enter);
    assert(s.desktop_launch&&s.station==station&&s.room==4&&s.x==(float)x+84&&s.y==(float)y);
    sr_canvas_free(&away);sr_canvas_free(&near);
}

static void test_four_rooms(void){
    zell_scene s;
    zell_controls c;
    const int hatches[4]={ZELL_GAME_HATCH,ZELL_THEATER_HATCH,ZELL_MEDITATION_HATCH,ZELL_LIBRARY_HATCH};
    const float xs[4]={.30f,.46f,.62f,.78f};
    const int expect_blocks[4]={3,3,3,3};
    const float floors[4]={380.f,735.f,400.f,400.f};
    float sx,sy,wx,wy;
    assert(ZELL_ART_GAME_ROOM==ZELL_ART_WORKSHOP_STOOL+1);
    assert(ZELL_ART_THEATER_ROOM==ZELL_ART_GAME_ROOM+1);
    assert(ZELL_ART_GAME_FOREGROUND==ZELL_ART_LIBRARY_ROOM+1);
    assert(ZELL_ART_THEATER_FOREGROUND==ZELL_ART_GAME_FOREGROUND+1);
    assert(ZELL_ART_MEDITATION_FOREGROUND==ZELL_ART_THEATER_FOREGROUND+1);
    assert(ZELL_ART_LIBRARY_FOREGROUND==ZELL_ART_MEDITATION_FOREGROUND+1);
    assert(ZELL_ART_LIBRARY_HATCH_OPEN==ZELL_ART_WORKSHOP_STOOL+16);
    assert(ZELL_ART_GAME_HATCH_OPEN==ZELL_ART_GAME_HATCH+4);
    for(int i=0;i<4;i++){
        float hx,hy;
        int blocks;
        assert(zell_stations[hatches[i]].x==xs[i]);
        assert(zell_stations[hatches[i]].y==.52f);
        assert(zell_stations[hatches[i]].y<zell_stations[13].y);
        assert(zell_desktop_actions(hatches[i])==1);
        hx=zell_stations[hatches[i]].x*(float)ZELL_WORLD_W;
        hy=zell_stations[hatches[i]].y*(float)ZELL_WORLD_H;
        place_room(&s,0,hx,hy);
        assert(zell_desktop_station_visible(&s,hatches[i]));
        assert(zell_desktop_near(&s)==hatches[i]);
        c=(zell_controls){.confirm=true};
        zell_scene_tick(&s,&c);
        assert(s.room==5+i&&s.mode==ZELL_MODE_WALK&&!s.desktop_launch);
        assert(!zell_desktop_on_stairs(&s));
        assert(!zell_desktop_blocked(&s,s.x,s.y));
        assert(!zell_desktop_blocked(&s,768.f,970.f));
        assert(!zell_desktop_blocked(&s,(float)ZELL_WORLD_W*.50f,(float)ZELL_WORLD_H*ZELL_ROOM_ENTRY_Y));
        assert(zell_desktop_blocked(&s,768.f,floors[i]-40.f));
        assert(fabsf(zell_desktop_north(5+i)-floors[i])<0.1f);
        if(i==1)assert(!zell_desktop_blocked(&s,886.f,862.f));
        assert(zell_desktop_block_count(&s)==expect_blocks[i]);
        blocks=zell_desktop_block_count(&s);
        for(int b=0;b<blocks;b++){
            float x0,y0,x1,y1,mx,my;
            zell_desktop_block_rect(&s,b,&x0,&y0,&x1,&y1);
            mx=(x0+x1)*0.5f;my=(y0+y1)*0.5f;
            assert(zell_desktop_blocked(&s,mx,my));
            assert(!(mx>676.f&&mx<860.f&&my>900.f));
            walk_into_rect(&s,x0,y0,x1,y1);
        }
        place_room(&s,5+i,(float)ZELL_WORLD_W*.50f,(float)ZELL_WORLD_H*.78f);
        c=(zell_controls){.move_y=1};
        {
            int ticks=0;
            while(s.room==5+i&&ticks<400){zell_scene_tick(&s,&c);ticks++;}
        }
        assert(s.room==0&&s.mode==ZELL_MODE_WALK&&!s.leave);
        assert(fabsf(s.x-hx)<1.f);
        assert(fabsf(s.y-hy-50.f)<1.f);
        for(int other=1;other<=4;other++){
            s.room=other;
            assert(!zell_desktop_station_visible(&s,hatches[i]));
        }
    }
    {
        const int views[4][2]={{1536,1000},{960,600},{640,400},{820,1000}};
        for(int v=0;v<4;v++){
            for(int i=0;i<4;i++){
                float hx=zell_stations[hatches[i]].x*(float)ZELL_WORLD_W;
                float hy=zell_stations[hatches[i]].y*(float)ZELL_WORLD_H;
                zell_scene_init(&s,views[v][0],views[v][1]);
                zell_desktop_init(&s);
                s.room=0;s.mode=ZELL_MODE_WALK;s.mouse_valid=true;
                zell_room_view_identity(&s);
                assert(view_contains(&s,hx,hy));
                click_world(&s,hx,hy);
                assert(zell_desktop_click(&s));
                assert(s.room==5+i&&s.mode==ZELL_MODE_WALK);
                zell_room_view_identity(&s);
                assert(s.room_view.logical_x==0&&s.room_view.logical_y==0);
                assert(s.room_view.logical_w==ZELL_WORLD_W&&s.room_view.logical_h==ZELL_WORLD_H);
                assert(s.room_view.scale<1.f);
                if(i==1){
                    assert(view_contains(&s,64.f,54.f)&&view_contains(&s,1471.f,54.f));
                    assert(view_contains(&s,64.f,638.f)&&view_contains(&s,1471.f,638.f));
                    assert(view_contains(&s,82.f,677.f)&&view_contains(&s,1451.f,720.f));
                    assert(view_contains(&s,840.f,790.f)&&view_contains(&s,931.f,934.f));
                }
                assert(view_contains(&s,768.f,980.f));
                zell_world_to_screen(&s,768.f,980.f,&sx,&sy);
                zell_screen_to_world(&s,sx,sy,&wx,&wy);
                assert(fabsf(wx-768.f)<1.f&&fabsf(wy-980.f)<1.f);
            }
        }
    }
    {
        const int sizes[2][2]={{1920,1080},{1536,1024}};
        for(int v=0;v<2;v++){
            zell_scene wide;
            zell_scene_init(&wide,sizes[v][0],sizes[v][1]);
            zell_desktop_init(&wide);
            wide.room=1;
            zell_room_view_identity(&wide);
            assert(wide.room_view.scale==1.f);
            assert(wide.room_view.offset_x==(wide.width-ZELL_WORLD_W)/2);
            assert(wide.room_view.offset_y==wide.room_view.logical_y);
            for(int room=5;room<=8;room++){
                wide.room=room;
                zell_room_view_identity(&wide);
                assert(wide.room_view.logical_x==0&&wide.room_view.logical_y==0);
                assert(wide.room_view.logical_w==ZELL_WORLD_W&&wide.room_view.logical_h==ZELL_WORLD_H);
                assert(wide.room_view.scale<1.f);
                assert(wide.room_view.viewport_y==ZELL_ROOM_HUD_BOTTOM);
            }
            wide.room=6;
            zell_room_view_identity(&wide);
            assert(view_contains(&wide,64.f,54.f)&&view_contains(&wide,1471.f,54.f));
            assert(view_contains(&wide,64.f,638.f)&&view_contains(&wide,1471.f,638.f));
            assert(view_contains(&wide,82.f,677.f)&&view_contains(&wide,1451.f,720.f));
        }
    }
#ifndef ZELL_RENDER_NEURONTE
    {
        sr_canvas big,small;
        zell_scene full,fitted;
        float bx,by,rx,ry;
        int ix,iy,jx,jy,ref_w,ref_h,sx_i,sy_i;
        ref_w=ZELL_WORLD_W;
        ref_h=ZELL_WORLD_H+ZELL_ROOM_HUD_BOTTOM+ZELL_ROOM_STATUS_STRIP;
        assert(sr_canvas_init(&big,ref_w,ref_h));
        assert(sr_canvas_init(&small,960,600));
        zell_scene_init(&full,ref_w,ref_h);zell_desktop_init(&full);
        full.room=6;full.flame_tick=0;full.x=700.f;full.y=500.f;
        zell_scene_init(&fitted,960,600);zell_desktop_init(&fitted);
        fitted.room=6;fitted.flame_tick=0;fitted.x=full.x;fitted.y=full.y;
        zell_room_view_identity(&full);zell_room_view_identity(&fitted);
        assert(full.room_view.scale==1.f);
        assert(full.room_view.offset_y==ZELL_ROOM_HUD_BOTTOM);
        zell_draw(&full,&big);zell_draw(&fitted,&small);
        zell_world_to_screen(&fitted,900.f,800.f,&sx,&sy);
        ix=(int)floorf(sx);iy=(int)floorf(sy);
        assert(ix>=0&&ix<960&&iy>=ZELL_ROOM_HUD_BOTTOM&&iy<600-ZELL_ROOM_STATUS_STRIP);
        zell_screen_to_world(&fitted,(float)ix,(float)iy,&bx,&by);
        jx=(int)floorf(bx);jy=(int)floorf(by);
        assert(jx>=0&&jy>=0&&jx<ZELL_WORLD_W&&jy<ZELL_WORLD_H);
        zell_world_to_screen(&full,(float)jx,(float)jy,&rx,&ry);
        sx_i=(int)floorf(rx);sy_i=(int)floorf(ry);
        assert(sx_i>=0&&sy_i>=ZELL_ROOM_HUD_BOTTOM&&sx_i<ref_w&&sy_i<ref_h-ZELL_ROOM_STATUS_STRIP);
        assert(small.px[(size_t)iy*960u+(size_t)ix]==big.px[(size_t)sy_i*(size_t)ref_w+(size_t)sx_i]);
        sr_canvas_free(&big);sr_canvas_free(&small);
    }
#endif
    if(ZELL_ART_COUNT>ZELL_ART_LIBRARY_HATCH_OPEN){
        const int rooms[4]={ZELL_ART_GAME_ROOM,ZELL_ART_THEATER_ROOM,ZELL_ART_MEDITATION_ROOM,ZELL_ART_LIBRARY_ROOM};
        const int fronts[4]={ZELL_ART_GAME_FOREGROUND,ZELL_ART_THEATER_FOREGROUND,ZELL_ART_MEDITATION_FOREGROUND,ZELL_ART_LIBRARY_FOREGROUND};
        const zell_art *command=zell_art_get(ZELL_ART_COMMAND_HATCH);
        for(int i=0;i<4;i++){
            const zell_art *back=zell_art_get(rooms[i]);
            const zell_art *front=zell_art_get(fronts[i]);
            const zell_art *closed=zell_art_get(ZELL_ART_GAME_HATCH+i);
            const zell_art *open=zell_art_get(ZELL_ART_GAME_HATCH_OPEN+i);
            assert(back&&back->width==1536&&back->height==1024);
            assert(front&&front->width==1536&&front->height==1024);
            assert(command&&closed&&closed->width==command->width&&closed->width==136&&closed->height==155);
            assert(open&&open->width==closed->width);
            int gap=hatch_rim_gap(closed);
            assert(gap>=0&&gap==hatch_rim_gap(open));
            float hx=zell_stations[hatches[i]].x*(float)ZELL_WORLD_W;
            float hy=zell_stations[hatches[i]].y*(float)ZELL_WORLD_H;
            int foot=(int)hy+14;
            int top=foot-open->height;
            int staff=(int)(zell_stations[1].y*(float)ZELL_WORLD_H);
            int old_top=(int)(zell_stations[13].y*(float)ZELL_WORLD_H)+14-command->height;
            assert(top>staff&&foot<old_top);
            place_room(&s,0,hx,hy);
            s.mouse_valid=true;
            zell_room_view_identity(&s);
            click_world(&s,hx-26.f,(float)(top+5));
            assert(zell_desktop_click(&s)&&s.room==5+i);
        }
    }
    {
        const float samples[4][2]={{768.f,350.f},{768.f,500.f},{768.f,350.f},{200.f,350.f}};
        for(int i=0;i<4;i++){
            zell_controls north={.move_y=-1},south={.move_y=1};
            place_room(&s,5+i,(float)ZELL_WORLD_W*.50f,(float)ZELL_WORLD_H*ZELL_ROOM_ENTRY_Y);
            assert(zell_desktop_blocked(&s,samples[i][0],samples[i][1]));
            for(int tick=0;tick<500&&s.room==5+i;tick++){
                float y=s.y;
                zell_scene_tick(&s,&north);
                if(s.y>=y-0.01f)break;
            }
            assert(s.room==5+i&&fabsf(s.y-floors[i])<1.f&&s.y>samples[i][1]);
            s.x=(float)ZELL_WORLD_W*.50f;
            for(int tick=0;tick<400&&s.room==5+i;tick++)zell_scene_tick(&s,&south);
            assert(s.room==0&&s.mode==ZELL_MODE_WALK);
        }
    }
}
static void put_le32(unsigned char *p,uint32_t v){
    p[0]=(unsigned char)(v&255u);
    p[1]=(unsigned char)((v>>8)&255u);
    p[2]=(unsigned char)((v>>16)&255u);
    p[3]=(unsigned char)((v>>24)&255u);
}
static int rects_overlap(const zell_rect *a,const zell_rect *b){
    return a->x<b->x+b->w&&b->x<a->x+a->w&&a->y<b->y+b->h&&b->y<a->y+a->h;
}
#ifndef ZELL_RENDER_NEURONTE
static int cue_ink(const sr_canvas *before,const sr_canvas *after,zell_rect r){
    int n=0;
    if(r.w<1||r.h<1)return 0;
    for(int y=r.y;y<r.y+r.h;y++){
        if(y<0||y>=after->h)continue;
        for(int x=r.x;x<r.x+r.w;x++){
            uint32_t now;
            if(x<0||x>=after->w)continue;
            now=canvas_px(after,x,y);
            if(now==canvas_px(before,x,y))continue;
            if((now&0xffffffu)==0xe7e2d4u)n++;
        }
    }
    return n;
}
static int cue_ink_outside(const sr_canvas *before,const sr_canvas *after,zell_rect bar){
    int n=0;
    for(int y=0;y<after->h;y++)for(int x=0;x<after->w;x++){
        uint32_t now=canvas_px(after,x,y);
        if(now==canvas_px(before,x,y))continue;
        if((now&0xffffffu)!=0xe7e2d4u)continue;
        if(x>=bar.x&&x<bar.x+bar.w&&y>=bar.y&&y<bar.y+bar.h)continue;
        n++;
    }
    return n;
}
static int rgb_count(const sr_canvas *c,zell_rect r,uint32_t rgb){
    int n=0,x0=r.x,y0=r.y,x1=r.x+r.w,y1=r.y+r.h;
    if(x0<0)x0=0;
    if(y0<0)y0=0;
    if(x1>c->w)x1=c->w;
    if(y1>c->h)y1=c->h;
    for(int y=y0;y<y1;y++)for(int x=x0;x<x1;x++)
        if((canvas_px(c,x,y)&0xffffffu)==rgb)n++;
    return n;
}
static void check_two_line_cue(int width,int height,bool fullscreen){
    zell_scene s;
    zell_controls key={0};
    sr_canvas before,after;
    zell_rect bar,line0,line1,queue,nav;
    char cue_buf[384];
    const char *cue="{\"event\":\"caption\",\"id\":\"cue\",\"text\":\"The train leaves at midnight.\\nDo not miss the last car.\",\"generation\":1}";
    bool had_nav=false;
    zell_theater_test_reset();
    zell_scene_init(&s,width,height);zell_desktop_init(&s);
    s.room=6;s.mode=ZELL_MODE_WALK;s.mouse_valid=true;s.flame_tick=0;
    zell_room_view_identity(&s);
    if(fullscreen){
        zell_rect overlap;
        int notice_y;
        /* Queued items and an error used to cover this bar. Fullscreen hides them. */
        for(int i=0;i<8;i++){
            snprintf(cue_buf,sizeof cue_buf,"{\"event\":\"status\",\"id\":\"q%d\",\"state\":\"paused\",\"generation\":%d,\"browser\":null}",i+1,i+1);
            assert(zell_theater_apply_line(&s,cue_buf)==0);
        }
        assert(zell_theater_queue_count()==8&&strcmp(zell_theater_picture(),"q1")==0);
        assert(zell_theater_apply_line(&s,"{\"event\":\"install\",\"id\":\"q1\",\"phase\":\"request\",\"argv\":[\"sudo\"]}")==0);
        assert(strstr(zell_theater_error(),"refused")&&!s.theater_install_request);
        zell_theater_queue_rect(0,width,height,&queue);
        zell_theater_fullscreen_layout(width,height,NULL,&overlap,NULL,NULL);
        assert(queue.w>0&&queue.y+queue.h<=overlap.y);
        notice_y=height-ZELL_ROOM_STATUS_STRIP-18;
        assert(notice_y<overlap.y+overlap.h&&notice_y+16>overlap.y);
        had_nav=zell_theater_queue_nav(width,height,1,&nav);
        key.fullscreen_key=true;
        assert(zell_desktop_tick(&s,&key)==1&&zell_theater_fullscreen());
        zell_theater_queue_rect(0,width,height,&queue);
        assert(queue.w==0);
        assert(!zell_theater_queue_nav(width,height,-1,&nav)&&!zell_theater_queue_nav(width,height,1,&nav));
        snprintf(cue_buf,sizeof cue_buf,"{\"event\":\"caption\",\"id\":\"q1\",\"text\":\"The train leaves at midnight.\\nDo not miss the last car.\",\"generation\":8}");
        cue=cue_buf;
    }
    assert(sr_canvas_init(&before,width,height)&&sr_canvas_init(&after,width,height));
    zell_draw(&s,&before);
    assert(zell_theater_apply_line(&s,cue)==0);
    assert(strchr(zell_theater_caption(),'\n'));
    s.flame_tick=0;
    zell_draw(&s,&after);
    zell_theater_caption_screen(&s,width,height,&bar,&line0,&line1);
    assert(bar.h>=32&&line0.h==16&&line1.h==16);
    assert(line0.y>=bar.y&&line0.y+line0.h<=bar.y+bar.h);
    assert(line1.y==line0.y+16&&line1.y+line1.h<=bar.y+bar.h);
    if(!fullscreen){
        float sx,sy;
        zell_world_to_screen(&s,64.f,639.f,&sx,&sy);
        assert(bar.y>=(int)floorf(sy));
        zell_world_to_screen(&s,840.f,790.f,&sx,&sy);
        assert(bar.y+bar.h<=(int)floorf(sy));
        assert(bar.y+bar.h<=height-ZELL_ROOM_STATUS_STRIP);
    }else assert(bar.h==44&&bar.y+bar.h==height);
    assert(cue_ink(&before,&after,line0)>12);
    assert(cue_ink(&before,&after,line1)>12);
    assert(cue_ink_outside(&before,&after,bar)==0);
    if(fullscreen){
        zell_rect help,whole=(zell_rect){0,0,width,height};
        zell_theater_fullscreen_layout(width,height,NULL,NULL,&help,NULL);
        assert(rgb_count(&after,whole,0xe8ffe9u)==0);
        assert(rgb_count(&after,whole,0xffb4a8u)==0);
        assert(rgb_count(&after,help,0xf0d69bu)>0);
        memset(&key,0,sizeof key);key.cancel=true;
        assert(zell_desktop_tick(&s,&key)==1&&!zell_theater_fullscreen());
        zell_theater_queue_rect(0,width,height,&queue);
        assert(queue.w>0&&zell_theater_queue_nav(width,height,1,&nav)==had_nav);
        s.flame_tick=0;
        zell_draw(&s,&before);
        assert(rgb_count(&before,whole,0xe8ffe9u)>0);
        assert(rgb_count(&before,whole,0xffb4a8u)>0);
    }
    sr_canvas_free(&before);sr_canvas_free(&after);
    zell_theater_test_reset();
}
#endif
static void test_theater(void){
    zell_scene s;
    zell_controls c;
    zell_rect queue,tab,panel,rows[12];
    unsigned char map[128];
    uint32_t pixels[8],seq;
    int x,y,w,h,ow,oh,nrow;
    char line[1024];
    float y0;
    zell_theater_test_reset();
    zell_theater_contain(1920,1080,1408,585,&x,&y,&w,&h);
    assert(x==184&&y==0&&w==1040&&h==585);
    memset(map,0,sizeof map);
    seq=0;
    assert(zell_theater_copy_frame(map,sizeof map,1,&seq,pixels,8,&ow,&oh)==0);
    map[0]='Z';map[1]='T';map[2]='H';map[3]='R';
    put_le32(map+4,1);put_le32(map+8,2);put_le32(map+12,2);put_le32(map+16,2);
    put_le32(map+32,8);put_le32(map+36,1);put_le32(map+40,7);
    memset(map+64,0x11,16);
    assert(zell_theater_copy_frame(map,sizeof map,7,&seq,pixels,8,&ow,&oh)==1);
    assert(seq==2&&ow==2&&oh==2);
    assert(zell_theater_copy_frame(map,sizeof map,7,&seq,pixels,8,&ow,&oh)==0);
    put_le32(map+16,3);seq=0;
    assert(zell_theater_copy_frame(map,sizeof map,7,&seq,pixels,8,&ow,&oh)==0&&seq==0);
    put_le32(map+16,2);seq=0;
    assert(zell_theater_copy_frame(map,sizeof map,9,&seq,pixels,8,&ow,&oh)==0);
    put_le32(map+16,0);
    assert(zell_theater_copy_frame(map,sizeof map,7,&seq,pixels,8,&ow,&oh)==-1);
    zell_theater_queue_rect(0,1536,1000,&queue);
    tab=(zell_rect){ZELL_WINDOW_ROW_X,1000-64,236,26};
    assert(queue.x>=260&&!rects_overlap(&queue,&tab));
    zell_scene_init(&s,1536,1000);zell_desktop_init(&s);
    s.room=6;s.mode=ZELL_MODE_WALK;s.mouse_valid=true;
    zell_room_view_identity(&s);
    click_world(&s,886.f,862.f);
    assert(zell_desktop_click(&s)&&zell_theater_menu_open()&&zell_theater_queue_count()==0);
    zell_theater_menu_layout(&s,&panel,rows,12,&nrow);
    assert(nrow>=1&&rows[0].w>0);
    s.mouse_click=true;s.mouse_x=rows[0].x+2;s.mouse_y=rows[0].y+2;
    assert(zell_desktop_click(&s)&&zell_theater_typing(&s));
    memset(&c,0,sizeof c);strcpy(c.edits,"f");
    assert(zell_desktop_tick(&s,&c)==1&&!zell_theater_fullscreen());
    memset(&c,0,sizeof c);c.confirm=true;
    assert(zell_desktop_tick(&s,&c)==1);
    assert(strstr(zell_theater_last_command(),"\"cmd\":\"open\""));
    assert(strstr(zell_theater_last_command(),"\"browser\":null"));
    assert(strstr(zell_theater_last_command(),"\"path\":\"f\""));
    assert(strstr(zell_theater_error(),"theater helper"));
    assert(zell_theater_queue_count()==0&&!zell_theater_fullscreen());
    zell_theater_test_reset();
    zell_scene_init(&s,1536,1000);zell_desktop_init(&s);
    s.room=6;s.mode=ZELL_MODE_WALK;s.mouse_valid=true;
    zell_room_view_identity(&s);
    click_world(&s,886.f,862.f);
    assert(zell_desktop_click(&s)&&zell_theater_menu_open());
    memset(&c,0,sizeof c);c.fullscreen_key=true;
    assert(zell_desktop_tick(&s,&c)==1&&zell_theater_fullscreen());
    memset(&c,0,sizeof c);c.cancel=true;
    assert(zell_desktop_tick(&s,&c)==1&&!zell_theater_fullscreen()&&s.mode==ZELL_MODE_WALK);
    zell_theater_test_reset();
    place_room(&s,0,(float)ZELL_WORLD_W*.50f,(float)ZELL_WORLD_H*.50f);
    y0=s.y;memset(&c,0,sizeof c);c.fullscreen_key=true;
    zell_desktop_tick(&s,&c);
    assert(!zell_theater_fullscreen()&&s.y==y0&&s.mode==ZELL_MODE_WALK);
    zell_theater_test_reset();
    zell_scene_init(&s,960,600);zell_desktop_init(&s);s.room=6;s.mode=ZELL_MODE_WALK;
    assert(zell_theater_apply_line(&s,"{\"event\":\"caption\",\"id\":\"q1\",\"text\":\"hello\",\"generation\":3}")==0);
    assert(strcmp(zell_theater_caption(),"hello")==0);
    map[0]='Z';map[1]='T';map[2]='H';map[3]='R';
    put_le32(map+4,1);put_le32(map+8,2);put_le32(map+12,2);put_le32(map+16,2);
    put_le32(map+32,8);put_le32(map+36,1);put_le32(map+40,3);
    assert(zell_theater_present(map,sizeof map)==1);
    put_le32(map+16,4);put_le32(map+40,9);
    assert(zell_theater_present(map,sizeof map)==0);
    assert(!zell_theater_submit(&s,"{\"cmd\":\"use\",\"id\":\"q1\"}"));
    assert(strcmp(zell_theater_caption(),"hello")==0);
    assert(strstr(zell_theater_last_command(),"\"cmd\":\"use\""));
    assert(strstr(zell_theater_error(),"theater helper"));
    put_le32(map+16,6);put_le32(map+40,3);
    assert(zell_theater_present(map,sizeof map)==1);
    /* Folder listing and a rejected open keep this picture. A newer
     * generation is the switch, and the old frames stop after that. */
    zell_theater_test_reset();
    zell_scene_init(&s,960,600);zell_desktop_init(&s);
    assert(zell_theater_apply_line(&s,"{\"event\":\"status\",\"id\":\"movie\",\"state\":\"playing\",\"generation\":4,\"browser\":null}")==0);
    assert(strcmp(zell_theater_picture(),"movie")==0&&zell_theater_playing());
    assert(zell_theater_apply_line(&s,"{\"event\":\"caption\",\"id\":\"movie\",\"text\":\"on screen\",\"generation\":4}")==0);
    map[0]='Z';map[1]='T';map[2]='H';map[3]='R';
    put_le32(map+4,1);put_le32(map+8,2);put_le32(map+12,2);put_le32(map+16,2);
    put_le32(map+32,8);put_le32(map+36,1);put_le32(map+40,4);
    memset(map+64,0x22,16);
    assert(zell_theater_present(map,sizeof map)==1);
    assert(!zell_theater_submit(&s,"{\"cmd\":\"open\",\"id\":\"fold\",\"browser\":null,\"source\":{\"kind\":\"folder\",\"path\":\"/clips\"}}"));
    assert(zell_theater_apply_line(&s,"{\"event\":\"listing\",\"id\":\"fold\",\"entries\":[{\"name\":\"b.mp4\",\"path\":\"/clips/b.mp4\"}]}")==0);
    assert(zell_theater_listing_count()==1);
    assert(strcmp(zell_theater_picture(),"movie")==0&&strcmp(zell_theater_caption(),"on screen")==0);
    put_le32(map+16,4);
    assert(zell_theater_present(map,sizeof map)==1);
    assert(zell_theater_apply_line(&s,"{\"event\":\"error\",\"error\":\"decode\",\"id\":\"bad\",\"detail\":\"file has no video\"}")==0);
    assert(strstr(zell_theater_error(),"file has no video"));
    assert(strcmp(zell_theater_picture(),"movie")==0&&strcmp(zell_theater_caption(),"on screen")==0);
    put_le32(map+16,6);
    assert(zell_theater_present(map,sizeof map)==1);
    assert(zell_theater_apply_line(&s,"{\"event\":\"error\",\"error\":\"install\",\"detail\":\"cancelled\"}")==0);
    assert(strcmp(zell_theater_picture(),"movie")==0&&strcmp(zell_theater_caption(),"on screen")==0);
    assert(!zell_theater_submit(&s,"{\"cmd\":\"use\",\"id\":\"missing\"}"));
    assert(zell_theater_apply_line(&s,"{\"event\":\"error\",\"error\":\"malformed\",\"detail\":\"unknown id\"}")==0);
    assert(!zell_theater_submit(&s,"{\"cmd\":\"close\",\"id\":\"movie\"}"));
    assert(zell_theater_apply_line(&s,"{\"event\":\"error\",\"error\":\"malformed\",\"detail\":\"unknown id\"}")==0);
    assert(strcmp(zell_theater_picture(),"movie")==0&&strcmp(zell_theater_caption(),"on screen")==0);
    put_le32(map+16,8);
    assert(zell_theater_present(map,sizeof map)==1);
    assert(zell_theater_apply_line(&s,"{\"event\":\"status\",\"id\":\"next\",\"state\":\"playing\",\"generation\":5,\"browser\":null}")==0);
    assert(strcmp(zell_theater_picture(),"next")==0&&zell_theater_caption()[0]==0);
    put_le32(map+16,10);put_le32(map+40,4);
    assert(zell_theater_present(map,sizeof map)==0);
    put_le32(map+16,2);put_le32(map+40,5);
    assert(zell_theater_present(map,sizeof map)==1);
    assert(zell_theater_apply_line(&s,"{\"event\":\"caption\",\"id\":\"movie\",\"text\":\"stale\",\"generation\":4}")==0);
    assert(zell_theater_caption()[0]==0);
    assert(zell_theater_apply_line(&s,"{\"event\":\"caption\",\"id\":\"next\",\"text\":\"next cue\",\"generation\":5}")==0);
    assert(strcmp(zell_theater_caption(),"next cue")==0);
    assert(zell_theater_format_open_file(line,sizeof line,"q1","clips/reel.mp4")>0);
    assert(strstr(line,"\"cmd\":\"open\"")&&strstr(line,"clips/reel.mp4")&&strstr(line,"\"browser\":null"));
    assert(zell_theater_install_action_ok("chrome","install-chrome-deb","google-chrome-stable_current_amd64.deb"));
    assert(zell_theater_install_action_ok("firefox-esr","install-firefox-esr",NULL));
    assert(zell_theater_install_action_ok("firefox-esr","install-firefox-esr",""));
    assert(zell_theater_install_action_ok("chromium","install-chromium",NULL));
    assert(!zell_theater_install_action_ok("chrome","install-chrome-deb",NULL));
    assert(!zell_theater_install_action_ok("chrome","install-chrome-deb","google-chrome-stable"));
    assert(!zell_theater_install_action_ok("firefox-esr","install-firefox-esr","google-chrome-stable_current_amd64.deb"));
    assert(!zell_theater_install_action_ok("chromium","install-chrome-deb",NULL));
    zell_theater_test_reset();
    zell_scene_init(&s,960,600);zell_desktop_init(&s);
    assert(zell_theater_apply_line(&s,"{\"event\":\"install\",\"id\":\"q1\",\"phase\":\"request\",\"argv\":[\"sudo\",\"apt-get\"]}")==0);
    assert(strstr(zell_theater_error(),"refused"));
    assert(strstr(zell_theater_last_command(),"\"ok\":false"));
    assert(!s.theater_install_request);
    zell_theater_test_reset();
    assert(zell_theater_apply_line(&s,"{\"event\":\"install\",\"id\":\"q1\",\"browser\":\"firefox-esr\",\"phase\":\"request\",\"action\":\"install-firefox-esr\"}")==1);
    assert(s.theater_install_request);
    assert(zell_theater_error()[0]==0);
    assert(zell_theater_install_json(line,sizeof line));
    assert(strstr(line,"Theater install")&&strstr(line,"firefox-esr")&&strstr(line,"install-firefox-esr"));
    assert(!strstr(line,"argv")&&!strstr(line,"sudo"));
    zell_theater_test_reset();
    assert(zell_theater_apply_line(&s,"{\"event\":\"install\",\"id\":\"q2\",\"browser\":\"chrome\",\"phase\":\"request\",\"action\":\"install-chrome-deb\",\"deb\":\"google-chrome-stable_current_amd64.deb\"}")==1);
    assert(zell_theater_install_json(line,sizeof line));
    assert(strstr(line,"install-chrome-deb")&&strstr(line,"google-chrome-stable_current_amd64.deb"));
    assert(!strstr(line,"argv")&&!strstr(line,"sudo"));
    zell_theater_test_reset();
    zell_scene_init(&s,960,600);zell_desktop_init(&s);
    assert(zell_theater_apply_line(&s,"{\"event\":\"install\",\"id\":\"q3\",\"browser\":\"chrome\",\"phase\":\"request\",\"action\":\"install-chrome-deb\"}")==0);
    assert(strstr(zell_theater_error(),"refused"));
    assert(!s.theater_install_request);
    zell_theater_test_reset();
    zell_scene_init(&s,960,600);zell_desktop_init(&s);
    assert(zell_theater_apply_line(&s,"{\"event\":\"caption\",\"id\":\"q1\",\"text\":\"caf\\u00e9\",\"generation\":1}")==0);
    assert(strcmp(zell_theater_caption(),"caf\xc3\xa9")==0);
    assert(zell_theater_apply_line(&s,"{\"event\":\"caption\",\"id\":\"q1\",\"text\":\"\\uD83D\\uDE00\",\"generation\":2}")==0);
    assert(strcmp(zell_theater_caption(),"\xf0\x9f\x98\x80")==0);
    assert(zell_theater_apply_line(&s,"{\"event\":\"caption\",\"id\":\"q1\",\"text\":\"caf\xc3\xa9\",\"generation\":3}")==0);
    assert(strcmp(zell_theater_caption(),"caf\xc3\xa9")==0);
    assert(zell_theater_apply_line(&s,"{\"event\":\"caption\",\"id\":\"q1\",\"text\":\"\\uD800\",\"generation\":4}")==0);
    assert(zell_theater_caption()[0]==0);
    assert(zell_theater_apply_line(&s,"{\"event\":\"status\",\"id\":\"a\",\"state\":\"playing\",\"generation\":5,\"browser\":null}")==0);
    assert(strcmp(zell_theater_picture(),"a")==0&&zell_theater_playing());
    assert(zell_theater_apply_line(&s,"{\"event\":\"caption\",\"id\":\"a\",\"text\":\"newer\",\"generation\":7}")==0);
    assert(strcmp(zell_theater_caption(),"newer")==0);
    assert(zell_theater_apply_line(&s,"{\"event\":\"caption\",\"id\":\"a\",\"text\":\"older\",\"generation\":6}")==0);
    assert(strcmp(zell_theater_caption(),"newer")==0);
    assert(zell_theater_apply_line(&s,"{\"event\":\"status\",\"id\":\"b\",\"state\":\"paused\",\"generation\":8,\"browser\":null}")==0);
    assert(strcmp(zell_theater_picture(),"a")==0&&zell_theater_playing());
    assert(zell_theater_queue_count()==2);
    map[0]='Z';map[1]='T';map[2]='H';map[3]='R';
    put_le32(map+4,1);put_le32(map+8,2);put_le32(map+12,2);put_le32(map+16,2);
    put_le32(map+32,8);put_le32(map+36,1);put_le32(map+40,8);
    assert(zell_theater_present(map,sizeof map)==1);
    put_le32(map+16,4);put_le32(map+40,7);
    assert(zell_theater_present(map,sizeof map)==0);
    {
        char listing[4600],path[3001];
        memset(path,'b',3000);path[3000]=0;
        snprintf(listing,sizeof listing,"{\"event\":\"listing\",\"id\":\"fold\",\"entries\":[{\"name\":\"caf\\u00e9.mp4\",\"path\":\"%s\"}]}",path);
        zell_theater_test_reset();
        assert(zell_theater_apply_line(&s,listing)==0);
        assert(zell_theater_listing_count()==1);
        assert(strcmp(zell_theater_listing_path(0),path)==0);
        assert(strlen(zell_theater_listing_path(0))==3000);
        assert(zell_theater_queue_count()==0);
    }
    zell_theater_test_reset();
    zell_scene_init(&s,1536,1000);zell_desktop_init(&s);
    s.room=6;s.mode=ZELL_MODE_WALK;s.mouse_valid=true;
    zell_room_view_identity(&s);
    assert(zell_theater_apply_line(&s,"{\"event\":\"status\",\"id\":\"q1\",\"state\":\"paused\",\"generation\":1,\"browser\":null}")==0);
    assert(strcmp(zell_theater_picture(),"q1")==0&&!zell_theater_playing()&&zell_theater_queue_count()==1);
    click_world(&s,886.f,862.f);
    assert(zell_desktop_click(&s)&&zell_theater_menu_open());
    zell_theater_menu_layout(&s,&panel,rows,12,&nrow);
    assert(nrow>=8);
    s.mouse_click=true;s.mouse_x=rows[7].x+2;s.mouse_y=rows[7].y+2;
    assert(zell_desktop_click(&s));
    assert(strstr(zell_theater_last_command(),"\"cmd\":\"use\""));
    assert(strstr(zell_theater_last_command(),"q1"));
    assert(!zell_theater_playing());
    zell_theater_test_reset();
    zell_scene_init(&s,640,400);zell_desktop_init(&s);
    s.room=6;s.mode=ZELL_MODE_WALK;s.mouse_valid=true;
    for(int i=0;i<8;i++){
        char cmd[180],st[180];
        snprintf(cmd,sizeof cmd,"{\"cmd\":\"open\",\"id\":\"q%d\",\"source\":{\"kind\":\"file\",\"path\":\"clips/reel%d.mp4\"},\"browser\":null}",i+1,i);
        assert(!zell_theater_submit(&s,cmd));
        snprintf(st,sizeof st,"{\"event\":\"status\",\"id\":\"q%d\",\"state\":\"paused\",\"generation\":%d,\"browser\":null}",i+1,i+1);
        assert(zell_theater_apply_line(&s,st)==0);
    }
    assert(zell_theater_queue_count()==8);
    assert(strcmp(zell_theater_queue_label(0),"reel0.mp4")==0);
    assert(strcmp(zell_theater_queue_label(7),"reel7.mp4")==0);
    assert(strcmp(zell_theater_picture(),"q1")==0);
    {
        zell_rect shown,hidden,nav;
        zell_theater_queue_rect(0,640,400,&shown);
        zell_theater_queue_rect(4,640,400,&hidden);
        assert(shown.w>0&&hidden.w==0);
        assert(zell_theater_queue_nav(640,400,1,&nav));
        s.mouse_click=true;s.mouse_x=nav.x+1;s.mouse_y=nav.y+1;
        assert(zell_desktop_click(&s));
        zell_theater_queue_rect(0,640,400,&hidden);
        zell_theater_queue_rect(7,640,400,&shown);
        assert(hidden.w==0&&shown.w>0);
    }
    zell_theater_test_reset();
    zell_scene_init(&s,960,600);zell_desktop_init(&s);
    s.room=6;s.mode=ZELL_MODE_WALK;s.mouse_valid=true;
    zell_room_view_identity(&s);
    assert(zell_theater_apply_line(&s,"{\"event\":\"status\",\"id\":\"q1\",\"state\":\"playing\",\"generation\":1,\"browser\":\"firefox-esr\"}")==0);
    click_world(&s,200.f,200.f);
    assert(zell_desktop_click(&s)&&zell_theater_wants_keys(&s));
    memset(&c,0,sizeof c);
    c.key_press=true;c.key_code='a';c.key_text='a';c.key_modifiers=1u|16u;
    assert(zell_desktop_tick(&s,&c)==1);
    assert(strstr(zell_theater_last_command(),"\"cmd\":\"key\""));
    assert(strstr(zell_theater_last_command(),"\"action\":\"down\""));
    assert(strstr(zell_theater_last_command(),"\"modifiers\":1,"));
    assert(strstr(zell_theater_last_command(),"\"text\":\"a\""));
    assert(!strstr(zell_theater_last_command(),"\"action\":\"up\""));
    memset(&c,0,sizeof c);
    c.key_press=true;c.key_code=0xe001u;c.key_text=0;
    assert(zell_desktop_tick(&s,&c)==1);
    assert(strstr(zell_theater_last_command(),"\"key\":\"Enter\""));
    assert(strstr(zell_theater_last_command(),"\"action\":\"down\""));
    {
        zell_rect picture,caption,help,drop;
        zell_theater_fullscreen_layout(1920,1080,&picture,&caption,&help,&drop);
        assert(picture.y==28&&caption.y==picture.y+picture.h&&caption.h==44);
        assert(help.y<picture.y&&drop.y==picture.y);
    }
    zell_theater_test_reset();
    zell_scene_init(&s,960,600);zell_desktop_init(&s);
    s.room=6;s.mode=ZELL_MODE_WALK;s.mouse_valid=true;
    zell_room_view_identity(&s);
    click_world(&s,886.f,862.f);
    assert(zell_desktop_click(&s)&&zell_theater_menu_open());
    memset(&c,0,sizeof c);c.fullscreen_key=true;
    assert(zell_desktop_tick(&s,&c)==1&&zell_theater_fullscreen());
    {
        zell_rect picture,caption,help,drop;
        zell_theater_fullscreen_layout(s.width,s.height,&picture,&caption,&help,&drop);
        s.mouse_click=true;s.mouse_x=help.x+2;s.mouse_y=help.y+2;
        assert(zell_desktop_click(&s)&&zell_theater_help_open());
        assert(drop.y==28);
    }
#ifndef ZELL_RENDER_NEURONTE
    check_two_line_cue(640,400,false);
    check_two_line_cue(960,600,false);
    check_two_line_cue(1536,1024,false);
    check_two_line_cue(640,400,true);
    check_two_line_cue(960,600,true);
    {
        sr_canvas cut,kept;
        assert(sr_canvas_init(&cut,80,20)&&sr_canvas_init(&kept,80,20));
        zell_ui_text(&cut,0,0,"abcd\xf0\x9f\x98\x80" "EXTRA",0xe7e2d4u,64);
        zell_ui_text(&kept,0,0,"abcd...",0xe7e2d4u,64);
        assert(memcmp(cut.px,kept.px,(size_t)80*20*sizeof(uint32_t))==0);
        sr_canvas_free(&cut);sr_canvas_free(&kept);
    }
#endif
    zell_theater_test_reset();
}
/* A character's name is shown only while Kilix stands by it. That holds for
 * the cave's first six and for the later staff alike, upstairs and down:
 * nobody carries a name tag about. */
static bool rects_meet(zell_rect a,zell_rect b){
    return a.x<b.x+b.w&&b.x<a.x+a.w&&a.y<b.y+b.h&&b.y<a.y+a.h;
}
/* Pixels that differ between two canvases inside a rectangle, in either renderer. */
static int region_diffs(const sr_canvas *a,const sr_canvas *b,zell_rect r){
    int n=0;
    for(int y=r.y;y<r.y+r.h;y++)for(int x=r.x;x<r.x+r.w;x++){
        if(x<0||y<0||x>=a->w||y>=a->h)continue;
        if(a->px[(size_t)y*(size_t)a->w+(size_t)x]!=b->px[(size_t)y*(size_t)b->w+(size_t)x])n++;
    }
    return n;
}
/* Pixels that differ outside every one of `n` rectangles. */
static int diffs_outside(const sr_canvas *a,const sr_canvas *b,const zell_rect *keep,int n){
    int count=0;
    for(int y=0;y<a->h;y++)for(int x=0;x<a->w;x++){
        bool kept=false;
        for(int i=0;i<n&&!kept;i++)kept=x>=keep[i].x&&x<keep[i].x+keep[i].w&&y>=keep[i].y&&y<keep[i].y+keep[i].h;
        if(!kept&&a->px[(size_t)y*(size_t)a->w+(size_t)x]!=b->px[(size_t)y*(size_t)b->w+(size_t)x])count++;
    }
    return count;
}
static int ink_count(const sr_canvas *c,zell_rect r,uint32_t rgb,int within){
    int n=0;
    for(int y=r.y<0?0:r.y;y<r.y+r.h&&y<c->h;y++)for(int x=r.x<0?0:r.x;x<r.x+r.w&&x<c->w;x++)
        if(pixel_gap(c->px[(size_t)y*(size_t)c->w+(size_t)x]&0xffffffu,rgb)<=within)n++;
    return n;
}
static void test_names_show_only_beside_kilix(void){
    static const struct{int room,station;}who[]={
        {0,0},{0,1},{0,2},{0,3},{0,4},{0,5},{0,32},{0,37},{0,38},
        {1,29},{1,30},{1,31},{2,34},{2,35},{3,39},{3,40},{4,33},{4,36},{4,41}};
    static const float beside[][2]={{-70,12},{70,12},{0,64},{-52,52},{52,52},{0,-64},{-80,0},{80,0}};
    static const float away[][2]={{-320,0},{320,0},{0,-260},{0,260},{-260,-200},{260,200},{260,-200},{-260,200}};
    /* The name beside Kilix is written in the first colour. The staff added
     * later used to carry theirs about in the second. */
    const uint32_t beside_ink=0xb9f8dbu,standing=0xe9d49fu;
    /* The bitmap font gives every pixel of a letter the ink itself. The
     * Neuronte letters have soft edges, so there a pixel counts when it is
     * close to the ink, and the count is taken over what was there before
     * the name. "DJ" is the shortest name. */
#ifdef ZELL_RENDER_NEURONTE
    const int within=48,need=12;
#else
    const int within=0,need=24;
#endif
    sr_canvas canvas;
    assert(sr_canvas_init(&canvas,1600,1100));
    for(size_t i=0;i<sizeof who/sizeof who[0];i++){
        int id=who[i].station;
        float wx=zell_stations[id].x*(float)ZELL_WORLD_W,wy=zell_stations[id].y*(float)ZELL_WORLD_H;
        float sx,sy;
        zell_scene s;
        size_t k;
        zell_scene_init(&s,1600,1100);zell_desktop_init(&s);
        s.room=who[i].room;s.mode=ZELL_MODE_WALK;s.leave=false;s.mouse_valid=false;
        assert(zell_desktop_station_visible(&s,id));
        /* Somewhere in the room that is not beside this character. */
        for(k=0;k<sizeof away/sizeof away[0];k++){
            s.x=wx+away[k][0];s.y=wy+away[k][1];
            if(s.x>120&&s.x<ZELL_WORLD_W-120&&s.y>160&&s.y<ZELL_WORLD_H-120&&zell_desktop_near(&s)!=id)break;
        }
        assert(k<sizeof away/sizeof away[0]);
        zell_draw(&s,&canvas);
        assert(s.room_view.scale==1.f);
        zell_world_to_screen(&s,wx,wy,&sx,&sy);
        /* The row a name is written on, under the feet. It stops short of
         * the "Enter: ..." line that the rooms downstairs add below it. */
        /* The Key Master's name is written in the strip under the AGENTS
         * panel, right aligned (ZELL_KEY_MASTER_LABEL_RIGHT). */
        float lx,ly;
        zell_world_to_screen(&s,(float)(ZELL_KEY_MASTER_LABEL_RIGHT-96),(float)ZELL_KEY_MASTER_LABEL_TOP,&lx,&ly);
        zell_rect under=id==31?(zell_rect){(int)lx,(int)ly,96,22}:(zell_rect){(int)sx-90,(int)sy+(id==29||id==30?-165:4),180,32};
        int ink_away=ink_count(&canvas,under,beside_ink,within);
        int standing_away=ink_count(&canvas,under,standing,0);
        /* Downstairs, the "Enter: ..." line goes under the name. */
        const uint32_t hint_ink=0xa8f3d5u;
        int hint_away=ink_count(&canvas,(zell_rect){(int)sx-130,(int)sy+(id==29||id==30?-141:37),260,22},hint_ink,within);
        bool gold_floor=false;
        if((!gold_floor&&standing_away)||ink_count(&canvas,under,beside_ink,0))
            fprintf(stderr,"%s carries a name with Kilix away\n",zell_stations[id].name);
        assert(gold_floor||standing_away==0);
        assert(ink_count(&canvas,under,beside_ink,0)==0);
        for(k=0;k<sizeof beside/sizeof beside[0];k++){
            s.x=wx+beside[k][0];s.y=wy+beside[k][1];
            if(zell_desktop_near(&s)==id)break;
        }
        assert(k<sizeof beside/sizeof beside[0]);
        zell_draw(&s,&canvas);
        zell_world_to_screen(&s,wx,wy,&sx,&sy);
        zell_world_to_screen(&s,(float)(ZELL_KEY_MASTER_LABEL_RIGHT-96),(float)ZELL_KEY_MASTER_LABEL_TOP,&lx,&ly);
        under=id==31?(zell_rect){(int)lx,(int)ly,96,22}:(zell_rect){(int)sx-90,(int)sy+(id==29||id==30?-165:4),180,32};
        int ink_beside=ink_count(&canvas,under,beside_ink,within)-ink_away;
        if(getenv("ZELL_TEST_SHOW"))fprintf(stderr,"%-14s room %d: %d px of name ink beside (%d there before), %d standing\n",zell_stations[id].name,who[i].room,ink_beside,ink_away,ink_count(&canvas,under,standing,0));
        if(ink_beside<need)fprintf(stderr,"%s shows no name with Kilix beside\n",zell_stations[id].name);
        assert(ink_beside>=need);
        /* Nothing in the old standing colour comes with it. */
        assert(ink_count(&canvas,under,standing,0)<=standing_away);
        if(who[i].room>=1&&id!=31){
            int hint_beside=ink_count(&canvas,(zell_rect){(int)sx-130,(int)sy+(id==29||id==30?-141:37),260,22},hint_ink,within)-hint_away;
            if(hint_beside<need*2)fprintf(stderr,"%s shows no Enter line with Kilix beside\n",zell_stations[id].name);
            assert(hint_beside>=need*2);
        }
    }
    sr_canvas_free(&canvas);
}

/* How many of a cutout's solid pixels are on the canvas as the room would
 * draw them with the cutout's feet at this world point. Nearly solid pixels
 * count, and may differ by the little that shows through them. */
static int cutout_matches(const zell_scene *s,const sr_canvas *canvas,int art,float wx,float wy,int *solid){
    const zell_art *a=zell_art_get(art);
    float sx,sy;
    int n=0,all=0;
    assert(a);
    zell_world_to_screen(s,wx,wy,&sx,&sy);
    int left=(int)sx-a->width/2,top=(int)sy+14-a->height;
    for(int y=0;y<a->height;y++)for(int x=0;x<a->width;x++){
        uint32_t p=a->pixels[(size_t)y*(size_t)a->width+(size_t)x];
        int cx=left+x,cy=top+y;
        if((p>>24)<0xf0u)continue;
        all++;
        if(cx<0||cy<0||cx>=canvas->w||cy>=canvas->h)continue;
        if(pixel_gap(canvas->px[(size_t)cy*(size_t)canvas->w+(size_t)cx]&0xffffffu,p&0xffffffu)<=16)n++;
    }
    *solid=all;
    return n;
}
/* The Command Center: no stairs, a way out along the whole bottom of the
 * floor, the Key Master in the bottom right corner and a shell computer in
 * the bottom left. */
static void test_command_center(void){
    static const float across[]={.14f,.20f,.30f,.44f,.50f,.56f,.70f,.80f,.86f};
    zell_scene s;
    zell_controls south={.move_y=1},east={.move_x=1},enter={.confirm=true};
    const zell_station *key=&zell_stations[31],*computer=&zell_stations[ZELL_COMMAND_COMPUTER];
    float hatch_x=zell_stations[13].x*(float)ZELL_WORLD_W,hatch_y=zell_stations[13].y*(float)ZELL_WORLD_H;
    /* Leaving: straight down from anywhere across the floor ends in the
     * cave, below the hatch that leads here. */
    for(size_t i=0;i<sizeof across/sizeof across[0];i++){
        int ticks=0;
        place_room(&s,1,(float)ZELL_WORLD_W*across[i],(float)ZELL_WORLD_H*.86f);
        float x=s.x;
        while(s.room==1&&ticks<200){
            assert(s.x==x&&!zell_desktop_on_stairs(&s));
            zell_scene_tick(&s,&south);ticks++;
        }
        if(s.room!=0)fprintf(stderr,"no way out of the Command Center at %.2f: stopped at %.1f\n",(double)across[i],(double)s.y);
        assert(s.room==0&&s.mode==ZELL_MODE_WALK&&!s.leave);
        assert(fabsf(s.x-hatch_x)<1&&fabsf(s.y-hatch_y-50)<1);
    }
    /* He is not out until the floor's last line, and only this room is open. */
    place_room(&s,1,(float)ZELL_WORLD_W*.30f,(float)ZELL_WORLD_H*.975f);
    assert(!zell_desktop_on_stairs(&s));
    s.y=(float)ZELL_WORLD_H*.976f;
    assert(zell_desktop_on_stairs(&s));
    for(int room=3;room<=4;room++){
        place_room(&s,room,(float)ZELL_WORLD_W*.30f,(float)ZELL_WORLD_H*.976f);
        assert(!zell_desktop_on_stairs(&s));
    }
    place_room(&s,2,(float)ZELL_WORLD_W*.30f,(float)ZELL_WORLD_H*.86f);
    for(int tick=0;tick<200&&s.room==2;tick++)zell_scene_tick(&s,&south);
    assert(s.room==0&&!s.leave);
    /* All staff stand below the floor panels and keep their normal interactions. */
    for(int i=29;i<=31;i++){
        assert(zell_stations[i].y==key->y&&key->y>.95f&&key->y<.975f);
        place_room(&s,1,zell_stations[i].x*ZELL_WORLD_W-65.f,key->y*ZELL_WORLD_H);
        assert(zell_desktop_near(&s)==i);
        zell_scene_tick(&s,&enter);
        assert(s.station==i&&s.desktop_launch);
    }
    /* From the south floor Kilix can walk right up to the Key Master. */
    place_room(&s,1,(float)ZELL_WORLD_W*.70f,key->y*ZELL_WORLD_H);
    for(int tick=0;tick<200;tick++)zell_scene_tick(&s,&east);
    assert(s.room==1&&s.x<key->x*ZELL_WORLD_W&&s.x>key->x*ZELL_WORLD_W-30.f);
    assert(zell_desktop_near(&s)==31);
    zell_scene_tick(&s,&enter);
    assert(s.station==31&&s.mode==ZELL_MODE_DESK&&s.desktop_launch);
    assert(zell_desktop_actions(ZELL_COMMAND_COMPUTER)==1);
    for(int room=0;room<9;room++){
        place_room(&s,room,0,0);
        assert(zell_desktop_station_visible(&s,ZELL_COMMAND_COMPUTER)==(room==1));
    }
    zell_rect tiles[ZELL_COMMAND_TERMS],body,mana,panel;
    for(int i=0;i<ZELL_COMMAND_TERMS;i++){
        zell_command_term_layout(i,&tiles[i],&body);
        assert(body.x>tiles[i].x&&body.y>tiles[i].y&&body.w>tiles[i].w-20);
        assert(body.y+body.h<=tiles[i].y+tiles[i].h);
    }
    zell_mana_console_layout(&mana,&body);
    zell_rect wall=zell_command_wall_bounds();
    assert(wall.x==0&&wall.w==ZELL_WORLD_W);
    assert(tiles[1].x<=24&&ZELL_WORLD_W-(tiles[2].x+tiles[2].w)==tiles[1].x);
    assert(wall.y==ZELL_ROOM_HUD_BOTTOM&&tiles[1].y>wall.y&&tiles[1].y-wall.y<35);
    assert(tiles[0].y==tiles[1].y&&tiles[2].y==tiles[1].y);
    assert(wall.x<tiles[1].x&&tiles[2].x+tiles[2].w<wall.x+wall.w);
    assert(tiles[4].y+tiles[4].h<wall.y+wall.h);
    assert(tiles[1].x+tiles[1].w==tiles[0].x&&tiles[0].x+tiles[0].w==tiles[2].x);
    assert(tiles[3].y==tiles[1].y+tiles[1].h&&mana.y==tiles[3].y&&tiles[4].y==mana.y);
    assert(tiles[3].x+tiles[3].w==mana.x&&mana.x+mana.w==tiles[4].x);
    assert(mana.x==tiles[5].x&&mana.w==tiles[5].w&&mana.y+mana.h==tiles[5].y);
    assert(mana.x+mana.w/2==ZELL_WORLD_W/2&&tiles[5].y+tiles[5].h==tiles[4].y+tiles[4].h);
    int last=0;const int order[]={0,2,1,3};
    for(int i=0;i<ZELL_FLOOR_PANELS;i++){
        zell_floor_panel_layout(order[i],&panel,NULL);
        assert(panel.x>last&&panel.y>tiles[5].y+tiles[5].h&&panel.y+panel.h<(int)(key->y*ZELL_WORLD_H)-128);
        assert(panel.x>=wall.x&&panel.x+panel.w<=wall.x+wall.w);
        last=panel.x+panel.w;
    }
    /* Sliding down either side wall still leaves the room. A step the wall
     * blocks is a way out too, and Kilix never walks off the world. */
    {
        zell_controls slide[2]={{.move_x=1,.move_y=1},{.move_x=-1,.move_y=1}};
        for(int k=0;k<2;k++){
            int ticks=0;
            place_room(&s,1,(float)ZELL_WORLD_W*(k?.15f:.85f),(float)ZELL_WORLD_H*.90f);
            while(s.room==1&&ticks<300){
                assert(s.y<=(float)ZELL_WORLD_H);
                zell_scene_tick(&s,&slide[k]);ticks++;
            }
            if(s.room!=0)fprintf(stderr,"sliding down the %s wall does not leave: stopped at %.1f, %.1f\n",k?"west":"east",(double)s.x,(double)s.y);
            assert(s.room==0&&s.mode==ZELL_MODE_WALK&&!s.leave);
            assert(fabsf(s.x-hatch_x)<1&&fabsf(s.y-hatch_y-50)<1);
        }
        /* The Laundry exit works while sliding against its side wall too. */
        place_room(&s,2,(float)ZELL_WORLD_W*.85f,(float)ZELL_WORLD_H*.86f);
        for(int tick=0;tick<300&&s.room==2;tick++)zell_scene_tick(&s,&slide[0]);
        assert(s.room==0&&!s.leave&&s.x==zell_stations[ZELL_LAUNDRY_HATCH].x*ZELL_WORLD_W);
    }
    /* The way out is the whole bottom edge. Nothing is drawn at one point of
     * it, so no point of it is selected or answers Enter. */
    for(int k=0;k<=4;k++){
        place_room(&s,1,(float)ZELL_WORLD_W*(.42f+.02f*(float)k),(float)ZELL_WORLD_H*.95f);
        assert(zell_desktop_near(&s)==-1);
        zell_scene_tick(&s,&enter);
        assert(s.room==1&&s.mode==ZELL_MODE_WALK&&!s.desktop_launch);
    }
    /* Enter at the former Laundry staircase leaves Kilix on the floor. */
    place_room(&s,2,zell_stations[28].x*(float)ZELL_WORLD_W,zell_stations[28].y*(float)ZELL_WORLD_H-40.f);
    assert(zell_desktop_near(&s)!=28);
    zell_scene_tick(&s,&enter);assert(s.room==2);
    /* A pane of any shape shows both corners: the room is fitted across its
     * whole width, not only across the floor Kilix walks. */
    {
        static const int panes[][2]={{960,1080},{820,1000},{1121,960},{1280,1024},{640,400},{1366,768},{1536,1024},{1920,1080}};
        zell_rect frame,display;
        zell_command_computer_layout(&frame,&display);
        for(size_t k=0;k<sizeof panes/sizeof panes[0];k++){
            float left,top,right,bottom,kx,ky;
            zell_scene_init(&s,panes[k][0],panes[k][1]);zell_desktop_init(&s);
            s.room=1;s.mode=ZELL_MODE_WALK;
            zell_room_view_identity(&s);
            assert(s.room_view.logical_x==0&&s.room_view.logical_w==ZELL_WORLD_W);
            zell_world_to_screen(&s,(float)frame.x,(float)frame.y,&left,&top);
            zell_world_to_screen(&s,(float)(frame.x+frame.w),(float)(frame.y+frame.h),&right,&bottom);
            /* The Key Master's figure is about 90 world pixels wide at his feet. */
            zell_world_to_screen(&s,key->x*(float)ZELL_WORLD_W+45.f,key->y*(float)ZELL_WORLD_H,&kx,&ky);
            if(left<0.f||kx>(float)panes[k][0])fprintf(stderr,"%dx%d cuts a corner: computer from %.0f, Key Master to %.0f\n",panes[k][0],panes[k][1],(double)left,(double)kx);
            assert(left>=0.f&&top>=0.f&&right<=(float)panes[k][0]&&bottom<=(float)panes[k][1]);
            assert(kx<=(float)panes[k][0]&&ky<=(float)panes[k][1]);
        }
        /* The older rooms keep the fit across the floor. */
        s.room=2;zell_room_view_identity(&s);
        assert(s.room_view.logical_x==(int)(0.135f*(float)ZELL_WORLD_W));
    }
    /* The Key Master's two lines: in the strip under the AGENTS panel, clear
     * of it, of the screens, of the Key Master and of Kilix wherever Kilix
     * stands beside him, and above the room's bottom edge. */
    {
        /* Both renderers advance 8 pixels per ASCII letter; a label box adds 6 a side. */
        int name_w=(int)strlen(key->name)*8+12,hint_w=(int)strlen(ZELL_KEY_MASTER_HINT)*8+12;
        zell_rect name={ZELL_KEY_MASTER_LABEL_RIGHT-name_w,ZELL_KEY_MASTER_LABEL_TOP,name_w,22};
        zell_rect hint={ZELL_KEY_MASTER_LABEL_RIGHT-hint_w,ZELL_KEY_MASTER_LABEL_TOP+25,hint_w,22};
        zell_rect agents,frame,display,keeper;
        zell_command_switch_layout(1,&agents);
        assert(!rects_meet(name,agents)&&!rects_meet(hint,agents));
        assert(name.y>=agents.y+agents.h&&hint.y+hint.h<(int)((float)ZELL_WORLD_H*.93f));
        for(int m=0;m<5;m++){zell_monitor_layout(m,&frame,&display);assert(!rects_meet(name,frame)&&!rects_meet(hint,frame));}
        /* His figure is 128 high and about 70 wide. */
        keeper=(zell_rect){(int)(key->x*(float)ZELL_WORLD_W)-40,(int)(key->y*(float)ZELL_WORLD_H)-128,80,128};
        assert(!rects_meet(name,keeper)&&!rects_meet(hint,keeper));
        assert(keeper.x+keeper.w<ZELL_WORLD_W-ZELL_COMMAND_OPENING_DEPTH&&keeper.y+keeper.h>ZELL_COMMAND_OPENING_BOTTOM);
        /* Every place Kilix can stand and have the Key Master beside him. */
        int places=0;
        for(int wy=820;wy<(int)((float)ZELL_WORLD_H*.975f);wy+=2)for(int wx=1180;wx<=ZELL_WORLD_W-ZELL_COMMAND_SIDE_WALL;wx+=2){
            place_room(&s,1,(float)wx,(float)wy);
            if(zell_desktop_blocked(&s,(float)wx,(float)wy)||zell_desktop_near(&s)!=31)continue;
            zell_rect kilix={wx-36,wy-128,72,128};
            if(rects_meet(name,kilix)||rects_meet(hint,kilix))fprintf(stderr,"Kilix at %d,%d stands under the Key Master's lines\n",wx,wy);
            assert(!rects_meet(name,kilix)&&!rects_meet(hint,kilix));
            places++;
        }
        assert(places>200);
        /* And they end right beside him: no further left than the nearest
         * Kilix can stand, and short of it. */
        {
            int leftmost=ZELL_WORLD_W;
            for(int wy=820;wy<(int)((float)ZELL_WORLD_H*.975f);wy+=2)for(int wx=1180;wx<=ZELL_WORLD_W-ZELL_COMMAND_SIDE_WALL;wx+=2){
                place_room(&s,1,(float)wx,(float)wy);
                if(!zell_desktop_blocked(&s,(float)wx,(float)wy)&&zell_desktop_near(&s)==31&&wx-36<leftmost)leftmost=wx-36;
            }
            assert(hint.x+hint.w<=leftmost&&hint.x+hint.w>=leftmost-8);
            assert(name.x+name.w==hint.x+hint.w);
        }
        /* What is drawn: both lines there with Kilix beside him, in the
         * label inks, and nothing of them above his head any more. */
        {
            sr_canvas canvas;
            float sx,sy;
            /* Tolerate antialiasing while excluding blue reflections on the slate. */
            const int ink_tolerance=24;
            assert(sr_canvas_init(&canvas,1600,1100));
            for(int beside=0;beside<2;beside++){
                zell_scene_init(&s,1600,1100);zell_desktop_init(&s);
                s.room=1;s.mode=ZELL_MODE_WALK;s.mouse_valid=false;
                s.x=beside?(float)ZELL_WORLD_W*.865f:(float)ZELL_WORLD_W*.50f;s.y=beside?(float)ZELL_WORLD_H*.95f:(float)ZELL_WORLD_H*.60f;
                assert((zell_desktop_near(&s)==31)==(bool)beside);
                zell_draw(&s,&canvas);
                assert(s.room_view.scale==1.f);
                zell_world_to_screen(&s,(float)name.x,(float)name.y,&sx,&sy);
                int name_ink=ink_count(&canvas,(zell_rect){(int)sx,(int)sy,name.w,name.h},0xb9f8dbu,ink_tolerance);
                zell_world_to_screen(&s,(float)hint.x,(float)hint.y,&sx,&sy);
                int hint_ink=ink_count(&canvas,(zell_rect){(int)sx,(int)sy,hint.w,hint.h},0xa8f3d5u,ink_tolerance);
                zell_world_to_screen(&s,key->x*(float)ZELL_WORLD_W-130.f,key->y*(float)ZELL_WORLD_H-240.f,&sx,&sy);
                int overhead=ink_count(&canvas,(zell_rect){(int)sx,(int)sy,260,110},0xa8f3d5u,0)+ink_count(&canvas,(zell_rect){(int)sx,(int)sy,260,110},0xb9f8dbu,0);
                if(beside)assert(name_ink>=40&&hint_ink>=120);
                else assert(name_ink==0&&hint_ink==0);
                /* The name is the short top row, the hint the long one under
                 * it: left of the name's box only the hint's row has ink. */
                {
                    float rx,ry;
                    zell_world_to_screen(&s,(float)hint.x,(float)name.y,&rx,&ry);
                    zell_rect name_row_left={(int)rx,(int)ry,name.x-hint.x,22},hint_row_left={(int)rx,(int)ry+25,name.x-hint.x,22};
                    int top_left=ink_count(&canvas,name_row_left,0xb9f8dbu,ink_tolerance)+ink_count(&canvas,name_row_left,0xa8f3d5u,ink_tolerance);
                    int under_left=ink_count(&canvas,hint_row_left,0xa8f3d5u,ink_tolerance);
                    assert(top_left==0);
                    if(beside)assert(under_left>=80);
                }
                /* No label ink anywhere else along the strip, from the middle
                 * of the room to where Kilix stands. */
                {
                    float rx,ry,nx,ny,hx,hy;
                    int strip_left=hint.x-20,strip_right=ZELL_KEY_MASTER_LABEL_RIGHT+34;
                    zell_world_to_screen(&s,(float)strip_left,(float)(ZELL_KEY_MASTER_LABEL_TOP-2),&rx,&ry);
                    zell_world_to_screen(&s,(float)name.x,(float)name.y,&nx,&ny);
                    zell_world_to_screen(&s,(float)hint.x,(float)hint.y,&hx,&hy);
                    zell_rect strip={(int)rx,(int)ry,strip_right-strip_left,51};
                    zell_rect name_at={(int)nx,(int)ny,name.w,name.h},hint_at={(int)hx,(int)hy,hint.w,hint.h};
                    int all=ink_count(&canvas,strip,0xb9f8dbu,ink_tolerance)+ink_count(&canvas,strip,0xa8f3d5u,ink_tolerance);
                    int boxed=ink_count(&canvas,name_at,0xb9f8dbu,ink_tolerance)+ink_count(&canvas,name_at,0xa8f3d5u,ink_tolerance)
                             +ink_count(&canvas,hint_at,0xb9f8dbu,ink_tolerance)+ink_count(&canvas,hint_at,0xa8f3d5u,ink_tolerance);
                    if(all!=boxed)fprintf(stderr,"label ink outside the Key Master's boxes: %d of %d\n",all-boxed,all);
                    assert(all==boxed);
                }
                assert(overhead==0);
            }
            sr_canvas_free(&canvas);
        }
    }
    /* From the middle of the room neither corner is beside him. */
    place_room(&s,1,(float)ZELL_WORLD_W*.50f,(float)ZELL_WORLD_H*.86f);
    assert(zell_desktop_near(&s)!=31&&zell_desktop_near(&s)!=ZELL_COMMAND_COMPUTER);
    /* What is drawn. */
    {
        sr_canvas canvas;
        int solid=0,drawn;
        float sx,sy;
        zell_rect frame,display;
        assert(sr_canvas_init(&canvas,1600,1100));
        /* Both open exits show the floor in place of the stair sprite. */
        zell_scene_init(&s,1600,1100);zell_desktop_init(&s);
        s.room=2;s.mode=ZELL_MODE_WALK;s.mouse_valid=false;s.x=(float)ZELL_WORLD_W*.30f;s.y=(float)ZELL_WORLD_H*.50f;
        zell_draw(&s,&canvas);
        assert(s.room_view.scale==1.f);
        drawn=cutout_matches(&s,&canvas,ZELL_ART_LAUNDRY_STAIRS,(float)(int)(zell_stations[28].x*ZELL_WORLD_W),(float)(int)(zell_stations[28].y*ZELL_WORLD_H),&solid);
        assert(solid>2000&&drawn<solid/4);
        /* The Command Center paints none. */
        s.room=1;
        zell_draw(&s,&canvas);
        drawn=cutout_matches(&s,&canvas,ZELL_ART_COMMAND_STAIRS,(float)(int)(zell_stations[28].x*ZELL_WORLD_W),(float)(int)(zell_stations[28].y*ZELL_WORLD_H),&solid);
        if(drawn>=solid/4)fprintf(stderr,"the Command Center still paints stairs: %d of %d pixels\n",drawn,solid);
        assert(solid>2000&&drawn<solid/4);
        /* The controller's turquoise crest and bronze rail frame the bank,
         * beyond the live glass. Both survive the fitted room transform. */
        zell_world_to_screen(&s,(float)(wall.x+wall.w/2-14),(float)wall.y,&sx,&sy);
        assert(ink_count(&canvas,(zell_rect){(int)sx,(int)sy,28,29},0x24c6b6u,38)>12);
        zell_world_to_screen(&s,(float)(wall.x+wall.w/2-220),(float)(wall.y+6),&sx,&sy);
        assert(ink_count(&canvas,(zell_rect){(int)sx,(int)sy,100,16},0xc18d2eu,38)>35);
        zell_command_computer_layout(&frame,&display);
        zell_world_to_screen(&s,(float)display.x,(float)display.y,&sx,&sy);
        zell_rect glass={(int)sx,(int)sy,display.w,display.h};
        assert(ink_count(&canvas,glass,0x090d12u,0)>display.w*display.h/2);
        /* The shell's labels follow the terminal, not where Kilix stands
         * when it is brought back: the same bytes at another station. Above
         * and below the picture they are not a private terminal's words. */
        {
            sr_canvas other;
            zell_terminal_layout l;
            uint32_t *pixels=calloc(1200u*640u,4);
            assert(pixels&&sr_canvas_init(&other,1600,1100));
            s.mode=ZELL_MODE_SERVICE;s.service_phase=5;
            s.terminal_pixels=pixels;s.terminal_width=1200;s.terminal_height=640;
            s.terminal_private=true;
            snprintf(s.terminal_title,sizeof s.terminal_title,"%s",computer->name);
            s.station=ZELL_COMMAND_COMPUTER;zell_draw(&s,&canvas);
            s.station=44;zell_draw(&s,&other);
            assert(!memcmp(canvas.px,other.px,(size_t)canvas.w*(size_t)canvas.h*4u));
            /* The same terminal with every other title is the private one. */
            zell_terminal_layout_for(&s,canvas.w,canvas.h,&l);
            zell_rect title_row={l.image.x,l.image.y-26,l.image.w-104,24},footer_row={l.image.x,l.image.y+l.image.h+4,l.image.w,24};
            snprintf(s.terminal_title,sizeof s.terminal_title,"%s",computer->name);
            s.terminal_title[0]='c';
            zell_draw(&s,&other);
            assert(region_diffs(&canvas,&other,title_row)>40&&region_diffs(&canvas,&other,footer_row)>40);
            /* Only the first letter of the title changed above; below, the
             * whole line did. A private terminal's footer is what is there. */
            s.station=ZELL_COMMAND_COMPUTER;zell_draw(&s,&canvas);
            assert(!memcmp(canvas.px,other.px,(size_t)canvas.w*(size_t)canvas.h*4u));
            s.terminal_pixels=NULL;s.terminal_width=s.terminal_height=0;s.terminal_title[0]=0;
            s.mode=ZELL_MODE_WALK;s.service_phase=0;s.terminal_private=false;
            free(pixels);sr_canvas_free(&other);
        }
        /* Nothing marks the bottom edge as a way out: the bottom quarter of
         * the floor, beside and under the floor panels, is the painting
         * alone, with no arrow's gold or dark plate, though walking off it
         * still leaves (test_obstacles). */
        s.x=(float)ZELL_WORLD_W*.50f;s.y=(float)ZELL_WORLD_H*.50f;
        zell_draw(&s,&canvas);
        {
            const zell_art *room=zell_art_get(ZELL_ART_COMMAND_ROOM);
            int checked=0,differ=0;
            assert(room&&room->width==ZELL_WORLD_W&&room->height==ZELL_WORLD_H);
            for(int py=0;py<canvas.h-ZELL_ROOM_STATUS_STRIP;py++)for(int px=0;px<canvas.w;px++){
                float wx,wy;
                zell_screen_to_world(&s,(float)px,(float)py,&wx,&wy);
                int ix=(int)floorf(wx),iy=(int)floorf(wy);
                if(iy<(int)((float)ZELL_WORLD_H*.75f)||iy>=ZELL_WORLD_H||ix<(int)((float)ZELL_WORLD_W*.18f)||ix>=(int)((float)ZELL_WORLD_W*.82f))continue;
                {
                    bool on_panel=false;
                    for(int panel=0;panel<ZELL_FLOOR_PANELS;panel++){
                        zell_rect plate;zell_floor_panel_layout(panel,&plate,NULL);
                        if(ix>=plate.x-8&&ix<plate.x+plate.w+8&&iy>=plate.y-8&&iy<plate.y+plate.h+8)on_panel=true;
                    }
                    for(int id=29;id<=31;id++){
                        int nx=(int)(zell_stations[id].x*ZELL_WORLD_W),ny=(int)(zell_stations[id].y*ZELL_WORLD_H);
                        if(ix>=nx-70&&ix<=nx+70&&iy>=ny-132&&iy<=ny+9)on_panel=true;
                    }
                    if(on_panel)continue;
                }
                checked++;
                if((canvas.px[(size_t)py*(size_t)canvas.w+(size_t)px]&0xffffffu)!=(room->pixels[(size_t)iy*ZELL_WORLD_W+(size_t)ix]&0xffffffu))differ++;
            }
            if(differ)fprintf(stderr,"%d of %d bottom-edge pixels are not the painting\n",differ,checked);
            assert(checked>2000&&differ==0);
        }
        sr_canvas_free(&canvas);
    }
}
/* Both pane rooms open straight off the Command Center landing. Trace each
 * trip from the cave hatch, including the closed wall above the doorway. */
static void test_command_openings(void){
    zell_scene s;
    zell_rect wall=zell_command_wall_bounds();
    float edge=(float)(wall.y+wall.h);
    const zell_station *hatch=&zell_stations[ZELL_COMMAND_HATCH];
    const zell_controls enter={.confirm=true},north={.move_y=-1},south={.move_y=1};
    for(int side=-1;side<=1;side+=2){
        int room=side<0?ZELL_ROOM_PANES_LEFT:ZELL_ROOM_PANES_RIGHT;
        for(int route=0;route<3;route++){
            zell_controls out={.move_x=side,.run=route==1},back={.move_x=-side,.run=route==1};
            place_room(&s,0,hatch->x*ZELL_WORLD_W,hatch->y*ZELL_WORLD_H);
            zell_scene_tick(&s,&enter);
            assert(s.room==1&&!s.command_focus&&s.x==ZELL_WORLD_W*.5f);
            assert(s.y>=ZELL_COMMAND_OPENING_TOP&&s.y<ZELL_COMMAND_OPENING_BOTTOM);
            if(route==2){
                /* The narrow center seam stops him. Go around the outer end
                 * of the panel row and follow the wall to the same tunnel. */
                zell_rect panel;zell_floor_panel_layout(2,&panel,NULL);
                for(int i=0;i<100;i++)zell_scene_tick(&s,&north);
                float front=(float)(panel.y+panel.h+ZELL_FOOT_RADIUS_Y);
                assert(s.y>=front&&s.y<front+ZELL_WALK_SPEED);
                for(int i=0;i<250&&fminf(s.x,ZELL_WORLD_W-s.x)>ZELL_COMMAND_RETURN_X;i++)zell_scene_tick(&s,&out);
                for(int i=0;i<150;i++)zell_scene_tick(&s,&north);
                assert(s.y>=edge&&s.y<edge+ZELL_WALK_SPEED);
                for(int i=0;i<500;i++)zell_scene_tick(&s,&out);
                float distance=fminf(s.x,ZELL_WORLD_W-s.x);
                assert(s.room==1&&distance>=ZELL_COMMAND_SIDE_WALL&&distance<ZELL_COMMAND_SIDE_WALL+ZELL_WALK_SPEED+.1f);
                /* Down along the side wall and into the painted opening. */
                for(int i=0;i<250&&s.room==1;i++)zell_scene_tick(&s,&south);
            }else{
                float landing=s.y;
                for(int i=0;i<500&&s.room==1;i++){
                    zell_scene_tick(&s,&out);
                    if(s.room==1)assert(s.y==landing);
                }
            }
            assert(s.room==room&&!s.leave&&s.mode==ZELL_MODE_WALK);
            for(int i=0;i<120&&s.room==room;i++)zell_scene_tick(&s,&back);
            assert(s.room==1&&!zell_desktop_blocked(&s,s.x,s.y));
            assert(fminf(s.x,ZELL_WORLD_W-s.x)==ZELL_COMMAND_RETURN_X&&ZELL_COMMAND_RETURN_X>ZELL_COMMAND_OPENING_DEPTH);
            assert((s.x-ZELL_WORLD_W*.5f)*(float)side>0.f);
            float arrival=s.y;
            assert(arrival>ZELL_COMMAND_OPENING_TOP&&arrival<ZELL_COMMAND_OPENING_BOTTOM);
            for(int i=0;i<500&&(s.x-ZELL_WORLD_W*.5f)*(float)side>0.f;i++)zell_scene_tick(&s,&back);
            assert(s.room==1&&s.y==arrival&&fabsf(s.x-ZELL_WORLD_W*.5f)<=ZELL_RUN_SPEED);
        }
        /* The former upper opening is part of the display wall. The
         * side below the new arch stays solid as the player walks past. */
        float outside=side<0?ZELL_COMMAND_OPENING_DEPTH:ZELL_WORLD_W-ZELL_COMMAND_OPENING_DEPTH;
        assert(zell_desktop_blocked(&s,outside,344.f));
        assert(!zell_desktop_blocked(&s,outside,ZELL_COMMAND_OPENING_TOP));
        place_room(&s,1,outside,ZELL_COMMAND_OPENING_BOTTOM+3.f);
        zell_controls out={.move_x=side,.run=true};
        for(int i=0;i<40;i++)zell_scene_tick(&s,&out);
        assert(s.room==1&&fminf(s.x,ZELL_WORLD_W-s.x)>=ZELL_COMMAND_SIDE_WALL);
    }
    assert(zell_desktop_block_count(&s)==1+ZELL_FLOOR_PANELS&&zell_desktop_north(1)==edge);
    place_room(&s,1,ZELL_WORLD_W*.4f,ZELL_WORLD_H*.95f);
    for(int i=0;i<40&&s.room==1;i++)zell_scene_tick(&s,&south);
    assert(s.room==0&&!s.leave);
}
/* Rooms of panes: two through the left opening, four through the right. */
static void pane_click(zell_scene *s,int x,int y){
    s->mouse_valid=true;s->mouse_click=true;s->mouse_x=x;s->mouse_y=y;
    assert(zell_desktop_click(s)&&!s->mouse_click);
}
static void test_pane_rooms(void){
    zell_scene s;
    zell_controls none={0},west={.move_x=-1},east={.move_x=1},north={.move_y=-1},south={.move_y=1};
    sr_canvas canvas;
    char want[512]="";
    assert(zell_panes_count(ZELL_ROOM_PANES_LEFT)==2&&zell_panes_count(ZELL_ROOM_PANES_RIGHT)==4);
    for(int room=0;room<9;room++)assert(zell_panes_count(room)==0&&zell_panes_exit_side(room)==0&&!ZELL_ROOM_IS_PANES(room));
    assert(zell_panes_exit_side(ZELL_ROOM_PANES_LEFT)==1&&zell_panes_exit_side(ZELL_ROOM_PANES_RIGHT)==-1);
    /* A pane room's picture is as big as the pane; elsewhere it keeps the cap. */
    {
        int w=0,h=0;
        zell_panes_frame_limits(ZELL_ROOM_PANES_LEFT,&w,&h);assert(w==3840&&h==2160);
        zell_panes_frame_limits(ZELL_ROOM_PANES_RIGHT,&w,&h);assert(w==3840&&h==2160);
        for(int room=0;room<9;room++){zell_panes_frame_limits(room,&w,&h);assert(w==1600&&h==1000);}
    }
    zell_panes_test_reset();
    assert(sr_canvas_init(&canvas,960,600));
    for(int room=ZELL_ROOM_PANES_LEFT;room<=ZELL_ROOM_PANES_RIGHT;room++){
        int side=zell_panes_exit_side(room);
        zell_controls toward=side>0?east:west,away=side>0?west:east;
        float x0=24.f,x1=936.f;
        place_room(&s,1,0,0);
        zell_panes_enter(&s,room);
        /* In on the walkway beside the line he came by, facing into the
         * room, with the keys. */
        assert(s.room==room&&s.mode==ZELL_MODE_WALK&&!zell_panes_keys_to_pane());
        assert(side>0?(s.x==x1-48.f&&s.direction==2):(s.x==x0+48.f&&s.direction==3));
        assert(s.y==58.f);
        /* Nothing in it is a station, the way back included. */
        for(int i=0;i<ZELL_STATIONS;i++)assert(!zell_desktop_station_visible(&s,i));
        assert(zell_desktop_near(&s)==-1);
        /* Tab, Enter and Esc do not move him or open anything. */
        {
            zell_controls keys={.launcher=true,.confirm=true,.cancel=true,.run=true};
            float x=s.x,y=s.y;
            zell_scene_tick(&s,&keys);
            assert(s.room==room&&s.mode==ZELL_MODE_WALK&&s.x==x&&s.y==y&&!s.running&&!s.desktop_launch);
        }
        /* Standing still at the line is not leaving. */
        {
            zell_scene at=s;at.x=side>0?x1:x0;
            for(int tick=0;tick<10;tick++)zell_scene_tick(&at,&none);
            assert(at.room==room);
        }
        /* Away from the line he walks to the walkway's far end and stays;
         * up and down only turn him. */
        {
            zell_scene far=s;
            for(int tick=0;tick<400;tick++)zell_scene_tick(&far,&away);
            assert(far.room==room&&far.x==(side>0?x0:x1)&&far.y==58.f);
            float x=far.x;
            for(int tick=0;tick<40;tick++)zell_scene_tick(&far,&south);
            assert(far.room==room&&far.x==x&&far.y==58.f&&far.direction==0);
            for(int tick=0;tick<40;tick++)zell_scene_tick(&far,&north);
            assert(far.room==room&&far.x==x&&far.y==58.f&&far.direction==1);
            /* All the way back along it and into the line, he leaves. */
            int ticks=0;
            while(far.room==room&&ticks<2000){zell_scene_tick(&far,&toward);ticks++;}
            assert(far.room==1&&ticks>=200&&ticks<2000);
        }
        /* While a pane has the keys he does not walk. */
        {
            zell_scene typing=s;
            float x=typing.x;
            pane_click(&typing,480,300);
            assert(zell_panes_keys_to_pane());
            for(int tick=0;tick<20;tick++)zell_scene_tick(&typing,&away);
            assert(typing.x==x&&typing.room==room);
            pane_click(&typing,480,40);
            assert(!zell_panes_keys_to_pane());
            for(int tick=0;tick<20;tick++)zell_scene_tick(&typing,&away);
            assert(typing.x!=x);
            zell_panes_test_reset();
        }
        /* A smaller pane keeps him on its walkway. */
        {
            zell_scene small=s;
            small.x=900.f;
            zell_scene_resize(&small,400,300);
            zell_scene_tick(&small,&none);
            assert(small.room==room&&small.x==376.f&&small.y==58.f);
        }
        /* Drawn edge to edge: the line at his end of the walkway only, no HUD. */
        zell_draw(&s,&canvas);
        {
            zell_rect line,other={side>0?0:957,0,3,64};
            zell_panes_exit_rect(&s,&line);
            assert(line.w==3&&line.h==64&&line.y==0&&line.x==(side>0?957:0));
            assert(ink_count(&canvas,line,ZELL_PANES_EXIT_RGB,0)==3*64);
            assert(ink_count(&canvas,other,ZELL_PANES_EXIT_RGB,40)==0);
            assert(ink_count(&canvas,(zell_rect){0,64,960,536},ZELL_PANES_EXIT_RGB,40)==0);
            /* No status strip (the title bar is looked for below, with
             * Kilix out of its way). */
            assert(ink_count(&canvas,(zell_rect){0,600-ZELL_ROOM_STATUS_STRIP,600,ZELL_ROOM_STATUS_STRIP},0xc7d8dau,30)==0);
            /* The walkway's edge is lit: Kilix has the keys. */
            assert(ink_count(&canvas,(zell_rect){400,62,100,2},0xd9ba79u,0)==200);
            /* Kilix is drawn on the walkway where he stands, and only there. */
            {
                sr_canvas moved;zell_scene there=s;
                assert(sr_canvas_init(&moved,960,600));
                there.x=480.f;zell_draw(&there,&moved);
                /* No title bar. */
                assert(ink_count(&moved,(zell_rect){0,0,420,30},0xf0d69bu,30)==0);
                zell_rect body={(int)s.x-34,0,68,64},moved_body={446,0,68,64};
                assert(!rects_meet(body,moved_body));
                assert(region_diffs(&canvas,&moved,body)>400);
                /* Elsewhere only what he aims at changes: the walkway's
                 * buttons and plaques, and the title bars. */
                {
                    zell_rect keep[2+1+4*(ZELL_PANES_AGENTS+1)];int n=0;
                    keep[n++]=body;keep[n++]=moved_body;
                    keep[n++]=(zell_rect){0,ZELL_PANES_WALK_H,960,ZELL_PANES_CHROME_H};
                    for(int i=0;i<zell_panes_count(s.room);i++){
                        zell_panes_pad_rect(&s,i,&keep[n]);if(keep[n].w)n++;
                        for(int agent=0;agent<ZELL_PANES_AGENTS;agent++){zell_panes_button_rect(&s,i,agent,&keep[n]);if(keep[n].w)n++;}
                    }
                    assert(diffs_outside(&canvas,&moved,keep,n)==0);
                }
                sr_canvas_free(&moved);
            }
            /* Before the helper paints, the room says it is opening, under
             * the walkway and title bars. */
            assert(ink_count(&canvas,(zell_rect){330,325,300,40},0xb9f8dbu,40)>40);
            assert(ink_count(&canvas,(zell_rect){200,0,560,64},0xb9f8dbu,40)==0);
        }
        /* Back in the Command Center, in the opening, facing the room. */
        {
            int ticks=0;
            while(s.room==room&&ticks<400){zell_scene_tick(&s,&toward);ticks++;}
            assert(s.room==1&&ticks<30);
            assert(s.x==(side>0?ZELL_COMMAND_RETURN_X:ZELL_WORLD_W-ZELL_COMMAND_RETURN_X)&&s.direction==(side>0?3:2));
            assert(s.y>(float)ZELL_COMMAND_OPENING_TOP&&s.y<(float)ZELL_COMMAND_OPENING_BOTTOM);
            assert(!zell_desktop_blocked(&s,s.x,s.y));
            /* Into the room, then back the way he came. */
            zell_controls into=side>0?east:west,back=side>0?west:east;
            for(int tick=0;tick<4;tick++)zell_scene_tick(&s,&into);
            assert(s.room==1);
            ticks=0;
            while(s.room==1&&ticks<200){zell_scene_tick(&s,&back);ticks++;}
            assert(s.room==room);
        }
    }
    /* The room grows past the Command Center's size a frame after he walks
     * in: until he moves he is placed beside the line in the room as it is. */
    zell_panes_test_reset();
    {
        place_room(&s,1,0,0);
        zell_panes_enter(&s,ZELL_ROOM_PANES_LEFT);
        assert(s.x==888.f);
        zell_scene_resize(&s,2400,1300);
        zell_scene_tick(&s,&none);
        assert(s.x==2328.f&&s.y==58.f&&s.direction==2);
        zell_scene_tick(&s,&south);
        assert(s.x==2328.f&&s.direction==0);
        zell_scene_tick(&s,&west);
        float x=s.x;
        assert(x<2328.f);
        zell_scene_resize(&s,3000,1300);
        zell_scene_tick(&s,&none);
        assert(s.x==x);
    }
    /* Cells sit under the walkway and the title bars, 8x16. */
    {
        int cols,rows;
        zell_panes_cells(960,600,&cols,&rows);assert(cols==120&&rows==32);
        zell_panes_cells(1908,990,&cols,&rows);assert(cols==238&&rows==56);
        zell_panes_cells(100,50,&cols,&rows);assert(cols==20&&rows==8);
        zell_panes_cells(4000,4000,&cols,&rows);assert(cols==480&&rows==135);
    }
    /* The panes split the row as the helper does, one column of line
     * between them; a line belongs to the pane on its left. */
    zell_panes_test_reset();
    place_room(&s,1,0,0);zell_panes_enter(&s,ZELL_ROOM_PANES_LEFT);
    {
        zell_rect r;
        zell_panes_pane_rect(&s,0,&r);assert(r.x==0&&r.y==84&&r.w==480&&r.h==512);
        zell_panes_pane_rect(&s,1,&r);assert(r.x==488&&r.y==84&&r.w==472&&r.h==512);
        zell_panes_pane_rect(&s,2,&r);assert(r.w==0);
        zell_panes_pane_rect(&s,-1,&r);assert(r.w==0);
        zell_panes_chrome_rect(&s,1,&r);assert(r.x==488&&r.y==64&&r.w==472&&r.h==20);
        assert(zell_panes_pane_at(&s,0)==0&&zell_panes_pane_at(&s,479)==0&&zell_panes_pane_at(&s,487)==0);
        assert(zell_panes_pane_at(&s,488)==1&&zell_panes_pane_at(&s,959)==1&&zell_panes_pane_at(&s,-1)==-1);
    }
    zell_panes_enter(&s,ZELL_ROOM_PANES_RIGHT);
    {
        static const int x960[4]={0,248,488,728},w960[4]={240,232,232,232};
        static const int x1908[4]={0,480,960,1440},w1908[4]={472,472,472,464};
        zell_rect r;
        for(int i=0;i<4;i++){zell_panes_pane_rect(&s,i,&r);assert(r.x==x960[i]&&r.w==w960[i]&&r.y==84&&r.h==512);}
        assert(zell_panes_pane_at(&s,247)==0&&zell_panes_pane_at(&s,248)==1&&zell_panes_pane_at(&s,487)==1&&zell_panes_pane_at(&s,727)==2&&zell_panes_pane_at(&s,728)==3);
        zell_scene_resize(&s,1908,990);
        for(int i=0;i<4;i++){zell_panes_pane_rect(&s,i,&r);assert(r.x==x1908[i]&&r.w==w1908[i]&&r.y==84&&r.h==896);}
        assert(zell_panes_pane_at(&s,959)==1&&zell_panes_pane_at(&s,960)==2&&zell_panes_pane_at(&s,1907)==3);
        zell_scene_resize(&s,960,600);
    }
    /* Each title bar has Zoom and Restart at its right end, if it is wide
     * enough for them and a title. */
    {
        zell_rect z,r;
        zell_panes_chrome_button(&s,1,0,&z);zell_panes_chrome_button(&s,1,1,&r);
        assert(r.x==408&&r.y==65&&r.w==68&&r.h==18);
        assert(z.x==344&&z.y==65&&z.w==60&&z.h==18);
        zell_panes_chrome_button(&s,1,2,&r);assert(r.w==0);
        zell_scene small=s;zell_scene_resize(&small,640,400);
        zell_panes_chrome_button(&small,0,0,&z);zell_panes_chrome_button(&small,0,1,&r);
        assert(z.w==0&&r.w==0);
    }
    /* The helper names the agents it found, what each pane runs, and
     * notices; lines that are not right change nothing. */
    {
        assert(zell_panes_agent_count()==0);
        zell_panes_test_line("G codex Codex");
        zell_panes_test_line("G grok Grok");
        zell_panes_test_line("G codex Again");
        zell_panes_test_line("G Bad Name");
        zell_panes_test_line("G nolabel");
        zell_panes_test_line("G x \x01");
        zell_panes_test_line("G 0123456789abcdef Long");
        zell_panes_test_line("Gx y Z");
        assert(zell_panes_agent_count()==2&&!strcmp(zell_panes_agent_label(0),"Codex")&&!strcmp(zell_panes_agent_label(1),"Grok"));
        assert(!strcmp(zell_panes_agent_label(2),"")&&!strcmp(zell_panes_agent_label(-1),""));
        zell_panes_test_line("T 10 3 vim");
        for(const char *const *bad=(const char *const[]){"T 10 4 bad","T 8 0 bad","T 9 2 bad","T 9 x bad","T 10 -1 bad","T 9 1 ","T 9 1 a\tb",NULL};*bad;bad++)zell_panes_test_line(*bad);
        assert(!strcmp(zell_panes_title(10,3),"vim"));
        for(int i=0;i<3;i++)assert(!strcmp(zell_panes_title(10,i),""));
        assert(!strcmp(zell_panes_title(9,0),"")&&!strcmp(zell_panes_title(9,1),"")&&!strcmp(zell_panes_title(10,4),""));
        zell_panes_test_line("T 10 3 top");assert(!strcmp(zell_panes_title(10,3),"top"));
        zell_panes_test_line("T 10 3 ");assert(!strcmp(zell_panes_title(10,3),"top"));
        zell_panes_test_line("G y ");assert(zell_panes_agent_count()==2);
        assert(!zell_panes_notice(10,2));
        zell_panes_test_line("N 10 2 is busy.");
        assert(zell_panes_notice(10,2)&&!strcmp(zell_panes_notice(10,2),"is busy."));
        assert(!zell_panes_notice(10,1)&&!zell_panes_notice(9,2));
        /* A failure line is the room's error. */
        zell_panes_test_line("E The panes stopped.");
        assert(!strcmp(zell_panes_error(),"The panes stopped."));
    }
    /* Agent buttons sit left to right over each pane, as many as fit. */
    {
        zell_rect b;
        /* Clear of where he comes in by the line on the left, and past
         * the plaque in the pane's middle (96 to 144 on pane 0). */
        zell_panes_pad_rect(&s,0,&b);assert(b.x==96&&b.y==6&&b.w==48&&b.h==22);
        zell_panes_pad_rect(&s,1,&b);assert(b.x==340&&b.w==48);
        zell_panes_button_rect(&s,0,0,&b);assert(b.x==150&&b.y==6&&b.w==56&&b.h==22);
        zell_panes_button_rect(&s,0,1,&b);assert(b.w==0);
        assert(s.x<104.f);
        zell_panes_button_rect(&s,1,0,&b);assert(b.x==260&&b.w==56);
        zell_panes_button_rect(&s,1,1,&b);assert(b.x==394&&b.w==48);
        /* The last pane's second would reach the Hotkeys button (876). */
        zell_panes_button_rect(&s,3,1,&b);assert(b.w==0);
        zell_panes_button_rect(&s,0,2,&b);assert(b.w==0);
        zell_panes_button_rect(&s,4,0,&b);assert(b.w==0);
        zell_scene narrow=s;zell_scene_resize(&narrow,400,300);
        /* There the Hotkeys button (316) leaves the last pane none. */
        zell_panes_button_rect(&narrow,3,0,&b);assert(b.w==0);
        zell_panes_button_rect(&narrow,2,0,&b);assert(b.x==208+12&&b.w==56);
        zell_panes_button_rect(&narrow,3,1,&b);assert(b.w==0);
        /* No plaque on a pane too narrow for one. */
        zell_panes_pad_rect(&narrow,3,&b);assert(b.w==0);
    }
    /* Clicks: a pane's cells or title give it the keys; the walkway gives
     * them back to Kilix; Zoom and a second Restart are lines too. */
    assert(!strcmp(zell_panes_pending(),""));
    assert(zell_panes_route(true)==ZELL_PANES_KEY_PRESS&&zell_panes_route(false)==ZELL_PANES_KEY_NONE);
    pane_click(&s,498,300);
    strcat(want,"F 2\n");assert(!strcmp(zell_panes_pending(),want));
    assert(zell_panes_keys_to_pane()&&zell_panes_focus(ZELL_ROOM_PANES_RIGHT)==2&&zell_panes_focus(ZELL_ROOM_PANES_LEFT)==0);
    assert(zell_panes_route(true)==ZELL_PANES_KEY_PANE&&zell_panes_route(false)==ZELL_PANES_KEY_PANE);
    pane_click(&s,600,500);
    assert(!strcmp(zell_panes_pending(),want));
    pane_click(&s,484,200);
    strcat(want,"F 1\n");assert(!strcmp(zell_panes_pending(),want));
    pane_click(&s,200,50);
    assert(!zell_panes_keys_to_pane()&&!strcmp(zell_panes_pending(),want));
    pane_click(&s,30,70);
    strcat(want,"F 0\n");assert(!strcmp(zell_panes_pending(),want)&&zell_panes_keys_to_pane());
    pane_click(&s,959,10);
    assert(!zell_panes_keys_to_pane());
    /* Zoom pane 2 over the room, then back. */
    pane_click(&s,349,70);
    strcat(want,"Z 1\n");assert(!strcmp(zell_panes_pending(),want)&&zell_panes_focus(ZELL_ROOM_PANES_RIGHT)==1);
    assert(zell_panes_zoom(ZELL_ROOM_PANES_RIGHT)==1&&zell_panes_zoom(ZELL_ROOM_PANES_LEFT)==-1&&zell_panes_keys_to_pane());
    {
        zell_rect r;
        zell_panes_pane_rect(&s,0,&r);assert(r.w==0);
        zell_panes_pane_rect(&s,1,&r);assert(r.x==0&&r.y==84&&r.w==960&&r.h==512);
        assert(zell_panes_pane_at(&s,5)==1&&zell_panes_pane_at(&s,900)==1);
        zell_panes_button_rect(&s,1,0,&r);assert(r.x==104&&r.w==56);
        zell_panes_button_rect(&s,0,0,&r);assert(r.w==0);
        zell_panes_chrome_button(&s,1,0,&r);assert(r.x==824&&r.w==60);
        pane_click(&s,830,70);
    }
    strcat(want,"Z -1\n");assert(!strcmp(zell_panes_pending(),want)&&zell_panes_zoom(ZELL_ROOM_PANES_RIGHT)==-1);
    /* Restart wants a second click on the same pane; the first, on its
     * title bar, gives that pane the keys. */
    pane_click(&s,200,50);
    pane_click(&s,650,70);
    strcat(want,"F 2\n");assert(!strcmp(zell_panes_pending(),want)&&zell_panes_keys_to_pane());
    assert(zell_panes_confirming(ZELL_ROOM_PANES_RIGHT,2)&&!zell_panes_confirming(ZELL_ROOM_PANES_RIGHT,1));
    pane_click(&s,170,70);
    strcat(want,"F 0\n");assert(!strcmp(zell_panes_pending(),want));
    assert(zell_panes_confirming(ZELL_ROOM_PANES_RIGHT,0)&&!zell_panes_confirming(ZELL_ROOM_PANES_RIGHT,2));
    pane_click(&s,170,70);
    strcat(want,"X 0\n");assert(!strcmp(zell_panes_pending(),want)&&!zell_panes_confirming(ZELL_ROOM_PANES_RIGHT,0));
    /* An agent button starts that agent in the pane under it, which gets
     * the keys. */
    pane_click(&s,265,10);
    strcat(want,"A 1 codex\nF 1\n");assert(!strcmp(zell_panes_pending(),want)&&zell_panes_keys_to_pane());
    /* Kilix presses what he aims at, only while he has the keys. */
    s.x=178.f;
    assert(!zell_panes_press(&s));
    {int i,w;assert(zell_panes_aim(&s,&i,&w)==ZELL_PANES_AIM_NONE);}
    pane_click(&s,200,50);
    {int i,w;assert(zell_panes_aim(&s,&i,&w)==ZELL_PANES_AIM_AGENT&&i==0&&w==0);}
    assert(zell_panes_press(&s));
    strcat(want,"A 0 codex\nF 0\n");assert(!strcmp(zell_panes_pending(),want)&&zell_panes_keys_to_pane());
    pane_click(&s,200,50);
    /* Where buttons overlap, the one whose middle is nearest him; where
     * none is, the pane. Pane 0: plaque 96-144, Zoom 104-164, Codex
     * 150-206, Restart 168-236. Pane 1: plaque 340-388, Zoom 344-404,
     * Grok 394-442, Restart 408-476. */
    {
        int i,w;
        s.x=120.f;assert(zell_panes_aim(&s,&i,&w)==ZELL_PANES_AIM_PAD&&i==0);
        s.x=134.f;assert(zell_panes_aim(&s,&i,&w)==ZELL_PANES_AIM_CHROME&&i==0&&w==ZELL_PANES_ZOOM);
        s.x=225.f;assert(zell_panes_aim(&s,&i,&w)==ZELL_PANES_AIM_CHROME&&i==0&&w==ZELL_PANES_RESTART);
        s.x=60.f;assert(zell_panes_aim(&s,&i,&w)==ZELL_PANES_AIM_PANE&&i==0);
        s.x=244.f;assert(zell_panes_aim(&s,&i,&w)==ZELL_PANES_AIM_PANE&&i==0);
        s.x=300.f;assert(zell_panes_aim(&s,&i,&w)==ZELL_PANES_AIM_AGENT&&i==1&&w==0);
        s.x=364.f;assert(zell_panes_aim(&s,&i,&w)==ZELL_PANES_AIM_PAD&&i==1);
        s.x=376.f;assert(zell_panes_aim(&s,&i,&w)==ZELL_PANES_AIM_CHROME&&i==1&&w==ZELL_PANES_ZOOM);
        s.x=418.f;assert(zell_panes_aim(&s,&i,&w)==ZELL_PANES_AIM_AGENT&&i==1&&w==1);
        s.x=460.f;assert(zell_panes_aim(&s,&i,&w)==ZELL_PANES_AIM_CHROME&&i==1&&w==ZELL_PANES_RESTART);
    }
    /* Zoom, pressed: the pane fills the room and he keeps the keys; its
     * Zoom (now 824-884) puts it back. */
    s.x=376.f;assert(zell_panes_press(&s));
    strcat(want,"Z 1\n");assert(!strcmp(zell_panes_pending(),want)&&!zell_panes_keys_to_pane());
    assert(zell_panes_zoom(ZELL_ROOM_PANES_RIGHT)==1);
    s.x=854.f;
    {int i,w;assert(zell_panes_aim(&s,&i,&w)==ZELL_PANES_AIM_CHROME&&i==1&&w==ZELL_PANES_ZOOM);}
    assert(zell_panes_press(&s));
    strcat(want,"Z -1\n");assert(!strcmp(zell_panes_pending(),want)&&zell_panes_zoom(ZELL_ROOM_PANES_RIGHT)==-1);
    /* The helper gave the pane it zoomed the focus, as Zell now has it: pane
     * 0's plaque says so again, so keys typed then go to pane 0. */
    assert(zell_panes_focus(ZELL_ROOM_PANES_RIGHT)==1);
    s.x=120.f;assert(zell_panes_press(&s));
    zell_panes_key((const unsigned char *)"x",1);
    strcat(want,"F 0\nK78\n");assert(!strcmp(zell_panes_pending(),want)&&zell_panes_focus(ZELL_ROOM_PANES_RIGHT)==0);
    pane_click(&s,959,10);
    /* Restart, pressed twice. */
    s.x=460.f;assert(zell_panes_press(&s));
    assert(!strcmp(zell_panes_pending(),want)&&zell_panes_confirming(ZELL_ROOM_PANES_RIGHT,1));
    assert(zell_panes_press(&s));
    strcat(want,"X 1\n");assert(!strcmp(zell_panes_pending(),want));
    /* The plaque, and the pane away from its buttons, give it the keys. */
    s.x=364.f;assert(zell_panes_press(&s));
    strcat(want,"F 1\n");assert(!strcmp(zell_panes_pending(),want)&&zell_panes_keys_to_pane());
    pane_click(&s,200,50);
    s.x=60.f;assert(zell_panes_press(&s));
    strcat(want,"F 0\n");assert(!strcmp(zell_panes_pending(),want)&&zell_panes_keys_to_pane());
    /* A plaque clicked does the same. */
    pane_click(&s,200,50);
    pane_click(&s,364,10);
    strcat(want,"F 1\n");assert(!strcmp(zell_panes_pending(),want)&&zell_panes_keys_to_pane());
    pane_click(&s,200,50);
    pane_click(&s,120,10);
    strcat(want,"F 0\n");assert(!strcmp(zell_panes_pending(),want)&&zell_panes_keys_to_pane());
    /* Walking into the left room he stands by its line on the right, at
     * the right pane's Restart: nothing is aimed at there until he moves. */
    {
        zell_scene in=s;int i,w;
        zell_panes_enter(&in,ZELL_ROOM_PANES_LEFT);
        assert(in.x==888.f);
        assert(zell_panes_aim(&in,&i,&w)==ZELL_PANES_AIM_NONE&&!zell_panes_press(&in));
        in.x+=1.f;
        assert(zell_panes_aim(&in,&i,&w)==ZELL_PANES_AIM_CHROME&&i==1&&w==ZELL_PANES_RESTART);
        in.x-=1.f;
        assert(zell_panes_aim(&in,&i,&w)==ZELL_PANES_AIM_CHROME&&i==1&&w==ZELL_PANES_RESTART);
    }
    /* The room grows a frame after he walks in, and he moves with its edge:
     * still nothing is aimed at until he moves. */
    {
        zell_scene g;zell_controls still={0};int i,w;
        place_room(&g,1,0,0);
        zell_panes_enter(&g,ZELL_ROOM_PANES_LEFT);
        assert(g.x==888.f);
        zell_scene_resize(&g,1920,1080);zell_scene_tick(&g,&still);
        assert(g.x==1848.f);
        assert(zell_panes_aim(&g,&i,&w)==ZELL_PANES_AIM_NONE&&!zell_panes_press(&g));
        g.x+=1.f;
        assert(zell_panes_aim(&g,&i,&w)==ZELL_PANES_AIM_CHROME&&i==1&&w==ZELL_PANES_RESTART);
    }
    /* Walking in again, Kilix has the keys. */
    zell_panes_enter(&s,ZELL_ROOM_PANES_RIGHT);
    assert(!zell_panes_keys_to_pane());
    /* Showing a room tells the helper its focus and zoom too. */
    {
        char line[80];
        pane_click(&s,349,70);
        assert(zell_panes_room_line(ZELL_ROOM_PANES_RIGHT,120,32,line,sizeof line)==(int)strlen("S 10 120 32\nZ 1\nF 1\n"));
        assert(!strcmp(line,"S 10 120 32\nZ 1\nF 1\n"));
        assert(zell_panes_room_line(ZELL_ROOM_PANES_LEFT,50,9,line,sizeof line)>0&&!strcmp(line,"S 9 50 9\nZ -1\nF 0\n"));
        assert(zell_panes_room_line(0,120,32,line,sizeof line)==2&&!strcmp(line,"H\n"));
        pane_click(&s,830,70);
        strcat(want,"Z 1\nZ -1\n");assert(!strcmp(zell_panes_pending(),want));
        pane_click(&s,200,50);
    }
    /* A notice lasts four seconds, an armed Restart three. */
    {
        zell_notice_set_clock(20000);
        zell_panes_test_line("N 10 1 is busy.");
        zell_notice_set_clock(23999);assert(zell_panes_notice(10,1));
        zell_notice_set_clock(24000);assert(!zell_panes_notice(10,1));
        zell_notice_set_clock(30000);
        pane_click(&s,410,70);
        zell_notice_set_clock(32999);assert(zell_panes_confirming(10,1));
        zell_notice_set_clock(33000);assert(!zell_panes_confirming(10,1));
        pane_click(&s,410,70);
        assert(!strcmp(zell_panes_pending(),want)&&zell_panes_confirming(10,1));
        pane_click(&s,410,70);
        strcat(want,"X 1\n");assert(!strcmp(zell_panes_pending(),want));
        pane_click(&s,200,50);
    }
    /* Drawn: the button under him lit, the pane with the keys brightest,
     * an armed Restart red, and a notice in its pane's bar. */
    {
        zell_rect b,lit,bar0,bar1;
        s.x=268.f;
        zell_panes_button_rect(&s,1,0,&lit);zell_panes_button_rect(&s,1,1,&b);
        {int i,w;assert(zell_panes_aim(&s,&i,&w)==ZELL_PANES_AIM_AGENT&&i==1&&w==0);}
        zell_draw(&s,&canvas);
        /* Sampled at the button's right end, clear of Kilix in front of it. */
        assert(ink_count(&canvas,(zell_rect){lit.x+lit.w-10,lit.y+2,4,4},0x2f5a4cu,0)==16);
        assert(ink_count(&canvas,(zell_rect){b.x+2,b.y+2,4,4},0x1f3444u,0)==16);
        assert(ink_count(&canvas,lit,0xe8f2f6u,60)>10);
        /* The plaque he aims at is lit and its pane's title bar edged in
         * gold; aiming at Zoom instead, neither is; away from every button,
         * the pane under him is edged. */
        {
            zell_rect pad;zell_panes_pad_rect(&s,1,&pad);
            zell_panes_chrome_rect(&s,0,&bar0);zell_panes_chrome_rect(&s,1,&bar1);
            zell_rect edge0={bar0.x+8,bar0.y,40,2},edge1={bar1.x+8,bar1.y,40,2};
            zell_scene at=s;
            at.x=364.f;zell_draw(&at,&canvas);
            assert(ink_count(&canvas,pad,0x5a4a24u,0)>0);
            assert(ink_count(&canvas,edge1,0xd9ba79u,0)==80&&ink_count(&canvas,edge0,0xd9ba79u,0)==0);
            at.x=376.f;zell_draw(&at,&canvas);
            assert(ink_count(&canvas,pad,0x5a4a24u,0)==0);
            assert(ink_count(&canvas,edge1,0xd9ba79u,0)==0&&ink_count(&canvas,edge0,0xd9ba79u,0)==0);
            at.x=60.f;zell_draw(&at,&canvas);
            assert(ink_count(&canvas,edge0,0xd9ba79u,0)==80&&ink_count(&canvas,edge1,0xd9ba79u,0)==0);
            zell_draw(&s,&canvas);
        }
        zell_panes_chrome_rect(&s,0,&bar0);zell_panes_chrome_rect(&s,1,&bar1);
        /* Pane 2 is focused but Kilix has the keys: not the brightest. */
        assert(zell_panes_focus(ZELL_ROOM_PANES_RIGHT)==1);
        assert(ink_count(&canvas,(zell_rect){bar1.x+4,bar1.y+1,2,2},0x23394bu,0)==4);
        assert(ink_count(&canvas,(zell_rect){bar0.x+4,bar0.y+1,2,2},0x18242eu,0)==4);
        pane_click(&s,170,70);
        zell_panes_test_line("N 10 3 is busy. Finish what runs there, then press again.");
        zell_draw(&s,&canvas);
        assert(zell_panes_focus(ZELL_ROOM_PANES_RIGHT)==0&&zell_panes_keys_to_pane());
        assert(ink_count(&canvas,(zell_rect){bar0.x+4,bar0.y+1,2,2},0x2f5878u,0)==4);
        assert(ink_count(&canvas,(zell_rect){bar0.x,bar0.y,3,bar0.h},0xd9ba79u,0)==3*20);
        assert(ink_count(&canvas,(zell_rect){170,67,2,2},0x6a2c2cu,0)==4);
        zell_rect bar3;zell_panes_chrome_rect(&s,3,&bar3);
        zell_rect said={bar3.x+8,bar3.y+2,120,16};
        assert(ink_count(&canvas,said,0xe5ad87u,40)>10);
        /* The notice's own words, not the title in its colour. */
        {
            sr_canvas later;
            assert(sr_canvas_init(&later,960,600));
            int64_t now=zell_notice_clock_ms();
            zell_notice_set_clock(now+4000);
            zell_draw(&s,&later);
            /* Ink against the bar, whatever its colour: the notice's
             * letters are not the title's. */
            int differ=0;
            for(int y=bar3.y+2;y<bar3.y+18;y++)for(int x=bar3.x+24;x<bar3.x+88;x++){
                size_t at=(size_t)y*960u+(size_t)x;
                differ+=((canvas.px[at]&0xffffffu)!=0x18242eu)!=((later.px[at]&0xffffffu)!=0x18242eu);
            }
            assert(differ>40);
            zell_notice_set_clock(now);
            sr_canvas_free(&later);
        }
        assert(ink_count(&canvas,(zell_rect){bar1.x+8,bar1.y+2,80,16},0xe5ad87u,40)==0);
        /* A pane has the keys: the walkway's edge is not lit and no button is. */
        assert(ink_count(&canvas,(zell_rect){400,62,100,2},0xd9ba79u,0)==0);
        assert(ink_count(&canvas,(zell_rect){lit.x+lit.w-10,lit.y+2,4,4},0x1f3444u,0)==16);
    }
    /* While the helper's queue is full, a click changes nothing Zell
     * cannot tell it about. */
    zell_panes_test_reset();
    {
        unsigned char bytes[200];memset(bytes,'x',sizeof bytes);
        size_t before;
        place_room(&s,1,0,0);zell_panes_enter(&s,ZELL_ROOM_PANES_RIGHT);
        do{before=strlen(zell_panes_pending());zell_panes_key(bytes,sizeof bytes);}while(strlen(zell_panes_pending())>before);
        do{before=strlen(zell_panes_pending());zell_panes_key(bytes,1);}while(strlen(zell_panes_pending())>before);
        assert(before>=8188);
        pane_click(&s,498,300);
        assert(zell_panes_focus(ZELL_ROOM_PANES_RIGHT)==0&&!zell_panes_keys_to_pane());
        pane_click(&s,349,70);
        assert(zell_panes_zoom(ZELL_ROOM_PANES_RIGHT)==-1&&zell_panes_focus(ZELL_ROOM_PANES_RIGHT)==0);
        zell_panes_test_line("G codex Codex");
        pane_click(&s,265,10);
        assert(zell_panes_focus(ZELL_ROOM_PANES_RIGHT)==0&&!zell_panes_keys_to_pane());
        /* Over the focused pane no focus line is needed, and still the
         * agent's line did not fit. */
        pane_click(&s,160,10);
        assert(!zell_panes_keys_to_pane());
        /* Pane 1 is focused already: it takes the keys with no line. */
        pane_click(&s,30,300);
        assert(zell_panes_keys_to_pane());
        assert(strlen(zell_panes_pending())==before);
    }
    /* Elsewhere a click is not the panes'. */
    zell_panes_test_reset();
    place_room(&s,1,(float)ZELL_WORLD_W*.5f,(float)ZELL_WORLD_H*.6f);
    s.mouse_valid=true;s.mouse_click=true;s.mouse_x=10;s.mouse_y=10;
    zell_desktop_click(&s);
    assert(!strcmp(zell_panes_pending(),""));
    /* Typing becomes lines for the helper. */
    {
        const unsigned char typed[]={'l','s','\r'};
        zell_panes_key(typed,sizeof typed);
        assert(!strcmp(zell_panes_pending(),"K6c730d\n"));
        zell_panes_key(typed,0);
        assert(!strcmp(zell_panes_pending(),"K6c730d\n"));
    }
    /* A frame in the map is taken only when whole, for a pane room, and is
     * drawn 1:1 under the title bars; a click lands in the pane drawn there. */
    zell_panes_test_reset();
    {
        size_t bytes=zell_panes_map_bytes();
        unsigned char *map=mmap(NULL,bytes,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);
        uint32_t *px=(uint32_t *)(void *)(map+64);
        int fw=0,fh=0;
        assert(map!=MAP_FAILED&&bytes==64u+3840u*2160u*4u);
        place_room(&s,1,0,0);zell_scene_resize(&s,960,600);zell_panes_enter(&s,ZELL_ROOM_PANES_LEFT);
        int cols,rows;zell_panes_cells(960,600,&cols,&rows);
        int w=cols*8,h=rows*16;
        uint32_t header[6]={0,1,(uint32_t)w,(uint32_t)h,3,ZELL_ROOM_PANES_LEFT};
        memcpy(map,"ZPNE",4);memcpy(map+4,header+1,20);
        for(int y=0;y<h;y++)for(int x=0;x<w;x++)px[y*w+x]=0xff000000u|(uint32_t)((x/8)<<8)|(uint32_t)(y/16);
        /* Odd: still being written. */
        zell_panes_test_copy(map);
        assert(!zell_panes_frame(ZELL_ROOM_PANES_LEFT,&fw,&fh));
        header[4]=4;memcpy(map+16,&header[4],4);
        zell_panes_test_copy(map);
        assert(zell_panes_frame(ZELL_ROOM_PANES_LEFT,&fw,&fh)&&fw==w&&fh==h);
        assert(!zell_panes_frame(ZELL_ROOM_PANES_RIGHT,&fw,&fh));
        /* Another room's number, a bad size or a bad mark is not taken. */
        {
            uint32_t bad_room=3,big=3841,seq=6;
            memcpy(map+16,&seq,4);memcpy(map+20,&bad_room,4);
            zell_panes_test_copy(map);
            assert(zell_panes_frame(ZELL_ROOM_PANES_LEFT,&fw,&fh));
            bad_room=ZELL_ROOM_PANES_RIGHT;memcpy(map+20,&bad_room,4);memcpy(map+8,&big,4);
            zell_panes_test_copy(map);
            assert(zell_panes_frame(ZELL_ROOM_PANES_LEFT,&fw,&fh)&&!zell_panes_frame(ZELL_ROOM_PANES_RIGHT,&fw,&fh));
            memcpy(map+8,&header[2],4);map[0]='X';
            zell_panes_test_copy(map);
            assert(!zell_panes_frame(ZELL_ROOM_PANES_RIGHT,&fw,&fh));
            map[0]='Z';bad_room=ZELL_ROOM_PANES_LEFT;memcpy(map+20,&bad_room,4);
        }
        zell_draw(&s,&canvas);
        int matched=0;
        for(int y=2;y<h;y+=37)for(int x=4;x<w;x+=53){
            assert((canvas.px[(size_t)(y+84)*960u+(size_t)x]&0xffffffu)==(px[y*w+x]&0xffffffu));
            /* Columns 0 to 60 are the first pane and its line. */
            assert(zell_panes_pane_at(&s,x)==(((px[y*w+x]>>8)&255u)<61u?0:1));
            matched++;
        }
        assert(matched>150);
        /* Above the cells are the walkway and bars, below them the ground. */
        assert((canvas.px[84u*960u+300u]&0xffffffu)==(px[300]&0xffffffu));
        assert((canvas.px[83u*960u+300u]&0xffffffu)!=(px[300]&0xffffffu));
        assert((canvas.px[599u*960u+100u]&0xffffffu)==0x0b1218u);
        /* A minimised terminal is not drawn over the panes. */
        {
            sr_canvas tab;zell_scene with=s;
            assert(sr_canvas_init(&tab,960,600));
            with.terminal_minimized=true;with.service_phase=5;
            zell_draw(&with,&tab);
            assert(!memcmp(tab.px,canvas.px,(size_t)960*600*4));
            sr_canvas_free(&tab);
        }
        /* When the helper fails its last picture goes and the room says so. */
        zell_panes_test_fail("The panes stopped.");
        assert(!zell_panes_frame(ZELL_ROOM_PANES_LEFT,&fw,&fh)&&!strcmp(zell_panes_error(),"The panes stopped."));
        zell_draw(&s,&canvas);
        assert(ink_count(&canvas,(zell_rect){330,325,300,40},0xe5ad87u,40)>40);
        assert(ink_count(&canvas,(zell_rect){330,325,300,40},0xb9f8dbu,40)==0);
        zell_panes_test_reset();
        assert(!zell_panes_error()[0]&&zell_panes_agent_count()==0&&zell_panes_zoom(ZELL_ROOM_PANES_LEFT)==-1);
    }
    /* At most six agents. */
    {
        static const char *const lines[]={"G a A","G b B","G c C","G d D","G e E","G f F","G g G"};
        for(size_t i=0;i<sizeof lines/sizeof lines[0];i++)zell_panes_test_line(lines[i]);
        assert(zell_panes_agent_count()==6&&!strcmp(zell_panes_agent_label(5),"F"));
        zell_panes_test_reset();
    }
    sr_canvas_free(&canvas);
}
/* What the input scanner hands on, written down as text. */
typedef struct { char log[8192]; size_t n; } scan_log;
static void scan_note(scan_log *l,const char *text,size_t count){
    if(l->n+count+1>=sizeof l->log)return;
    memcpy(l->log+l->n,text,count);l->n+=count;l->log[l->n]=0;
}
static void scan_keys(void *user,const char *bytes,size_t count){scan_note(user,"K[",2);scan_note(user,bytes,count);scan_note(user,"]",1);}
static void scan_pointer(void *user,unsigned buttons,int x,int y,bool release){
    char text[64];int n=snprintf(text,sizeof text,"P[%u %d %d %c]",buttons,x,y,release?'m':'M');
    scan_note(user,text,(size_t)n);
}
static void scan_paste(void *user,const char *bytes,size_t count){
    char text[32];int n;
    /* No bytes: one over the cap, said by its count. */
    if(!bytes){n=snprintf(text,sizeof text,"X%zu",count);scan_note(user,text,(size_t)n);return;}
    n=snprintf(text,sizeof text,"V%zu[",count);
    scan_note(user,text,(size_t)n);scan_note(user,bytes,count<200?count:200);scan_note(user,"]",1);
}
static const char *scan(zell_input *in,scan_log *l,const char *bytes,int64_t now){
    zell_input_sinks sinks={scan_keys,scan_pointer,scan_paste,l};
    zell_input_feed(in,&sinks,bytes,strlen(bytes),now);
    return l->log;
}
static void test_input_scan(void){
    zell_input in={0};
    scan_log l={0};
    zell_input_sinks sinks={scan_keys,scan_pointer,scan_paste,&l};
    /* Keys, pointer reports and keys around them. */
    assert(!strcmp(scan(&in,&l,"ab\033[<0;10;20Mc\033[<35;11;21M\033[<64;1;2M\033[<0;10;20md",0),
        "K[ab]P[0 10 20 M]K[c]P[35 11 21 M]P[64 1 2 M]P[0 10 20 m]K[d]"));
    /* Cut between reads, a report waits for its end. */
    l.n=0;l.log[0]=0;
    scan(&in,&l,"\033[<2;3",0);scan(&in,&l,"0;4",1);
    assert(!strcmp(scan(&in,&l,"0m",2),"P[2 30 40 m]"));
    /* Bad reports are dropped: no zero or huge places, no buttons past 1023. */
    l.n=0;l.log[0]=0;
    assert(!strcmp(scan(&in,&l,"\033[<0;0;5M\033[<0;5;100000M\033[<1024;1;1M\033[<0;x;1Mz",0),"K[z]"));
    /* Other escapes are keys: arrows, CSI 2 keys at their final byte. */
    l.n=0;l.log[0]=0;
    assert(!strcmp(scan(&in,&l,"\033[A\033[2~\033[27;5u\033[24;2~\033OP",0),"K[\033[A]K[\033[2~]K[\033[27;5u]K[\033[24;2~]K[\033O]K[P]"));
    /* A paste, whole, with what looks like an end marker inside it. */
    l.n=0;l.log[0]=0;
    assert(!strcmp(scan(&in,&l,"x\033[200~line 1\nline 2\033[201\033[A\033[201~y",0),"K[x]V21[line 1\nline 2\033[201\033[A]K[y]"));
    /* Markers and paste cut anywhere. */
    l.n=0;l.log[0]=0;
    scan(&in,&l,"\033[20",0);scan(&in,&l,"0~ab\033[2",1);assert(zell_input_pasting(&in));
    zell_input_idle(&in,&sinks,1000);
    scan(&in,&l,"01",1000);
    assert(!strcmp(scan(&in,&l,"~",1000),"V2[ab]"));
    assert(!zell_input_pasting(&in));
    /* A broken marker followed at once by the real end. */
    l.n=0;l.log[0]=0;
    assert(!strcmp(scan(&in,&l,"\033[200~ab\033[2\033[201~z",0),"V5[ab\033[2]K[z]"));
    /* An empty paste is a paste of nothing. */
    l.n=0;l.log[0]=0;
    assert(!strcmp(scan(&in,&l,"\033[200~\033[201~",0),"V0[]"));
    /* Held escapes: a lone one, ESC [, and a CSI 2 key cut short go on as
     * keys after 30 ms; half a pointer report is dropped. */
    l.n=0;l.log[0]=0;
    scan(&in,&l,"\033",100);zell_input_idle(&in,&sinks,130);assert(!l.n);
    zell_input_idle(&in,&sinks,131);assert(!strcmp(l.log,"K[\033]"));
    l.n=0;l.log[0]=0;
    scan(&in,&l,"\033[",200);zell_input_idle(&in,&sinks,231);assert(!strcmp(l.log,"K[\033[]"));
    l.n=0;l.log[0]=0;
    scan(&in,&l,"\033[2;5",300);zell_input_idle(&in,&sinks,331);assert(!strcmp(l.log,"K[\033[2;5]"));
    l.n=0;l.log[0]=0;
    scan(&in,&l,"\033[<0;1",400);zell_input_idle(&in,&sinks,431);assert(!l.n);
    assert(!strcmp(scan(&in,&l,"q",432),"K[q]"));
    /* A paste is not cut short by the 30 ms, nor by 5 seconds while its
     * bytes keep coming. */
    l.n=0;l.log[0]=0;
    scan(&in,&l,"\033[200~abc",500);zell_input_idle(&in,&sinks,5500);assert(!l.n&&zell_input_pasting(&in));
    scan(&in,&l,"d",5500);zell_input_idle(&in,&sinks,10500);assert(!l.n&&zell_input_pasting(&in));
    assert(!strcmp(scan(&in,&l,"\033[201~",10500),"V4[abcd]"));
    /* 5 seconds with nothing: its end is lost. It is dropped (said as no
     * bytes and ZELL_INPUT_PASTE_UNENDED), and what comes next is keys
     * again. */
    l.n=0;l.log[0]=0;
    scan(&in,&l,"\033[200~abc\033[20",500);zell_input_idle(&in,&sinks,5501);
    {
        char want[64];
        snprintf(want,sizeof want,"X%zu",(size_t)ZELL_INPUT_PASTE_UNENDED);
        assert(!strcmp(l.log,want)&&!zell_input_pasting(&in));
        strcat(want,"K[q]");
        assert(!strcmp(scan(&in,&l,"q",5502),want));
    }
    /* One past the cap is dropped whole; one at it goes. */
    {
        size_t big=ZELL_INPUT_PASTE_MAX;
        char *text=malloc(big+1);
        assert(text);memset(text,'p',big);text[big]=0;
        l.n=0;l.log[0]=0;
        scan(&in,&l,"\033[200~",0);zell_input_feed(&in,&sinks,text,big,0);scan(&in,&l,"\033[201~",0);
        assert(!strncmp(l.log,"V4194304[ppp",12));
        l.n=0;l.log[0]=0;
        scan(&in,&l,"\033[200~",0);zell_input_feed(&in,&sinks,text,big,0);scan(&in,&l,"q\033[201~z",0);
        assert(!strcmp(l.log,"X4194305K[z]"));
        free(text);
    }
    zell_input_free(&in);
    assert(!in.paste&&!in.used);
}
static void pane_pointer(zell_scene *s,unsigned buttons,int x,int y,bool release,bool taken){
    assert(zell_panes_pointer(s,buttons,x,y,release)==taken);
}
static void test_pane_io(void){
    zell_scene s;
    char want[2048]="";
    /* Four panes at 960x600: x 0, 248, 488, 728; cells from y 84, 512 high. */
    zell_panes_test_reset();
    place_room(&s,1,0,0);zell_panes_enter(&s,ZELL_ROOM_PANES_RIGHT);
    assert(!zell_panes_pointer(&s,0,100,50,false));
    /* Presses, drags and releases over the cells, at pixels from the pane. */
    pane_pointer(&s,0,260,100,false,true);
    pane_pointer(&s,32,300,120,false,true);
    /* A drag out of the pane stays its own, at the nearest pixel. */
    pane_pointer(&s,32,900,20,false,true);
    pane_pointer(&s,32,-5,700,false,true);
    pane_pointer(&s,0,900,20,true,true);
    strcat(want,"M 1 0 1 0 12 16\nM 1 2 1 0 52 36\nM 1 2 1 0 231 0\nM 1 2 1 0 0 511\nM 1 1 1 0 231 0\n");
    assert(!strcmp(zell_panes_pending(),want));
    /* After the release, a report over the walkway is not the panes'. */
    pane_pointer(&s,0,900,20,true,false);
    /* Right and middle, with Shift, Alt and Ctrl; the wheel; extra buttons. */
    pane_pointer(&s,2|16u,10,90,false,true);
    pane_pointer(&s,1|4u,10,90,false,true);
    pane_pointer(&s,64|4u|8u|16u,500,300,false,true);
    pane_pointer(&s,65|8u,500,300,false,true);
    pane_pointer(&s,128,10,90,false,true);
    strcat(want,"M 0 0 2 2 10 6\nM 0 0 3 1 10 6\nM 2 0 4 7 12 216\nM 2 0 5 4 12 216\nM 0 0 8 0 10 6\n");
    assert(!strcmp(zell_panes_pending(),want));
    /* The last press holds pane 1 until its release, which goes there. */
    pane_pointer(&s,0,0,0,true,true);
    strcat(want,"M 0 1 1 0 0 0\n");
    /* Motion with no button goes only to a program that asked for it
     * (flags 2), once a cell. */
    pane_pointer(&s,35,10,90,false,true);
    assert(!strcmp(zell_panes_pending(),want));
    zell_panes_test_line("B 10 0 20 0 20 3");
    pane_pointer(&s,35,10,90,false,true);
    pane_pointer(&s,35,15,99,false,true);
    pane_pointer(&s,35,16,99,false,true);
    pane_pointer(&s,35,16,100,false,true);
    strcat(want,"M 0 2 0 0 10 6\nM 0 2 0 0 16 15\nM 0 2 0 0 16 16\n");
    assert(!strcmp(zell_panes_pending(),want));
    /* B lines: checked, kept per room and pane. */
    {
        uint64_t b[3]={9,9,9};
        assert(!zell_panes_scrollback(ZELL_ROOM_PANES_RIGHT,1,b)&&b[0]==9);
        const char *bad[]={"B 10 1 10 5 6 0","B 10 1 10 0 0 0","B 10 4 10 0 5 0","B 8 1 10 0 5 0","B 10 1 10 0 5 4",
                           "B 10 1 10 0 5","B 10 1 10 0 5 0 x","B 10 1 x 0 5 0","B 10 1 10 11 5 0"};
        for(size_t i=0;i<sizeof bad/sizeof bad[0];i++){zell_panes_test_line(bad[i]);assert(!zell_panes_scrollback(ZELL_ROOM_PANES_RIGHT,1,b));}
        zell_panes_test_line("B 10 1 532 400 32 1");
        assert(zell_panes_scrollback(ZELL_ROOM_PANES_RIGHT,1,b)&&b[0]==532&&b[1]==400&&b[2]==32);
        assert(zell_panes_lines_back(ZELL_ROOM_PANES_RIGHT,1)==100&&zell_panes_lines_back(ZELL_ROOM_PANES_RIGHT,0)==0);
        assert(zell_panes_lines_back(ZELL_ROOM_PANES_LEFT,1)==0);
        zell_panes_test_line("B 9 1 50 18 32 0");
        assert(zell_panes_lines_back(ZELL_ROOM_PANES_LEFT,1)==0&&zell_panes_lines_back(ZELL_ROOM_PANES_RIGHT,1)==100);
    }
    /* The scrollbar: the right 10 pixels of the cells, a thumb 32/532 of
     * them at 400/500 of the way down. */
    {
        zell_rect track,thumb;
        zell_panes_scrollbar_rect(&s,1,&track,&thumb);
        assert(track.x==470&&track.y==84&&track.w==10&&track.h==512);
        assert(thumb.x==472&&thumb.w==6&&thumb.h==30*0+30&&thumb.y==84+(512-30)*400/500);
        zell_panes_scrollbar_rect(&s,0,&track,&thumb);
        assert(!track.w&&!thumb.w);
        zell_panes_scrollbar_rect(&s,2,&track,&thumb);
        assert(!track.w);
        /* A thumb is at least 16 high. */
        zell_panes_test_line("B 10 2 100000 0 32 0");
        zell_panes_scrollbar_rect(&s,2,&track,&thumb);
        assert(thumb.h==16&&thumb.y==84);
    }
    /* A press on it moves the rows shown and drags them with it until the
     * button goes up, wherever the pointer goes. */
    assert(zell_panes_bar_hover()==-1);
    pane_pointer(&s,35,475,300,false,true);
    assert(zell_panes_bar_hover()==1);
    pane_pointer(&s,0,475,84,false,true);
    assert(zell_panes_bar_hover()==1);
    pane_pointer(&s,32,10,1000,false,true);
    pane_pointer(&s,32,10,350,false,true);
    pane_pointer(&s,0,10,350,true,true);
    assert(zell_panes_bar_hover()==-1);
    /* Lit while the pointer is on it; off when it goes, even to the walkway. */
    pane_pointer(&s,35,475,300,false,true);
    assert(zell_panes_bar_hover()==1);
    pane_pointer(&s,35,475,30,false,false);
    assert(zell_panes_bar_hover()==-1);
    /* (350-84-15)*500/482 */
    strcat(want,"V 1 0\nV 1 500\nV 1 260\nV 1 260\n");
    assert(!strcmp(zell_panes_pending(),want));
    assert(zell_panes_lines_back(ZELL_ROOM_PANES_RIGHT,1)==240);
    /* Pane 4 has no scrollback: its right edge is cells. */
    pane_pointer(&s,0,955,300,false,true);
    pane_pointer(&s,0,955,300,true,true);
    strcat(want,"M 3 0 1 0 227 216\nM 3 1 1 0 227 216\n");
    assert(!strcmp(zell_panes_pending(),want));
    /* End, in the title bar while scrolled back, goes back to the live
     * rows. Four panes at 960 are too narrow for it beside Zoom and Restart;
     * the left room's are not. */
    {
        zell_scene left;zell_rect end,zoom;
        zell_panes_chrome_button(&s,1,ZELL_PANES_END,&end);assert(!end.w);
        place_room(&left,1,0,0);zell_panes_enter(&left,ZELL_ROOM_PANES_LEFT);
        zell_panes_test_line("B 9 1 50 10 32 0");
        zell_panes_chrome_button(&left,1,ZELL_PANES_END,&end);
        zell_panes_chrome_button(&left,1,ZELL_PANES_ZOOM,&zoom);
        assert(zoom.x==824&&end.x==776&&end.y==65&&end.w==44&&end.h==18);
        zell_panes_chrome_button(&left,0,ZELL_PANES_END,&end);assert(!end.w);
        zell_panes_chrome_button(&left,1,ZELL_PANES_END+1,&end);assert(!end.w);
        zell_panes_chrome_button(&left,1,ZELL_PANES_END,&end);
        pane_click(&left,end.x+2,end.y+2);
        strcat(want,"W 1 bottom\nF 1\n");
        assert(!strcmp(zell_panes_pending(),want)&&zell_panes_keys_to_pane());
        /* At 480 its first pane is 240 wide: Zoom and Restart, no End. */
        zell_scene narrow;
        place_room(&narrow,1,0,0);zell_scene_resize(&narrow,480,600);zell_panes_enter(&narrow,ZELL_ROOM_PANES_LEFT);
        zell_panes_test_line("B 9 0 100 0 32 0");
        zell_panes_chrome_button(&narrow,0,ZELL_PANES_END,&end);assert(!end.w);
        zell_panes_chrome_button(&narrow,0,ZELL_PANES_ZOOM,&zoom);assert(zoom.w);
        zell_panes_enter(&s,ZELL_ROOM_PANES_RIGHT);
    }
    /* Shift with the scroll keys, while a pane has the keys: its focus. */
    zell_panes_test_reset();want[0]=0;
    place_room(&s,1,0,0);zell_panes_enter(&s,ZELL_ROOM_PANES_RIGHT);
    assert(!zell_panes_scroll_key(&s,0xe00au,1u));
    pane_click(&s,300,300);
    strcat(want,"F 1\n");
    assert(zell_panes_scroll_key(&s,0xe00au,1u)&&zell_panes_scroll_key(&s,0xe00bu,1u|64u|128u));
    assert(zell_panes_scroll_key(&s,0xe00cu,1u)&&zell_panes_scroll_key(&s,0xe00du,1u));
    assert(!zell_panes_scroll_key(&s,0xe00au,0)&&!zell_panes_scroll_key(&s,0xe00au,1u|4u)&&!zell_panes_scroll_key(&s,0xe00au,2u));
    assert(!zell_panes_scroll_key(&s,0xe009u,1u)&&!zell_panes_scroll_key(&s,'a',1u));
    {
        const char *pending=zell_panes_pending();
        assert(!strcmp(pending,"F 1\nW 1 up\nW 1 down\nW 1 top\nW 1 bottom\n"));
    }
    /* A paste: for the pane with the keys, two kilobytes of hex a line,
     * then the end. */
    zell_panes_test_reset();
    place_room(&s,1,0,0);zell_panes_enter(&s,ZELL_ROOM_PANES_RIGHT);
    assert(!zell_panes_paste(&s,"x",1));
    pane_click(&s,500,300);
    {
        char *text=malloc(4096+3);
        assert(text);
        for(int i=0;i<4099;i++)text[i]=(char)(i%251);
        assert(zell_panes_paste(&s,text,4099));
        assert(zell_panes_paste_left()==4099+1);
        zell_panes_test_drain();
        assert(!zell_panes_paste_left());
        const char *pending=zell_panes_pending();
        assert(!strncmp(pending,"F 2\nP 10 2 000102",17));
        const char *second=strchr(pending+4,'\n')+1,*third=strchr(second,'\n')+1,*end=strchr(third,'\n')+1;
        assert(second-pending-4==(ptrdiff_t)(7+4096+1)&&third-second==(ptrdiff_t)(7+4096+1)&&end-third==(ptrdiff_t)(7+6+1));
        /* Bytes 4096 to 4098 are 4096%251 = 80 on. */
        assert(!strncmp(third,"P 10 2 505152\n",14)&&!strcmp(end,"P 10 2 .\n"));
        /* Another pane's paste waits for this one, and its pane says so; an
         * empty one is nothing. */
        assert(zell_panes_paste(&s,"a",1));
        pane_click(&s,10,300);
        assert(!zell_panes_paste(&s,"b",1)&&zell_panes_paste(&s,"",0));
        assert(!strcmp(zell_panes_notice(ZELL_ROOM_PANES_RIGHT,0),"Another pane's paste is still being sent. Paste again."));
        free(text);
    }
    /* Keys typed while a paste goes follow its end, in order. */
    zell_panes_test_reset();
    place_room(&s,1,0,0);zell_panes_enter(&s,ZELL_ROOM_PANES_RIGHT);pane_click(&s,10,300);
    assert(zell_panes_paste(&s,"ab",2));
    zell_panes_key((const unsigned char *)"x",1);zell_panes_key((const unsigned char *)"\r",1);
    /* (pane 0 had the focus already: no F line) */
    assert(!strcmp(zell_panes_pending(),"")&&zell_panes_paste_left()==3+8);
    zell_panes_test_drain();
    assert(!strcmp(zell_panes_pending(),"P 10 0 6162\nP 10 0 .\nK78\nK0d\n")&&!zell_panes_paste_left());
    zell_panes_key((const unsigned char *)"y",1);
    assert(!strcmp(zell_panes_pending(),"P 10 0 6162\nP 10 0 .\nK78\nK0d\nK79\n"));
    /* A click on another pane while keys wait behind a paste: its focus
     * line waits too, so the keys go where they were typed. */
    zell_panes_test_reset();
    place_room(&s,1,0,0);zell_panes_enter(&s,ZELL_ROOM_PANES_RIGHT);pane_click(&s,10,300);
    assert(zell_panes_paste(&s,"ab",2));
    zell_panes_key((const unsigned char *)"l",1);
    /* More pasted into the same pane would go before the key: refused, and
     * said. */
    assert(!zell_panes_paste(&s,"zz",2));
    assert(!strcmp(zell_panes_notice(ZELL_ROOM_PANES_RIGHT,0),"The last paste is still being sent. Paste again."));
    pane_click(&s,300,300);
    assert(!strcmp(zell_panes_pending(),"")&&zell_panes_focus(ZELL_ROOM_PANES_RIGHT)==1);
    assert(!zell_panes_paste(&s,"cd",2));
    assert(!strcmp(zell_panes_notice(ZELL_ROOM_PANES_RIGHT,1),"Another pane's paste is still being sent. Paste again."));
    zell_panes_test_drain();
    assert(!strcmp(zell_panes_pending(),"P 10 0 6162\nP 10 0 .\nK6c\nF 1\n")&&!zell_panes_paste_left());
    assert(zell_panes_paste(&s,"cd",2));
    zell_panes_test_drain();
    assert(!strcmp(zell_panes_pending(),"P 10 0 6162\nP 10 0 .\nK6c\nF 1\nP 10 1 6364\nP 10 1 .\n"));
    /* A paste whose end never came: said. */
    assert(!zell_panes_paste(&s,NULL,ZELL_INPUT_PASTE_UNENDED));
    assert(!strcmp(zell_panes_notice(ZELL_ROOM_PANES_RIGHT,1),"The paste never ended and was not sent."));
    /* An agent started while the lines waiting behind a paste have room for
     * its line but not the pane's focus: neither is sent. */
    zell_panes_test_reset();
    place_room(&s,1,0,0);zell_panes_enter(&s,ZELL_ROOM_PANES_RIGHT);pane_click(&s,10,300);
    zell_panes_test_line("G codex Codex");
    assert(zell_panes_paste(&s,"ab",2));
    for(int k=0;k<8189;k++)zell_panes_key((const unsigned char *)"x",1);
    {
        size_t left=zell_panes_paste_left();
        zell_rect b;zell_panes_button_rect(&s,1,0,&b);
        pane_click(&s,b.x+b.w/2,b.y+b.h/2);
        assert(zell_panes_paste_left()==left&&zell_panes_focus(ZELL_ROOM_PANES_RIGHT)==0);
        zell_panes_test_drain();
        pane_click(&s,b.x+b.w/2,b.y+b.h/2);
        const char *p=zell_panes_pending();size_t n=strlen(p);
        assert(n>14&&!strcmp(p+n-14,"A 1 codex\nF 1\n")&&zell_panes_focus(ZELL_ROOM_PANES_RIGHT)==1);
    }
    /* The paste has gone out but the lines behind it still wait for room in
     * the queue: a drop then is said too. */
    zell_panes_test_reset();
    place_room(&s,1,0,0);zell_panes_enter(&s,ZELL_ROOM_PANES_RIGHT);pane_click(&s,10,300);
    {
        static char big[31000];memset(big,'a',sizeof big);
        assert(zell_panes_paste(&s,big,sizeof big));
        for(int k=0;k<1000;k++)zell_panes_key((const unsigned char *)"x",1);
        zell_panes_test_drain();
        assert(zell_panes_paste_left()==4000&&strlen(zell_panes_pending())>60000);
        assert(zell_panes_notice(ZELL_ROOM_PANES_RIGHT,0)==NULL);
        int keys=0;size_t before;
        do{before=zell_panes_paste_left();zell_panes_key((const unsigned char *)"x",1);keys++;}while(zell_panes_paste_left()>before);
        assert(keys==7193);
        assert(!strcmp(zell_panes_notice(ZELL_ROOM_PANES_RIGHT,0),"Input made while the paste is sent was dropped."));
    }
    /* Input made while the lines waiting behind a paste are full is dropped,
     * and said; a room line made then is not taken as sent, so it is made
     * once they have gone. */
    zell_panes_test_reset();
    place_room(&s,1,0,0);zell_panes_enter(&s,ZELL_ROOM_PANES_RIGHT);pane_click(&s,10,300);
    assert(zell_panes_paste(&s,"ab",2));
    {
        size_t before,keys=0;
        do{before=zell_panes_paste_left();zell_panes_key((const unsigned char *)"x",1);keys++;}while(zell_panes_paste_left()>before);
        assert(keys==8193);
        assert(!strcmp(zell_panes_notice(ZELL_ROOM_PANES_RIGHT,0),"Input made while the paste is sent was dropped."));
        char line[80];int cols,rows;
        zell_scene_resize(&s,1200,700);
        zell_panes_cells(1200,700,&cols,&rows);
        assert(zell_panes_room_line(ZELL_ROOM_PANES_RIGHT,cols,rows,line,sizeof line)>0&&!strcmp(line,"S 10 150 38\nZ -1\nF 0\n"));
        zell_panes_test_room(&s);
        assert(!strstr(zell_panes_pending(),"S 10 150 38"));
        zell_panes_test_drain();
        assert(!zell_panes_paste_left()&&!strstr(zell_panes_pending(),"S 10 150 38"));
        zell_panes_test_room(&s);
        const char *p=zell_panes_pending();size_t n=strlen(p);
        assert(n>strlen(line)&&!strcmp(p+n-strlen(line),line));
        zell_panes_test_room(&s);
        assert(strlen(zell_panes_pending())==n);
    }
    zell_panes_test_reset();
    place_room(&s,1,0,0);zell_panes_enter(&s,ZELL_ROOM_PANES_RIGHT);pane_click(&s,10,300);
    assert(zell_panes_paste(&s,"ab",2));
    zell_panes_key((const unsigned char *)"x",1);zell_panes_key((const unsigned char *)"\r",1);
    zell_panes_test_drain();
    zell_panes_key((const unsigned char *)"y",1);
    /* A paste the input scanner dropped as too long: said, nothing sent. */
    assert(!zell_panes_paste(&s,NULL,5)&&!zell_panes_notice(ZELL_ROOM_PANES_RIGHT,0));
    assert(!zell_panes_paste(&s,NULL,ZELL_PANES_PASTE_MAX+1u));
    assert(!strcmp(zell_panes_notice(ZELL_ROOM_PANES_RIGHT,0),"The paste is over 4 MiB and was not sent.")&&!zell_panes_paste_left());
    /* With the queue full, the paste waits; none of it is lost. */
    zell_panes_test_reset();
    place_room(&s,1,0,0);zell_panes_enter(&s,ZELL_ROOM_PANES_RIGHT);pane_click(&s,10,300);
    {
        unsigned char bytes[400];memset(bytes,'k',sizeof bytes);
        size_t before;
        do{before=strlen(zell_panes_pending());zell_panes_key(bytes,sizeof bytes);}while(strlen(zell_panes_pending())>before);
        do{before=strlen(zell_panes_pending());zell_panes_key(bytes,1);}while(strlen(zell_panes_pending())>before);
        assert(zell_panes_paste(&s,"abc",3));
        zell_panes_test_drain();
        assert(strlen(zell_panes_pending())==before&&zell_panes_paste_left()==4);
        /* Keys now wait behind it, counted with it. */
        zell_panes_key((const unsigned char *)"z",1);
        assert(strlen(zell_panes_pending())==before&&zell_panes_paste_left()==4+4);
    }
    /* Not in a pane room, and not while Kilix has the keys. */
    {
        zell_scene out;place_room(&out,1,0,0);
        assert(!zell_panes_paste(&out,"x",1)&&!zell_panes_pointer(&out,0,10,100,false)&&!zell_panes_scroll_key(&out,0xe00au,1u));
    }
    /* Too long: refused, and the pane says so. */
    zell_panes_test_reset();
    place_room(&s,1,0,0);zell_panes_enter(&s,ZELL_ROOM_PANES_RIGHT);pane_click(&s,10,300);
    {
        size_t big=ZELL_PANES_PASTE_MAX;
        char *text=calloc(big+1,1);
        assert(text);
        assert(!zell_panes_paste(&s,text,big+1));
        assert(!strcmp(zell_panes_notice(ZELL_ROOM_PANES_RIGHT,0),"The paste is over 4 MiB and was not sent."));
        assert(zell_panes_paste(&s,text,big)&&!zell_panes_paste(&s,text,1));
        free(text);
    }
    /* Copied text, from Y pieces to the clipboard sequence. */
    zell_panes_test_reset();
    {
        size_t n=0;
        assert(!zell_panes_copy_sequence(&n));
        zell_panes_test_line("Y aGVsbG8g");
        assert(!zell_panes_copy_sequence(&n));
        zell_panes_test_line("Y d29ybGQ=");
        zell_panes_test_line("Y .");
        const char *seq=zell_panes_copy_sequence(&n);
        assert(seq&&n==24&&!memcmp(seq,"\033]52;c;aGVsbG8gd29ybGQ=\a",24));
        zell_panes_copy_written();
        assert(!zell_panes_copy_sequence(&n));
        /* A bad piece spoils it; the next copy starts clean. */
        zell_panes_test_line("Y aGVs");zell_panes_test_line("Y a\033b");zell_panes_test_line("Y bG8=");zell_panes_test_line("Y .");
        assert(!zell_panes_copy_sequence(&n));
        zell_panes_test_line("Y .");assert(!zell_panes_copy_sequence(&n));
        zell_panes_test_line("Y eA==");zell_panes_test_line("Y .");
        seq=zell_panes_copy_sequence(&n);
        assert(seq&&n==12&&!memcmp(seq,"\033]52;c;eA==\a",12));
        /* More ends add nothing to a copy that has ended. */
        for(int i=0;i<3000;i++)zell_panes_test_line("Y .");
        seq=zell_panes_copy_sequence(&n);
        assert(seq&&n==12&&!memcmp(seq,"\033]52;c;eA==\a",12));
        /* A newer copy before main wrote it wins. */
        zell_panes_test_line("Y eQ==");zell_panes_test_line("Y .");
        seq=zell_panes_copy_sequence(&n);
        assert(seq&&n==12&&!memcmp(seq,"\033]52;c;eQ==\a",12));
        zell_panes_copy_written();
        /* At most 1 MiB of base64. */
        static char piece[3003];
        memset(piece,'A',sizeof piece-1);piece[0]='Y';piece[1]=' ';piece[sizeof piece-1]=0;
        size_t sent=0;
        while(sent+3000<=(1u<<20)){zell_panes_test_line(piece);sent+=3000;}
        piece[2+((1u<<20)-sent)]=0;zell_panes_test_line(piece);
        zell_panes_test_line("Y .");
        seq=zell_panes_copy_sequence(&n);
        assert(seq&&n==7+(1u<<20)+1);
        zell_panes_copy_written();
        memset(piece,'A',sizeof piece-1);piece[0]='Y';piece[1]=' ';piece[sizeof piece-1]=0;
        sent=0;
        while(sent<=(1u<<20)){zell_panes_test_line(piece);sent+=3000;}
        zell_panes_test_line("Y .");
        assert(!zell_panes_copy_sequence(&n));
    }
    /* From the helper's pipe: a line too long for the reader is dropped, not
     * taken cut short; the next is read. */
    zell_panes_test_reset();
    {
        int fds[2];
        size_t n=0;
        static char longline[6000];
        assert(pipe(fds)==0);
        /* Non-blocking, as the helper's pipe is: a read with nothing there
         * returns. */
        assert(fcntl(fds[0],F_SETFL,O_NONBLOCK)==0);
        memset(longline,'A',sizeof longline);longline[0]='Y';longline[1]=' ';longline[sizeof longline-1]='\n';
        assert(write(fds[1],longline,sizeof longline)==(ssize_t)sizeof longline);
        assert(write(fds[1],"Y .\n",4)==4);
        for(int i=0;i<4;i++)zell_panes_test_read(fds[0]);
        /* Taken cut short, its first 4 KiB would be a copy by now. */
        assert(!zell_panes_copy_sequence(&n));
        assert(write(fds[1],"Y eA==\nY .\n",11)==11);
        close(fds[1]);
        for(int i=0;i<2;i++)zell_panes_test_read(fds[0]);
        close(fds[0]);
        const char *seq=zell_panes_copy_sequence(&n);
        assert(seq&&n==12&&!memcmp(seq,"\033]52;c;eA==\a",12));
    }
    /* A copy of a full 4K pane, about 360 KB of base64, arrives in one read
     * of the helper's pipe, which holds it whole. */
    zell_panes_test_reset();
    {
        int fds[2];
        size_t n=0;
        static char piece[3003];
        assert(pipe(fds)==0);
        assert(zell_panes_test_grow_pipe(fds[0])>=(1<<20));
        assert(fcntl(fds[0],F_SETFL,O_NONBLOCK)==0&&fcntl(fds[1],F_SETFL,O_NONBLOCK)==0);
        memset(piece,'A',sizeof piece);piece[0]='Y';piece[1]=' ';piece[sizeof piece-1]='\n';
        for(int i=0;i<120;i++)assert(write(fds[1],piece,sizeof piece)==(ssize_t)sizeof piece);
        assert(write(fds[1],"Y .\n",4)==4);
        zell_panes_test_read(fds[0]);
        const char *seq=zell_panes_copy_sequence(&n);
        assert(seq&&n==7+120u*3000u+1);
        close(fds[0]);close(fds[1]);
    }
    /* Lines a paste would make: none without a break; CR LF is one break. */
    assert(zell_paste_lines("ls",2)==0&&zell_paste_lines("",0)==0);
    assert(zell_paste_lines("ls\n",3)==1&&zell_paste_lines("ls\r",3)==1&&zell_paste_lines("ls\r\n",4)==1);
    assert(zell_paste_lines("a\nb",3)==2&&zell_paste_lines("a\r\nb\r\n",6)==2&&zell_paste_lines("a\rb\nc",5)==3);
    assert(zell_paste_lines("\n\n",2)==2&&zell_paste_lines("\r\r\n",3)==2);
    /* Alt and the arrows: between the panes, up to the walkway over the
     * pane left, along it pane by pane, and down into the pane he is over.
     * Four panes at 960 are 30, 29, 29 and 29 cells with a cell between:
     * their middles are 120, 364, 604 and 844. */
    zell_panes_test_reset();
    {
        char moves[256]="";
        place_room(&s,1,0,0);
        assert(!zell_panes_move(&s,1,0));
        zell_panes_enter(&s,ZELL_ROOM_PANES_RIGHT);pane_click(&s,10,300);
        assert(zell_panes_keys_to_pane()&&!strcmp(zell_panes_pending(),""));
        for(int to=1;to<4;to++){
            assert(zell_panes_move(&s,1,0));
            char line[8];snprintf(line,sizeof line,"F %d\n",to);strcat(moves,line);
            assert(!strcmp(zell_panes_pending(),moves)&&zell_panes_focus(ZELL_ROOM_PANES_RIGHT)==to&&zell_panes_keys_to_pane());
        }
        /* At the last pane, Right goes nowhere; Down in a pane neither. */
        assert(zell_panes_move(&s,1,0)&&zell_panes_move(&s,0,1));
        assert(!strcmp(zell_panes_pending(),moves)&&zell_panes_focus(ZELL_ROOM_PANES_RIGHT)==3);
        assert(zell_panes_move(&s,-1,0));
        strcat(moves,"F 2\n");assert(!strcmp(zell_panes_pending(),moves)&&zell_panes_focus(ZELL_ROOM_PANES_RIGHT)==2);
        /* Up: Kilix has the keys, over the pane left, facing it. */
        s.direction=3;
        assert(zell_panes_move(&s,0,-1));
        assert(!zell_panes_keys_to_pane()&&s.x==604.f&&s.direction==0&&!strcmp(zell_panes_pending(),moves));
        /* Along the walkway, pane by pane, stopping at the ends. */
        assert(zell_panes_move(&s,1,0)&&s.x==844.f);
        assert(zell_panes_move(&s,1,0)&&s.x==844.f);
        float along[]={604.f,364.f,120.f,120.f};
        for(int i=0;i<4;i++)assert(zell_panes_move(&s,-1,0)&&s.x==along[i]&&!zell_panes_keys_to_pane());
        assert(zell_panes_move(&s,0,-1)&&s.x==120.f&&!zell_panes_keys_to_pane());
        assert(!strcmp(zell_panes_pending(),moves));
        /* Shift runs him along the walkway; let go, he walks. */
        {
            zell_controls c={0};
            float factor=zell_desktop_speed_factor(&s);
            c.move_x=1;c.run=true;
            assert(zell_panes_tick(&s,&c)==1&&s.running&&fabsf(s.x-(120.f+ZELL_RUN_SPEED*factor))<.01f&&s.direction==3);
            c.run=false;
            assert(zell_panes_tick(&s,&c)==1&&!s.running&&fabsf(s.x-(120.f+(ZELL_RUN_SPEED+ZELL_WALK_SPEED)*factor))<.01f);
            c.move_x=0;c.run=true;
            assert(zell_panes_tick(&s,&c)==1&&!s.running);
            s.x=120.f;
        }
        /* Down: the pane he is over gets the keys. */
        assert(zell_panes_move(&s,0,1));
        strcat(moves,"F 0\n");assert(!strcmp(zell_panes_pending(),moves)&&zell_panes_keys_to_pane()&&zell_panes_focus(ZELL_ROOM_PANES_RIGHT)==0);
        /* Zoomed, the others cannot be gone to, from the pane or the walkway;
         * Up and Down still work. */
        zell_rect r;zell_panes_chrome_button(&s,0,ZELL_PANES_ZOOM,&r);
        pane_click(&s,r.x+r.w/2,r.y+r.h/2);
        strcat(moves,"Z 0\n");assert(!strcmp(zell_panes_pending(),moves)&&zell_panes_zoom(ZELL_ROOM_PANES_RIGHT)==0);
        assert(zell_panes_move(&s,1,0)&&!strcmp(zell_panes_pending(),moves)&&zell_panes_focus(ZELL_ROOM_PANES_RIGHT)==0);
        assert(zell_panes_move(&s,0,-1)&&!zell_panes_keys_to_pane()&&s.x==480.f);
        assert(zell_panes_move(&s,1,0)&&s.x==480.f&&zell_panes_move(&s,-1,0)&&s.x==480.f);
        assert(zell_panes_move(&s,0,1)&&zell_panes_keys_to_pane()&&!strcmp(zell_panes_pending(),moves));
    }
    /* The Hotkeys button at the walkway's right end (104 in where the way
     * back is), clear of the plaques and agent buttons; Kilix or a click
     * opens its window; a click, anywhere, closes it and does nothing else;
     * under it the panes get no pointer. */
    zell_panes_test_reset();
    {
        char want[256]="";
        zell_rect hk,b;
        place_room(&s,1,0,0);
        zell_panes_test_line("G qwen Qwen");zell_panes_test_line("G codex Codex");zell_panes_test_line("G grok Grok");
        zell_panes_enter(&s,ZELL_ROOM_PANES_RIGHT);
        zell_panes_hotkeys_rect(&s,&hk);
        assert(hk.x==960-12-72&&hk.w==72&&hk.y==ZELL_PANES_BUTTON_Y&&hk.h==ZELL_PANES_BUTTON_H);
        for(int index=0;index<4;index++){
            zell_panes_pad_rect(&s,index,&b);
            assert(!b.w||b.x+b.w+6<=hk.x||b.x>=hk.x+hk.w);
            for(int agent=0;agent<3;agent++){
                zell_panes_button_rect(&s,index,agent,&b);
                assert(!b.w||b.x+b.w+6<=hk.x);
            }
        }
        /* Before it, the last pane showed Codex at 874: now it does not. */
        zell_panes_button_rect(&s,3,0,&b);assert(b.w&&b.x==740);
        zell_panes_button_rect(&s,3,1,&b);assert(!b.w);
        int index,which;
        s.x=(float)(hk.x+hk.w/2);
        assert(zell_panes_aim(&s,&index,&which)==ZELL_PANES_AIM_HOTKEYS&&!zell_panes_hotkeys_open());
        assert(zell_panes_press(&s)&&zell_panes_hotkeys_open()&&!zell_panes_keys_to_pane());
        assert(zell_panes_pointer(&s,0,100,300,false)&&!strcmp(zell_panes_pending(),want));
        pane_click(&s,100,300);
        assert(!zell_panes_hotkeys_open()&&!zell_panes_keys_to_pane()&&!strcmp(zell_panes_pending(),want));
        pane_click(&s,hk.x+3,hk.y+3);
        assert(zell_panes_hotkeys_open()&&!zell_panes_keys_to_pane());
        /* Into the room again, it is closed. */
        zell_panes_enter(&s,ZELL_ROOM_PANES_RIGHT);
        assert(!zell_panes_hotkeys_open());
        /* A plaque the button would cover moves left of it; one with no room
         * there is not drawn. */
        s.width=900;
        zell_panes_hotkeys_rect(&s,&hk);zell_panes_pad_rect(&s,3,&b);
        assert(hk.x==900-12-72&&b.w&&b.x==hk.x-6-ZELL_PANES_PAD_W);
        /* The left room's way back is at its right end. */
        place_room(&s,1,0,0);zell_panes_enter(&s,ZELL_ROOM_PANES_LEFT);
        zell_panes_hotkeys_rect(&s,&hk);assert(hk.x==960-104-72);
        s.width=150;zell_panes_hotkeys_rect(&s,&hk);assert(!hk.w);
        /* What it says: each line fits its window at 820 pixels. */
        int parts=0;
        for(int i=0;i<zell_panes_hotkeys_lines;i++){
            const char *line=zell_panes_hotkeys_text[i],*bar=strchr(line,'|');
            parts+=line[0]=='#';
            if(bar)assert(bar-line<=25&&strlen(bar+1)<=70);
            else assert(strlen(line)<=98);
        }
        assert(parts==2&&zell_panes_hotkeys_lines>20);
        /* What a Kilix user can press: W, ] and [, L are there; R's own keys. */
        bool w=false,brackets=false,layout=false,resize=false;
        for(int i=0;i<zell_panes_hotkeys_lines;i++){
            const char *line=zell_panes_hotkeys_text[i];
            w|=!strncmp(line,"Ctrl+Shift+W|",13);brackets|=!strncmp(line,"Ctrl+Shift+] / [|",17);
            layout|=!strncmp(line,"Ctrl+Shift+L|",13);
            resize|=!strncmp(line,"Ctrl+Shift+R|",13)&&strstr(line,"W / N")&&!strstr(line,"arrows");
        }
        assert(w&&brackets&&layout&&resize);
        /* All of it fits a tall Zell; a short one shows what fits and a line
         * for the rest, and scrolls no further than the last line. */
        int all=zell_panes_hotkeys_lines*ZELL_PANES_HOTKEYS_LINE_H+ZELL_PANES_WALK_H+16+ZELL_PANES_HOTKEYS_EDGES;
        assert(zell_panes_hotkeys_fit(all)==zell_panes_hotkeys_lines&&zell_panes_hotkeys_fit(all-1)==zell_panes_hotkeys_lines-2);
        assert(zell_panes_hotkeys_fit(100)==1);
        int saved_w=s.width,saved_h=s.height;
        s.width=960;s.height=480;
        int fit=zell_panes_hotkeys_fit(480);
        zell_panes_hotkeys_show(true);
        zell_panes_hotkeys_scroll(&s,-5);assert(zell_panes_hotkeys_top(480)==0);
        zell_panes_hotkeys_scroll(&s,1000);assert(zell_panes_hotkeys_top(480)==zell_panes_hotkeys_lines-fit);
        /* Shorter again, it is where it was left, not further. */
        assert(zell_panes_hotkeys_top(300)==zell_panes_hotkeys_lines-fit);
        /* The wheel scrolls it, and nothing under it. */
        assert(zell_panes_pointer(&s,64u,500,300,false)&&zell_panes_hotkeys_top(480)==zell_panes_hotkeys_lines-fit-3);
        assert(zell_panes_pointer(&s,65u,500,300,false)&&zell_panes_hotkeys_top(480)==zell_panes_hotkeys_lines-fit);
        /* Scrolled to its end, then a Zell tall enough for all of it: from
         * the first line; one a little taller, from no further than fits. */
        assert(zell_panes_hotkeys_top(all)==0&&zell_panes_hotkeys_top(600)==zell_panes_hotkeys_lines-zell_panes_hotkeys_fit(600));
        s.height=all;zell_panes_hotkeys_scroll(&s,-1);assert(zell_panes_hotkeys_top(all)==0);
        s.height=480;zell_panes_hotkeys_scroll(&s,1000);s.height=600;zell_panes_hotkeys_scroll(&s,-1);
        assert(zell_panes_hotkeys_top(600)==zell_panes_hotkeys_lines-zell_panes_hotkeys_fit(600)-1);
        s.height=480;
        zell_panes_hotkeys_show(false);zell_panes_hotkeys_show(true);assert(zell_panes_hotkeys_top(480)==0);
        zell_panes_hotkeys_show(false);
        s.width=saved_w;s.height=saved_h;
    }
    /* Ctrl+Tab and Ctrl+Shift+Tab go round the panes; from the walkway to the
     * pane below Kilix; zoomed, the room shows the pane they go to. Ctrl+Alt+Z
     * zooms the pane with the keys, or the one below Kilix. */
    zell_panes_test_reset();
    {
        char want[256]="";
        place_room(&s,1,0,0);
        assert(!zell_panes_cycle(&s,1)&&!zell_panes_zoom_key(&s));
        zell_panes_enter(&s,ZELL_ROOM_PANES_RIGHT);
        s.x=604.f;
        assert(zell_panes_cycle(&s,1));
        strcat(want,"F 2\n");assert(!strcmp(zell_panes_pending(),want)&&zell_panes_keys_to_pane());
        assert(zell_panes_cycle(&s,1));strcat(want,"F 3\n");
        assert(zell_panes_cycle(&s,1));strcat(want,"F 0\n");
        assert(zell_panes_cycle(&s,-1));strcat(want,"F 3\n");
        assert(!strcmp(zell_panes_pending(),want)&&zell_panes_focus(ZELL_ROOM_PANES_RIGHT)==3);
        assert(zell_panes_zoom_key(&s));
        strcat(want,"Z 3\n");assert(!strcmp(zell_panes_pending(),want)&&zell_panes_zoom(ZELL_ROOM_PANES_RIGHT)==3);
        assert(zell_panes_cycle(&s,-1));
        strcat(want,"Z 2\n");assert(!strcmp(zell_panes_pending(),want)&&zell_panes_zoom(ZELL_ROOM_PANES_RIGHT)==2&&zell_panes_focus(ZELL_ROOM_PANES_RIGHT)==2);
        assert(zell_panes_zoom_key(&s));
        strcat(want,"Z -1\n");assert(!strcmp(zell_panes_pending(),want)&&zell_panes_zoom(ZELL_ROOM_PANES_RIGHT)==-1);
        /* From the walkway: the pane below him, not the one with the focus. */
        assert(zell_panes_move(&s,0,-1)&&zell_panes_move(&s,-1,0)&&!zell_panes_keys_to_pane()&&s.x==364.f);
        assert(zell_panes_zoom_key(&s));
        strcat(want,"Z 1\n");assert(!strcmp(zell_panes_pending(),want)&&!zell_panes_keys_to_pane());
    }
    /* Drawn: the End button and lines back in the title, the scrollbar over
     * the cells while scrolled back, brighter under the pointer. */
    zell_panes_test_reset();
    {
        sr_canvas canvas;
        size_t bytes=zell_panes_map_bytes();
        unsigned char *map=mmap(NULL,bytes,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);
        assert(map!=MAP_FAILED&&sr_canvas_init(&canvas,960,600));
        place_room(&s,1,0,0);zell_panes_enter(&s,ZELL_ROOM_PANES_RIGHT);
        int cols,rows;zell_panes_cells(960,600,&cols,&rows);
        int w=cols*8,h=rows*16;
        uint32_t header[6]={0,1,(uint32_t)w,(uint32_t)h,2,ZELL_ROOM_PANES_RIGHT};
        memcpy(map,"ZPNE",4);memcpy(map+4,header+1,20);
        uint32_t *px=(uint32_t *)(void *)(map+64);
        for(int i=0;i<w*h;i++)px[i]=0xff202020u;
        zell_panes_test_copy(map);
        zell_draw(&s,&canvas);
        zell_rect track,thumb;
        zell_panes_test_line("B 10 1 532 400 32 0");
        zell_panes_test_line("B 10 2 532 500 32 0");
        zell_draw(&s,&canvas);
        zell_panes_scrollbar_rect(&s,1,&track,&thumb);
        assert(ink_count(&canvas,thumb,0x5f8faeu,0)==thumb.w*thumb.h);
        /* At the live rows its bar is not drawn until the pointer is on it. */
        zell_panes_scrollbar_rect(&s,2,&track,&thumb);
        assert(!ink_count(&canvas,thumb,0x5f8faeu,0)&&ink_count(&canvas,track,0x202020u,0)==track.w*track.h);
        pane_pointer(&s,35,track.x+2,300,false,true);
        zell_draw(&s,&canvas);
        assert(ink_count(&canvas,thumb,0x9ff0c8u,0)==thumb.w*thumb.h);
        /* In the left room: End, and "2 shell  8 lines back" where the
         * live rows say "2 shell". */
        zell_rect end,bar;
        zell_panes_enter(&s,ZELL_ROOM_PANES_LEFT);
        zell_panes_test_line("B 9 1 50 10 32 0");
        zell_draw(&s,&canvas);
        zell_panes_chrome_button(&s,1,ZELL_PANES_END,&end);
        zell_panes_chrome_rect(&s,1,&bar);
        assert(end.w&&ink_count(&canvas,end,0x2f5a4cu,0)>end.w*end.h/2);
        {
            sr_canvas live;
            assert(sr_canvas_init(&live,960,600));
            zell_panes_test_line("B 9 1 50 18 32 0");
            zell_draw(&s,&live);
            assert(!ink_count(&live,end,0x2f5a4cu,0));
            assert(region_diffs(&canvas,&live,(zell_rect){bar.x+8+7*8,bar.y+2,8*16,16})>40);
            assert(region_diffs(&canvas,&live,(zell_rect){bar.x+8,bar.y+2,8*7,16})==0);
            sr_canvas_free(&live);
        }
        /* The Hotkeys button, lit while its window is open; the window over
         * the cells, edged in gold, the keys in white in its left column. */
        {
            zell_rect hk;zell_panes_hotkeys_rect(&s,&hk);
            /* Its left margin, clear of the label in either build. */
            zell_rect inside={hk.x+2,hk.y+2,4,hk.h-4};
            int wx=(960-820)/2,wy=ZELL_PANES_WALK_H+8;
            zell_rect edge={wx,wy-2,820,5},keys={wx+22,wy+34+18,27*8,16};
            zell_draw(&s,&canvas);
            assert(hk.x==784&&ink_count(&canvas,inside,0x2a2f45u,0)==inside.w*inside.h);
            assert(!ink_count(&canvas,edge,0xd9ba79u,0)&&!ink_count(&canvas,keys,0xffffffu,0));
            zell_panes_hotkeys_show(true);zell_draw(&s,&canvas);
            assert(ink_count(&canvas,inside,0x5a4a24u,0)==inside.w*inside.h);
            assert(ink_count(&canvas,edge,0xd9ba79u,0)>=820&&ink_count(&canvas,keys,0xffffffu,0)>20);
            /* 600 tall: a line says there are more; scrolled to its end, its
             * last line, the closing note, is on the last row drawn, and the
             * same scrolled from a shorter Zell. */
            int fit=zell_panes_hotkeys_fit(600),last=zell_panes_hotkeys_lines-fit;
            zell_rect note={wx+14,wy+34+fit*ZELL_PANES_HOTKEYS_LINE_H,400,16};
            zell_rect bottom={wx+14,wy+34+(fit-1)*ZELL_PANES_HOTKEYS_LINE_H,400,16};
            assert(fit<zell_panes_hotkeys_lines&&!strchr(zell_panes_hotkeys_text[fit-1],'#')&&strchr(zell_panes_hotkeys_text[fit-1],'|'));
            assert(!strchr(zell_panes_hotkeys_text[zell_panes_hotkeys_lines-1],'|'));
            assert(ink_count(&canvas,note,0xd9ba79u,0)>20&&!ink_count(&canvas,bottom,0x9fb4c0u,0));
            int height=s.height;
            s.height=600;zell_panes_hotkeys_scroll(&s,1000);zell_draw(&s,&canvas);
            assert(zell_panes_hotkeys_top(600)==last&&ink_count(&canvas,bottom,0x9fb4c0u,0)>20);
            zell_panes_hotkeys_show(true);s.height=480;zell_panes_hotkeys_scroll(&s,1000);zell_draw(&s,&canvas);
            assert(ink_count(&canvas,bottom,0x9fb4c0u,0)>20);
            s.height=height;
            zell_panes_hotkeys_show(false);zell_draw(&s,&canvas);
            assert(!ink_count(&canvas,edge,0xd9ba79u,0)&&!ink_count(&canvas,note,0xd9ba79u,0));
        }
        sr_canvas_free(&canvas);
        /* The reset unmaps the map the copy was handed. */
        zell_panes_test_reset();
    }
    zell_panes_test_reset();
}
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
    s.x=zell_stations[10].x*ZELL_WORLD_W;s.y=zell_stations[10].y*ZELL_WORLD_H;
    assert(zell_desktop_near(&s)==10);
    zell_scene_tick(&s,&c);assert(s.voice_on!=voice&&s.voice_changed);
    s.mode=ZELL_MODE_WALK;s.x=ZELL_WORLD_W*.5f;s.y=ZELL_WORLD_H*.98f;
    c=(zell_controls){0};assert(zell_scene_tick(&s,&c));
    for(int i=0;i<=48;i++)assert(zell_desktop_actions(i)>0);
    assert(!strcmp(zell_stations[ZELL_WORKSHOP_COMPUTER].name,"Workshop computer"));
    assert(!strcmp(zell_stations[ZELL_FLEET_BOARD].name,"Fleet board"));
    assert(!strcmp(zell_stations[ZELL_FLOOR_PANEL].name,"Floor panel"));
    {
        int room=s.room;float kept_x=s.x,kept_y=s.y;char query[64];
        snprintf(query,sizeof query,"%s",s.launcher_query);
        assert(strstr(zell_stations[ZELL_WORKSHOP_COMPUTER].role,"stool"));
        assert(zell_stations[ZELL_WORKSHOP_COMPUTER].art==-1);
        assert(zell_desktop_actions(ZELL_WORKSHOP_COMPUTER)==0);
        for(int id=ZELL_FLEET_BOARD;id<=ZELL_FLOOR_PANEL;id++){
            assert(zell_desktop_actions(id)==0);
            assert(zell_stations[id].art==-1);
            assert(strstr(zell_stations[id].role,id==ZELL_FLEET_BOARD?"fleets":"Reserved"));
            s.x=zell_stations[id].x*ZELL_WORLD_W;s.y=zell_stations[id].y*ZELL_WORLD_H;
            for(int r=0;r<=4;r++){
                s.room=r;
                /* The spare plate is the floor interaction. 49 and 50 stay hidden. */
                if(r==1){
                    assert(zell_desktop_station_visible(&s,id));
                    assert((zell_desktop_near(&s)==id)==(id==ZELL_FLOOR_PANEL));
                }else{
                    assert(!zell_desktop_station_visible(&s,id));
                    assert(zell_desktop_near(&s)!=id);
                }
            }
        }
        snprintf(s.launcher_query,sizeof s.launcher_query,"Fleet board");
        for(int i=0;i<zell_desktop_launcher_count(&s);i++)assert(zell_desktop_launcher_station(&s,i)<ZELL_WORKSHOP_COMPUTER);
        snprintf(s.launcher_query,sizeof s.launcher_query,"%s",query);
        s.room=room;s.x=kept_x;s.y=kept_y;
    }
    s.leave=false;s.mode=ZELL_MODE_SERVICE;s.service_phase=2;s.service_count=3;
    c=(zell_controls){.move_y=1};s.menu_hold=0;zell_scene_tick(&s,&c);
    assert(s.service_index==1);
    c=(zell_controls){.confirm=true};zell_scene_tick(&s,&c);
    assert(s.service_send&&strcmp(s.service_reply,"1\n")==0&&s.service_phase==0);
    s.service_send=false;s.service_phase=1;
    c=(zell_controls){.edits="a"};zell_scene_tick(&s,&c);
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
    /* It is drawn on the east wall: past the walkable edge, inside the world. */
    assert(zell_stations[12].x*ZELL_WORLD_W+ZELL_SWITCH_WALL_DX>ZELL_WORLD_W*.94f);
    assert(zell_stations[12].x*ZELL_WORLD_W+ZELL_SWITCH_WALL_DX<ZELL_WORLD_W-24);
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
        if(id<33)assert(art->height==(id>=5&&id<=10?112:84));
        else assert(art->height>0&&art->width>0);
        for (int pixel=0; pixel<art->width*art->height; pixel++) {
            uint32_t rgba=art->pixels[pixel], alpha=rgba>>24;
            assert(((rgba>>16)&255u)<=alpha);
            assert(((rgba>>8)&255u)<=alpha);
            assert((rgba&255u)<=alpha);
        }
    }
    /* Round-two sheets append after the workshop stairs. Cells keep the
     * painted anchor: floor 384x512, monitors 768x1024, button 512x512,
     * stool the full 1536x1024 canvas. Monitor glass is cleared. */
    assert(ZELL_ART_COMMAND_ROOM==33);
    assert(ZELL_ART_FLOOR_OFF==ZELL_ART_WORKSHOP_STAIRS+1);
    assert(zell_art_get(ZELL_ART_FLOOR_OFF)->width==384&&zell_art_get(ZELL_ART_FLOOR_OFF)->height==512);
    assert(zell_art_get(ZELL_ART_FLOOR_SPARE)->width==384&&zell_art_get(ZELL_ART_FLOOR_SPARE)->height==512);
    assert(zell_art_get(ZELL_ART_MONITOR_CONTROLLER)->width==768&&zell_art_get(ZELL_ART_MONITOR_CONTROLLER)->height==1024);
    assert(zell_art_get(ZELL_ART_MONITOR_WORKER)->width==768&&zell_art_get(ZELL_ART_MONITOR_WORKER)->height==1024);
    assert(zell_art_get(ZELL_ART_CHECK_IDLE)->width==512&&zell_art_get(ZELL_ART_CHECK_IDLE)->height==512);
    assert(zell_art_get(ZELL_ART_CHECK_BUSY_LL)->width==512&&zell_art_get(ZELL_ART_CHECK_BUSY_LL)->height==512);
    assert(zell_art_get(ZELL_ART_WORKSHOP_STOOL)->width==1536&&zell_art_get(ZELL_ART_WORKSHOP_STOOL)->height==1024);
    {
        const zell_art *glass=zell_art_get(ZELL_ART_MONITOR_CONTROLLER);
        uint32_t px=glass->pixels[400*glass->width+200];
        assert((px>>24)==0);
        glass=zell_art_get(ZELL_ART_MONITOR_WORKER);
        px=glass->pixels[400*glass->width+200];
        assert((px>>24)==0);
    }
    zell_scene_init(&s,640,400);zell_desktop_init(&s);
    for (int actor=-1; actor<6; actor++) {
        s.flame_tick=0;zell_actor_pose a=zell_desktop_pose(&s,actor);
        s.flame_tick=32;zell_actor_pose b=zell_desktop_pose(&s,actor);
        assert(a.art==b.art && a.offset_y!=b.offset_y);
    }
    for(int actor=6;actor<=8;actor++){
        int seen[4]={0};
        for(int frame=0;frame<4;frame++){
            s.flame_tick=frame*20;
            int art=zell_desktop_pose(&s,actor).art;
            int base=actor==6?ZELL_ART_GIT_IDLE:actor==7?ZELL_ART_PILOT_IDLE:ZELL_ART_KEY_MASTER_IDLE;
            assert(art>=base&&art<base+4);seen[art-base]++;
        }
        for(int frame=0;frame<4;frame++)assert(seen[frame]==1);
    }
    /* Large screens are clickable even when the player is across the room. */
    zell_scene_init(&s,ZELL_WORLD_W,ZELL_WORLD_H);zell_desktop_init(&s);
    s.room=1;s.mode=ZELL_MODE_WALK;s.mouse_valid=true;
    zell_room_view_identity(&s);
    for(int screen=0;screen<5;screen++){
        zell_rect frame,display;zell_monitor_layout(screen,&frame,&display);
        assert(frame.w>=320&&display.w>=304);
        assert(display.x>frame.x&&display.x+display.w<frame.x+frame.w);
        click_world(&s,(float)(display.x+display.w/2),(float)(display.y+display.h/2));
        assert(zell_desktop_click(&s)&&s.command_focus==screen+1&&s.mode==ZELL_MODE_WALK);
        click_world(&s,(float)(frame.x+frame.w/2),(float)(frame.y+8));
        assert(zell_desktop_click(&s)&&!s.command_focus&&s.station==15+screen&&s.mode==ZELL_MODE_DESK);
        s.mode=ZELL_MODE_WALK;
    }
    /* Wall hits belong only to the Command Center. Cave hatches retain their own bounds. */
    s.room=0;zell_room_view_identity(&s);
    {
        zell_rect frame,display;
        zell_monitor_layout(2,&frame,&display);
        click_world(&s,(float)(display.x+display.w/2),(float)(display.y+display.h/2));
        assert(!zell_desktop_click(&s));
        click_world(&s,zell_stations[ZELL_LAUNDRY_HATCH].x*ZELL_WORLD_W,zell_stations[ZELL_LAUNDRY_HATCH].y*ZELL_WORLD_H-14);
        assert(zell_desktop_click(&s)&&s.room==2&&s.mode==ZELL_MODE_WALK);
    }
    zell_scene_init(&s,640,400);zell_desktop_init(&s);
    c=(zell_controls){.move_x=1};
    for (int frame=0;frame<4;frame++) {
        for (int tick=0;tick<7;tick++) zell_scene_tick(&s,&c);
        assert(zell_desktop_pose(&s,-1).art==17+3*4+frame);
    }
    c=(zell_controls){0};zell_scene_tick(&s,&c);
    assert(s.step_ticks==0&&zell_desktop_pose(&s,-1).art==4);
    s.x=ZELL_WORLD_W*.865f;c=(zell_controls){.move_x=1};
    zell_scene_tick(&s,&c);assert(s.step_ticks==0);
    /* Shift runs immediately, steers freely, and requires movement. */
    zell_scene_init(&s,640,400);zell_desktop_init(&s);s.x=700;s.y=550;
    c=(zell_controls){.run=true};zell_scene_tick(&s,&c);
    assert(!s.running&&s.x==700.f&&s.y==550.f);
    c.move_x=c.move_y=1;zell_scene_tick(&s,&c);
    assert(s.running&&s.x>700.f&&s.y>550.f&&fabsf(hypotf(s.x-700.f,s.y-550.f)-ZELL_RUN_SPEED)<.02f);
    float old_x=s.x,old_y=s.y;c.move_x=0;c.move_y=-1;zell_scene_tick(&s,&c);
    assert(s.running&&s.direction==ZELL_DIR_UP&&s.x==old_x&&fabsf(s.y-old_y+ZELL_RUN_SPEED)<.02f);
    c.run=false;old_y=s.y;zell_scene_tick(&s,&c);
    assert(!s.running&&fabsf(s.y-old_y+ZELL_WALK_SPEED)<.02f);
    c=(zell_controls){.launcher=true};zell_scene_tick(&s,&c);
    assert(s.mode==ZELL_MODE_LAUNCHER&&!s.running);

    /* Trapdoors, room-local launcher, stairs, and resource slowdown. */
    zell_scene_init(&s,640,400);zell_desktop_init(&s);
    s.x=zell_stations[13].x*ZELL_WORLD_W;s.y=zell_stations[13].y*ZELL_WORLD_H;
    c=(zell_controls){.confirm=true};zell_scene_tick(&s,&c);
    assert(s.room==1&&s.mode==ZELL_MODE_WALK&&!s.desktop_launch);
    assert(zell_desktop_launcher_count(&s)==13);
    assert(zell_desktop_launcher_station(&s,12)==ZELL_COMMAND_COMPUTER);
    for(int i=0;i<zell_desktop_launcher_count(&s);i++)assert(zell_desktop_launcher_station(&s,i)!=ZELL_FLOOR_PANEL);
    assert(s.coding_yolo_refresh&&!s.coding_yolo_ready);
    assert(zell_desktop_launcher_station(&s,0)==15);
    assert(zell_desktop_launcher_station(&s,6)==29);
    /* The Command Center's way out is walked through; Enter where its
     * stairs once stood does nothing. */
    s.x=zell_stations[28].x*ZELL_WORLD_W;s.y=zell_stations[28].y*ZELL_WORLD_H;
    zell_scene_tick(&s,&c);assert(s.room==1&&s.mode==ZELL_MODE_WALK);
    walk_out_of_command_center(&s);
    assert(s.room==0);
    s.x=zell_stations[14].x*ZELL_WORLD_W;s.y=zell_stations[14].y*ZELL_WORLD_H;
    zell_scene_tick(&s,&c);assert(s.room==2);
    s.disk_count=3;assert(zell_desktop_launcher_count(&s)==5);
    assert(!zell_desktop_station_visible(&s,28));
    strcpy(s.launcher_query,"Way back to the cave");assert(zell_desktop_launcher_count(&s)==0);
    s.launcher_query[0]=0;
    assert(!zell_desktop_station_visible(&s,23));
    assert(zell_desktop_station_visible(&s,34)&&zell_desktop_station_visible(&s,35));
    s.room=3;assert(zell_desktop_station_visible(&s,40)&&!zell_desktop_station_visible(&s,41));
    s.room=4;assert(zell_desktop_station_visible(&s,41)&&!zell_desktop_station_visible(&s,40));
    /* The way out at the bottom of every downstairs room is walked onto. */
    for(int room=1;room<=4;room++){
        int hatch=room==1?13:room==2?14:room==3?42:43;
        zell_scene_init(&s,640,400);zell_desktop_init(&s);
        s.x=zell_stations[hatch].x*ZELL_WORLD_W;s.y=zell_stations[hatch].y*ZELL_WORLD_H;
        c=(zell_controls){.confirm=true};zell_scene_tick(&s,&c);
        assert(s.room==room&&!zell_desktop_on_stairs(&s));
        /* The Command Center and Laundry open along their whole south edge. */
        s.x=ZELL_WORLD_W*.30f;c=(zell_controls){.move_y=1};
        for(int i=0;i<400&&s.room==room;i++)zell_scene_tick(&s,&c);
        if(room<=2)assert(s.room==0);
        else assert(s.room==room&&s.y<=ZELL_WORLD_H*.91f+1);
        if(room>2){s.x=ZELL_WORLD_W*.50f;s.y=ZELL_WORLD_H*.78f;}
        int ticks=0;while(s.room==room&&ticks<400){zell_scene_tick(&s,&c);ticks++;}
        assert(s.room==0&&s.mode==ZELL_MODE_WALK&&!s.leave);
        assert(fabsf(s.x-zell_stations[hatch].x*ZELL_WORLD_W)<1);
        assert(fabsf(s.y-zell_stations[hatch].y*ZELL_WORLD_H-50)<1);
    }
    /* Standing things hide him only when he is further away than their feet. */
    zell_scene_init(&s,640,400);zell_desktop_init(&s);
    s.y=zell_stations[0].y*ZELL_WORLD_H-30;assert(zell_desktop_station_in_front(&s,0));
    s.y=zell_stations[0].y*ZELL_WORLD_H+30;assert(!zell_desktop_station_in_front(&s,0));
    s.y=0;assert(!zell_desktop_station_in_front(&s,12)&&!zell_desktop_station_in_front(&s,13));
    s.room=1;assert(!zell_desktop_station_in_front(&s,18)&&zell_desktop_station_in_front(&s,31));
    assert(!zell_desktop_station_in_front(&s,28)&&!zell_desktop_station_in_front(&s,40));
    s.room=0;
    /* The Mana console lives in the Command Center. Its browser pages open
     * only after the screen has zoomed in; its other tasks start at once. */
    zell_scene_init(&s,640,400);zell_desktop_init(&s);
    assert(!zell_desktop_station_visible(&s,ZELL_MANA_CONSOLE));
    s.room=1;assert(zell_desktop_station_visible(&s,ZELL_MANA_CONSOLE));
    assert(zell_desktop_actions(ZELL_MANA_CONSOLE)==7);
    s.x=zell_stations[ZELL_MANA_CONSOLE].x*ZELL_WORLD_W;s.y=zell_stations[ZELL_MANA_CONSOLE].y*ZELL_WORLD_H+30;
    assert(zell_desktop_near(&s)==ZELL_MANA_CONSOLE);
    c=(zell_controls){.confirm=true};zell_scene_tick(&s,&c);
    assert(s.mode==ZELL_MODE_DESK&&s.station==ZELL_MANA_CONSOLE&&!s.desktop_launch);
    s.menu_index=2;zell_scene_tick(&s,&c);
    assert(s.zoom_ticks==1&&!s.desktop_launch);
    c=(zell_controls){.cancel=true,.move_y=1};
    for(int i=1;i<ZELL_ZOOM_TICKS;i++){zell_scene_tick(&s,&c);assert(!s.desktop_launch&&s.zoom_ticks==i+1);}
    zell_scene_tick(&s,&c);
    assert(s.desktop_launch&&s.zoom_ticks==0&&s.station==ZELL_MANA_CONSOLE&&s.menu_index==2);
    s.desktop_launch=false;s.mode=ZELL_MODE_DESK;s.menu_index=4;
    c=(zell_controls){.confirm=true};zell_scene_tick(&s,&c);
    assert(s.desktop_launch&&s.zoom_ticks==0);
    /* The Sentinel stands in the Vault; the Signalman and Merchant in the Workshop. */
    s.room=0;assert(!zell_desktop_station_visible(&s,33)&&!zell_desktop_station_visible(&s,36)&&!zell_desktop_station_visible(&s,39));
    assert(zell_desktop_station_visible(&s,32)&&zell_desktop_station_visible(&s,37)&&zell_desktop_station_visible(&s,38));
    s.room=3;assert(zell_desktop_station_visible(&s,39)&&zell_desktop_station_visible(&s,40)&&!zell_desktop_station_visible(&s,33));
    s.room=4;assert(zell_desktop_station_visible(&s,33)&&zell_desktop_station_visible(&s,36)&&zell_desktop_station_visible(&s,41)&&!zell_desktop_station_visible(&s,39));
    s.room=3;assert(!zell_desktop_station_visible(&s,36));
    s.room=0;s.mode=ZELL_MODE_WALK;s.desktop_launch=false;
    strcpy(s.launcher_query,"checksum");
    assert(zell_desktop_launcher_count(&s)==2);
    assert(zell_desktop_launcher_station(&s,0)==34);
    assert(zell_desktop_launcher_station(&s,1)==40);
    strcpy(s.launcher_query,"no-matching-action-123");assert(zell_desktop_launcher_count(&s)==0);
    s.launcher_query[0]=0;s.room=0;s.mode=ZELL_MODE_WALK;
    zell_scene_resize(&s,1536,1024);s.mouse_valid=true;
    s.mouse_x=119;s.mouse_y=330;s.mouse_click=true;
    assert(zell_desktop_click(&s)&&s.dictation_on&&s.dictation_changed);
    s.mouse_click=true;assert(zell_desktop_click(&s)&&!s.dictation_on);
    s.mouse_x=140;s.mouse_click=true;assert(zell_desktop_click(&s)&&s.station==45&&s.desktop_launch);
    for(int station=32;station<42;station++){
        const zell_art *a=zell_art_get(zell_stations[station].art),*b=zell_art_get(zell_stations[station].art+1);
        assert(a&&b&&a->height==112&&b->height==112);
        assert(a->width==b->width);
        assert(memcmp(a->pixels,b->pixels,(size_t)a->width*(size_t)a->height*4)!=0);
    }
    /* Switch picking shares the painted rectangles, including a panned view. */
    for(int width=640;width<=1600;width+=960){
        zell_scene t;zell_scene_init(&t,width,width==640?400:1000);zell_desktop_init(&t);
        t.room=1;t.mouse_valid=true;
        for(int i=0;i<2;i++){
            t.x=zell_stations[46+i].x*ZELL_WORLD_W;t.y=zell_stations[46+i].y*ZELL_WORLD_H;
            zell_rect r;zell_command_switch_layout(i,&r);
            t.mode=ZELL_MODE_WALK;click_world(&t,(float)(r.x+r.w/2),(float)(r.y+r.h/2));
            assert(zell_desktop_click(&t)&&t.station==46+i&&t.desktop_launch);
            t.mode=ZELL_MODE_WALK;t.desktop_launch=false;
            c=(zell_controls){.confirm=true};zell_scene_tick(&t,&c);
            assert(t.station==46+i&&t.desktop_launch);
            t.room=0;assert(!zell_desktop_station_visible(&t,46+i));t.room=1;
        }
    }
    s.hp=10;s.stamina=100;assert(zell_desktop_speed_factor(&s)<.5f);
    s.hp=100;s.stamina=0;assert(zell_desktop_speed_factor(&s)==.35f);
    s.hp=-1;s.stamina=-1;assert(zell_desktop_speed_factor(&s)==1.0f);

    /* Check visible motion in the actual renderer, with unmoving floor
     * pixels as a control. This catches a pose that is never drawn. */
    /* Floating terminal window buttons. Windowed is smaller than maximized;
     * minimizing frees Kilix and leaves a bottom-bar button that reopens it. */
    {
        zell_scene t;zell_scene_init(&t,1000,700);zell_desktop_init(&t);
        int cols,rows,big_cols,big_rows;
        zell_terminal_cells(&t,&cols,&rows);
        t.terminal_maximized=true;zell_terminal_cells(&t,&big_cols,&big_rows);t.terminal_maximized=false;
        assert(big_cols==117&&big_rows==33&&cols==81&&rows==23);
        t.mode=ZELL_MODE_SERVICE;t.service_phase=5;t.terminal_width=cols*8;t.terminal_height=rows*16;
        zell_terminal_layout l;zell_terminal_layout_for(&t,t.width,t.height,&l);
        /* Both buttons sit inside the title bar, at its right end, apart. */
        assert(l.maximize.x+l.maximize.w<=l.frame.x+l.frame.w&&l.maximize.y>=l.frame.y);
        assert(l.maximize.y+l.maximize.h<=l.image.y&&l.minimize.x+l.minimize.w<=l.maximize.x);
        assert(l.minimize.x>l.frame.x+l.frame.w/2);
        t.mouse_valid=true;
        /* A press on the terminal picture is not a button. */
        t.mouse_x=l.image.x+20;t.mouse_y=l.image.y+20;t.mouse_click=true;
        assert(!zell_terminal_click(&t)&&!t.mouse_click&&t.mode==ZELL_MODE_SERVICE);
        /* Moving over a button without pressing does nothing. */
        t.mouse_x=l.minimize.x+3;t.mouse_y=l.minimize.y+3;
        assert(!zell_terminal_click(&t)&&!t.terminal_minimized);
        t.mouse_x=l.maximize.x+3;t.mouse_y=l.maximize.y+3;t.mouse_click=true;
        assert(zell_terminal_click(&t)&&t.terminal_maximized&&t.mode==ZELL_MODE_SERVICE);
        t.mouse_click=true;assert(zell_terminal_click(&t)&&!t.terminal_maximized);
        t.mouse_x=l.minimize.x+3;t.mouse_y=l.minimize.y+3;t.mouse_click=true;
        assert(zell_terminal_click(&t)&&t.terminal_minimized&&t.mode==ZELL_MODE_WALK&&t.service_phase==5);
        /* Minimized: the old button positions are dead, only the bar button works. */
        t.mouse_click=true;assert(!zell_terminal_click(&t)&&t.terminal_minimized);
        assert(l.tab.y+l.tab.h<=t.height-30&&l.tab.y>t.height/2);
        /* The button starts the bottom row and is as wide as its title needs. */
        {
            zell_terminal_layout named;char saved[96];memcpy(saved,t.terminal_title,sizeof saved);
            snprintf(t.terminal_title,sizeof t.terminal_title,"Browser");zell_terminal_layout_for(&t,t.width,t.height,&named);
            assert(named.tab.x==ZELL_WINDOW_ROW_X&&named.tab.w==7*8+36);
            snprintf(t.terminal_title,sizeof t.terminal_title,"A very long window title that would not fit a button");
            zell_terminal_layout_for(&t,t.width,t.height,&named);assert(named.tab.w==236);
            memcpy(t.terminal_title,saved,sizeof saved);
        }
        assert(zell_desktop_actions(ZELL_DJ_STATION)==10);
        assert(strstr(zell_desktop_action(ZELL_DJ_STATION,ZELL_DJ_BROWSER_ACTION),"browser window"));
        t.mouse_x=l.tab.x+5;t.mouse_y=l.tab.y+5;t.mouse_click=true;
        assert(zell_terminal_click(&t)&&!t.terminal_minimized&&t.mode==ZELL_MODE_SERVICE);
        /* No terminal, no buttons: the same press is ignored. */
        t.service_phase=2;t.mouse_x=l.minimize.x+3;t.mouse_y=l.minimize.y+3;t.mouse_click=true;
        assert(!zell_terminal_click(&t)&&!t.terminal_minimized);
        /* The bar button is drawn only while minimized. */
        sr_canvas shown,hidden;
        assert(sr_canvas_init(&shown,t.width,t.height)&&sr_canvas_init(&hidden,t.width,t.height));
        t.mode=ZELL_MODE_WALK;t.service_phase=5;t.mouse_valid=false;
        t.terminal_minimized=false;zell_draw(&t,&hidden);
        t.terminal_minimized=true;zell_draw(&t,&shown);
        int inside=0,outside=0;
        for(int py=0;py<t.height;py++)for(int px=0;px<t.width;px++){
            if(shown.px[py*t.width+px]==hidden.px[py*t.width+px])continue;
            if(px>=l.tab.x&&px<l.tab.x+l.tab.w&&py>=l.tab.y&&py<l.tab.y+l.tab.h)inside++;else outside++;
        }
        assert(inside>1000&&outside==0);
        sr_canvas_free(&shown);sr_canvas_free(&hidden);
    }
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
        /* Draw order: the Vault Keeper and the Pilot cover
         * Kilix when he stands behind them, and he covers them from in front.
         * Cutouts are a shade translucent, so hidden means within 3 per channel. */
        for(int pass=0;pass<2;pass++){
            int station=pass?30:40,top=-60,bottom=-36,behind=-30;
            int ox=(int)(zell_stations[station].x*ZELL_WORLD_W),oy=(int)(zell_stations[station].y*ZELL_WORLD_H);
            zell_scene_init(&s,ZELL_WORLD_W,ZELL_WORLD_H);zell_desktop_init(&s);s.room=pass?1:3;
            s.x=ZELL_WORLD_W*.30f;s.y=ZELL_WORLD_H*.88f;zell_draw(&s,&before);
            s.x=(float)ox;s.y=(float)(oy+behind);zell_draw(&s,&after);
            int hidden=0,shown=0;
            for(int py=oy+top;py<oy+bottom;py++)for(int px=ox-6;px<ox+6;px++)
                if(pixel_gap(before.px[py*ZELL_WORLD_W+px],after.px[py*ZELL_WORLD_W+px])>3)hidden++;
            assert(hidden==0);
            s.y=(float)(oy+30);zell_draw(&s,&after);
            for(int py=oy-50;py<oy-36;py++)for(int px=ox-6;px<ox+6;px++)
                if(pixel_gap(before.px[py*ZELL_WORLD_W+px],after.px[py*ZELL_WORLD_W+px])>24)shown++;
            assert(shown>40);
        }
        sr_canvas_free(&before);sr_canvas_free(&after);
        test_notice_pixels();
    }
#ifdef ZELL_RENDER_NEURONTE
    zell_neuronte_close();
#endif
    test_room_fit();
    test_lower_hatches();
    test_workshop_puzzllm_hatch();
    test_four_rooms();
    test_fleet_board();
    test_notice_deadlines();
    test_floor_panels();
    test_contract_fixtures();
    test_obstacles();
    test_workshop_hop();
    test_theater();
    test_names_show_only_beside_kilix();
    test_command_center();
    test_command_openings();
    test_pane_rooms();
    test_input_scan();
    test_pane_io();
    puts("desktop world, camera, alpha, idle/walk animation, launcher and voice: ok");
    return 0;
}
