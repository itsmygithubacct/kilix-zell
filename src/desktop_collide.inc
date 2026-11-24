/* Furniture footprints measured from the room paintings. The Command Center
 * adds the live display wall and the four floor panels below.
 * tools/room_grid.py prints this table; keep one brace per line. */
typedef struct { int room; float x0,y0,x1,y1; } zell_block;
static const zell_block zell_blocks[]={
{0,924.f,858.f,1176.f,932.f}, /* cave: DJ decks and speakers */
{0,1286.f,628.f,1328.f,856.f}, /* cave: librarian bookshelf, ladder, chair */
{3,207.f,328.f,420.f,640.f}, /* vault: left shelves and chests */
{3,1100.f,328.f,1328.f,680.f}, /* vault: right shelves and chests */
{3,207.f,850.f,380.f,932.f}, /* vault: left lantern pedestal */
{3,1120.f,850.f,1328.f,932.f}, /* vault: right lantern pedestal */
{4,207.f,328.f,430.f,555.f}, /* workshop: left table, cabinets, chest, scroll bin */
{4,530.f,328.f,1100.f,376.f}, /* workshop: center desk */
{4,752.f,332.f,840.f,396.f}, /* workshop: stool */
{4,1000.f,328.f,1328.f,468.f}, /* workshop: right desk and cabinets */
{4,1255.f,430.f,1328.f,528.f}, /* workshop: right scroll bin */
{4,207.f,760.f,310.f,932.f}, /* workshop: left stair-mouth rock */
{4,1220.f,770.f,1328.f,932.f}, /* workshop: right stair-mouth rock */
{5,0.f,0.f,1536.f,380.f}, /* game: back wall and center cabinets; floor starts at 380 */
/* Game Room furniture stands on its floor footprint; the floor behind each
 * piece stays open and the foreground's depth hides him there. */
{5,60.f,336.f,368.f,420.f}, /* game: bookshelf */
{5,78.f,565.f,358.f,652.f}, /* game: chess table, the floor under its top */
{5,316.f,560.f,430.f,630.f}, /* game: right stool, the chess seat */
{5,1058.f,380.f,1110.f,470.f}, /* game: pool table, left end on its back leg */
{5,1110.f,380.f,1150.f,492.f}, /* game: pool table, front rail stepping down */
{5,1150.f,380.f,1190.f,515.f}, /* game: pool table, front rail stepping down */
{5,1190.f,380.f,1252.f,552.f}, /* game: pool table, to its front leg */
{5,1240.f,380.f,1504.f,646.f}, /* game: pinball, to its front legs within reach */
{5,200.f,724.f,256.f,1024.f}, /* game: left crystal rocks */
{5,256.f,744.f,336.f,1024.f}, /* game: left boulder */
{5,336.f,778.f,384.f,1024.f}, /* game: ferns on the left boulder */
{5,384.f,772.f,476.f,1024.f}, /* game: left lantern and its rock */
{5,476.f,836.f,540.f,1024.f}, /* game: rock right of the left lantern */
{5,540.f,894.f,600.f,1024.f}, /* game: rock beside the stairs, left */
{5,920.f,890.f,968.f,1024.f}, /* game: rock beside the stairs, right */
{5,968.f,836.f,1072.f,1024.f}, /* game: right fern rock */
{5,1072.f,772.f,1160.f,1024.f}, /* game: right lantern and its rock */
{5,1160.f,820.f,1240.f,1024.f}, /* game: ferns between the right rocks */
{5,1240.f,752.f,1340.f,1024.f}, /* game: right boulder and crystals */
{6,0.f,0.f,1536.f,735.f}, /* theater: screen and caption wall; aisle starts at 735 */
{6,0.f,592.f,600.f,1024.f}, /* theater: left seats, aisle stays open */
{6,912.f,688.f,1536.f,1024.f}, /* theater: right seats, aisle stays open */
{7,0.f,0.f,1536.f,400.f}, /* meditation: back wall; floor starts at 400 */
{7,32.f,160.f,480.f,592.f}, /* meditation: left altar */
{7,1104.f,312.f,1504.f,600.f}, /* meditation: right shrine */
{8,0.f,0.f,1536.f,400.f}, /* library: back wall, ladders, plant, upper shelves; floor starts at 400 */
{8,40.f,256.f,400.f,600.f}, /* library: left shelves */
{8,1136.f,256.f,1496.f,592.f} /* library: right shelves */
};
#define ZELL_BLOCK_CAP 64
_Static_assert(sizeof zell_blocks/sizeof zell_blocks[0] + 1 + ZELL_FLOOR_PANELS <= ZELL_BLOCK_CAP,
 "raise ZELL_BLOCK_CAP");
