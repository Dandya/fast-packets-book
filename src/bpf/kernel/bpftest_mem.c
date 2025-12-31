
#include <linux/vmalloc.h>
#include <linux/uaccess.h>
#include <linux/mm.h>

#include "bpftest.h"

int bpftest_map_mem(void __user* addr, unsigned long size, struct bpftest_shared_mem* desc) {
	int ret = 0;
	int nr_pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
	struct page** pages = NULL;
	void* vaddr = NULL;
	struct mm_struct* mm = current->mm;

	pr_debug("bpftest: Map memory: size (%lu), pages (%d)\n", size, nr_pages);

	if ((unsigned long)addr % PAGE_SIZE != 0) {
		PRINT_ERROR("The address is not aligned");
		return -EINVAL;
	}

	if (!access_ok(addr, size)) {
		PRINT_ERROR("The address is not access");
		return -EFAULT;
	}

	pages = vmalloc(nr_pages * sizeof(void*));
	if (!pages)
		return -ENOMEM;

	mmap_read_lock(mm);

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 5, 0)
	ret = pin_user_pages((unsigned long)addr, nr_pages, FOLL_LONGTERM, pages, NULL);
#else
	ret = pin_user_pages((unsigned long)addr, nr_pages, FOLL_LONGTERM, pages);
#endif

	mmap_read_unlock(mm);

	if (ret < nr_pages) {
		PRINT_ERROR("Error with pinning user memory");
		if (ret > 0)
			for (int i = 0; i < ret; ++i)
				unpin_user_page(pages[i]);
		vfree(pages);
		return -EFAULT;
	}

	vaddr = vmap(pages, nr_pages, VM_MAP, PAGE_KERNEL);
	if (!vaddr) {
		PRINT_ERROR("Error with mapping user memory");
		for (int i = 0; i < nr_pages; ++i)
			unpin_user_page(pages[i]);
		vfree(pages);
		return -EFAULT;
	}

	desc->addr = vaddr;
	desc->pages = pages;
	desc->pages_count = nr_pages;

	return 0;
}

void bpftest_unmap_mem(struct bpftest_shared_mem* desc) {
	if (unlikely(!desc))
		return;

	vunmap(desc->addr);
	desc->addr = NULL;
	for (int i = 0; i < desc->pages_count; ++i)
		unpin_user_page(desc->pages[i]);
	desc->pages_count = 0;
	vfree(desc->pages);
	desc->pages = NULL;
}
