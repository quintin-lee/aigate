/** @file main.c
 *  @brief aigate process entry point.
 *
 *  Task 1 placeholder: the full boot sequence (config, PG, core, civetweb,
 *  signal handling) is wired in task 11.
 */
#include "aigate_log.h"

int
main(void)
{
    AIGATE_LOG_INFO("aigate bootstrap OK");
    return 0;
}
