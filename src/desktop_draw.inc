/* Included after the Neuronte drawing adapters, so artwork and text use them. */
#include "desktop.h"
#include "desktop_assets.h"
#include "command_term.h"
#include <time.h>
#include <stdlib.h>
#include <math.h>
#ifndef desktop_cross
static void desktop_cross(sr_canvas *c,int cx,int cy,int reach,uint32_t rgb){
 sr_line(c,(float)(cx-reach),(float)(cy-reach),(float)(cx+reach),(float)(cy+reach),2,rgb,1,0,0);
 sr_line(c,(float)(cx-reach),(float)(cy+reach),(float)(cx+reach),(float)(cy-reach),2,rgb,1,0,0);
}
#endif
static void desktop_art(sr_canvas *c,int id,int x,int foot,int height){
 const zell_art *a=zell_art_get(id);if(!a)return;sr_canvas src;sr_canvas_wrap(&src,a->pixels,a->width,a->height);
 int width=height*a->width/a->height;sr_blit_scaled(c,&src,x-width/2,foot-height,width,height,1.0f);
}
/* Draw one cutout mirrored left to right. The mirrored copy is built once. */
static void desktop_art_mirrored(sr_canvas *c,int id,int x,int foot,int height){
 static uint32_t *pixels;static int cached=-1;
 const zell_art *a=zell_art_get(id);if(!a)return;
 if(cached!=id){
  uint32_t *copy=realloc(pixels,(size_t)a->width*(size_t)a->height*sizeof *copy);if(!copy)return;
  pixels=copy;cached=id;
  for(int row=0;row<a->height;row++)for(int col=0;col<a->width;col++)
   pixels[row*a->width+col]=a->pixels[row*a->width+a->width-1-col];
 }
 sr_canvas src;sr_canvas_wrap(&src,pixels,a->width,a->height);
 int width=height*a->width/a->height;sr_blit_scaled(c,&src,x-width/2,foot-height,width,height,1.0f);
}
/* The stool sheet is the full room. Only its front rim and legs are opaque. */
static void desktop_stool_front(sr_canvas *c,int cx,int cy){
 const zell_art *a=zell_art_get(ZELL_ART_WORKSHOP_STOOL);if(!a)return;
 sr_canvas src;sr_canvas_wrap(&src,a->pixels,a->width,a->height);
 sr_blit_scaled(c,&src,-cx,-cy,a->width,a->height,1.f);
}
/* Full-canvas foreground. The asset compiler keeps the 1536x1024 anchor. */
static void desktop_registered_front(sr_canvas *c,int art,int cx,int cy){
 const zell_art *sheet=zell_art_get(art);if(!sheet)return;
 sr_canvas src;sr_canvas_wrap(&src,sheet->pixels,sheet->width,sheet->height);
 sr_blit_scaled(c,&src,-cx,-cy,sheet->width,sheet->height,1.f);
}
static int desktop_room_background(int room){
 if(room==1)return ZELL_ART_COMMAND_ROOM;
 if(room==2)return ZELL_ART_LAUNDRY_ROOM;
 if(room==3)return ZELL_ART_VAULT_ROOM;
 if(room==4)return ZELL_ART_WORKSHOP_ROOM;
 if(room==5)return ZELL_ART_GAME_ROOM;
 if(room==6)return ZELL_ART_THEATER_ROOM;
 if(room==7)return ZELL_ART_MEDITATION_ROOM;
 if(room==8)return ZELL_ART_LIBRARY_ROOM;
 return 0;
}
static int desktop_room_foreground(int room){
 if(room==5)return ZELL_ART_GAME_FOREGROUND;
 if(room==6)return ZELL_ART_THEATER_FOREGROUND;
 if(room==7)return ZELL_ART_MEDITATION_FOREGROUND;
 if(room==8)return ZELL_ART_LIBRARY_FOREGROUND;
 return -1;
}
static const char *desktop_room_title(int room){
 if(room==1)return "ZELL / COMMAND";
 if(room==2)return "ZELL / LAUNDRY";
 if(room==3)return "ZELL / VAULT";
 if(room==4)return "ZELL / WORKSHOP";
 if(room==5)return "ZELL / GAME";
 if(room==6)return "ZELL / THEATER";
 if(room==7)return "ZELL / MEDITATION";
 if(room==8)return "ZELL / LIBRARY";
 return "ZELL / " ZELL_VERSION;
}
static void desktop_label(sr_canvas *c,int x,int y,const char *text,uint32_t color){
 int w=sr_text_width_in(SR_FONT_FIXED_8X16,text,1);
 /* Keep labels for wall objects inside the view. */
 if(w+16<=c->w){if(x>c->w-w/2-8)x=c->w-w/2-8;if(x<w/2+8)x=w/2+8;}
 sr_fill_rect(c,(float)(x-w/2-6),(float)(y-3),(float)(w+12),22,0x101f26u,.92f);
 sr_text(c,(float)(x-w/2),(float)y,text,color,1,1);
}
static void desktop_text(sr_canvas *c,int x,int y,const char *text,uint32_t color,int width);
static void desktop_hatch(sr_canvas *c,int art,int x,int y,bool selected,const char *label){
 /* Each cutout is compiled at the height it is drawn at. */
 const zell_art *cut=zell_art_get(art);int height=cut?cut->height:76;
 if(selected)sr_fill_ellipse(c,(float)x,(float)(y+2),54,12,0x80e5c4u,.55f);
 desktop_art(c,art,x,y+ZELL_HATCH_FOOT_OFFSET,height);
 if(selected)desktop_label(c,x,y+28,label,0xa8f3d5u);
}
/* NeuroTerm owns VT state, colors and cell attributes. Cache the pixels at
 * the fitted terminal's resolution so even a small room has 8x16 text. */
static void cell_fill(void *canvas,int x,int y,int w,int h,uint32_t rgb){
 sr_fill_rect((sr_canvas *)canvas,(float)x,(float)y,(float)w,(float)h,rgb,1);
}
static void cell_glyph(void *canvas,uint32_t scalar,unsigned flags,int x,int y,int w,int h,uint32_t rgb){
 sr_canvas *c=canvas;
#ifdef ZELL_RENDER_NEURONTE
 if(!zell_neuronte_open())return;
 neuronte_draw_scalar(zn_text_font(scalar),c->px,c->w,c->w,c->h,x,y,w,h,scalar,zn_color(rgb,1));
 if(flags&NEUROTERM_BOLD)neuronte_draw_scalar(zn_text_font(scalar),c->px,c->w,c->w,c->h,x+1,y,w-1,h,scalar,zn_color(rgb,.66f));
#else
 (void)w;(void)h;
 char text[2]={(char)(scalar<128?scalar:'?'),0};
 sr_text(c,(float)x,(float)y,text,rgb,1,1);
 if(flags&NEUROTERM_BOLD)sr_text(c,(float)(x+1),(float)y,text,rgb,.66f,1);
#endif
}
enum { COMMAND_GLASS=0x090d12, FLOOR_GLASS=0x050809 };
static void desktop_cells(sr_canvas *c,int index,zell_rect body,bool focused){
 static struct {sr_canvas cells;unsigned long revision;bool focused;} cache[ZELL_ALL_TERMS];
 const zell_command_screen *screen=zell_command_term_screen(index);
 const neuroterm *t=&screen->terminal;
 if(!t->term||screen->message){
  desktop_text(c,body.x+4,body.y+4,screen->message?screen->message:"Opening shell...",0x97aab0u,body.w-8);
  return;
 }
 sr_canvas *cells=&cache[index].cells;
 int w=t->cols*ZELL_TERM_CELL_W,h=t->rows*ZELL_TERM_CELL_H;
 bool redraw=cache[index].revision!=screen->revision||cache[index].focused!=focused;
 if(cells->w!=w||cells->h!=h||!cells->px){
  sr_canvas_free(cells);
  if(!sr_canvas_init(cells,w,h))return;
  redraw=true;
 }
 if(redraw){
  sr_clear(cells,COMMAND_GLASS);
  VTermPos cursor=neuroterm_cursor(t);
  neuroterm_cell_style style={.foreground=NEUROTERM_FG,.background=COMMAND_GLASS,
      .focused=focused,.cursor_shape=t->cursor_shape};
  const neuroterm_painter painter={cells,cell_fill,cell_glyph};
  for(int row=0;row<t->rows;row++)for(int col=0;col<t->cols;){
   VTermScreenCell cell;
   if(!neuroterm_cell(t,col,row,&cell)){col++;continue;}
   int span=cell.width==2&&col+1<t->cols?2:1;
   style.cursor=t->cursor_visible&&row==cursor.row&&cursor.col>=col&&cursor.col<col+span;
   neuroterm_paint_cell(t->screen,&cell,col*ZELL_TERM_CELL_W,row*ZELL_TERM_CELL_H,
                        ZELL_TERM_CELL_W,ZELL_TERM_CELL_H,span,&style,&painter);
   col+=span;
  }
  cache[index].revision=screen->revision;cache[index].focused=focused;
 }
 sr_blit_scaled(c,cells,body.x,body.y,body.w,body.h,1);
}
static void screen_bezel(sr_canvas *c,zell_rect frame,const char *title,const char *action,bool focused){
 int x=frame.x,y=frame.y,w=frame.w;
 uint32_t accent=focused?0x9de6c9u:0x749c9fu;
 cell_fill(c,x,y,w,frame.h,0x44585cu);
 cell_fill(c,x+1,y+1,w-2,frame.h-2,COMMAND_GLASS);
 cell_fill(c,x+1,y+1,w-2,26,focused?0x20382fu:0x17252cu);
 cell_fill(c,x+1,y+27,w-2,1,0x32474bu);
 cell_fill(c,x+9,y+9,4,11,accent);
 desktop_text(c,x+21,y+6,title,0xe3eae7u,w/2);
 int label=(int)strlen(action)*8;
 desktop_text(c,x+w-label-12,y+6,action,accent,label);
}
static void desktop_monitor(const zell_scene *s,sr_canvas *c,int index,int cx,int cy){
 static const char *const titles[ZELL_COMMAND_TERMS]={"CONTROLLER","WORKER 1","WORKER 2","WORKER 3","WORKER 4","SHELL"};
 zell_rect frame,body;zell_command_term_layout(index,&frame,&body);
 frame.x-=cx;frame.y-=cy;body.x-=cx;body.y-=cy;
 bool focused=s->command_focus==index+1;
 screen_bezel(c,frame,titles[index],index==ZELL_COMMAND_SHELL?"New window":"Sessions",focused);
 desktop_cells(c,index,body,focused);
}
/* Four inset black-glass panels. Small chamfered corners and a light front
 * lip keep the metal frame legible against the stone floor. */
