#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Mehmet Furkan Geçkil");
MODULE_DESCRIPTION("Module for visualizing process trees");

static int pid = 1; // Default to PID 1 (init process)

module_param(pid, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(pid, "PID of the root process");

static void print_process_tree(struct task_struct *task, int level) {
    struct task_struct *child;
    struct list_head *list;
    
    printk(KERN_INFO "%*s%s [%d]\n", level * 2, "", task->comm, task->pid);
    list_for_each(list, &task->children) {
        child = list_entry(list, struct task_struct, sibling);
        print_process_tree(child, level + 1);
    }
}

static int __init psvis_init(void) {
    struct pid *pid_struct;
    struct task_struct *task;
    
    printk(KERN_INFO "Loading psvis Module for PID: %d\n", pid);
    pid_struct = find_get_pid(pid);
    task = pid_task(pid_struct, PIDTYPE_PID);
    if (task) {
        print_process_tree(task, 0);
    } else {
        printk(KERN_INFO "No such PID: %d\n", pid);
    }
    return 0;
}

static void __exit psvis_exit(void) {
    printk(KERN_INFO "Removing psvis Module\n");
}

module_init(psvis_init);
module_exit(psvis_exit);
