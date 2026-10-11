#include "ark.h"
void *memset(void *p,int v,size_t n){unsigned char *d=p;while(n--)*d++=(unsigned char)v;return p;}
void *memcpy(void *d,const void *s,size_t n){unsigned char *a=d;const unsigned char *b=s;while(n--)*a++=*b++;return d;}
void *memmove(void *d,const void *s,size_t n){unsigned char *a=d;const unsigned char *b=s;if(a<b){while(n--)*a++=*b++;}else if(a>b){while(n){--n;a[n]=b[n];}}return d;}
size_t strlen(const char *s){size_t n=0;while(s[n])++n;return n;}
int strcmp(const char *a,const char *b){while(*a&&*a==*b){++a;++b;}return (unsigned char)*a-(unsigned char)*b;}
int strncmp(const char *a,const char *b,size_t n){for(size_t i=0;i<n;i++){if(a[i]!=b[i])return (unsigned char)a[i]-(unsigned char)b[i];if(!a[i])return 0;}return 0;}
void strcopy(char *d,const char *s,size_t cap){if(!cap)return;size_t n=0;while(s[n]&&n+1<cap){d[n]=s[n];++n;}d[n]=0;}
void uint_to_str(uint64_t n,char *out){char b[24];int i=0,j=0;do{b[i++]=(char)('0'+n%10);n/=10;}while(n);while(i)out[j++]=b[--i];out[j]=0;}