typedef struct { float x0,y0,x1,y1; } zell_span;
static bool span_has(float x,float y,float x0,float y0,float x1,float y1){
 return x>=x0&&y>=y0&&x<x1&&y<y1;
}
static int spans_for(const zell_scene *s,zell_span *out,int cap){
 int n=0;
 if(!s||!out||cap<1)return 0;
 for(int i=0;i<(int)(sizeof zell_blocks/sizeof zell_blocks[0]);i++){
  if(zell_blocks[i].room!=s->room)continue;
  if(n>=cap)break;
  out[n].x0=zell_blocks[i].x0;out[n].y0=zell_blocks[i].y0;
  out[n].x1=zell_blocks[i].x1;out[n].y1=zell_blocks[i].y1;
  n++;
 }
 if(s->room!=1)return n;
 zell_rect wall=zell_command_wall_bounds();
 if(n<cap)out[n++]=(zell_span){(float)wall.x,(float)wall.y,(float)(wall.x+wall.w),(float)(wall.y+wall.h)};
 /* Movement tests the foot center. Allow for both feet at the frame edges
  * so Kilix cannot squeeze through a gap narrower than his stance. */
 for(int i=0;i<ZELL_FLOOR_PANELS&&n<cap;i++){
  zell_rect panel;zell_floor_panel_layout(i,&panel,NULL);
  out[n++]=(zell_span){(float)(panel.x-ZELL_FOOT_RADIUS_X),(float)(panel.y-ZELL_FOOT_RADIUS_Y),
                      (float)(panel.x+panel.w+ZELL_FOOT_RADIUS_X),(float)(panel.y+panel.h+ZELL_FOOT_RADIUS_Y)};
 }
 return n;
}
float zell_desktop_north(int room){
 if(room==1){
  zell_rect wall=zell_command_wall_bounds();
  return (float)(wall.y+wall.h);
 }
 if(room==5)return 380.f;
 if(room==6)return 735.f;
 if(room==7||room==8)return 400.f;
 return (float)ZELL_WORLD_H*.32f;
}
static bool command_tunnel_y(float y){
 return y>=(float)ZELL_COMMAND_OPENING_TOP&&y<(float)ZELL_COMMAND_OPENING_BOTTOM;
}
static int command_passage(float x,float y){
 if(!command_tunnel_y(y))return 0;
 if(x<=(float)ZELL_COMMAND_OPENING_DEPTH)return ZELL_ROOM_PANES_LEFT;
 if(x>=(float)(ZELL_WORLD_W-ZELL_COMMAND_OPENING_DEPTH))return ZELL_ROOM_PANES_RIGHT;
 return 0;
}
bool zell_desktop_blocked(const zell_scene *s,float x,float y){
 zell_span spans[ZELL_BLOCK_CAP];
 if(s&&s->room==1&&!command_tunnel_y(y)&&
    (x<(float)ZELL_COMMAND_SIDE_WALL||x>(float)(ZELL_WORLD_W-ZELL_COMMAND_SIDE_WALL)))return true;
 int n=spans_for(s,spans,ZELL_BLOCK_CAP);
 for(int i=0;i<n;i++)if(span_has(x,y,spans[i].x0,spans[i].y0,spans[i].x1,spans[i].y1))return true;
 return false;
}
int zell_desktop_block_count(const zell_scene *s){
 zell_span spans[ZELL_BLOCK_CAP];
 return spans_for(s,spans,ZELL_BLOCK_CAP);
}
void zell_desktop_block_rect(const zell_scene *s,int index,float *x0,float *y0,float *x1,float *y1){
 zell_span spans[ZELL_BLOCK_CAP];
 int n=spans_for(s,spans,ZELL_BLOCK_CAP);
 if(!x0||!y0||!x1||!y1)return;
 if(index<0||index>=n){*x0=*y0=*x1=*y1=0;return;}
 *x0=spans[index].x0;*y0=spans[index].y0;*x1=spans[index].x1;*y1=spans[index].y1;
}
int zell_desktop_block_table(int index,int *room,float *x0,float *y0,float *x1,float *y1){
 int count=(int)(sizeof zell_blocks/sizeof zell_blocks[0]);
 if(index<0||index>=count||!room||!x0||!y0||!x1||!y1)return 0;
 *room=zell_blocks[index].room;
 *x0=zell_blocks[index].x0;*y0=zell_blocks[index].y0;
 *x1=zell_blocks[index].x1;*y1=zell_blocks[index].y1;
 return 1;
}