static void floor_bezel(sr_canvas *c,zell_rect f,zell_rect g,uint32_t edge){
 int x=f.x,y=f.y,w=f.w,h=f.h;
 cell_fill(c,x+5,y+6,w-5,h,0x171815u);
 cell_fill(c,x+3,y,w-6,h,0x514b3du);
 cell_fill(c,x,y+3,w,h-7,0x514b3du);
 cell_fill(c,x+6,y,w-12,2,0xab9977u);
 cell_fill(c,x+3,y+h-4,w-6,3,0x756750u);
 cell_fill(c,g.x-1,g.y-1,g.w+2,g.h+2,0x293333u);
 cell_fill(c,g.x,g.y,g.w,g.h,FLOOR_GLASS);
 cell_fill(c,g.x+12,g.y+10,4,13,edge);
 cell_fill(c,g.x+12,g.y+29,g.w-24,1,0x293333u);
}
static void desktop_floor_panels(const zell_scene *s,sr_canvas *c,int cx,int cy){
 if(s->room!=1)return;
 static const char *const titles[]={"YOLO","AGENTS","SPARE","FLEET BOARD"};
 for(int i=0;i<ZELL_FLOOR_PANELS;i++){
  zell_rect frame,g;zell_floor_panel_layout(i,&frame,&g);
  frame.x-=cx;frame.y-=cy;g.x-=cx;g.y-=cy;
  uint32_t ink=0x9ba6a6u;
  const char *detail="Inactive";
  char count[48];
  bool progress=false;
  if(i==0){
   detail=!s->coding_yolo_ready?"Reading":s->coding_yolo?"Enabled":"Disabled";
   ink=!s->coding_yolo_ready?0xa8ada5u:s->coding_yolo?0xf3ac65u:0x8cccb5u;
  }else if(i==1){
   detail="Idle";ink=0x94c9cfu;
   if(s->update_job_active){
    if(!strcmp(s->update_state,"running")){
     snprintf(count,sizeof count,"%d/%d",s->update_done,s->update_total);
     detail=count;progress=s->update_total>0;ink=0xdca6d5u;
    }else if(!strcmp(s->update_state,"done"))detail="Done";
    else if(!strcmp(s->update_state,"failed")){detail="Failed";ink=0xea9879u;}
    else if(!strcmp(s->update_state,"cancelled"))detail="Cancelled";
   }
  }else if(i==3){detail="No saved fleets";ink=0xd7be8eu;}
  floor_bezel(c,frame,g,ink);
  desktop_text(c,g.x+23,g.y+8,titles[i],0xe5e8dfu,g.w-34);
  if(i==3&&s->fleet_count){
   for(int row=0;row<s->fleet_count&&row<4;row++){
    const zell_fleet_row *fleet=&s->fleet_rows[row];
    char state[48];snprintf(state,sizeof state,"%d/%d live",fleet->live_count,fleet->worker_count);
    int right=(int)strlen(state)*8,yy=g.y+32+16*row;
    desktop_text(c,g.x+12,yy,fleet->display_name,0xd0d7d4u,g.w-right-32);
    desktop_text(c,g.x+g.w-12-right,yy,state,ink,right);
   }
  }else{
   desktop_text(c,g.x+13,g.y+41,detail,ink,g.w-26);
   if(progress){
    int bw=g.w-26;
    cell_fill(c,g.x+13,g.y+72,bw,4,0x2e3334u);
    int filled=bw*s->update_done/s->update_total;
    if(filled>0)cell_fill(c,g.x+13,g.y+72,filled>bw?bw:filled,4,ink);
   }
  }
 }
}
static void desktop_mana_rows(const zell_scene *s,sr_canvas *c,int x,int y,int w,int row,int scale){
 static const char *const names[3]={"CODEX","CLAUDE","GROK"};
 for(int i=0;i<3;i++){
  int value=s->provider_mana_ready?s->provider_mana[i]:-1,yy=y+i*row;
  uint32_t ink=value==-2?0xf0b35eu:value<0?0x7b8990u:value<10?0xe18c70u:value<30?0xd6b867u:0x6cc9b7u;
  char text[24];if(value==-2)snprintf(text,sizeof text,"ASK");else if(value<0)snprintf(text,sizeof text,"?");else snprintf(text,sizeof text,"%d%%",value);
  int name_w=56*scale,value_w=40*scale,bar=w-name_w-value_w-8;
  sr_text(c,(float)x,(float)yy,names[i],0xc9dcd8u,(float)scale,1);
  if(bar>8){
   sr_fill_rect(c,(float)(x+name_w),(float)(yy+3*scale),(float)bar,(float)(9*scale),0x13262eu,1);
   if(value>0)sr_fill_rect(c,(float)(x+name_w),(float)(yy+3*scale),(float)(bar*value)/100,(float)(9*scale),ink,1);
  }
  sr_text(c,(float)(x+w-value_w+4),(float)yy,text,ink,(float)scale,1);
 }
}
static void desktop_mana_console(const zell_scene *s,sr_canvas *c,int cx,int cy){
 zell_rect frame,body;zell_mana_console_layout(&frame,&body);
 frame.x-=cx;frame.y-=cy;body.x-=cx;body.y-=cy;
 screen_bezel(c,frame,"MANA","Usage",false);
 desktop_mana_rows(s,c,body.x+5,body.y+1,body.w-10,19,1);
}
/* The console screen grows to fill the view before its browser tab opens. */
static void desktop_zoom(const zell_scene *s,sr_canvas *c){
 zell_rect frame,display;zell_mana_console_layout(&frame,&display);
 float x0,y0,x1,y1,dw,dh,t,x,y,w,h;
 zell_world_to_screen(s,(float)display.x,(float)display.y,&x0,&y0);
 zell_world_to_screen(s,(float)(display.x+display.w),(float)(display.y+display.h),&x1,&y1);
 dw=x1-x0;dh=y1-y0;
 t=(float)s->zoom_ticks/(float)ZELL_ZOOM_TICKS;if(t>1)t=1;t=t*t*(3-2*t);
 x=x0*(1-t);y=y0*(1-t);
 w=dw+((float)c->w-dw)*t;h=dh+((float)c->h-dh)*t;
 sr_fill_rect(c,x,y,w,h,0x07141au,1);
 sr_stroke_rect(c,x,y,w,h,2,0x96ead0u,1);
 int scale=t>.6f&&w>520?2:1;
 if(w>220&&h>120){
  sr_text(c,x+18,y+14,"MANA CONSOLE",0xe9d49fu,(float)scale,1);
  desktop_mana_rows(s,c,(int)x+18,(int)y+14+26*scale,(int)w-36,22*scale,scale);
  static const char *const pages[4]={"usage dashboard","Codex usage page","Claude usage page","Grok usage page"};
  char line[80];snprintf(line,sizeof line,"> opening the %s in a browser tab...",pages[s->menu_index>=0&&s->menu_index<4?s->menu_index:0]);
  sr_text(c,x+18,y+h-30*(float)scale,line,0x85f7b2u,(float)scale,1);
 }
}
static void desktop_room(const zell_scene *s,sr_canvas *c,int cx,int cy,bool front){
 int near=zell_desktop_near(s);
 /* Floor pass, before Kilix and before anything that stands over him. */
 if(!front)desktop_floor_panels(s,c,cx,cy);
 for(int i=15;i<ZELL_STATIONS;i++){
  if(!zell_desktop_station_visible(s,i)||zell_desktop_station_in_front(s,i)!=front)continue;
  if(i==ZELL_COMMAND_COMPUTER){if(!front)desktop_monitor(s,c,ZELL_COMMAND_SHELL,cx,cy);continue;}
  if(i>=ZELL_WORKSHOP_COMPUTER)continue;
  int x=(int)(zell_stations[i].x*ZELL_WORLD_W)-cx,y=(int)(zell_stations[i].y*ZELL_WORLD_H)-cy;
  if(i==ZELL_MANA_CONSOLE){desktop_mana_console(s,c,cx,cy);continue;}
  if(i>=46)continue;
  if(i==28)continue;
  if(i>=29){
   zell_actor_pose staff=i<32?zell_desktop_pose(s,i-23):(zell_actor_pose){zell_stations[i].art+(s->flame_tick/20+i)%4,0,0};
   /* Staff are shown as the cave's first six are: a glow and a name only
    * while Kilix stands by them. */
   if(i==near)sr_fill_ellipse(c,(float)x,(float)y,128*.42f,6,0x80e5c4u,.65f);
   sr_fill_ellipse(c,(float)x,(float)y,24,7,0x071719u,.45f);
   desktop_art(c,staff.art,x+staff.offset_x,y+staff.offset_y,128);
   /* The Key Master's lines go in the strip under the AGENTS panel, right
    * aligned (see ZELL_KEY_MASTER_LABEL_RIGHT): above his head they would
    * cover that panel, and under his feet the status bar. */
   if(i==31){
    if(i==near){
     int right=ZELL_KEY_MASTER_LABEL_RIGHT-cx,top=ZELL_KEY_MASTER_LABEL_TOP-cy;
     int name_w=sr_text_width_in(SR_FONT_FIXED_8X16,zell_stations[i].name,1),hint_w=sr_text_width_in(SR_FONT_FIXED_8X16,ZELL_KEY_MASTER_HINT,1);
     desktop_label(c,right-6-name_w/2,top+3,zell_stations[i].name,0xb9f8dbu);
     desktop_label(c,right-6-hint_w/2,top+28,ZELL_KEY_MASTER_HINT,0xa8f3d5u);
    }
    continue;
   }
   if(i==near&&(i==29||i==30)){
    desktop_label(c,x,y-161,zell_stations[i].name,0xb9f8dbu);
    desktop_label(c,x,y-138,i==29?"Enter: projects and Git":"Enter: coding fleets",0xa8f3d5u);
    continue;
   }
   if(i==near)desktop_label(c,x,y+15,zell_stations[i].name,0xb9f8dbu);
   if(i==near)desktop_label(c,x,y+40,i==29?"Enter: projects and Git":i==30?"Enter: coding fleets":"Enter: tasks",0xa8f3d5u);
   continue;
  }
  if(s->room==1){desktop_monitor(s,c,i-15,cx,cy);continue;}
  int bin=i-20,percent=s->disk_used[bin];
  uint32_t color=percent<0?0x7b8990u:percent>=90?0xe18c70u:percent>=75?0xd6b867u:0x6cc9b7u;
  desktop_art(c,ZELL_ART_BIN,x,y,164);
  /* The generated bin's dark front window remains a live capacity gauge. */
  int width=70,top=y-94,height=28;
  sr_fill_rect(c,(float)(x-width/2),(float)top,(float)width,(float)height,0x071318u,1);
  if(percent>0)sr_fill_rect(c,(float)(x-width/2),(float)top,(float)(width*percent)/100,(float)height,color,.7f);
  char value[24];if(percent<0)snprintf(value,sizeof value,"?");else snprintf(value,sizeof value,"%d%%",percent);
  sr_text(c,(float)(x-sr_text_width_in(SR_FONT_FIXED_8X16,value,1)/2),(float)(top+6),value,0xf3eee0u,1,1);
  if(i==near)sr_stroke_rect(c,(float)(x-70),(float)(y-164),140,166,2,0x96ead0u,1);
  char label[28];snprintf(label,sizeof label,"%.27s",s->disk_labels[bin]);desktop_label(c,x,y+16,label,color);
  if(i==near)desktop_label(c,x,y+40,"Enter: dig through drive",0xa8f3d5u);
 }
 if(!front&&s->room==2&&s->disk_count==0)desktop_label(c,ZELL_WORLD_W/2-cx,ZELL_WORLD_H/2-cy,s->disks_ready?"No eligible mounted disks found":"Reading mounted drives...",0xc9d8d2u);
}
static void desktop_vitals(const zell_scene *s,sr_canvas *c){
 int span=c->w-24;if(span>420)span=420;int width=(span-16)/3;
 if(width<32)return;
 int values[3]={s->hp,s->mana,s->stamina};const char *names[3]={"HEALTH","MANA","STAMINA"};
 uint32_t colors[3]={0xde8a79u,0x8ea6efu,0x87c5a4u};
 for(int i=0;i<3;i++){
  int x=12+i*(width+8);char label[40];
  if(values[i]<0)snprintf(label,sizeof label,"%s ?",names[i]);else snprintf(label,sizeof label,"%s %d%s",width<110?(i==0?"HP":i==1?"MP":"FT"):names[i],values[i],i==1&&s->mana_age>=3600?"*":"%");
  sr_fill_rect(c,(float)x,ZELL_ROOM_VITALS_Y,(float)width,ZELL_ROOM_VITALS_H,0x0c1921u,.95f);
  sr_text(c,(float)(x+4),ZELL_ROOM_VITALS_Y+3,label,colors[i],1,1);
  sr_fill_rect(c,(float)(x+4),ZELL_ROOM_VITALS_Y+25,(float)(width-8),7,0x263940u,1);
  if(values[i]>0)sr_fill_rect(c,(float)(x+4),ZELL_ROOM_VITALS_Y+25,(float)(width-8)*(float)values[i]/100,7,colors[i],1);
 }
 if(s->hp>=0&&s->hp<100&&s->room==0&&s->mode==ZELL_MODE_WALK)sr_text(c,12,83,s->restart_needed?"Restart needed: visit Gatekeeper":s->dirty_files>0?"Health low: hover bar for details":"Admin: OS update due",0xe5ad87u,1,1);
 if(s->mouse_valid&&s->mouse_x>=12&&s->mouse_x<12+width&&s->mouse_y>=ZELL_ROOM_VITALS_Y&&s->mouse_y<ZELL_ROOM_HUD_BOTTOM){
  char tip[600];
  char os[240],git[200];
  if(s->update_age_days<0)snprintf(os,sizeof os,"OS upgrade history is unknown; no age penalty is assumed.");
  else if(s->os_health>=100)snprintf(os,sizeof os,"OS health is full: upgraded less than 24 hours ago.");
  else snprintf(os,sizeof os,"OS upgrade was %d full days ago: OS health %d%%. Age costs 10 points/day from day two, floor 10%%.",s->update_age_days,s->os_health);
  if(s->dirty_files>0)snprintf(git,sizeof git,"%d dirty files in watched Git projects: minus %d points (combined floor 1%%). Visit Git Wizard downstairs.",s->dirty_files,s->dirty_files);
  else if(s->dirty_files<0)snprintf(git,sizeof git,"Some watched repositories are unavailable. Open Git Wizard for errors and known penalties.");
  else snprintf(git,sizeof git,"No known dirty-file penalty. Register watches with Git Wizard in Command Center.");
  snprintf(tip,sizeof tip,"%s %s %s Movement speed: %.0f%%.",os,git,s->restart_needed?"Restart needed: Gatekeeper shows installed kernel/driver changes.":"Administrator > Update OS restores OS health.",100*zell_desktop_speed_factor(s));
  int tw=c->w-32;if(tw>430)tw=430;int lines=wrap_count(tip,tw-24),height=lines*18+24;
  sr_fill_rect(c,12,105,(float)tw,(float)height,0x10232cu,.98f);sr_stroke_rect(c,12,105,(float)tw,(float)height,1,0xd6ba87u,1);
  draw_wrapped(c,24,117,tw-24,tip,0xf0e5cdu,0,lines);
 }
}
static void desktop_weather(const zell_scene *s,sr_canvas *c,int cx,int cy){
 if(s->room!=0||!s->weather_state)return;
 int x=157-cx,y=244-cy;
 int art=s->weather_icon>=0&&s->weather_icon<8?s->weather_icon:7;
 sr_fill_ellipse(c,(float)x,(float)(y-40),54,39,0x133a43u,.35f);
 desktop_art(c,ZELL_ART_WEATHER+art,x,y,s->weather_state==1?88:64);
 char label[96];
 if(s->weather_state==1)snprintf(label,sizeof label,"%s / %s",s->weather_temperature,s->weather_condition);
 else snprintf(label,sizeof label,"%s",s->weather_condition);
 desktop_label(c,x,y+6,label,0xffe7a6u);
}
static void desktop_cave(const zell_scene *s,sr_canvas *c,int cx,int cy,bool front){
 int near=zell_desktop_near(s);int height=112;
 for(int i=0;i<ZELL_STATIONS&&s->room==0;i++){
  if(!zell_desktop_station_visible(s,i)||zell_desktop_station_in_front(s,i)!=front)continue;
  const zell_station *p=&zell_stations[i];int x=(int)(p->x*(float)ZELL_WORLD_W)-cx,y=(int)(p->y*(float)ZELL_WORLD_H)-cy;
  int room=zell_hatch_room(i);
  if(room){
   const zell_cave_hatch *hatch=&zell_cave_hatches[room];
   desktop_hatch(c,i==near?hatch->open_art:p->art,x,y,i==near,hatch->label);
   continue;
  }
  if(i>=ZELL_WORKSHOP_COMPUTER)continue;
  int h=(i<6||(i>=32&&i<42))?height:height*3/4;
  if(i==12){x+=ZELL_SWITCH_WALL_DX;y+=ZELL_SWITCH_WALL_DY;}
  /* A floor glow under a wall fitting would hang in the air; its label marks it. */
  if(i==near&&i!=12){sr_fill_ellipse(c,(float)x,(float)y, (float)h*.42f,6,0x80e5c4u,.65f);}
  if(i<6){zell_actor_pose pose=zell_desktop_pose(s,i);sr_fill_ellipse(c,(float)x,(float)y,23,7,0x071719u,.45f);desktop_art(c,pose.art,x+pose.offset_x,y+pose.offset_y,h);}
  /* The later staff stand as the first six do: a shadow, and no name until
   * Kilix is beside them, when the last line here gives it. */
  if(i>=32&&i<42){sr_fill_ellipse(c,(float)x,(float)y,23,7,0x071719u,.45f);desktop_art(c,p->art+(s->flame_tick/20+i)%4,x,y,h);}
  /* The lever hangs on the east wall beside the shell desk, mirrored to face
   * the room. Its interaction point stays on the reachable floor edge. */
  if(i==12){desktop_art_mirrored(c,ZELL_ART_SWITCH,x,y+22,62);if(i!=near)desktop_label(c,x,y+28,"N-SHELL",0x91e9d8u);}
  if(i==6||i==7)desktop_label(c,x,y+8,i==6?"TUI":"SHELL",0x91e9d8u);
  if(i==near)desktop_label(c,x,y+(i==12?28:8),p->name,0xb9f8dbu);
 }
}
static int desktop_view_identity(const zell_scene *s){
 float x,y;
 if(s->room_view.scale!=1.f)return 0;
 zell_world_to_screen(s,0.f,0.f,&x,&y);
 return x==0.f&&y==0.f;
}
/* Copy one world-pixel room through the fitted view. HUD is drawn later. */
static void desktop_present_room(const zell_scene *s,sr_canvas *dst,const sr_canvas *world){
 sr_clear(dst,0x081319u);
 if(!world->px)return;
 if(desktop_view_identity(s)&&dst->w==world->w&&dst->h==world->h){
  memcpy(dst->px,world->px,(size_t)dst->w*(size_t)dst->h*4u);
  return;
 }
 for(int sy=0;sy<dst->h;sy++)for(int sx=0;sx<dst->w;sx++){
  float wx,wy;int ix,iy;
  zell_screen_to_world(s,(float)sx,(float)sy,&wx,&wy);
  ix=(int)floorf(wx);iy=(int)floorf(wy);
  if(ix<0||iy<0||ix>=world->w||iy>=world->h)continue;
  dst->px[(size_t)sy*(size_t)dst->w+(size_t)sx]=world->px[(size_t)iy*(size_t)world->w+(size_t)ix];
 }
}
/* Right side of the bottom strip. Hints stay on the left. Elapsed time, not frames. */
static void zell_notice_show(const zell_scene *s,sr_canvas *c,const char *hint){
 char line[160];
 int hint_w,left,right,room,width;
 size_t len;
 if(!s->notice_text[0]||s->notice_until_ms<=zell_notice_clock_ms())return;
 hint_w=hint&&hint[0]?sr_text_width_in(SR_FONT_FIXED_8X16,hint,1):0;
 if(hint_w<0)hint_w=0;
 left=12+hint_w+16;
 right=c->w-12;
 if(right-left<8)return;
 room=right-left;
 len=strlen(s->notice_text);
 if(len>159)len=159;
 memcpy(line,s->notice_text,len);
 line[len]=0;
 while(len>0&&sr_text_width_in(SR_FONT_FIXED_8X16,line,1)>room){
  len--;
  line[len]=0;
  if(len>=3)memcpy(line+len-3,"...",3);
 }
 if(!line[0])return;
 width=sr_text_width_in(SR_FONT_FIXED_8X16,line,1);
 if(width<1||width>room)return;
 sr_text(c,(float)(right-width),(float)(c->h-22),line,0xe9d49fu,1,1);
}
/* Stretch only the straight rails of the original controller bezel. The
 * corner plates and the turquoise crest keep their shape at wall scale. */
