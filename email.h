#ifndef EMAIL
#define EMAIL

typedef struct email_s
{
    int last_index;
    int exhausted;
    int yield_if_empty_chain;  /* yield body even if no separator found (single email) */
    char *body;
} email_t;

#endif
