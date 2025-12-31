
// #include <linux/delay.h>

#include "bpftest.h"

static int __init bpftest_init(void) {
	int ret;

	ret = bpftest_init_ioctl();
	if (ret < 0) {
		PRINT_ERROR("Module init error");
		return ret;
	}
	pr_debug("bpftest: Module loaded\n");
	return 0;
}

static void __exit bpftest_exit(void) {
	bpftest_deinit_ioctl();
	pr_debug("bpftest: Module unloaded\n");
}

module_init(bpftest_init);
module_exit(bpftest_exit);
MODULE_LICENSE("GPL");
