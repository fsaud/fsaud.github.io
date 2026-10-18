#include <stdlib.h>

int main()
{

    void* p = malloc(15);
    void *q = malloc(31);
    void *r = malloc(120);
    if (!p || !q || !r) {
        return 1;
    }

    free(p);
    free(q);
    free(r);
    p = q =r = NULL;
    p = malloc(16);
    q = malloc(32);
    r = malloc(128);
    volatile int cp = 0;
    return cp;
}