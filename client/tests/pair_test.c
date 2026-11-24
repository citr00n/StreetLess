#include "pair.h"
#include <assert.h>
#include <string.h>

int main(void) {
    PairData p;
    const char *ok="streetless1|relay.example.org|192.0.2.1|443|0123456789abcdef0123456789abcdef";
    assert(pair_parse((const unsigned char *)ok,strlen(ok),&p));
    assert(!strcmp(p.config.host,"relay.example.org") && !strcmp(p.config.ipv4,"192.0.2.1"));
    assert(p.config.port==443 && !strcmp(p.code,"0123456789abcdef0123456789abcdef"));
    const char *bad[]={
        "https://relay.example.org|192.0.2.1|443|0123456789abcdef0123456789abcdef",
        "streetless1|user@relay.example.org|192.0.2.1|443|0123456789abcdef0123456789abcdef",
        "streetless1|relay.example.org|127.0.0.999|443|0123456789abcdef0123456789abcdef",
        "streetless1|relay.example.org|192.0.2.1|0|0123456789abcdef0123456789abcdef",
        "streetless1|relay.example.org|192.0.2.1|443|0123456789ABCDEF0123456789abcdef",
        "streetless1|relay.example.org|192.0.2.1|443|0123456789abcdef0123456789abcdef|extra",
    };
    for(size_t i=0;i<sizeof(bad)/sizeof(bad[0]);++i)
        assert(!pair_parse((const unsigned char *)bad[i],strlen(bad[i]),&p));
    unsigned char embedded[]="streetless1|relay.example.org|192.0.2.1|443|0123456789abcdef0123456789abcdef";
    embedded[15]=0;
    assert(!pair_parse(embedded,sizeof(embedded)-1,&p));
}
