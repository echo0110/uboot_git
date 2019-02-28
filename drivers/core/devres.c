/*
 * Copyright (C) 2015 Masahiro Yamada <yamada.masahiro@socionext.com>
 *
 * Based on the original work in Linux by
 * Copyright (c) 2006  SUSE Linux Products GmbH
 * Copyright (c) 2006  Tejun Heo <teheo@suse.de>
 *
 * SPDX-License-Identifier:	GPL-2.0+
 */

#include <common.h>
#include <linux/compat.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <dm/device.h>
#include <dm/root.h>
#include <dm/util.h>

/**
 * struct devres - Bookkeeping info for managed device resource
 * @entry: List to associate this structure with a device
 * @release: Callback invoked when this resource is released
 * @probe: Flag to show when this resource was allocated
	   (true = probe, false = bind)
 * @name: Name of release function
 * @size: Size of resource data
 * @data: Resource data
 */
struct devres {
	struct list_head		entry;
	dr_release_t			release;
	bool				probe;
#ifdef CONFIG_DEBUG_DEVRES
	const char			*name;
	size_t				size;
#endif
	unsigned long long		data[];
};

#ifdef CONFIG_DEBUG_DEVRES
static void set_node_dbginfo(struct devres *dr, const char *name, size_t size)
{
	dr->name = name;
	dr->size = size;
}

static void devres_log(struct udevice *dev, struct devres *dr,
		       const char *op)
{
	printf("%s: DEVRES %3s %p %s (%lu bytes)\n",
	       dev->name, op, dr, dr->name, (unsigned long)dr->size);
}
#else /* CONFIG_DEBUG_DEVRES */
#define set_node_dbginfo(dr, n, s)	do {} while (0)
#define devres_log(dev, dr, op)		do {} while (0)
#endif

#if CONFIG_DEBUG_DEVRES
void *__devres_alloc(dr_release_t release, size_t size, gfp_t gfp,
		     const char *name)
#else
void *_devres_alloc(dr_release_t release, size_t size, gfp_t gfp)
#endif
{
	size_t tot_size = sizeof(struct devres) + size;
	struct devres *dr;

	dr = kmalloc(tot_size, gfp);
	if (unlikely(!dr))
		return NULL;

	INIT_LIST_HEAD(&dr->entry);
	dr->release = release;
	set_node_dbginfo(dr, name, size);

	return dr->data;
}

void devres_free(void *res)
{
	if (res) {
		struct devres *dr = container_of(res, struct devres, data);

		BUG_ON(!list_empty(&dr->entry));
		kfree(dr);
	}
}

void devres_add(struct udevice *dev, void *res)
{
	struct devres *dr = container_of(res, struct devres, data);

	devres_log(dev, dr, "ADD");
	BUG_ON(!list_empty(&dr->entry));
	dr->probe = dev->flags & DM_FLAG_BOUND ? true : false;
	list_add_tail(&dr->entry, &dev->devres_head);
}

void *devres_find(struct udevice *dev, dr_release_t release,
		  dr_match_t match, void *match_data)
{
	struct devres *dr;

	list_for_each_entry_reverse(dr, &dev->devres_head, entry) {
		if (dr->release != release)
			continue;
		if (match && !match(dev, dr->data, match_data))
			continue;
		return dr->data;
	}

	return NULL;
}

void *devres_get(struct udevice *dev, void *new_res,
		 dr_match_t match, void *match_data)
{
	struct devres *new_dr = container_of(new_res, struct devres, data);
	void *res;

	res = devres_find(dev, new_dr->release, match, match_data);
	if (!res) {
		devres_add(dev, new_res);
		res = new_res;
		new_res = NULL;
	}
	devres_free(new_res);

	return res;
}

void *devres_remove(struct udevice *dev, dr_release_t release,
		    dr_match_t match, void *match_data)
{
	void *res;

	res = devres_find(dev, release, match, match_data);
	if (res) {
		struct devres *dr = container_of(res, struct devres, data);

		list_del_init(&dr->entry);
		devres_log(dev, dr, "REM");
	}

	return res;
}

int devres_destroy(struct udevice *dev, dr_release_t release,
		   dr_match_t match, void *match_data)
{
	void *res;

	res = devres_remove(dev, release, match, match_data);
	if (unlikely(!res))
		return -ENOENT;

	devres_free(res);
	return 0;
}

int devres_release(struct udevice *dev, dr_release_t release,
		   dr_match_t match, void *match_data)
{
	void *res;

	res = devres_remove(dev, release, match, match_data);
	if (unlikely(!res))
		return -ENOENT;

	(*release)(dev, res);
	devres_free(res);
	return 0;
}

static void release_nodes(struct udevice *dev, struct list_head *head,
			  bool probe_only)
{
	struct devres *dr, *tmp;

	list_for_each_entry_safe_reverse(dr, tmp, head, entry)  {
		if (probe_only && !dr->probe)
			break;
		devres_log(dev, dr, "REL");
		dr->release(dev, dr->data);
		list_del(&dr->entry);
		kfree(dr);
	}
}

void devres_release_probe(struct udevice *dev)
{
	release_nodes(dev, &dev->devres_head, true);
}

void devres_release_all(struct udevice *dev)
{
	release_nodes(dev, &dev->devres_head, false);
}

#ifdef CONFIG_DEBUG_DEVRES
static void dump_resources(struct udevice *dev, int depth)
{
	struct devres *dr;
	struct udevice *child;

	printf("- %s\n", dev->name);

	list_for_each_entry(dr, &dev->devres_head, entry)
		printf("    %p (%lu byte) %s  %s\n", dr,
		       (unsigned long)dr->size, dr->name,
		       dr->probe ? "PROBE" : "BIND");

	list_for_each_entry(child, &dev->child_head, sibling_node)
		dump_resources(child, depth + 1);
}

void dm_dump_devres(void)
{
	struct udevice *root;

	root = dm_root();
	if (root)
		dump_resources(root, 0);
}
#endif

/*
 * Managed kmalloc/kfree
 */
static void devm_kmalloc_release(struct udevice *dev, void *res)
{
	/* noop */
}

static int devm_kmalloc_match(struct udevice *dev, void *res, void *data)
{
	return res == data;
}

void *devm_kmalloc(struct udevice *dev, size_t size, gfp_t gfp)
{
	void *data;

	data = _devres_alloc(devm_kmalloc_release, size, gfp);
	if (unlikely(!data))
		return NULL;

	devres_add(dev, data);

	return data;
}

void devm_kfree(struct udevice *dev, void *p)
{
	int rc;

	rc = devres_destroy(dev, devm_kmalloc_release, devm_kmalloc_match, p);
	WARN_ON(rc);
}
