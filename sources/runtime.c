/* SPDX-License-Identifier: MIT. Compiler helpers; ordinary RAM only. */
#include <stddef.h>
void *memset(void *dst,int value,size_t size) {
    volatile unsigned char *p=dst;
    for(size_t i=0;i<size;++i)p[i]=(unsigned char)value;
    return dst;
}
void *memcpy(void *dst,const void *src,size_t size) {
    volatile unsigned char *d=dst;
    const volatile unsigned char *s=src;
    for(size_t i=0;i<size;++i)d[i]=s[i];
    return dst;
}
