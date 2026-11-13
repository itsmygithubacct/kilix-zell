/* Furniture footprints measured from the room paintings. Command Center
 * screens are not copied here: collision calls zell_monitor_layout and
 * zell_mana_console_layout. Floor-panel rims are not obstacles.
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
{4,1220.f,770.f,1328.f,932.f} /* workshop: right stair-mouth rock */
};
#define ZELL_BLOCK_CAP 64
_Static_assert(sizeof zell_blocks/sizeof zell_blocks[0] + 6 <= ZELL_BLOCK_CAP,
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
 for(int screen=0;screen<5&&n<cap;screen++){
  zell_rect frame,display;
  zell_monitor_layout(screen,&frame,&display);
  out[n].x0=(float)frame.x;out[n].y0=(float)frame.y;
  out[n].x1=(float)(frame.x+frame.w);out[n].y1=(float)(frame.y+frame.h);
  n++;
 }
 if(n<cap){
  zell_rect frame,display;
  zell_mana_console_layout(&frame,&display);
  out[n].x0=(float)frame.x;out[n].y0=(float)frame.y;
  out[n].x1=(float)(frame.x+frame.w);out[n].y1=(float)(frame.y+frame.h);
  n++;
 }
 return n;
}
bool zell_desktop_blocked(const zell_scene *s,float x,float y){
 zell_span spans[ZELL_BLOCK_CAP];
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