static void desktop_command_surround(sr_canvas *background){
 const zell_art *art=zell_art_get(ZELL_ART_MONITOR_CONTROLLER);
 if(!art)return;
 zell_rect wall=zell_command_wall_bounds(),upper,lower,body;
 zell_command_term_layout(1,&upper,&body);
 zell_command_term_layout(3,&lower,&body);
 int mid=wall.x+wall.w/2,right=wall.x+wall.w,bottom=wall.y+wall.h;
 const int sx[]={46,128,302,466,640,723},sy[]={260,325,692,737};
 const int dx[]={wall.x,wall.x+36,mid-36,mid+36,right-36,right};
 const int dy[]={wall.y,upper.y,lower.y+lower.h,bottom};
 for(int row=0;row<3;row++)for(int col=0;col<5;col++){
  if(row==1&&col>0&&col<4)continue;
  sr_canvas strip;
  if(!sr_canvas_init(&strip,sx[col+1]-sx[col],sy[row+1]-sy[row]))return;
  for(int y=0;y<strip.h;y++)
   memcpy(strip.px+(size_t)y*(size_t)strip.w,art->pixels+(size_t)(sy[row]+y)*(size_t)art->width+(size_t)sx[col],(size_t)strip.w*sizeof *strip.px);
  sr_blit_scaled(background,&strip,dx[col],dy[row],dx[col+1]-dx[col],dy[row+1]-dy[row],1);
  sr_canvas_free(&strip);
 }
}
static void desktop_world_layer(const zell_scene *s,sr_canvas *c,int cx,int cy){
 static sr_canvas backgrounds[9];
 int room=s->room>=0&&s->room<9?s->room:0;
 sr_canvas *background=&backgrounds[room];
 const zell_art *a=zell_art_get(desktop_room_background(room));
 if(!background->px&&a&&sr_canvas_init(background,ZELL_WORLD_W,ZELL_WORLD_H)){
  sr_canvas src;sr_canvas_wrap(&src,a->pixels,a->width,a->height);
  sr_blit_scaled(background,&src,0,0,ZELL_WORLD_W,ZELL_WORLD_H,1);
  if(room==1)desktop_command_surround(background);
 }
 sr_clear(c,0x081319u);
 if(background->px){for(int y=0;y<c->h&&y+cy<ZELL_WORLD_H;y++){int w=c->w;if(w+cx>ZELL_WORLD_W)w=ZELL_WORLD_W-cx;memcpy(c->px+(size_t)y*(size_t)c->w,background->px+(size_t)(y+cy)*ZELL_WORLD_W+(size_t)cx,(size_t)w*4);}}
 if(s->room)desktop_room(s,c,cx,cy,false);
 desktop_weather(s,c,cx,cy);
 int height=112;
 desktop_cave(s,c,cx,cy,false);
 if(s->room==0){
  bool enabled[3]={s->voice_on,s->dictation_on,!s->music_muted};
  const char *names[3]={"VOICE","DICTATE","MUSIC"};
  for(int i=0;i<3;i++){
   int x=98+21*i-cx,y=349-cy;
   float mwx,mwy;int mx=s->mouse_x,my=s->mouse_y;
   if(s->mouse_valid){zell_screen_to_world(s,(float)s->mouse_x,(float)s->mouse_y,&mwx,&mwy);mx=(int)floorf(mwx)-cx;my=(int)floorf(mwy)-cy;}
   sr_fill_circle(c,(float)x,(float)y,5,enabled[i]?0x85f7b2u:0x775446u,1);
   if(s->mouse_valid&&mx>=x-10&&mx<x+11&&my>=298-cy&&my<375-cy){
    char label[64];snprintf(label,sizeof label,"%s / %s",names[i],i==2?(s->music_muted?"PAUSED":s->dj_active?"DJ":"ON"):(enabled[i]?"ON":"OFF"));desktop_label(c,120-cx,386-cy,label,0xe9d49fu);
   }
  }
 }
 if(s->mode==ZELL_MODE_WALK&&s->running){
  float px=s->x-(float)cx,py=s->y-(float)cy;
   float dx=s->direction==2?-1.0f:s->direction==3?1.0f:0;
   float dy=s->direction==1?-1.0f:s->direction==0?1.0f:0;
   for(int dust=0;dust<3;dust++){
    float age=(float)((s->flame_tick+4*dust)%12),back=8+age*2;
    sr_fill_ellipse(c,px-dx*back+(dy!=0?(float)(dust-1)*5:0),py-dy*back-2,2+age*.3f,1+age*.15f,0xd6c5a2u,(12-age)/24);
   }
 }
 zell_actor_pose pose=zell_desktop_pose(s,-1);
 int seat_dy=zell_seat_draw_dy(s);
 float shadow=1.f;
 if(s->seat_ticks>0&&s->seat_ticks<ZELL_SEAT_HOP_TICKS){
  int lift=zell_seat_lift(s->seat_ticks);
  if(lift>0)shadow=1.f-0.55f*(float)lift/(float)ZELL_SEAT_LIFT_PEAK;
 }
 /* The shadow stays on the floor line and shrinks as the hop lifts him. */
 sr_fill_ellipse(c,s->x-(float)cx,s->y-(float)cy,17.f*shadow,5.f*shadow,0x071719u,.45f);
 desktop_art(c,pose.art,(int)s->x-cx+pose.offset_x,(int)s->y-cy+pose.offset_y+seat_dy,height*3/4);
 if(s->room==4&&s->seated&&s->seat_ticks==ZELL_SEAT_HOP_TICKS)desktop_stool_front(c,cx,cy);
 if(desktop_room_foreground(s->room)>=0)desktop_registered_front(c,desktop_room_foreground(s->room),cx,cy);
 if(s->room)desktop_room(s,c,cx,cy,true);else desktop_cave(s,c,cx,cy,true);
 if(s->debug_blocks[0].enabled){
  int blocks=zell_desktop_block_count(s);
  for(int i=0;i<blocks;i++){
   float x0,y0,x1,y1;
   zell_desktop_block_rect(s,i,&x0,&y0,&x1,&y1);
   sr_stroke_rect(c,x0-(float)cx,y0-(float)cy,x1-x0,y1-y0,2.f,0xff3040u,1.f);
  }
 }
}
static void desktop_chrome(const zell_scene *s,sr_canvas *c,int cx,int cy){
 int near=zell_desktop_near(s);
 (void)cx;(void)cy;
 /* The wall screens and theater use the full space below the vitals. */
 if(c->w>=500&&s->mode==ZELL_MODE_WALK&&s->room!=6&&s->room!=1){
  int mx=c->w-140,my=42;sr_fill_rect(c,(float)mx,(float)my,128,88,0x0c1921u,.9f);
  sr_stroke_rect(c,(float)mx,(float)my,128,88,1,0x4b7777u,1);
  for(int i=0;i<ZELL_STATIONS;i++)if((i<ZELL_WORKSHOP_COMPUTER||(i>=ZELL_GAME_HATCH&&i<=ZELL_LIBRARY_HATCH)||i==ZELL_COMMAND_COMPUTER)&&zell_desktop_station_visible(s,i))sr_fill_circle(c,(float)(mx+4)+zell_stations[i].x*120,(float)(my+4)+zell_stations[i].y*80,2,0xd9ba79u,1);
  {
   float x0,y0,x1,y1;
   zell_screen_to_world(s,0.f,0.f,&x0,&y0);
   zell_screen_to_world(s,(float)c->w,(float)c->h,&x1,&y1);
   sr_stroke_rect(c,(float)(mx+4)+(x0/(float)ZELL_WORLD_W)*120,(float)(my+4)+(y0/(float)ZELL_WORLD_H)*80,((x1-x0)/(float)ZELL_WORLD_W)*120,((y1-y0)/(float)ZELL_WORLD_H)*80,1,0x407b85u,1);
  }
  sr_fill_circle(c,(float)(mx+4)+s->x/ZELL_WORLD_W*120,(float)(my+4)+s->y/ZELL_WORLD_H*80,3,0x8af8d4u,1);
 }
 sr_fill_rect(c,0,0,(float)c->w,30,0x0c1921u,.97f);
 sr_text(c,12,7,desktop_room_title(s->room),0xf0d69bu,1,1);
 if(s->dictation_on)desktop_label(c,c->w/2,82,"Listening / click dictation to finish",0xb7f3d5u);
 char status[100];time_t now=time(NULL);struct tm local;localtime_r(&now,&local);
 snprintf(status,sizeof status,"%02d:%02d  %d fps",local.tm_hour,local.tm_min,s->frame_rate);
 int sw=sr_text_width_in(SR_FONT_FIXED_8X16,status,1);sr_text(c,(float)(c->w-sw-12),7,status,0xa6d9ccu,1,1);
 sr_fill_rect(c,0,(float)(c->h-ZELL_ROOM_STATUS_STRIP),(float)c->w,ZELL_ROOM_STATUS_STRIP,0x0c1921u,.97f);
 const char *hint=s->mode==ZELL_MODE_WALK&&s->running?"Running | Release Shift to walk":s->mode==ZELL_MODE_SERVICE?"Station window | Esc back":near>=0?"Enter interact | Tab stations":c->w<500?"WASD | Shift run | Tab":"WASD walk | Hold Shift run | Tab stations";
 if(s->room==1&&s->mode==ZELL_MODE_WALK){
  if(s->command_focus)hint="F6 next shell | Alt+Esc walk";
  else if(near<0)hint="Click a shell or F6 | WASD walk | Tab stations";
 }
 sr_text(c,12,(float)(c->h-22),hint,0xc7d8dau,1,1);
 /* L8 calls zell_notice_show. */
 zell_notice_show(s,c,hint);
 desktop_vitals(s,c);
}
static void desktop_panes_button(sr_canvas *c,zell_rect r,const char *label,uint32_t fill,uint32_t edge,uint32_t ink){
 sr_fill_rect(c,(float)r.x,(float)r.y,(float)r.w,(float)r.h,fill,1);
 sr_stroke_rect(c,(float)r.x,(float)r.y,(float)r.w,(float)r.h,1,edge,1);
 int tw=sr_text_width_in(SR_FONT_FIXED_8X16,label,1);
 sr_text(c,(float)(r.x+(r.w-tw)/2),(float)(r.y+(r.h-16)/2),label,ink,1,1);
}
/* The Hotkeys window, over the panes below the walkway: the keys in a
 * column, what they do beside them, each cut to fit; on a Zell too short
 * for them all, the lines from its top and a line saying there are more. */
