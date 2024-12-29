#include <stdio.h>
#include <stdlib.h>
#include <syslog.h>

int main(int argc, char *argv[])
{
    openlog("writer", LOG_PID | LOG_CONS, LOG_USER);
    // check inputs arguements
    if (argc != 3)
    {
        printf("arguments error\n");
        syslog(LOG_ERR, "arguments error\n");
        exit(1);
    }

    // get arguements
    char *writefile = argv[1];
    char *writestr = argv[2];

    FILE *file = fopen(writefile, "w");
    if (!file)
    {
        printf("can not create file!\n");
        syslog(LOG_ERR, "can not create file!\n");
        exit(1);
    }
    int stat = fprintf(file, "%s\n", writestr);
    if (!stat)
    {
        printf("failed to write %s in %s\n", writestr, writefile);
        syslog(LOG_ERR, "failed to write %s in %s\n", writestr, writefile);
        exit(1);
    }

    fclose(file);
    printf("Writing %s to %s", writestr, writefile);
    syslog(LOG_DEBUG, "Writing %s to %s", writestr, writefile);
    closelog();
    return 0;
}