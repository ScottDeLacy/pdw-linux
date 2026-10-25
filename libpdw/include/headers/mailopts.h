// SMTP option bits stored in pdw.ini ("Options="), kept identical to the original so
// existing settings files keep their meaning. Mail sending itself: see support.cpp.
#pragma once
#include "compat/windows.h"

#define MAIL_OPTION_MODE_ALL 0x000000
#define MAIL_OPTION_MODE_FILTER 0x000001
#define MAIL_OPTION_MODE_MONITOR 0x000002
#define MAIL_OPTION_MODE_SELECTABLE 0x000003
#define MAIL_OPTION_MODES 0x00000F
#define MAIL_OPTION_ADDRESS 0x000010
#define MAIL_OPTION_TIME 0x000020
#define MAIL_OPTION_DATE 0x000040
#define MAIL_OPTION_MODE 0x000080
#define MAIL_OPTION_TYPE 0x000100
#define MAIL_OPTION_BITRATE 0x000200
#define MAIL_OPTION_MESSAGE 0x000400
#define MAIL_OPTION_LABEL 0x000800
#define MAIL_OPTION_SUBJECT 0x001000
#define MAIL_OPTION_MSG 0x002000
#define MAIL_OPTION_AUTH 0x004000
#define MAIL_OPTION_ENABLE 0x008000
#define MAIL_OPTION_SSL 0x200000

int MailInit(char *host, char *helo_domain, char *from, char *to, char *user, char *password, int port,
             int options);
int SendMail(HWND response, bool match, bool monitor_only, int separate_smtp, char *capcode, char *time,
             char *date, char *mode, char *type, char *bitrate, char *message, char *label);
void MailFlush(int timeout_ms);  // wait for queued mails to be sent