static void desktop_panes_hotkeys(sr_canvas *c){
 int line_h=ZELL_PANES_HOTKEYS_LINE_H,keys_w=27*8,w=c->w-32<820?c->w-32:820;
 int fit=zell_panes_hotkeys_fit(c->h),top=zell_panes_hotkeys_top(c->h),more=fit<zell_panes_hotkeys_lines;
 int h=(fit+more)*line_h+ZELL_PANES_HOTKEYS_EDGES;
 if(w<120||h>c->h-ZELL_PANES_WALK_H-8)return;
 int x=(c->w-w)/2,y=ZELL_PANES_WALK_H+8;
 sr_fill_rect(c,0,(float)ZELL_PANES_WALK_H,(float)c->w,(float)(c->h-ZELL_PANES_WALK_H),0x05090cu,.55f);
 sr_fill_rect(c,(float)x,(float)y,(float)w,(float)h,0x101b24u,.97f);
 sr_stroke_rect(c,(float)x,(float)y,(float)w,(float)h,2,0xd9ba79u,1);
 desktop_text(c,x+14,y+8,"Hotkeys",0xd9ba79u,w-28);
 desktop_text(c,x+w-14-15*8,y+8,"Esc closes",0x9fb4c0u,15*8);
 int row=y+34;
 if(more){
  char note[96];
  snprintf(note,sizeof note,"Up / Down or the wheel for more: lines %d-%d of %d",top+1,top+fit,zell_panes_hotkeys_lines);
  desktop_text(c,x+14,y+34+fit*line_h,note,0xd9ba79u,w-28);
 }
 for(int i=top;i<top+fit&&i<zell_panes_hotkeys_lines;i++,row+=line_h){
  const char *text=zell_panes_hotkeys_text[i],*bar=strchr(text,'|');
  if(text[0]=='#'){desktop_text(c,x+14,row,text+1,0x9ff0c8u,w-28);continue;}
  if(!bar){desktop_text(c,x+14,row,text,0x9fb4c0u,w-28);continue;}
  char keys[64];size_t n=(size_t)(bar-text)<sizeof keys-1?(size_t)(bar-text):sizeof keys-1;
  memcpy(keys,text,n);keys[n]=0;
  if(w>keys_w+120){
   desktop_text(c,x+22,row,keys,0xffffffu,keys_w-12);
   desktop_text(c,x+22+keys_w,row,bar+1,0xd6e4ecu,w-36-keys_w);
  }else desktop_text(c,x+14,row,n?keys:bar+1,n?0xffffffu:0xd6e4ecu,w-28);
 }
}
/* A pane room is the walkway, the panes' title bars and their cells, edge
 * to edge: no HUD, no status strip, no border. The thin green line at the
 * walkway's end is the way back. */
