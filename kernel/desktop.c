#include "ark.h"
#include "font.h"
#define MAX_W 1920
#define MAX_H 1200
#define APP_COUNT 5
static BootInfo boot;
static uint32_t canvas[MAX_W*MAX_H], wallpaper[MAX_W*MAX_H];
static int sw,sh,mx=510,my=390,selected=-1,note_file=-1;
static bool dirty=true,dark=true,alt_wall=false,note_error=false;
static int drag=-1,drag_x,drag_y;
static uint8_t mouse_buttons;
static char command[96];
static int command_len,shell_scroll,file_scroll;
static char note[VFS_FILE_CAP];
static size_t note_len;
static char status[80]="All files live in RAM. Restart clears your changes.";
static const char *titles[]={
    "Terminal","Files","Notes","Settings","About ArkOS"
};
static const char *shortcuts[]={
    "F1","F2","F3","F4",""
};
typedef struct {
    int x,y,w,h,ox,oy,ow,oh;
    bool visible,maximized;
} Window;
static Window wins[APP_COUNT];
static int order[APP_COUNT]={
    4,3,2,1,0
};
static uint32_t surface,body,ink,muted,line,accent=0x62dce9;
static int min(int a,int b){
    return a<b?a:b;
} static int max(int a,int b){
    return a>b?a:b;
} static bool inside(int x,int y,int a,int b,int w,int h){
    return x>=a&&x<a+w&&y>=b&&y<b+h;
} static void rect(int x,int y,int w,int h,uint32_t c){
    int x1=max(0,x),x2=min(sw,x+w),y1=max(0,y),y2=min(sh,y+h);
    for(int yy=y1;yy<y2;yy++)for(int xx=x1;xx<x2;xx++)canvas[yy*sw+xx]=c;
} static void roundrect(int x,int y,int w,int h,int r,uint32_t c){
    rect(x+r,y,w-2*r,h,c);
    rect(x,y+r,w,h-2*r,c);
    for(int yy=0;yy<r;yy++)for(int xx=0;xx<r;xx++)if((xx-r)*(xx-r)+(yy-r)*(yy-r)<=r*r){
        rect(x+xx,y+yy,1,1,c);
        rect(x+w-xx-1,y+yy,1,1,c);
        rect(x+xx,y+h-yy-1,1,1,c);
        rect(x+w-xx-1,y+h-yy-1,1,1,c);
    }
} static void text(int x,int y,const char *s,uint32_t c,int scale){
    int start=x;
    while(*s){
        unsigned char ch=*s++;
        if(ch=='\n'){
            x=start;
            y+=18*scale;
            continue;
        }if(ch<32||ch>126)ch='?';
        for(int yy=0;yy<16;yy++)for(int xx=0;xx<8;xx++)if(font[ch-32][yy]&(1<<xx))rect(x+xx*scale,y+yy*scale,scale,scale,c);
        x+=9*scale;
    }
} static void clipped_text(int x,int y,const char *s,uint32_t c,int cols){
    char b[160];
    int i=0;
    while(s[i]&&i<cols&&i<159){
        b[i]=s[i];
        i++;
    }b[i]=0;
    text(x,y,b,c,1);
} static void number(int x,int y,uint64_t v,uint32_t c){
    char b[24];
    uint_to_str(v,b);
    text(x,y,b,c,1);
} static void line_draw(int x0,int y0,int x1,int y1,uint32_t c){
    int dx=x1>x0?x1-x0:x0-x1,sx=x0<x1?1:-1,dy=-(y1>y0?y1-y0:y0-y1),sy=y0<y1?1:-1,err=dx+dy;
    for(;;){
        rect(x0,y0,1,1,c);
        if(x0==x1&&y0==y1)break;
        int e2=2*err;
        if(e2>=dy){
            err+=dy;
            x0+=sx;
        }if(e2<=dx){
            err+=dx;
            y0+=sy;
        }
    }
} static void theme(void){
    accent=dark?0x62dce9:0x07667b;
    surface=dark?0x172437:0xeaf0f6;
    body=dark?0x0f1b2c:0xf8fafc;
    ink=dark?0xe4edf7:0x16283e;
    muted=dark?0x96a9bf:0x51667d;
    line=dark?0x2d3d52:0xccd8e5;
} static void make_wallpaper(void){
    for(int y=0;y<sh;y++)for(int x=0;x<sw;x++){
        int dx=x-sw*3/4,dy=y-sh/2;
        int d=dx*dx+dy*dy;
        int r=8+y*11/sh,g=21+y*14/sh,b=42+y*20/sh;
        if(alt_wall){
            r=20+y*18/sh;
            g=17+y*13/sh;
            b=46+y*18/sh;
        }int radius=sh*7/16;
        int inner=radius-40;
        if(d<radius*radius&&d>inner*inner){
            int v=(d-inner*inner)*100/(radius*radius-inner*inner);
            r+=v/10;
            g+=v/3;
            b+=v/2;
        }if(d<inner*inner){
            g+=5;
            b+=10;
        }wallpaper[y*sw+x]=(r<<16)|(g<<8)|b;
    }dirty=true;
} static void icon(int app,int x,int y,int size){
    uint32_t colors[]={
        0x367b9f,0x317fd1,0xd1a148,0x516984,0x288e96
    };
    roundrect(x,y,size,size,9,colors[app]);
    uint32_t c=0xf1f7ff;
    int k=size/4;
    if(app==0){
        line_draw(x+k,y+k,x+k+7,y+size/2,c);
        line_draw(x+k+7,y+size/2,x+k,y+size-k,c);
        rect(x+size/2,y+size-k,10,2,c);
    }else if(app==1){
        roundrect(x+9,y+13,size-18,size-23,3,c);
        rect(x+9,y+10,13,7,c);
    }else if(app==2){
        rect(x+12,y+8,size-24,size-16,c);
        for(int i=0;i<3;i++)rect(x+15,y+15+i*6,size-30,2,0xb78835);
    }else if(app==3){
        rect(x+11,y+size/2-1,size-22,2,c);
        rect(x+size/2-1,y+11,2,size-22,c);
        roundrect(x+size/2-6,y+size/2-6,12,12,4,c);
        roundrect(x+size/2-3,y+size/2-3,6,6,2,colors[app]);
    }else{
        for(int i=0;i<3;i++){
            line_draw(x+size/2+i,y+10,x+10+i,y+size-10,c);
            line_draw(x+size/2+i,y+10,x+size-12+i,y+size-10,c);
        }rect(x+16,y+size-19,size-32,3,c);
    }
} static int focus(void){
    for(int i=APP_COUNT-1;i>=0;i--)if(wins[order[i]].visible)return order[i];
    return -1;
} static void activate(int app){
    int idx=0;
    while(order[idx]!=app)idx++;
    for(int i=idx;i<APP_COUNT-1;i++)order[i]=order[i+1];
    order[APP_COUNT-1]=app;
    wins[app].visible=true;
    dirty=true;
} static void open_app(int app){
    activate(app);
    serial_write("[ui] open ");
    serial_write(titles[app]);
    serial_write("\n");
} static void button(int x,int y,int w,const char *label,bool active){
    roundrect(x,y,w,30,6,active?0x285a72:line);
    text(x+10,y+7,label,active?0xe3faff:ink,1);
} static void load_note(int idx){
    if(idx<0||!vfs_files[idx].used)return;
    note_file=idx;
    note_error=false;
    strcopy(note,vfs_files[idx].data,sizeof note);
    note_len=strlen(note);
    open_app(2);
} static void draw_terminal(Window *w){
    int x=w->x+18,y=w->y+54,cols=max(1,(w->w-36)/9),rows=max(1,(w->h-108)/18),total=0;
    for(int i=0;i<shell_line_count;i++)total+=max(1,((int)strlen(shell_lines[i])+cols-1)/cols);
    shell_scroll=min(shell_scroll,max(0,total-rows));
    int first=max(0,total-rows-shell_scroll),row=0;
    for(int i=0;i<shell_line_count;i++){
        int chunks=max(1,((int)strlen(shell_lines[i])+cols-1)/cols);
        for(int c=0;c<chunks;c++,row++)if(row>=first&&row<first+rows){
            clipped_text(x,y,shell_lines[i]+c*cols,i==0?accent:ink,cols);
            y+=18;
        }
    }  rect(x,w->y+w->h-45,w->w-36,1,line);
    text(x,w->y+w->h-31,"ark@arkos >",accent,1);
    int available=max(1,(w->w-151)/9),start=max(0,command_len-available+1);
    clipped_text(x+108,w->y+w->h-31,command+start,ink,available);
    if((platform_ticks()/50)%2==0)rect(x+108+(command_len-start)*9,w->y+w->h-30,8,16,accent);
} static void draw_files(Window *w){
    int x=w->x+18,y=w->y+54;
    button(x,y,104,"New note",false);
    button(x+114,y,68,"Open",true);
    button(x+192,y,86,"Delete",false);
    text(x,y+44,"RAM / home / ark",accent,1);
    int row=0,maxrows=(w->h-158)/26,selrow=-1,count=0;
    for(int i=0;i<VFS_MAX_FILES;i++)if(vfs_files[i].used){
        if(i==selected)selrow=count;
        count++;
    }if(selrow>=0){
        if(selrow<file_scroll)file_scroll=selrow;
        if(selrow>=file_scroll+maxrows)file_scroll=selrow-maxrows+1;
    }file_scroll=min(file_scroll,max(0,count-maxrows));
    for(int i=0;i<VFS_MAX_FILES;i++)if(vfs_files[i].used){
        if(row>=file_scroll&&row<file_scroll+maxrows){
            if(i==selected)roundrect(x-4,y+73+(row-file_scroll)*26,w->w-28,25,4,line);
            text(x+6,y+77+(row-file_scroll)*26,vfs_files[i].name,ink,1);
            number(w->x+w->w-106,y+77+(row-file_scroll)*26,vfs_files[i].size,muted);
            text(w->x+w->w-50,y+77+(row-file_scroll)*26,"B",muted,1);
        }row++;
    }clipped_text(x,w->y+w->h-29,status,muted,(w->w-36)/9);
} static void draw_notes(Window *w){
    int x=w->x+18,y=w->y+53;
    const char *name=note_file>=0?vfs_files[note_file].name:"notes.txt";
    clipped_text(x,y,name,accent,(w->w-36)/9);
    text(x,w->y+w->h-28,note_error?"Cannot save: RAM directory is full.":note_file<0?"Type to create notes.txt in RAM.":"Saved to RAM - type to edit",note_error?0xd66c62:muted,1);
    int cols=max(1,(w->w-36)/9),rows=max(1,(w->h-115)/18),total=1,col=0;
    for(size_t i=0;i<note_len;i++){
        if(note[i]=='\n'){
            total++;
            col=0;
        }else if(++col>=cols){
            total++;
            col=0;
        }
    }int skip=max(0,total-rows),r=0,c=0;
    for(size_t i=0;i<note_len;i++){
        if(note[i]=='\n'){
            r++;
            c=0;
            continue;
        }if(r>=skip){
            char s[2]={
                note[i],0
            };
            text(x+c*9,y+31+(r-skip)*18,s,ink,1);
        }if(++c>=cols){
            r++;
            c=0;
        }
    }if((platform_ticks()/50)%2==0&&focus()==2)rect(x+c*9,y+31+(r-skip)*18,2,16,accent);
} static void draw_settings(Window *w){
    int x=w->x+23,y=w->y+61;
    text(x,y,"Appearance",ink,1);
    button(x,y+31,130,"Dark",dark);
    button(x+142,y+31,130,"Light",!dark);
    text(x,y+90,"Wallpaper",ink,1);
    button(x,y+121,130,"Orbit blue",!alt_wall);
    button(x+142,y+121,130,"Dusk",alt_wall);
    text(x,y+182,"Keyboard shortcuts",ink,1);
    text(x,y+215,"F1 Terminal    F2 Files",muted,1);
    text(x,y+237,"F3 Notes       F4 Settings",muted,1);
    text(x,y+259,"Tab: next window   Esc: close",muted,1);
} static void draw_about(Window *w){
    int x=w->x+24,y=w->y+60;
    icon(4,x,y,54);
    text(x+70,y+2,"ArkOS",ink,2);
    text(x+71,y+38,"0.1.0 / First light",accent,1);
    text(x,y+86,"Your own little operating system.",muted,1);
    rect(x,y+124,w->w-48,1,line);
    text(x,y+147,"KERNEL",muted,1);
    text(x+102,y+147,"x86_64 / ring 0",ink,1);
    text(x,y+178,"MEMORY",muted,1);
    number(x+102,y+178,boot.memory_mib,ink);
    text(x+165,y+178,"MiB usable",ink,1);
    text(x,y+209,"DISPLAY",muted,1);
    char b[24];
    uint_to_str(sw,b);
    text(x+102,y+209,b,ink,1);
    text(x+147,y+209,"x",ink,1);
    number(x+165,y+209,sh,ink);
    text(x,y+257,"Custom kernel. RAM-only files.",muted,1);
    text(x,y+279,"No disk installation or networking.",muted,1);
} static void draw_window(int app){
    Window *w=&wins[app];
    if(!w->visible)return;
    roundrect(w->x+6,w->y+8,w->w,w->h,12,0x091321);
    roundrect(w->x,w->y,w->w,w->h,10,surface);
    rect(w->x+1,w->y+40,w->w-2,w->h-51,body);
    rect(w->x+1,w->y+39,w->w-2,1,line);
    text(w->x+16,w->y+13,titles[app],focus()==app?ink:muted,1);
    text(w->x+w->w-100,w->y+13,"_",muted,1);
    rect(w->x+w->w-67,w->y+14,10,9,muted);
    rect(w->x+w->w-66,w->y+15,8,7,surface);
    text(w->x+w->w-33,w->y+13,"x",0xef9f9b,1);
    if(app==0)draw_terminal(w);
    if(app==1)draw_files(w);
    if(app==2)draw_notes(w);
    if(app==3)draw_settings(w);
    if(app==4)draw_about(w);
} static void render(void){
    memcpy(canvas,wallpaper,(size_t)sw*sh*4);
    rect(0,0,sw,36,0x101d2f);
    text(23,10,"ARK",0xe2f3ff,1);
    rect(66,10,1,16,0x385068);
    int active=focus();
    text(82,10,active>=0?titles[active]:"Desktop",0xa6bcd1,1);
    text(sw-285,10,"x86_64   RAM session",0x839ab1,1);
    int h,m,s;
    platform_time(&h,&m,&s);
    char clock[6]={
        (char)('0'+h/10),(char)('0'+h%10),':',(char)('0'+m/10),(char)('0'+m%10),0
    };
    text(sw-82,10,clock,0xdae8f6,1);
    text(40,sh-184,"ArkOS",0x668caf,3);
    text(42,sh-124,"FIRST LIGHT / 0.1",0x89abc7,1);
    for(int i=0;i<APP_COUNT;i++)draw_window(order[i]);
    int dockw=APP_COUNT*76+20,dx=(sw-dockw)/2,dy=sh-84;
    roundrect(dx,dy,dockw,72,15,0x1c2d44);
    for(int i=0;i<APP_COUNT;i++){
        int ix=dx+18+i*76;
        icon(i,ix,dy+10,46);
        if(wins[i].visible)rect(ix+17,dy+61,12,2,0x62dce9);
        text(ix+48,dy+17,shortcuts[i],0x91a9c1,1);
    }for(int y=0;y<18;y++){
        for(int x=0;x<=y/2;x++)rect(mx+x,my+y,1,1,x==0||x==y/2||y==17?0x0a1521:0xf4fbff);
    }volatile uint32_t *fb=(volatile uint32_t *)(uintptr_t)boot.framebuffer;
    for(int y=0;y<sh;y++){
        volatile uint32_t *row=(volatile uint32_t *)((uintptr_t)fb+(size_t)y*boot.pitch);
        for(int x=0;x<sw;x++){
            uint32_t c=canvas[y*sw+x];
            row[x]=(((c>>16)&255)<<boot.red_pos)|(((c>>8)&255)<<boot.green_pos)|((c&255)<<boot.blue_pos);
        }
    }dirty=false;
} static void maximize(int a){
    Window *w=&wins[a];
    if(!w->maximized){
        w->ox=w->x;
        w->oy=w->y;
        w->ow=w->w;
        w->oh=w->h;
        w->x=12;
        w->y=46;
        w->w=sw-24;
        w->h=sh-142;
    }else{
        w->x=w->ox;
        w->y=w->oy;
        w->w=w->ow;
        w->h=w->oh;
    }w->maximized=!w->maximized;
    dirty=true;
} static void click_app(int a,int x,int y){
    Window *w=&wins[a];
    if(a==1){
        int bx=w->x+18,by=w->y+54;
        if(inside(x,y,bx,by,104,30)){
            char name[40];
            int idx=-1;
            for(int n=1;n<100;n++){
                strcopy(name,"note",sizeof name);
                uint_to_str(n,name+4);
                size_t l=strlen(name);
                strcopy(name+l,".txt",sizeof(name)-l);
                if(vfs_find(name)<0){
                    idx=vfs_create(name);
                    break;
                }
            }if(idx>=0){
                selected=idx;
                strcopy(status,"New file created in RAM.",sizeof status);
                load_note(idx);
            }else strcopy(status,"RAM directory is full (24 files).",sizeof status);
        }else if(inside(x,y,bx+114,by,68,30)){
            if(selected>=0)load_note(selected);
        }else if(inside(x,y,bx+192,by,86,30)){
            if(selected>=0){
                if(selected==note_file){
                    note_file=-1;
                    note[0]=0;
                    note_len=0;
                }vfs_remove(vfs_files[selected].name);
                selected=-1;
                strcopy(status,"File deleted from RAM.",sizeof status);
            }
        }else{
            int row=0;
            for(int i=0;i<VFS_MAX_FILES;i++)if(vfs_files[i].used){
                if(row>=file_scroll&&row<file_scroll+(w->h-158)/26&&inside(x,y,bx-4,by+73+(row-file_scroll)*26,w->w-28,25))selected=i;
                row++;
            }
        }
    }else if(a==3){
        int bx=w->x+23,by=w->y+61;
        if(inside(x,y,bx,by+31,130,30)){
            dark=true;
            theme();
        }if(inside(x,y,bx+142,by+31,130,30)){
            dark=false;
            theme();
        }if(inside(x,y,bx,by+121,130,30)){
            alt_wall=false;
            make_wallpaper();
        }if(inside(x,y,bx+142,by+121,130,30)){
            alt_wall=true;
            make_wallpaper();
        }
    }dirty=true;
} static void mouse_event(Event *e){
    mx=max(0,min(sw-12,mx+e->dx));
    my=max(0,min(sh-20,my+e->dy));
    bool pressed=(e->buttons&1)&&!(mouse_buttons&1);
    if(!(e->buttons&1))drag=-1;
    if(drag>=0&&e->buttons&1){
        Window *w=&wins[drag];
        w->x=max(0,min(sw-w->w,mx-drag_x));
        w->y=max(38,min(sh-96-w->h,my-drag_y));
    }if(pressed){
        int dockw=APP_COUNT*76+20,dx=(sw-dockw)/2,dy=sh-84;
        if(inside(mx,my,dx,dy,dockw,72)){
            int a=(mx-dx-10)/76;
            if(a>=0&&a<APP_COUNT)open_app(a);
        }else{
            for(int i=APP_COUNT-1;i>=0;i--){
                int a=order[i];
                Window *w=&wins[a];
                if(w->visible&&inside(mx,my,w->x,w->y,w->w,w->h)){
                    activate(a);
                    if(my<w->y+40){
                        if(mx>=w->x+w->w-44){
                            w->visible=false;
                        }else if(mx>=w->x+w->w-78){
                            maximize(a);
                        }else if(mx>=w->x+w->w-112){
                            w->visible=false;
                        }else if(!w->maximized){
                            drag=a;
                            drag_x=mx-w->x;
                            drag_y=my-w->y;
                        }
                    }else click_app(a,mx,my);
                    break;
                }
            }
        }
    }mouse_buttons=e->buttons;
    dirty=true;
} static void key_event(int k){
    if(k>=KEY_F1&&k<=KEY_F4){
        open_app(k-KEY_F1);
        return;
    }int a=focus();
    if(k==KEY_ESCAPE&&a>=0){
        wins[a].visible=false;
        dirty=true;
        return;
    }if(k==KEY_TAB){
        for(int i=0;i<APP_COUNT;i++)if(wins[order[i]].visible){
            activate(order[i]);
            break;
        }return;
    }if(a==0){
        if(k==KEY_UP){
            shell_scroll++;
            dirty=true;
            return;
        }if(k==KEY_DOWN){
            shell_scroll=max(0,shell_scroll-1);
            dirty=true;
            return;
        }if(k==KEY_BACKSPACE&&command_len)command[--command_len]=0;
        else if(k==KEY_ENTER){
            shell_scroll=0;
            shell_execute(command);
            command_len=0;
            command[0]=0;
            if(selected>=0&&!vfs_files[selected].used)selected=-1;
            if(shell_action){
                int map[]={
                    0,1,2,3,4
                };
                if(shell_action>=1&&shell_action<=4)open_app(map[shell_action]);
                shell_action=0;
            }if(note_file>=0&&vfs_files[note_file].used){
                strcopy(note,vfs_files[note_file].data,sizeof note);
                note_len=strlen(note);
            }else note_file=-1;
        }else if(k>=32&&k<127&&command_len<(int)sizeof(command)-1){
            command[command_len++]=(char)k;
            command[command_len]=0;
        }
    }else if(a==2){
        if(note_file<0||!vfs_files[note_file].used){
            note_file=vfs_find("notes.txt");
            if(note_file<0)note_file=vfs_create("notes.txt");
            if(note_file>=0){
                strcopy(note,vfs_files[note_file].data,sizeof note);
                note_len=strlen(note);
            }
        }
        if(note_file<0){note_error=true;serial_write("[ui] Notes: RAM directory full\n");dirty=true;return;}
        note_error=false;
        if(k==KEY_BACKSPACE&&note_len)note[--note_len]=0;
        else if(note_len<sizeof(note)-1){
            if(k==KEY_ENTER)note[note_len++]='\n';
            else if(k>=32&&k<127)note[note_len++]=(char)k;
            note[note_len]=0;
        }if(note_file>=0)vfs_write(note_file,note);
    }else if(a==1){
        if(k==KEY_ENTER&&selected>=0)load_note(selected);
        if(k==KEY_UP||k==KEY_DOWN){
            int start=selected;
            for(int n=0;n<VFS_MAX_FILES;n++){
                start=(start+(k==KEY_DOWN?1:VFS_MAX_FILES-1))%VFS_MAX_FILES;
                if(vfs_files[start].used){
                    selected=start;
                    break;
                }
            }
        }
    }dirty=true;
} void kernel_main(uint32_t magic,uint32_t mb_addr){
    platform_init(magic,mb_addr,&boot);
    sw=(int)boot.width;
    sh=(int)boot.height;
    if(sw<800||sh<600||sw>MAX_W||sh>MAX_H){
        serial_write("[fatal] display must be 800x600 through 1920x1200\n");
        for(;;)platform_idle();
    }theme();
    make_wallpaper();
    vfs_init();
    shell_init(&boot);
    for(int i=0;i<APP_COUNT;i++)wins[i]=(Window){
        70+i*35,90+i*18,620,430,0,0,0,0,false,false
    };
    wins[0]=(Window){
        42,137,550,438,0,0,0,0,true,false
    };
    wins[4]=(Window){
        sw-414,78,380,390,0,0,0,0,true,false
    };
    wins[1].h=480;
    wins[2].h=465;
    wins[3].w=440;
    wins[3].h=400;
    for(int i=0;i<APP_COUNT;i++){
        Window *w=&wins[i];
        w->w=min(w->w,sw-24);
        w->h=min(w->h,sh-142);
        w->x=max(12,min(w->x,sw-w->w-12));
        w->y=max(46,min(w->y,sh-96-w->h));
    }note_file=vfs_find("notes.txt");
    if(note_file>=0){
        strcopy(note,vfs_files[note_file].data,sizeof note);
        note_len=strlen(note);
    }selected=vfs_find("README.txt");
    mx=sw/2;
    my=sh/2;
    serial_write("[arkos] desktop ready\n");
    render();
    uint64_t last=platform_ticks();
    for(;;){
        Event ev;
        int n=0;
        while(n++<64&&platform_next_event(&ev)){
            if(ev.type==EV_KEY)key_event(ev.key);
            else if(ev.type==EV_MOUSE)mouse_event(&ev);
        }uint64_t now=platform_ticks();
        if(now/50!=last/50)dirty=true;
        last=now;
        if(dirty)render();
        platform_idle();
    }
}
