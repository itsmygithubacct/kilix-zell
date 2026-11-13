#include <math.h>
/* Fitted-room transform. Each room supplies its own interactive rectangle.
 * Older rooms keep 1:1 pixels when the canvas can hold their rectangle.
 * Rooms 5–8 always fit between the HUD and the status strip, so a large
 * window still starts the picture below the HUD. Translation is fixed. */
/* Interactive world rectangle for this room. The avatar is not an input:
 * the fitted translation stays put while he walks. */
static void zell_room_logical(const zell_scene *s,int *left,int *top,int *right,int *bottom){
 if(s->room==0){
  /* Cave wall switches are x=88..151, y=298..375. Weather stands at
   * (157,244) with an 88px art and a glow of radius 54. The east shell
   * lever is drawn 104px past the .90W floor point. The south lip leaves. */
  *left=64;*top=140;*right=ZELL_WORLD_W;*bottom=ZELL_WORLD_H;
  return;
 }
 /* Rooms 5–8 fill the canvas. Their video and furniture run wider than
  * the old .135W..865W corridor, and the fit below reserves HUD and status. */
 if(s->room>=5&&s->room<=8){
  *left=0;*top=0;*right=ZELL_WORLD_W;*bottom=ZELL_WORLD_H;
  return;
 }
 /* Command Center and the older downstairs rooms: floor corridor x=.135W..865W,
  * y=64 through the exit. The Workshop stool (opaque from y=291) and the
  * stair corridors are inside this rectangle. */
 *left=(int)(0.135f*(float)ZELL_WORLD_W);
 *right=(int)(0.865f*(float)ZELL_WORLD_W);
 *top=64;*bottom=ZELL_WORLD_H;
}
static void zell_room_view_fit(zell_scene *s){
 int logical_x,logical_y,logical_right,logical_bottom,logical_w,logical_h;
 zell_room_logical(s,&logical_x,&logical_y,&logical_right,&logical_bottom);
 logical_w=logical_right-logical_x;logical_h=logical_bottom-logical_y;
 int width=s->width>0?s->width:1,height=s->height>0?s->height:1;
 int hud=ZELL_ROOM_HUD_BOTTOM,status=ZELL_ROOM_STATUS_STRIP,avail_h=height-hud-status;
 int full=s->room>=5&&s->room<=8;
 float bare_w,bare_h,bare,fit_h,scale,scaled_w,left,top;
 if(logical_w<1)logical_w=1;
 if(logical_h<1)logical_h=1;
 if(avail_h<1)avail_h=1;
 bare_w=(float)width/(float)logical_w;
 bare_h=(float)height/(float)logical_h;
 bare=bare_w<bare_h?bare_w:bare_h;
 fit_h=(float)avail_h/(float)logical_h;
 /* Older rooms keep 1:1 pixels on a large canvas. Rooms 5–8 always use the
  * height under the HUD, including 1920x1080 and 1536x1024. */
 if(!full&&bare>=1.f)scale=1.f;
 else {scale=bare_w<fit_h?bare_w:fit_h;if(scale>1.f)scale=1.f;}
 s->room_view.logical_x=logical_x;
 s->room_view.logical_y=logical_y;
 s->room_view.logical_w=logical_w;
 s->room_view.logical_h=logical_h;
 s->room_view.scale=scale;
 s->room_view.viewport_x=0;
 s->room_view.viewport_y=0;
 s->room_view.viewport_w=width;
 s->room_view.viewport_h=height;
 s->room_view.offset_x=0;
 s->room_view.offset_y=0;
 /* A canvas that holds the whole world shows older rooms 1:1 from their
  * logical origin. Rooms 5–8 keep the origin below the HUD instead. */
 if(!full&&scale==1.f&&width>=ZELL_WORLD_W&&height>=ZELL_WORLD_H){
  s->room_view.offset_x=logical_x;
  s->room_view.offset_y=logical_y;
  return;
 }
 scaled_w=(float)logical_w*scale;
 left=((float)width-scaled_w)*0.5f;
 if(left<0.f)left=0.f;
 s->room_view.offset_x=(int)floorf(left);
 if(scale<1.f){
  s->room_view.viewport_y=hud;
  s->room_view.viewport_h=avail_h;
  return;
 }
 top=(float)(height-status)-(float)logical_h*scale;
 if(top>(float)hud)top=(float)hud;
 if(top<0.f)top=0.f;
 s->room_view.offset_y=(int)floorf(top);
}
