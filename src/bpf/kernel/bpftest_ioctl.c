
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/ioctl.h>
#include <linux/string.h>
#include <linux/ktime.h>

#include "bpftest.h"

struct filter_stats {
	u64 filtered_packets;
	u64 true_filtered;
	u64 start_usec;
	u64 end_usec;
};

struct bpftest_ioctl_bpf_info {
	u16 insns_len;
	u64 insns;
	u32 pkts_descs_count;
	u32 pkts_descs_len;
	u64 pkts_descs;
	u8  type;
	u8  jit_enable;
	u32 tests_count;
	u64 stats;
};

#define NEXT_PKT(desc) (struct bpftest_pkt_desc*)((u8*)desc + desc->offset_to_next)

#define IOCTL_DEVICE_NAME "bpftest"
#define IOCTL_CLASS_NAME "control"

#define BPFTEST_IOC_MAGIC 'b'

#define BPFTEST_IOCTL_RUN_BPF _IOWR(BPFTEST_IOC_MAGIC, 0, struct bpftest_ioctl_bpf_info)

#define BPFTEST_IOC_MAXNR 1

static int ioctl_major_number;
static struct class* ioctl_class;
static struct device* ioctl_device;
static struct cdev ioctl_cdev;


static int bpftest_ioctl_fops_open(struct inode *inodep, struct file *filep) {
	return 0;
}

static int bpftest_ioctl_fops_release(struct inode *inodep, struct file *filep) {
	return 0;
}

static long bpftest_ioctl_fops_exec(struct file* f, unsigned int cmd,
		unsigned long arg) {
	int ret = 0;
	struct bpftest_ioctl_bpf_info bpf_info;
	void* insns = NULL;
	size_t size_insn = 0;
	struct bpftest_bpf bpf;
	struct bpftest_shared_mem mem;
	struct bpftest_pkt_desc* desc = NULL;
	struct filter_stats* stats;

	if (_IOC_TYPE(cmd) != BPFTEST_IOC_MAGIC) {
		PRINT_ERROR("Unknown ioctl magic number");
		return -ENOTTY;
	}

	if (_IOC_NR(cmd) > BPFTEST_IOC_MAXNR) {
		PRINT_ERROR("Bad ioctl command");
		return -ENOTTY;
	}

	switch (cmd) {
		case BPFTEST_IOCTL_RUN_BPF:
			if (copy_from_user(&bpf_info, (void __user *)arg, sizeof(bpf_info)))
				return -EFAULT;

			if (unlikely(bpf_info.insns_len <= 0))
				return -EINVAL;

			bpf.type = bpf_info.type;
			size_insn = (bpf_info.type == BPFTEST_FILTER) ? sizeof(struct sock_filter) :
					sizeof(struct bpf_insn);

			insns = kmalloc_array(bpf_info.insns_len, size_insn, GFP_KERNEL);
			if (!insns) {
				PRINT_ERROR("Error of create instructions array");
				return -ENOMEM;
			}

			stats = kmalloc_array(bpf_info.tests_count, sizeof(struct filter_stats), GFP_KERNEL);
			if (!stats) {
				PRINT_ERROR("Error of create stats array");
				kfree(insns);
				return -ENOMEM;
			}

			pr_debug("Create filter\n");
			pr_debug("Type: %d\n", bpf_info.type);
			pr_debug("Len: %d\n", bpf_info.insns_len);
			pr_debug("Size: %lu\n", sizeof(bpf_info));

			if (copy_from_user(insns, (void __user *)bpf_info.insns, bpf_info.insns_len * size_insn)) {
				PRINT_ERROR("Error of copy instructions array");
				kfree(insns);
				return -EFAULT;
			}

			ret = bpftest_map_mem((void __user *)bpf_info.pkts_descs, bpf_info.pkts_descs_len, &mem);
			if (ret < 0) {
				PRINT_ERROR("Error of get shared mem");
				kfree(insns);
				return -EFAULT;
			}

			desc = (struct bpftest_pkt_desc*)mem.addr;

			ret = bpftest_create_bpf(&bpf, insns, bpf_info.insns_len, bpf_info.jit_enable);
			kfree(insns);
			if (ret < 0) {
				PRINT_ERROR("Error of create bpf");
				break;
			}

			if (bpftest_is_jitted_bpf(&bpf) != bpf_info.jit_enable) {
				bpftest_unmap_mem(&mem);
				bpftest_remove_bpf(&bpf);
				PRINT_ERROR("Error of settings jit");
				return -EINVAL;
			}

			for (u32 i = 0; i < bpf_info.tests_count; ++i) {
				struct filter_stats stat = {0, 0, 0, 0};
				stat.start_usec = ktime_get() / 1000;
				stat.filtered_packets = bpf_info.pkts_descs_count;
				for (u32 i = 0; i < bpf_info.pkts_descs_count; ++i) {
					if (bpftest_run_bpf(&bpf, desc))
						++stat.true_filtered;
					desc = NEXT_PKT(desc);
				}
				stat.end_usec = ktime_get() / 1000;
				stats[i] = stat;
			}
			
			bpftest_unmap_mem(&mem);

			ret = bpftest_remove_bpf(&bpf);
			if (ret < 0) {
				PRINT_ERROR("Error of remove bpf");
				break;
			}

			if (copy_to_user((void __user *)bpf_info.stats, stats, bpf_info.tests_count * sizeof(struct filter_stats)))
				return -EFAULT;

			kfree(stats);

			if (copy_to_user((void __user *)arg, &bpf_info, sizeof(bpf_info)))
				return -EFAULT;

			break;
		default:
			PRINT_ERROR("Unknown ioctl command");
			ret = -ENOTTY;
			break;
	}

	return ret;
}

