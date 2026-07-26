/*
 * utils/json.h - parser JSON minimo (variante minificada de jsmn)
 *
 * NOMBRE
 *     json.h - tokenizador JSON de una sola pasada, sin asignacion
 *     dinamica de memoria
 *
 * DESCRIPCION
 *     Implementacion basada en jsmn (jsmn.h, Serge Zaitsev, licencia
 *     MIT), minificada a una funcion por linea. No arma un arbol ni
 *     hace copias: jsmn_parse() solo llena un arreglo de jsmntok_t con
 *     offsets (start/end) hacia el string JSON original, que el llamador
 *     debe mantener vivo mientras use los tokens (ver
 *     http_parse_json_body en utils/http/http.h).
 *
 *     Flujo tipico de uso:
 *       1. jsmn_init(&parser)
 *       2. jsmn_parse(&parser, json_str, len, tokens, max_tokens)
 *       3. Recorrer tokens[] comparando claves con json_key_eq()
 *          (utils/http/http.h)
 *
 *     No se reformatea el cuerpo de las funciones (una linea cada una):
 *     es codigo denso pero probado, y reformatearlo aumenta el riesgo de
 *     introducir un bug en el tokenizador sin aportar claridad real.
 */
#ifndef JSMN_H
#define JSMN_H
#include<stddef.h>

/* Tipo de cada token: objeto, arreglo, string o valor primitivo
 * (numero, true/false/null). JSMN_UNDEFINED marca un token sin usar. */
typedef enum{JSMN_UNDEFINED=0,JSMN_OBJECT=1,JSMN_ARRAY=2,JSMN_STRING=4,JSMN_PRIMITIVE=8}jsmntype_t;

/* Codigos de error de jsmn_parse: sin espacio en el arreglo de tokens,
 * JSON invalido, o JSON incompleto (cortado a mitad de un token). */
enum jsmnerr{JSMN_ERROR_NOMEM=-1,JSMN_ERROR_INVAL=-2,JSMN_ERROR_PART=-3};

/* Un token: su tipo y el rango [start,end) que ocupa dentro del string
 * JSON original (nunca copia el contenido). 'size' es la cantidad de
 * hijos directos (pares clave/valor si es objeto, elementos si es
 * arreglo). */
typedef struct{jsmntype_t type;int start,end,size;}jsmntok_t;

/* Estado del parser entre llamadas: posicion actual en el string
 * (pos), siguiente slot libre en el arreglo de tokens (toknext), e
 * indice del token contenedor abierto mas reciente (toksuper, -1 si
 * ninguno). */
typedef struct{unsigned pos,toknext;int toksuper;}jsmn_parser;

/* jsmn_init - reinicia un jsmn_parser para empezar un parseo nuevo. */
static inline void jsmn_init(jsmn_parser*p){p->pos=p->toknext=0;p->toksuper=-1;}

/* jsmn_alloc_token - toma el siguiente token libre de t[], o NULL si ya
 * no hay espacio (n tokens de capacidad). */
static inline jsmntok_t*jsmn_alloc_token(jsmn_parser*p,jsmntok_t*t,size_t n){
if(p->toknext>=n)return NULL;
jsmntok_t*k=&t[p->toknext++];k->start=k->end=-1;k->size=0;return k;}

/* jsmn_parse_primitive - consume un valor primitivo (numero, true,
 * false, null) a partir de p->pos, hasta el separador que lo termina
 * (whitespace, ',', ']' o '}'). Retorna 0 en exito, JSMN_ERROR_NOMEM si
 * no hay token libre, o error si aparece un byte de control invalido. */
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

/* jsmn_parse_string - consume un string entre comillas dobles a partir
 * de p->pos (que debe apuntar a la comilla de apertura), validando las
 * secuencias de escape soportadas (\", \\, \/, \b, \f, \r, \n, \t,
 * \uXXXX). Retorna 0 en exito, o un codigo de error si el string queda
 * sin cerrar o trae un escape invalido. */
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

/*
 * jsmn_parse - tokeniza un documento JSON completo
 *
 * Recorre el string j (longitud l) delimitando objetos, arreglos,
 * strings y primitivos, delegando en jsmn_parse_string /
 * jsmn_parse_primitive segun el caracter actual. Si t es NULL, solo
 * cuenta cuantos tokens harian falta (util para reservar el arreglo del
 * tamano exacto antes de una segunda pasada real).
 *
 * Parametros:
 *   p - parser inicializado con jsmn_init (se reutiliza entre llamadas
 *       si se quiere parsear en varios pedazos)
 *   j - string JSON de entrada (no necesita terminador NUL si l lo cubre)
 *   l - longitud de j en bytes
 *   t - arreglo de tokens de salida, o NULL para solo contar
 *   n - capacidad de t
 *
 * Retorna:
 *   cantidad de tokens de nivel superior consumidos (>= 0) en exito, o
 *   un codigo negativo de jsmnerr en error (JSON invalido, incompleto, o
 *   sin espacio en t).
 */
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
