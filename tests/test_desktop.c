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

/* Largest per-channel difference between two pixels. */
static int pixel_gap(uint32_t a,uint32_t b){
    int gap=0;
    for(int shift=0;shift<24;shift+=8){int d=(int)((a>>shift)&255u)-(int)((b>>shift)&255u);if(d<0)d=-d;if(d>gap)gap=d;}
    return gap;
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
        if(!zell_desktop_station_visible(s,i)||i==28||i==46||i==47)continue;
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
    static const int expect_count[5]={2,6,0,4,7};
    for(int room=0;room<5;room++){
        place_room(&s,room,(float)ZELL_WORLD_W*.50f,(float)ZELL_WORLD_H*.80f);
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
        place_room(&s,room,(float)ZELL_WORLD_W*.30f,(float)ZELL_WORLD_H*.80f);
        zell_controls south={.move_y=1};
        for(int tick=0;tick<80;tick++)zell_scene_tick(&s,&south);
        assert(s.room==room&&!s.leave&&s.y<=(float)ZELL_WORLD_H*.91f+1.f);
        place_room(&s,room,(float)ZELL_WORLD_W*.50f,(float)ZELL_WORLD_H*.80f);
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
    /* Controller south edge: stop, then slide east without sticking. */
    zell_rect frame,display;
    zell_monitor_layout(0,&frame,&display);
    float edge=(float)(frame.y+frame.h);
    float foot_x=(float)frame.x+40.f;
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
    place_room(&s,1,(float)frame.x+30.f,edge+2.f);
    float held_x=s.x,held_y=s.y;
    zell_controls into={.move_x=1,.move_y=-1};
    for(int tick=0;tick<8;tick++)zell_scene_tick(&s,&into);
    assert(s.y==held_y&&s.x>held_x+15.f);
    /* A fitted view must not move world footprints. */
    s.room_view.scale=0.5f;s.room_view.offset_x=40;s.room_view.offset_y=-15;
    assert(zell_desktop_blocked(&s,foot_x,edge-1.f));
    assert(!zell_desktop_blocked(&s,(float)ZELL_WORLD_W*.50f,(float)ZELL_WORLD_H*.84f));
    /* Floor-panel rims are walkable. Their circles are not walls. */
    zell_rect sw;zell_command_switch_layout(0,&sw);
    float panel_x=(float)sw.x+(float)sw.w*0.5f;
    float panel_y=(float)sw.y+(float)sw.h*0.5f;
    place_room(&s,1,panel_x,(float)sw.y-4.f);
    assert(!zell_desktop_blocked(&s,s.x,s.y));
    assert(!zell_desktop_blocked(&s,panel_x,panel_y));
    zell_controls across={.move_y=1};
    for(int tick=0;tick<80;tick++)zell_scene_tick(&s,&across);
    assert(s.room==1&&s.y>880.f);
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
    zell_rect mana_frame,mana_display;
    place_room(&s,1,(float)ZELL_WORLD_W*.50f,(float)ZELL_WORLD_H*.80f);
    zell_monitor_layout(0,&frame,&display);
    float bx,by,bx1,by1;
    zell_desktop_block_rect(&s,0,&bx,&by,&bx1,&by1);
    assert(bx==(float)frame.x&&by==(float)frame.y);
    assert(bx1==(float)(frame.x+frame.w)&&by1==(float)(frame.y+frame.h));
    zell_mana_console_layout(&mana_frame,&mana_display);
    zell_desktop_block_rect(&s,5,&bx,&by,&bx1,&by1);
    assert(bx==(float)mana_frame.x&&by1==(float)(mana_frame.y+mana_frame.h));
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
static void test_room_fit(void){
    zell_scene s;
    float x,y,sx,sy,wx,wy,sx2,sy2;
    int room,cx,cy;
    const int fitted[2][2]={{960,600},{640,400}};
    zell_scene_init(&s,800,500);
    zell_desktop_init(&s);
    x=s.x;y=s.y;room=s.room;
    zell_scene_resize(&s,1536,1000);
    zell_room_view_identity(&s);
    assert(s.room_view.scale==1.f);
    assert(s.x==x&&s.y==y&&s.room==room);
    assert(s.room_view.logical_y==64);
    assert(s.room_view.logical_h==ZELL_WORLD_H-64);
    assert(s.room_view.logical_x==(int)(0.135f*(float)ZELL_WORLD_W));
    assert(s.room_view.logical_w==(int)(0.865f*(float)ZELL_WORLD_W)-s.room_view.logical_x);
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
#endif
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
    assert(board.w==168&&board.h==120);
    assert(board.y==frame.y);
    assert(board.x+board.w<=frame.x);
    assert(board.x>=8);
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
        assert(!zell_desktop_station_visible(&s,id));
        assert(zell_desktop_near(&s)!=id);
    }
    assert(sr_canvas_init(&canvas,ZELL_WORLD_W,ZELL_WORLD_H));
    zell_draw(&s,&canvas);
    assert((canvas.px[(board.y+100)*ZELL_WORLD_W+board.x+10]&0xffffffu)==0x101f26u);
    sr_canvas_free(&canvas);
    s.mode=ZELL_MODE_WALK;s.mouse_valid=true;s.mouse_click=true;
    s.mouse_x=board.x+4;s.mouse_y=board.y+4;
    zell_desktop_click(&s);
    assert(s.station!=ZELL_FLEET_BOARD&&!s.desktop_launch);
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
        for(int id=ZELL_WORKSHOP_COMPUTER;id<=ZELL_FLOOR_PANEL;id++){
            assert(zell_desktop_actions(id)==0);
            assert(zell_stations[id].art==-1);
            assert(strstr(zell_stations[id].role,"Reserved"));
            assert(strstr(zell_stations[id].role,"not an available action"));
            s.x=zell_stations[id].x*ZELL_WORLD_W;s.y=zell_stations[id].y*ZELL_WORLD_H;
            for(int r=0;r<=4;r++){
                s.room=r;
                assert(!zell_desktop_station_visible(&s,id));
                assert(zell_desktop_near(&s)!=id);
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
    s.room=1;s.mode=ZELL_MODE_WALK;s.mouse_valid=true;
    for(int screen=0;screen<5;screen++){
        zell_rect frame,display;zell_monitor_layout(screen,&frame,&display);
        assert(frame.w>=(screen?320:420)&&display.w>=249);
        assert(display.x>frame.x&&display.x+display.w<frame.x+frame.w);
        click_world(&s,(float)(display.x+display.w/2),(float)(display.y+display.h/2));
        assert(zell_desktop_click(&s)&&s.station==15+screen&&s.mode==ZELL_MODE_DESK);
        s.mode=ZELL_MODE_WALK;
    }
    s.room=0;s.mouse_click=true;assert(!zell_desktop_click(&s));
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
    assert(zell_desktop_launcher_count(&s)==12);
    assert(s.coding_yolo_refresh&&!s.coding_yolo_ready);
    assert(zell_desktop_launcher_station(&s,0)==15);
    assert(zell_desktop_launcher_station(&s,6)==29);
    s.x=zell_stations[28].x*ZELL_WORLD_W;s.y=zell_stations[28].y*ZELL_WORLD_H;
    zell_scene_tick(&s,&c);assert(s.room==0);
    s.x=zell_stations[14].x*ZELL_WORLD_W;s.y=zell_stations[14].y*ZELL_WORLD_H;
    zell_scene_tick(&s,&c);assert(s.room==2);
    s.disk_count=3;assert(zell_desktop_launcher_count(&s)==6);
    assert(zell_desktop_launcher_station(&s,3)==28);
    assert(!zell_desktop_station_visible(&s,23));
    assert(zell_desktop_station_visible(&s,34)&&zell_desktop_station_visible(&s,35));
    s.room=3;assert(zell_desktop_station_visible(&s,40)&&!zell_desktop_station_visible(&s,41));
    s.room=4;assert(zell_desktop_station_visible(&s,41)&&!zell_desktop_station_visible(&s,40));
    /* Stairs at the bottom of every downstairs room are walked onto. */
    for(int room=1;room<=4;room++){
        int hatch=room==1?13:room==2?14:room==3?42:43;
        zell_scene_init(&s,640,400);zell_desktop_init(&s);
        s.x=zell_stations[hatch].x*ZELL_WORLD_W;s.y=zell_stations[hatch].y*ZELL_WORLD_H;
        c=(zell_controls){.confirm=true};zell_scene_tick(&s,&c);
        assert(s.room==room&&!zell_desktop_on_stairs(&s));
        /* Away from the stairs the bottom edge still stops him. */
        s.x=ZELL_WORLD_W*.30f;c=(zell_controls){.move_y=1};
        for(int i=0;i<400;i++)zell_scene_tick(&s,&c);
        assert(s.room==room&&s.y<=ZELL_WORLD_H*.91f+1);
        s.x=ZELL_WORLD_W*.50f;s.y=ZELL_WORLD_H*.78f;
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
    s.room=1;assert(zell_desktop_station_in_front(&s,18)&&zell_desktop_station_in_front(&s,31));
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
    s.width=1536;s.height=1024;s.mouse_valid=true;
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
    s.hp=10;s.fatigue=100;assert(zell_desktop_speed_factor(&s)<.5f);
    s.hp=100;s.fatigue=0;assert(zell_desktop_speed_factor(&s)==.35f);
    s.hp=-1;s.fatigue=-1;assert(zell_desktop_speed_factor(&s)==1.0f);

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
        /* Draw order: a staff member (Vault Keeper) and a worker screen cover
         * Kilix when he stands behind them, and he covers them from in front.
         * Cutouts are a shade translucent, so hidden means within 3 per channel. */
        for(int pass=0;pass<2;pass++){
            int station=pass?18:40,top=pass?-140:-60,bottom=pass?-64:-36,behind=pass?-60:-30;
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
    }
#ifdef ZELL_RENDER_NEURONTE
    zell_neuronte_close();
#endif
    test_room_fit();
    test_fleet_board();
    test_contract_fixtures();
    test_obstacles();
    puts("desktop world, camera, alpha, idle/walk animation, launcher and voice: ok");
    return 0;
}