static void desktop_panes_room(const zell_scene *s,sr_canvas *c){
 int w=0,h=0,count=zell_panes_count(s->room),focus=zell_panes_focus(s->room),zoom=zell_panes_zoom(s->room);
 bool typing=zell_panes_keys_to_pane();
 const uint32_t *pixels=zell_panes_frame(s->room,&w,&h);
 sr_clear(c,0x0b1218u);
 /* The walkway: a stone back wall with the buttons on it, then planks. Its
  * edge is lit while Kilix has the keys. */
 sr_fill_rect(c,0,0,(float)c->w,34,0x16222bu,1);
 for(int x=0;x<c->w;x+=48)sr_fill_rect(c,(float)x,(float)((x/48)%2?0:17),1,17,0x0f181eu,1);
 sr_fill_rect(c,0,17,(float)c->w,1,0x0f181eu,1);
 sr_fill_rect(c,0,34,(float)c->w,(float)(ZELL_PANES_WALK_H-36),0x26343cu,1);
 for(int x=0;x<c->w;x+=32)sr_fill_rect(c,(float)x,34,1,(float)(ZELL_PANES_WALK_H-36),0x1b272eu,1);
 sr_fill_rect(c,0,(float)(ZELL_PANES_WALK_H-2),(float)c->w,2,typing?0x2a3a44u:0xd9ba79u,1);
 /* What Enter would press where Kilix stands is lit, and only that. */
 int aim_index,aim_which,aim=zell_panes_aim(s,&aim_index,&aim_which);
 for(int index=0;index<count;index++){
  zell_rect bar;zell_panes_chrome_rect(s,index,&bar);
  if(!bar.w)continue;
  for(int agent=0;agent<zell_panes_agent_count();agent++){
   zell_rect b;zell_panes_button_rect(s,index,agent,&b);
   if(!b.w)break;
   bool under=aim==ZELL_PANES_AIM_AGENT&&aim_index==index&&aim_which==agent;
   desktop_panes_button(c,b,zell_panes_agent_label(agent),under?0x2f5a4cu:0x1f3444u,under?0x9ff0c8u:0x5f8faeu,0xe8f2f6u);
  }
  zell_rect pad;zell_panes_pad_rect(s,index,&pad);
  if(pad.w){
   bool under=aim==ZELL_PANES_AIM_PAD&&aim_index==index;
   desktop_panes_button(c,pad,"Type",under?0x5a4a24u:0x1b2a33u,under?0xd9ba79u:0x4f6a7au,under?0xffffffu:0xb9c8d0u);
  }
  /* The title bar: brightest on the pane that has the keys; edged in gold
   * on the pane Enter would give them to. */
  bool focused=index==focus,keys=focused&&typing;
  sr_fill_rect(c,(float)bar.x,(float)bar.y,(float)bar.w,(float)bar.h,keys?0x2f5878u:focused?0x23394bu:0x18242eu,1);
  if(keys)sr_fill_rect(c,(float)bar.x,(float)bar.y,3,(float)bar.h,0xd9ba79u,1);
  if((aim==ZELL_PANES_AIM_PANE||aim==ZELL_PANES_AIM_PAD)&&aim_index==index)
   sr_fill_rect(c,(float)bar.x,(float)bar.y,(float)bar.w,2,0xd9ba79u,1);
  zell_rect zoom_b,restart_b,end_b;
  zell_panes_chrome_button(s,index,ZELL_PANES_ZOOM,&zoom_b);zell_panes_chrome_button(s,index,ZELL_PANES_RESTART,&restart_b);
  zell_panes_chrome_button(s,index,ZELL_PANES_END,&end_b);
  char text[160];const char *notice=zell_panes_notice(s->room,index),*title=zell_panes_title(s->room,index);
  unsigned long long back=(unsigned long long)zell_panes_lines_back(s->room,index);
  if(notice)snprintf(text,sizeof text,"%d %s",index+1,notice);
  else if(back)snprintf(text,sizeof text,"%d %s  %llu %s back",index+1,title[0]?title:"shell",back,back==1?"line":"lines");
  else snprintf(text,sizeof text,"%d %s",index+1,title[0]?title:"shell");
  int room_for=(end_b.w?end_b.x:zoom_b.w?zoom_b.x:bar.x+bar.w)-bar.x-16;
  desktop_text(c,bar.x+8,bar.y+2,text,notice?0xe5ad87u:keys?0xffffffu:0x9fb4c0u,room_for);
  bool aimed=aim==ZELL_PANES_AIM_CHROME&&aim_index==index;
  if(end_b.w){
   bool under=aimed&&aim_which==ZELL_PANES_END;
   desktop_panes_button(c,end_b,"End",under?0x3f7a64u:0x2f5a4cu,under?0xffffffu:0x9ff0c8u,0xe8f2f6u);
  }
  if(zoom_b.w){
   bool under=aimed&&aim_which==ZELL_PANES_ZOOM;
   desktop_panes_button(c,zoom_b,zoom==index?"Unzoom":"Zoom",under?0x2f5a4cu:0x1f3444u,under?0x9ff0c8u:0x5f8faeu,0xd6e4ec);
  }
  if(restart_b.w){
   bool sure=zell_panes_confirming(s->room,index),under=aimed&&aim_which==ZELL_PANES_RESTART;
   desktop_panes_button(c,restart_b,sure?"Sure?":"Restart",sure?0x6a2c2cu:under?0x2f5a4cu:0x1f3444u,sure?0xe5ad87u:under?0x9ff0c8u:0x5f8faeu,0xd6e4ec);
  }
 }
 if(pixels){
  sr_canvas frame;sr_canvas_wrap(&frame,(uint32_t *)pixels,w,h);
  sr_blit_scaled(c,&frame,0,ZELL_PANES_CELLS_Y,w,h,1);
  /* Over the cells, a pane's scrollbar while it is scrolled back or the
   * pointer is on it. */
  for(int index=0;index<count;index++){
   zell_rect track,thumb;zell_panes_scrollbar_rect(s,index,&track,&thumb);
   bool hover=zell_panes_bar_hover()==index;
   if(!track.w||(!hover&&!zell_panes_lines_back(s->room,index)))continue;
   sr_fill_rect(c,(float)track.x,(float)track.y,(float)track.w,(float)track.h,0x0b1218u,.75f);
   sr_fill_rect(c,(float)thumb.x,(float)thumb.y,(float)thumb.w,(float)thumb.h,hover?0x9ff0c8u:0x5f8faeu,1);
  }
 }else{
  char text[200];const char *error=zell_panes_error();
  if(error[0])snprintf(text,sizeof text,"%s",error);
  else snprintf(text,sizeof text,"Opening %d panes...",count);
  desktop_label(c,c->w/2,ZELL_PANES_CELLS_Y+(c->h-ZELL_PANES_CELLS_Y)/2,text,error[0]?0xe5ad87u:0xb9f8dbu);
 }
 zell_rect hotkeys;zell_panes_hotkeys_rect(s,&hotkeys);
 if(hotkeys.w){
  bool under=aim==ZELL_PANES_AIM_HOTKEYS||zell_panes_hotkeys_open();
  desktop_panes_button(c,hotkeys,ZELL_PANES_HOTKEYS_LABEL,under?0x5a4a24u:0x2a2f45u,under?0xd9ba79u:0x8f8fd0u,0xffffffu);
 }
 zell_rect exit;zell_panes_exit_rect(s,&exit);
 sr_fill_rect(c,(float)exit.x,(float)exit.y,(float)exit.w,(float)exit.h,ZELL_PANES_EXIT_RGB,1);
 zell_actor_pose pose=zell_desktop_pose(s,-1);
 sr_fill_ellipse(c,s->x,s->y,12.f,4.f,0x071719u,.45f);
 desktop_art(c,pose.art,(int)s->x+pose.offset_x,(int)s->y+pose.offset_y,ZELL_PANES_KILIX_H);
 if(zell_panes_hotkeys_open())desktop_panes_hotkeys(c);
}
static void desktop_base(const zell_scene *s,sr_canvas *c){
 if(ZELL_ROOM_IS_PANES(s->room)){desktop_panes_room(s,c);return;}
 int cx,cy;zell_desktop_camera(s,&cx,&cy);
 static sr_canvas world;
 if(!world.px&&!sr_canvas_init(&world,ZELL_WORLD_W,ZELL_WORLD_H)){
  desktop_world_layer(s,c,cx,cy);
  desktop_chrome(s,c,cx,cy);
  zell_theater_draw(s,c);
  return;
 }
 /* Every room is drawn in world pixels, then placed with one transform. */
 desktop_world_layer(s,&world,0,0);
 desktop_present_room(s,c,&world);
 desktop_chrome(s,c,cx,cy);
 zell_theater_draw(s,c);
}
static void desktop_text(sr_canvas *c,int x,int y,const char *text,uint32_t color,int width){
 char line[160];int cap=width/8;if(cap<1)return;if(cap>159)cap=159;
 size_t n=strlen(text);if(n>(size_t)cap){memcpy(line,text,(size_t)cap);if(cap>3)memcpy(line+cap-3,"...",3);line[cap]=0;}else{memcpy(line,text,n+1);}
 sr_text(c,(float)x,(float)y,line,color,1,1);
}
static void desktop_panel(const zell_scene *s,sr_canvas *c){
 int launch=s->mode==ZELL_MODE_LAUNCHER;
 int count=launch?zell_desktop_launcher_count(s):zell_desktop_actions(s->station);
 int width=c->w-48;if(width>640)width=640;int x=(c->w-width)/2;
 int rows=(c->h-150)/26;if(rows<1)rows=1;if(rows>count)rows=count;
 int first=s->menu_index>=rows?s->menu_index-rows+1:0;
 int height=88+rows*26;int y=(c->h-height)/2;
 sr_fill_rect(c,0,30,(float)c->w,(float)(c->h-60),0x061017u,.68f);
 sr_fill_rect(c,(float)x,(float)y,(float)width,(float)height,0x10232cu,.98f);
 sr_stroke_rect(c,(float)x,(float)y,(float)width,(float)height,1,0x5b9989u,1);
 desktop_text(c,x+18,y+14,launch?"ZELL / STATIONS AND ACTIONS":zell_stations[s->station].name,0xf0d69bu,width-36);
 char warning[120];snprintf(warning,sizeof warning,"OS upgrade overdue: health %d%%. Choose Update OS.",s->hp);
 desktop_text(c,x+18,y+36,(!launch&&s->station==2&&s->hp>=0&&s->hp<100)?warning:launch?(s->launcher_query[0]?s->launcher_query:"Type to search every action in every room"):zell_stations[s->station].role,0x89aaa9u,width-36);
 for(int r=0;r<rows;r++){int i=r+first,yy=y+56+r*26;bool selected=i==s->menu_index;
 if(selected)sr_fill_rect(c,(float)(x+10),(float)(yy-4),(float)(width-20),27,0x254b50u,1);
 char label[240];if(launch)zell_desktop_launcher_label(s,i,label,sizeof label);
 desktop_text(c,x+20,yy,launch?label:zell_desktop_action(s->station,i),selected?0xe8ffe9u:0xb5c9cdu,width-40);
 }
 desktop_text(c,x+18,y+height-25,width<400?"Enter open | Esc back":"Up/Down choose  Enter open  Esc back",0x89aaa9u,width-36);
}

