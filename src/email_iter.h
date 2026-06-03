#ifndef EMAILPARSER_EMAIL_ITER_H
#define EMAILPARSER_EMAIL_ITER_H
#include "email.h"
#include <stdbool.h>

#ifndef SEPARATOR_REGEX
#define SEPARATOR_REGEX_GEN_EN "(From|Sent|To|Subject|Cc|Bcc) ?(&nbsp;:|:) ?"
#define SEPARATOR_REGEX_GEN_FR "(De|À|Envoyé|Objet|Cc|Cci) ?(&nbsp;:|:) ?"
#define SEPARATOR_REGEX_STA_ALL "(De|From) ?(&nbsp;:|:) ?"
#define SEPARATOR_REGEX_END_ALL "(Objet|Subject) ?(&nbsp;:|:) ?"
#define SEPARATOR_REGEX SEPARATOR_REGEX_STA_ALL
#endif

email_t *new_email(char *raw);
bool get_next_val(email_t *email);
#endif
