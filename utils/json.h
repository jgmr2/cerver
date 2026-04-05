#ifndef JSMN_H
#define JSMN_H
#include<stddef.h>

typedef enum{JSMN_UNDEFINED=0,JSMN_OBJECT=1,JSMN_ARRAY=2,JSMN_STRING=4,JSMN_PRIMITIVE=8}jsmntype_t;
enum jsmnerr{JSMN_ERROR_NOMEM=-1,JSMN_ERROR_INVAL=-2,JSMN_ERROR_PART=-3};
typedef struct{jsmntype_t type;int start,end,size;}jsmntok_t;
typedef struct{unsigned pos,toknext;int toksuper;}jsmn_parser;

static inline void jsmn_init(jsmn_parser*p){p->pos=p->toknext=0;p->toksuper=-1;}

static inline jsmntok_t*jsmn_alloc_token(jsmn_parser*p,jsmntok_t*t,size_t n){
if(p->toknext>=n)return NULL;
jsmntok_t*k=&t[p->toknext++];k->start=k->end=-1;k->size=0;return k;}

static inline int jsmn_parse_primitive(jsmn_parser*p,const char*j,size_t l,jsmntok_t*t,size_t n){
int s=p->pos;
for(;p->pos<l&&j[p->pos];p->pos++){
char c=j[p->pos];
if(c=='\t'||c=='\r'||c=='\n'||c==' '||c==','||c==']'||c=='}')goto f;
if(c<32||c>=127)return p->pos=s,-2;}
f:if(!t)return p->pos--,0;
jsmntok_t*k=jsmn_alloc_token(p,t,n);
if(!k)return p->pos=s,-1;
k->type=JSMN_PRIMITIVE;k->start=s;k->end=p->pos--;return 0;}

static inline int jsmn_parse_string(jsmn_parser*p,const char*j,size_t l,jsmntok_t*t,size_t n){
int s=p->pos++;
for(;p->pos<l&&j[p->pos];p->pos++){
char c=j[p->pos];
if(c=='\"'){if(!t)return 0;jsmntok_t*k=jsmn_alloc_token(p,t,n);if(!k)return p->pos=s,-1;k->type=JSMN_STRING;k->start=s+1;k->end=p->pos;return 0;}
if(c=='\\'&&p->pos+1<l){
char y=j[++p->pos];
if(y=='u'){p->pos++;for(int i=0;i<4&&p->pos<l&&j[p->pos];i++,p->pos++){
char x=j[p->pos];if(!((x>=48&&x<=57)||(x>=65&&x<=70)||(x>=97&&x<=102)))return p->pos=s,-2;}p->pos--;}
else if(y!='\"'&&y!='/'&&y!='\\'&&y!='b'&&y!='f'&&y!='r'&&y!='n'&&y!='t')return p->pos=s,-2;}}
return p->pos=s,-3;}

static inline int jsmn_parse(jsmn_parser*p,const char*j,size_t l,jsmntok_t*t,unsigned n){
int r,i,c=p->toknext;jsmntok_t*k;
for(;p->pos<l&&j[p->pos];p->pos++){
char x=j[p->pos];jsmntype_t y;
if(x=='{'||x=='['){c++;if(!t)continue;if(!(k=jsmn_alloc_token(p,t,n)))return -1;if(p->toksuper!=-1)t[p->toksuper].size++;k->type=(x=='{'?JSMN_OBJECT:JSMN_ARRAY);k->start=p->pos;p->toksuper=p->toknext-1;}
else if(x=='}'||x==']'){if(!t)continue;y=(x=='}'?JSMN_OBJECT:JSMN_ARRAY);for(i=p->toknext-1;i>=0;i--)if((k=&t[i])->start!=-1&&k->end==-1){if(k->type!=y)return -2;p->toksuper=-1;k->end=p->pos+1;break;}if(i==-1)return -2;for(;i>=0;i--)if(t[i].start!=-1&&t[i].end==-1){p->toksuper=i;break;}}
else if(x=='\"'){if((r=jsmn_parse_string(p,j,l,t,n))<0)return r;c++;if(p->toksuper!=-1&&t)t[p->toksuper].size++;}
else if(x==':')p->toksuper=p->toknext-1;
else if(x==','){if(t&&p->toksuper!=-1&&t[p->toksuper].type!=JSMN_ARRAY&&t[p->toksuper].type!=JSMN_OBJECT)for(i=p->toknext-1;i>=0;i--)if((t[i].type==JSMN_ARRAY||t[i].type==JSMN_OBJECT)&&t[i].start!=-1&&t[i].end==-1){p->toksuper=i;break;}}
else if(x!='\t'&&x!='\r'&&x!='\n'&&x!=' '){if((r=jsmn_parse_primitive(p,j,l,t,n))<0)return r;c++;if(p->toksuper!=-1&&t)t[p->toksuper].size++;}}
if(t)for(i=p->toknext-1;i>=0;i--)if(t[i].start!=-1&&t[i].end==-1)return -3;return c;}

#endif