static void desktop_dialogue(const zell_scene *s,sr_canvas *c) {
 int left=20,top=44,width=c->w-40;
 sr_fill_rect(c,8,36,(float)(c->w-16),(float)(c->h-72),0x10232cu,.98f);
 if(s->mode==ZELL_MODE_ANSWER){
  int y,rows,span;answer_layout(c->w,c->h-42,&y,&rows,&span);y=72;
  desktop_text(c,left,top,s->answerer,0xf0d69bu,width);
  draw_wrapped(c,left,y,span,s->answer,0xe3efe8u,s->answer_scroll,rows);
  desktop_text(c,left,c->h-54,"Up/Down scroll | Enter close",0x89aaa9u,width);
  return;
 }
 if(s->mode==ZELL_MODE_WAIT){
  desktop_text(c,left,top,"The wizard is thinking...",0xf0d69bu,width);
  desktop_text(c,left,top+26,s->answerer,0x9edecbu,width);
  desktop_text(c,left,c->h-54,"Esc cancels the question",0x89aaa9u,width);
  return;
 }
 desktop_text(c,left,top,"WIZARD / ASK",0xf0d69bu,width);
 int visible=(width-16)/8;if(visible<1)visible=1;
 int offset=s->question_len>visible?s->question_len-visible:0;
 input_field(c,left,top+24,width,s->question+offset,s->question_len-offset);
 if(s->mode==ZELL_MODE_QUESTION){
  int count=zell_scene_choice_rows(s)+1;
  int rows=(c->h-154)/22;if(rows<1)rows=1;if(rows>count)rows=count;
  int first=s->menu_index>=rows?s->menu_index-rows+1:0;
  for(int r=0;r<rows;r++){
   int i=first+r,y=top+56+r*22;
   if(i==s->menu_index)sr_fill_rect(c,(float)(left-4),(float)(y-2),(float)(width+8),21,0x254b50u,1);
   const char *label=i==count-1?(s->history_on?"History [ON] - toggle":"History [OFF] - toggle"):zell_scene_choice(s,i);
   desktop_text(c,left,y,label,i==s->menu_index?0xf5dda7u:0xc7d8dau,width);
  }
 }
 desktop_text(c,left,c->h-54,"Enter ask | Esc back",0x89aaa9u,width);
}