static struct file_operations fops = {
	.open = bpftest_ioctl_fops_open,
	.release = bpftest_ioctl_fops_release,
	.unlocked_ioctl = bpftest_ioctl_fops_exec
};

int bpftest_init_ioctl(void) {
  ioctl_major_number = register_chrdev(0, IOCTL_DEVICE_NAME, &fops);
	if (ioctl_major_number < 0) {
		pr_alert("bpftest: Device registration error\n");
		return ioctl_major_number;
	}

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 4, 0)
	ioctl_class = class_create(THIS_MODULE, IOCTL_CLASS_NAME);
#else
	ioctl_class = class_create(IOCTL_CLASS_NAME);
#endif
	if (IS_ERR(ioctl_class)) {
		unregister_chrdev(ioctl_major_number, IOCTL_DEVICE_NAME);
		pr_alert("bpftest: Device class creation error\n");
		return PTR_ERR(ioctl_class);
	}

	ioctl_device = device_create(ioctl_class, NULL,
								  MKDEV(ioctl_major_number, 0), NULL, IOCTL_DEVICE_NAME);
	if (IS_ERR(ioctl_device)) {
		class_destroy(ioctl_class);
		unregister_chrdev(ioctl_major_number, IOCTL_DEVICE_NAME);
		pr_alert("bpftest: Device creation error\n");
		return PTR_ERR(ioctl_device);
	}

	cdev_init(&ioctl_cdev, &fops);
	ioctl_cdev.owner = THIS_MODULE;
	if (cdev_add(&ioctl_cdev, MKDEV(ioctl_major_number, 0), 1) < 0) {
		device_destroy(ioctl_class, MKDEV(ioctl_major_number, 0));
		class_destroy(ioctl_class);
		unregister_chrdev(ioctl_major_number, IOCTL_DEVICE_NAME);
		pr_alert("bpftest: Addding cdev error\n");
		return -1;
	}
  return 0;
}

void bpftest_deinit_ioctl(void) {
  cdev_del(&ioctl_cdev);
	device_destroy(ioctl_class, MKDEV(ioctl_major_number, 0));
	class_destroy(ioctl_class);
	unregister_chrdev(ioctl_major_number, IOCTL_DEVICE_NAME);
}