/* Window buttons: 0 minimize (bar at the foot), 1 maximize (one frame),
 * 2 restore (two overlapped frames), 3 close (a cross). */
static void desktop_button(const zell_scene *s,sr_canvas *c,const zell_rect *r,int kind){
 bool hover=s->mouse_valid&&s->mouse_x>=r->x&&s->mouse_x<r->x+r->w&&s->mouse_y>=r->y&&s->mouse_y<r->y+r->h;
 uint32_t ink=hover?0xf4ffe9u:0xabc8c0u;
 sr_fill_rect(c,(float)r->x,(float)r->y,(float)r->w,(float)r->h,hover?0x2f5f63u:0x1b3a42u,1);
 sr_stroke_rect(c,(float)r->x,(float)r->y,(float)r->w,(float)r->h,1,0x74b8a9u,1);
 int cx=r->x+r->w/2,cy=r->y+r->h/2;
 if(kind==0)sr_fill_rect(c,(float)(cx-6),(float)(cy+4),12,2,ink,1);
 else if(kind==1)sr_stroke_rect(c,(float)(cx-6),(float)(cy-5),12,10,2,ink,1);
 else if(kind==3)desktop_cross(c,cx,cy,5,ink);
 else {sr_stroke_rect(c,(float)(cx-3),(float)(cy-6),10,8,1,ink,1);sr_fill_rect(c,(float)(cx-7),(float)(cy-2),10,8,hover?0x2f5f63u:0x1b3a42u,1);sr_stroke_rect(c,(float)(cx-7),(float)(cy-2),10,8,1,ink,1);}
}
/* The minimized terminal: a button on the bottom bar, in every mode. */
static void desktop_terminal_tab(const zell_scene *s,sr_canvas *c){
 if(!s->terminal_minimized||s->service_phase!=5)return;
 zell_terminal_layout l;zell_terminal_layout_for(s,c->w,c->h,&l);
 bool hover=s->mouse_valid&&s->mouse_x>=l.tab.x&&s->mouse_x<l.tab.x+l.tab.w&&s->mouse_y>=l.tab.y&&s->mouse_y<l.tab.y+l.tab.h;
 sr_fill_rect(c,(float)l.tab.x,(float)l.tab.y,(float)l.tab.w,(float)l.tab.h,hover?0x2f5f63u:0x10232cu,.97f);
 sr_stroke_rect(c,(float)l.tab.x,(float)l.tab.y,(float)l.tab.w,(float)l.tab.h,2,0x74b8a9u,1);
 /* A blinking lamp says the command is still running behind the button. */
 sr_fill_rect(c,(float)(l.tab.x+8),(float)(l.tab.y+9),8,8,(s->flame_tick/16)%2?0x7bd88fu:0x2f6b45u,1);
 char title[120];snprintf(title,sizeof title,"%s",s->terminal_title[0]?s->terminal_title:"Neuronte terminal");
 desktop_text(c,l.tab.x+24,l.tab.y+5,title,hover?0xf4ffe9u:0xe5cf98u,l.tab.w-30);
}
/* The scene keeps animating beneath every station conversation. */
static void desktop_service(const zell_scene *s,sr_canvas *c){
 if(s->service_phase==5){
  zell_terminal_layout l;zell_terminal_layout_for(s,c->w,c->h,&l);
  int w=l.image.w,h=l.image.h,x=l.image.x,y=l.image.y;
  sr_fill_rect(c,0,30,(float)c->w,(float)(c->h-60),0x041017u,.35f);
  if(s->terminal_pixels&&w>0&&h>0){
   sr_fill_rect(c,(float)l.frame.x,(float)l.frame.y,(float)l.frame.w,(float)l.frame.h,0x10232cu,1);
   sr_stroke_rect(c,(float)l.frame.x,(float)l.frame.y,(float)l.frame.w,(float)l.frame.h,2,0x74b8a9u,1);
   sr_canvas frame;sr_canvas_wrap(&frame,s->terminal_pixels,w,h);sr_blit_scaled(c,&frame,x,y,w,h,1);
   /* The Command computer's shell is known by the terminal's own title,
    * which stays with it: Kilix may stand at another station by the time a
    * minimised terminal is brought back. */
   bool shell=!strcmp(s->terminal_title,zell_stations[ZELL_COMMAND_COMPUTER].name);
   char title[160];snprintf(title,sizeof title,"%s / %s",s->terminal_title[0]?s->terminal_title:"ZELL",shell?"SHELL":s->terminal_private?"PRIVATE TERMINAL":"INTERACTIVE TERMINAL");
   desktop_text(c,x,y-23,title,0xe5cf98u,w-104);
   desktop_button(s,c,&l.minimize,0);
   desktop_button(s,c,&l.maximize,s->terminal_maximized?2:1);
   desktop_button(s,c,&l.close,3);
   desktop_text(c,x,y+h+7,shell?"Keyboard goes to the shell | type exit to leave":s->terminal_private?"Hidden secret input | Ctrl-C cancels":"Type here for sudo | Ctrl-C interrupts command",0xabc8c0u,w);
  }else desktop_label(c,c->w/2,c->h/2,"Opening Neuronte terminal...",0xc9e4d5u);
  return;
 }

 if(c->w<240||c->h<220){
  sr_fill_rect(c,0,30,(float)c->w,(float)(c->h-60),0x10232cu,1);
  desktop_text(c,8,40,"Enlarge the pane",0xf0d69bu,c->w-16);
  desktop_text(c,8,62,"for station windows",0xc7d8dau,c->w-16);
  return;
 }
 int width=c->w-32;if(width>900)width=900;if(width<64)width=64;
 int x=(c->w-width)/2,top=44,bottom=c->h-44;
 sr_fill_rect(c,0,30,(float)c->w,(float)(c->h-60),0x061017u,.58f);
 sr_fill_rect(c,(float)x,36,(float)width,(float)(c->h-72),0x10232cu,.98f);
 sr_stroke_rect(c,(float)x,36,(float)width,(float)(c->h-72),1,0x5b9989u,1);
 desktop_text(c,x+12,top,zell_stations[s->station].name,0xf0d69bu,width-24);
 if(s->service_phase==2){
  desktop_text(c,x+12,top+24,s->service_title,0x9edecbu,width-24);
  int rows=(bottom-top-74)/24;if(rows<1)rows=1;if(rows>s->service_count)rows=s->service_count;
  int first=s->service_index>=rows?s->service_index-rows+1:0;
  for(int r=0;r<rows;r++){
   int index=first+r,y=top+50+r*24;bool selected=index==s->service_index;
   if(selected)sr_fill_rect(c,(float)(x+6),(float)(y-3),(float)(width-12),24,0x254b50u,1);
   desktop_text(c,x+14,y,s->service_choices[index],selected?0xe8ffe9u:0xb5c9cdu,width-28);
  }
 }else{
  int reserved=s->service_phase==1?100:32;
  int rows=(bottom-top-28-reserved)/18;if(rows<1)rows=1;
  draw_wrapped(c,x+12,top+28,width-24,s->service_text,0xe3efe8u,s->service_scroll,rows);
  if(s->service_phase==1){
   desktop_text(c,x+12,bottom-80,s->service_title,0x9edecbu,width-24);
   int visible=(width-32)/8;if(visible<1)visible=1;
   int length=(int)strlen(s->service_input),offset=length>visible?length-visible:0;
   input_field(c,x+12,bottom-54,width-24,s->service_input+offset,length-offset);
  }
 }
 const char *hint=s->service_phase==0?"Working... | Esc cancel":s->service_phase==2?"Up/Down choose | Enter select | Esc back":s->service_phase==1?"Enter submit | Up/Down scroll | Esc back":s->service_phase==4?"Up/Down scroll | Enter return to station":"Up/Down scroll | Enter continue | Esc back";
 desktop_text(c,x+12,bottom-18,hint,0x89aaa9u,width-24);
}
/* Theater chrome. These live in the draw unit so the Neuronte macros apply. */
void zell_ui_fill(struct sr_canvas *canvas,int x,int y,int w,int h,uint32_t color,float alpha){
 if(!canvas||w<1||h<1)return;
 sr_fill_rect(canvas,(float)x,(float)y,(float)w,(float)h,color,alpha);
}
void zell_ui_stroke(struct sr_canvas *canvas,int x,int y,int w,int h,float thickness,uint32_t color,float alpha){
 if(!canvas||w<1||h<1)return;
 sr_stroke_rect(canvas,(float)x,(float)y,(float)w,(float)h,thickness,color,alpha);
}
static int ui_utf8_len(const char *s){
 unsigned char c;int n,i;
 if(!s||!s[0])return 0;
 c=(unsigned char)s[0];
 if((c&0x80u)==0)return 1;
 if((c&0xe0u)==0xc0u)n=2;
 else if((c&0xf0u)==0xe0u)n=3;
 else if((c&0xf8u)==0xf0u)n=4;
 else return 1;
 for(i=1;i<n;i++)if(((unsigned char)s[i]&0xc0u)!=0x80u)return 1;
 return n;
}
void zell_ui_text(struct sr_canvas *canvas,int x,int y,const char *text,uint32_t color,int width){
 char line[512];int cap,fit,out;const char *p;
 if(!canvas||!text)return;
 cap=width/8;if(cap<1)return;if(cap>511)cap=511;
 fit=0;
 for(p=text;*p&&*p!='\n';){
  int n=ui_utf8_len(p);
  if(fit+n>cap)break;
  fit+=n;p+=n;
 }
 if(*p==0||*p=='\n'){memcpy(line,text,(size_t)fit);line[fit]=0;}
 else{
  int budget=cap>3?cap-3:cap;
  out=0;
  for(p=text;*p&&*p!='\n';){
   int n=ui_utf8_len(p);
   if(out+n>budget||out+n>511)break;
   memcpy(line+out,p,(size_t)n);out+=n;p+=n;
  }
  if(cap>3&&out+3<=511){memcpy(line+out,"...",3);out+=3;}
  line[out]=0;
 }
 sr_text(canvas,(float)x,(float)y,line,color,1.f,1);
}
void zell_ui_blit(struct sr_canvas *canvas,int dx,int dy,int dw,int dh,const uint32_t *pixels,int src_w,int src_h){
 sr_canvas src;int x,y,w,h;
 if(!canvas||!pixels||dw<1||dh<1||src_w<1||src_h<1)return;
 sr_fill_rect(canvas,(float)dx,(float)dy,(float)dw,(float)dh,0x000000u,1.f);
 zell_theater_contain(src_w,src_h,dw,dh,&x,&y,&w,&h);
 if(w<1||h<1)return;
 sr_canvas_wrap(&src,(uint32_t *)pixels,src_w,src_h);
 sr_blit_scaled(canvas,&src,dx+x,dy+y,w,h,1.f);
}

#include "desktop_hud.inc"
