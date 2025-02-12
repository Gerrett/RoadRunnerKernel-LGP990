/*
 *  linux/mm/vmscan.c
 *
 *  Copyright (C) 1991, 1992, 1993, 1994  Linus Torvalds
 *
 *  Swap reorganised 29.12.95, Stephen Tweedie.
 *  kswapd added: 7.1.96  sct
 *  Removed kswapd_ctl limits, and swap out as many pages as needed
 *  to bring the system back to freepages.high: 2.4.97, Rik van Riel.
 *  Zone aware kswapd started 02/00, Kanoj Sarcar (kanoj@sgi.com).
 *  Multiqueue VM started 5.8.00, Rik van Riel.
 */

#include <linux/mm.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/kernel_stat.h>
#include <linux/swap.h>
#include <linux/pagemap.h>
#include <linux/init.h>
#include <linux/highmem.h>
#include <linux/vmstat.h>
#include <linux/file.h>
#include <linux/writeback.h>
#include <linux/blkdev.h>
#include <linux/buffer_head.h>	/* for try_to_release_page(),
					buffer_heads_over_limit */
#include <linux/mm_inline.h>
#include <linux/pagevec.h>
#include <linux/backing-dev.h>
#include <linux/rmap.h>
#include <linux/topology.h>
#include <linux/cpu.h>
#include <linux/cpuset.h>
#include <linux/notifier.h>
#include <linux/rwsem.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/freezer.h>
#include <linux/memcontrol.h>
#include <linux/delayacct.h>
#include <linux/sysctl.h>

#include <asm/tlbflush.h>
#include <asm/div64.h>

#include <linux/swapops.h>

#include "internal.h"

#include <linux/vorkKernel.h>

struct scan_control {
	/* Incremented by the number of inactive pages that were scanned */
	unsigned long nr_scanned;

	/* Number of pages freed so far during a call to shrink_zones() */
	unsigned long nr_reclaimed;

	/* This context's GFP mask */
	gfp_t gfp_mask;

	int may_writepage;

	/* Can mapped pages be reclaimed? */
	int may_unmap;

	/* Can pages be swapped as part of reclaim? */
	int may_swap;

	/* This context's SWAP_CLUSTER_MAX. If freeing memory for
	 * suspend, we effectively ignore SWAP_CLUSTER_MAX.
	 * In this context, it doesn't matter that we scan the
	 * whole list at once. */
	int swap_cluster_max;

	int swappiness;

	int all_unreclaimable;

	int order;

	/* Which cgroup do we reclaim from */
	struct mem_cgroup *mem_cgroup;

	/*
	 * Nodemask of nodes allowed by the caller. If NULL, all nodes
	 * are scanned.
	 */
	nodemask_t	*nodemask;

	/* Pluggable isolate pages callback */
	unsigned long (*isolate_pages)(unsigned long nr, struct list_head *dst,
			unsigned long *scanned, int order, int mode,
			struct zone *z, struct mem_cgroup *mem_cont,
			int active, int file);
};

#define lru_to_page(_head) (list_entry((_head)->prev, struct page, lru))

#ifdef ARCH_HAS_PREFETCH
#define prefetch_prev_lru_page(_page, _base, _field)			\
	do {								\
		if ((_page)->lru.prev != _base) {			\
			struct page *prev;				\
									\
			prev = lru_to_page(&(_page->lru));		\
			prefetch(&prev->_field);			\
		}							\
	} while (0)
#else
#define prefetch_prev_lru_page(_page, _base, _field) do { } while (0)
#endif

#ifdef ARCH_HAS_PREFETCHW
#define prefetchw_prev_lru_page(_page, _base, _field)			\
	do {								\
		if ((_page)->lru.prev != _base) {			\
			struct page *prev;				\
									\
			prev = lru_to_page(&(_page->lru));		\
			prefetchw(&prev->_field);			\
		}							\
	} while (0)
#else
#define prefetchw_prev_lru_page(_page, _base, _field) do { } while (0)
#endif

/*
 * From 0 .. 100.  Higher means more swappy.
 */
int vm_swappiness = 60;
long vm_total_pages;	/* The total number of pages which the VM controls */

/*
 * Only start shrinking active file list when inactive is below this percentage.
 */
int inactive_file_ratio = inactive_file_ratio_default;

static LIST_HEAD(shrinker_list);
static DECLARE_RWSEM(shrinker_rwsem);

#ifdef CONFIG_CGROUP_MEM_RES_CTLR
#define scanning_global_lru(sc)	(!(sc)->mem_cgroup)
#else
#define scanning_global_lru(sc)	(1)
#endif

static struct zone_reclaim_stat *get_reclaim_stat(struct zone *zone,
						  struct scan_control *sc)
{
	if (!scanning_global_lru(sc))
		return mem_cgroup_get_reclaim_stat(sc->mem_cgroup, zone);

	return &zone->reclaim_stat;
}

static unsigned long zone_nr_lru_pages(struct zone *zone,
				struct scan_control *sc, enum lru_list lru)
{
	if (!scanning_global_lru(sc))
		return mem_cgroup_zone_nr_pages(sc->mem_cgroup, zone, lru);

	return zone_page_state(zone, NR_LRU_BASE + lru);
}


/*
 * Add a shrinker callback to be called from the vm
 */
void register_shrinker(struct shrinker *shrinker)
{
	shrinker->nr = 0;
	down_write(&shrinker_rwsem);
	list_add_tail(&shrinker->list, &shrinker_list);
	up_write(&shrinker_rwsem);
}
EXPORT_SYMBOL(register_shrinker);

/*
 * Remove one
 */
void unregister_shrinker(struct shrinker *shrinker)
{
	down_write(&shrinker_rwsem);
	list_del(&shrinker->list);
	up_write(&shrinker_rwsem);
}
EXPORT_SYMBOL(unregister_shrinker);

#define SHRINK_BATCH 128
/*
 * Call the shrink functions to age shrinkable caches
 *
 * Here we assume it costs one seek to replace a lru page and that it also
 * takes a seek to recreate a cache object.  With this in mind we age equal
 * percentages of the lru and ageable caches.  This should balance the seeks
 * generated by these structures.
 *
 * If the vm encountered mapped pages on the LRU it increase the pressure on
 * slab to avoid swapping.
 *
 * We do weird things to avoid (scanned*seeks*entries) overflowing 32 bits.
 *
 * `lru_pages' represents the number of on-LRU pages in all the zones which
 * are eligible for the caller's allocation attempt.  It is used for balancing
 * slab reclaim versus page reclaim.
 *
 * Returns the number of slab objects which we shrunk.
 */
unsigned long shrink_slab(unsigned long scanned, gfp_t gfp_mask,
			unsigned long lru_pages)
{
	struct shrinker *shrinker;
	unsigned long ret = 0;

	if (scanned == 0)
		scanned = SWAP_CLUSTER_MAX;

	if (!down_read_trylock(&shrinker_rwsem))
		return 1;	/* Assume we'll be able to shrink next time */

	list_for_each_entry(shrinker, &shrinker_list, list) {
		unsigned long long delta;
		unsigned long total_scan;
		unsigned long max_pass = (*shrinker->shrink)(0, gfp_mask);

		delta = (4 * scanned) / shrinker->seeks;
		delta *= max_pass;
		do_div(delta, lru_pages + 1);
		shrinker->nr += delta;
		if (shrinker->nr < 0) {
			printk(KERN_ERR "shrink_slab: %pF negative objects to "
			       "delete nr=%ld\n",
			       shrinker->shrink, shrinker->nr);
			shrinker->nr = max_pass;
		}

		/*
		 * Avoid risking looping forever due to too large nr value:
		 * never try to free more than twice the estimate number of
		 * freeable entries.
		 */
		if (shrinker->nr > max_pass * 2)
			shrinker->nr = max_pass * 2;

		total_scan = shrinker->nr;
		shrinker->nr = 0;

		while (total_scan >= SHRINK_BATCH) {
			long this_scan = SHRINK_BATCH;
			int shrink_ret;
			int nr_before;

			nr_before = (*shrinker->shrink)(0, gfp_mask);
			shrink_ret = (*shrinker->shrink)(this_scan, gfp_mask);
			if (shrink_ret == -1)
				break;
			if (shrink_ret < nr_before)
				ret += nr_before - shrink_ret;
			count_vm_events(SLABS_SCANNED, this_scan);
			total_scan -= this_scan;

			cond_resched();
		}

		shrinker->nr += total_scan;
	}
	up_read(&shrinker_rwsem);
	return ret;
}

/* Called without lock on whether page is mapped, so answer is unstable */
static inline int page_mapping_inuse(struct page *page)
{
	struct address_space *mapping;

	/* Page is in somebody's page tables. */
	if (page_mapped(page))
		return 1;

	/* Be more reluctant to reclaim swapcache than pagecache */
	if (PageSwapCache(page))
		return 1;

	mapping = page_mapping(page);
	if (!mapping)
		return 0;

	/* File is mmap'd by somebody? */
	return mapping_mapped(mapping);
}

static inline int is_page_cache_freeable(struct page *page)
{
	/*
	 * A freeable page cache page is referenced only by the caller
	 * that isolated the page, the page cache radix tree and
	 * optional buffer heads at page->private.
	 */
	return page_count(page) - page_has_private(page) == 2;
}

static int may_write_to_queue(struct backing_dev_info *bdi)
{
	if (current->flags & PF_SWAPWRITE)
		return 1;
	if (!bdi_write_congested(bdi))
		return 1;
	if (bdi == current->backing_dev_info)
		return 1;
	return 0;
}

/*
 * We detected a synchronous write error writing a page out.  Probably
 * -ENOSPC.  We need to propagate that into the address_space for a subsequent
 * fsync(), msync() or close().
 *
 * The tricky part is that after writepage we cannot touch the mapping: nothing
 * prevents it from being freed up.  But we have a ref on the page and once
 * that page is locked, the mapping is pinned.
 *
 * We're allowed to run sleeping lock_page() here because we know the caller has
 * __GFP_FS.
 */
static void handle_write_error(struct address_space *mapping,
				struct page *page, int error)
{
	lock_page_nosync(page);
	if (page_mapping(page) == mapping)
		mapping_set_error(mapping, error);
	unlock_page(page);
}

/* Request for sync pageout. */
enum pageout_io {
	PAGEOUT_IO_ASYNC,
	PAGEOUT_IO_SYNC,
};

/* possible outcome of pageout() */
typedef enum {
	/* failed to write page out, page is locked */
	PAGE_KEEP,
	/* move page to the active list, page is locked */
	PAGE_ACTIVATE,
	/* page has been sent to the disk successfully, page is unlocked */
	PAGE_SUCCESS,
	/* page is clean and locked */
	PAGE_CLEAN,
} pageout_t;

/*
 * pageout is called by shrink_page_list() for each dirty page.
 * Calls ->writepage().
 */
static pageout_t pageout(struct page *page, struct address_space *mapping,
						enum pageout_io sync_writeback)
{
	/*
	 * If the page is dirty, only perform writeback if that write
	 * will be non-blocking.  To prevent this allocation from being
	 * stalled by pagecache activity.  But note that there may be
	 * stalls if we need to run get_block().  We could test
	 * PagePrivate for that.
	 *
	 * If this process is currently in generic_file_write() against
	 * this page's queue, we can perform writeback even if that
	 * will block.
	 *
	 * If the page is swapcache, write it back even if that would
	 * block, for some throttling. This happens by accident, because
	 * swap_backing_dev_info is bust: it doesn't reflect the
	 * congestion state of the swapdevs.  Easy to fix, if needed.
	 */
	if (!is_page_cache_freeable(page))
		return PAGE_KEEP;
	if (!mapping) {
		/*
		 * Some data journaling orphaned pages can have
		 * page->mapping == NULL while being dirty with clean buffers.
		 */
		if (page_has_private(page)) {
			if (try_to_free_buffers(page)) {
				ClearPageDirty(page);
				printk("%s: orphaned page\n", __func__);
				return PAGE_CLEAN;
			}
		}
		return PAGE_KEEP;
	}
	if (mapping->a_ops->writepage == NULL)
		return PAGE_ACTIVATE;
	if (!may_write_to_queue(mapping->backing_dev_info))
		return PAGE_KEEP;

	if (clear_page_dirty_for_io(page)) {
		int res;
		struct writeback_control wbc = {
			.sync_mode = WB_SYNC_NONE,
			.nr_to_write = SWAP_CLUSTER_MAX,
			.range_start = 0,
			.range_end = LLONG_MAX,
			.nonblocking = 1,
			.for_reclaim = 1,
		};

		SetPageReclaim(page);
		res = mapping->a_ops->writepage(page, &wbc);
		if (res < 0)
			handle_write_error(mapping, page, res);
		if (res == AOP_WRITEPAGE_ACTIVATE) {
			ClearPageReclaim(page);
			return PAGE_ACTIVATE;
		}

		/*
		 * Wait on writeback if requested to. This happens when
		 * direct reclaiming a large contiguous area and the
		 * first attempt to free a range of pages fails.
		 */
		if (PageWriteback(page) && sync_writeback == PAGEOUT_IO_SYNC)
			wait_on_page_writeback(page);

		if (!PageWriteback(page)) {
			/* synchronous write or broken a_ops? */
			ClearPageReclaim(page);
		}
		inc_zone_page_state(page, NR_VMSCAN_WRITE);
		return PAGE_SUCCESS;
	}

	return PAGE_CLEAN;
}

/*
 * Same as remove_mapping, but if the page is removed from the mapping, it
 * gets returned with a refcount of 0.
 */
static int __remove_mapping(struct address_space *mapping, struct page *page)
{
	BUG_ON(!PageLocked(page));
	BUG_ON(mapping != page_mapping(page));

	spin_lock_irq(&mapping->tree_lock);
	/*
	 * The non racy check for a busy page.
	 *
	 * Must be careful with the order of the tests. When someone has
	 * a ref to the page, it may be possible that they dirty it then
	 * drop the reference. So if PageDirty is tested before page_count
	 * here, then the following race may occur:
	 *
	 * get_user_pages(&page);
	 * [user mapping goes away]
	 * write_to(page);
	 *				!PageDirty(page)    [good]
	 * SetPageDirty(page);
	 * put_page(page);
	 *				!page_count(page)   [good, discard it]
	 *
	 * [oops, our write_to data is lost]
	 *
	 * Reversing the order of the tests ensures such a situation cannot
	 * escape unnoticed. The smp_rmb is needed to ensure the page->flags
	 * load is not satisfied before that of page->_count.
	 *
	 * Note that if SetPageDirty is always performed via set_page_dirty,
	 * and thus under tree_lock, then this ordering is not required.
	 */
	if (!page_freeze_refs(page, 2))
		goto cannot_free;
	/* note: atomic_cmpxchg in page_freeze_refs provides the smp_rmb */
	if (unlikely(PageDirty(page))) {
		page_unfreeze_refs(page, 2);
		goto cannot_free;
	}

	if (PageSwapCache(page)) {
		swp_entry_t swap = { .val = page_private(page) };
		__delete_from_swap_cache(page);
		spin_unlock_irq(&mapping->tree_lock);
		swapcache_free(swap, page);
	} else {
		__remove_from_page_cache(page);
		spin_unlock_irq(&mapping->tree_lock);
		mem_cgroup_uncharge_cache_page(page);
	}

	return 1;

cannot_free:
	spin_unlock_irq(&mapping->tree_lock);
	return 0;
}

/*
 * Attempt to detach a locked page from its ->mapping.  If it is dirty or if
 * someone else has a ref on the page, abort and return 0.  If it was
 * successfully detached, return 1.  Assumes the caller has a single ref on
 * this page.
 */
int remove_mapping(struct address_space *mapping, struct page *page)
{
	if (__remove_mapping(mapping, page)) {
		/*
		 * Unfreezing the refcount with 1 rather than 2 effectively
		 * drops the pagecache ref for us without requiring another
		 * atomic operation.
		 */
		page_unfreeze_refs(page, 1);
		return 1;
	}
	return 0;
}

/**
 * putback_lru_page - put previously isolated page onto appropriate LRU list
 * @page: page to be put back to appropriate lru list
 *
 * Add previously isolated @page to appropriate LRU list.
 * Page may still be unevictable for other reasons.
 *
 * lru_lock must not be held, interrupts must be enabled.
 */
void putback_lru_page(struct page *page)
{
	int lru;
	int active = !!TestClearPageActive(page);
	int was_unevictable = PageUnevictable(page);

	VM_BUG_ON(PageLRU(page));

redo:
	ClearPageUnevictable(page);

	if (page_evictable(page, NULL)) {
		/*
		 * For evictable pages, we can use the cache.
		 * In event of a race, worst case is we end up with an
		 * unevictable page on [in]active list.
		 * We know how to handle that.
		 */
		lru = active + page_lru_base_type(page);
		lru_cache_add_lru(page, lru);
	} else {
		/*
		 * Put unevictable pages directly on zone's unevictable
		 * list.
		 */
		lru = LRU_UNEVICTABLE;
		add_page_to_unevictable_list(page);
		/*
		 * When racing with an mlock clearing (page is
		 * unlocked), make sure that if the other thread does
		 * not observe our setting of PG_lru and fails
		 * isolation, we see PG_mlocked cleared below and move
		 * the page back to the evictable list.
		 *
		 * The other side is TestClearPageMlocked().
		 */
		smp_mb();
	}

	/*
	 * page's status can change while we move it among lru. If an evictable
	 * page is on unevictable list, it never be freed. To avoid that,
	 * check after we added it to the list, again.
	 */
	if (lru == LRU_UNEVICTABLE && page_evictable(page, NULL)) {
		if (!isolate_lru_page(page)) {
			put_page(page);
			goto redo;
		}
		/* This means someone else dropped this page from LRU
		 * So, it will be freed or putback to LRU again. There is
		 * nothing to do here.
		 */
	}

	if (was_unevictable && lru != LRU_UNEVICTABLE)
		count_vm_event(UNEVICTABLE_PGRESCUED);
	else if (!was_unevictable && lru == LRU_UNEVICTABLE)
		count_vm_event(UNEVICTABLE_PGCULLED);

	put_page(page);		/* drop ref from isolate */
}

/*
 * shrink_page_list() returns the number of reclaimed pages
 */
static unsigned long shrink_page_list(struct list_head *page_list,
					struct scan_control *sc,
					enum pageout_io sync_writeback)
{
	LIST_HEAD(ret_pages);
	struct pagevec freed_pvec;
	int pgactivate = 0;
	unsigned long nr_reclaimed = 0;
	unsigned long vm_flags;

	cond_resched();

	pagevec_init(&freed_pvec, 1);
	while (!list_empty(page_list)) {
		struct address_space *mapping;
		struct page *page;
		int may_enter_fs;
		int referenced;

		cond_resched();

		page = lru_to_page(page_list);
		list_del(&page->lru);

		if (!trylock_page(page))
			goto keep;

		VM_BUG_ON(PageActive(page));

		sc->nr_scanned++;

		if (unlikely(!page_evictable(page, NULL)))
			goto cull_mlocked;

		if (!sc->may_unmap && page_mapped(page))
			goto keep_locked;

		/* Double the slab pressure for mapped and swapcache pages */
		if (page_mapped(page) || PageSwapCache(page))
			sc->nr_scanned++;

		may_enter_fs = (sc->gfp_mask & __GFP_FS) ||
			(PageSwapCache(page) && (sc->gfp_mask & __GFP_IO));

		if (PageWriteback(page)) {
			/*
			 * Synchronous reclaim is performed in two passes,
			 * first an asynchronous pass over the list to
			 * start parallel writeback, and a second synchronous
			 * pass to wait for the IO to complete.  Wait here
			 * for any page for which writeback has already
			 * started.
			 */
			if (sync_writeback == PAGEOUT_IO_SYNC && may_enter_fs)
				wait_on_page_writeback(page);
			else
				goto keep_locked;
		}

		referenced = page_referenced(page, 1,
						sc->mem_cgroup, &vm_flags);
		/*
		 * In active use or really unfreeable?  Activate it.
		 * If page which have PG_mlocked lost isoltation race,
		 * try_to_unmap moves it to unevictable list
		 */
		if (sc->order <= PAGE_ALLOC_COSTLY_ORDER &&
					referenced && page_mapping_inuse(page)
					&& !(vm_flags & VM_LOCKED))
			goto activate_locked;

		/*
		 * Anonymous process memory has backing store?
		 * Try to allocate it some swap space here.
		 */
		if (PageAnon(page) && !PageSwapCache(page)) {
			if (!(sc->gfp_mask & __GFP_IO))
				goto keep_locked;
			if (!add_to_swap(page))
				goto activate_locked;
			may_enter_fs = 1;
		}

		mapping = page_mapping(page);

		/*
		 * The page is mapped into the page tables of one or more
		 * processes. Try to unmap it here.
		 */
		if (page_mapped(page) && mapping) {
			switch (try_to_unmap(page, TTU_UNMAP)) {
			case SWAP_FAIL:
				goto activate_locked;
			case SWAP_AGAIN:
				goto keep_locked;
			case SWAP_MLOCK:
				goto cull_mlocked;
			case SWAP_SUCCESS:
				; /* try to free the page below */
			}
		}

		if (PageDirty(page)) {
			if (sc->order <= PAGE_ALLOC_COSTLY_ORDER && referenced)
				goto keep_locked;
			if (!may_enter_fs)
				goto keep_locked;
			if (!sc->may_writepage)
				goto keep_locked;

			/* Page is dirty, try to write it out here */
			switch (pageout(page, mapping, sync_writeback)) {
			case PAGE_KEEP:
				goto keep_locked;
			case PAGE_ACTIVATE:
				goto activate_locked;
			case PAGE_SUCCESS:
				if (PageWriteback(page) || PageDirty(page))
					goto keep;
				/*
				 * A synchronous write - probably a ramdisk.  Go
				 * ahead and try to reclaim the page.
				 */
				if (!trylock_page(page))
					goto keep;
				if (PageDirty(page) || PageWriteback(page))
					goto keep_locked;
				mapping = page_mapping(page);
			case PAGE_CLEAN:
				; /* try to free the page below */
			}
		}

		/*
		 * If the page has buffers, try to free the buffer mappings
		 * associated with this page. If we succeed we try to free
		 * the page as well.
		 *
		 * We do this even if the page is PageDirty().
		 * try_to_release_page() does not perform I/O, but it is
		 * possible for a page to have PageDirty set, but it is actually
		 * clean (all its buffers are clean).  This happens if the
		 * buffers were written out directly, with submit_bh(). ext3
		 * will do this, as well as the blockdev mapping.
		 * try_to_release_page() will discover that cleanness and will
		 * drop the buffers and mark the page clean - it can be freed.
		 *
		 * Rarely, pages can have buffers and no ->mapping.  These are
		 * the pages which were not successfully invalidated in
		 * truncate_complete_page().  We try to drop those buffers here
		 * and if that worked, and the page is no longer mapped into
		 * process address space (page_count == 1) it can be freed.
		 * Otherwise, leave the page on the LRU so it is swappable.
		 */
		if (page_has_private(page)) {
			if (!try_to_release_page(page, sc->gfp_mask))
				goto activate_locked;
			if (!mapping && page_count(page) == 1) {
				unlock_page(page);
				if (put_page_testzero(page))
					goto free_it;
				else {
					/*
					 * rare race with speculative reference.
					 * the speculative reference will free
					 * this page shortly, so we may
					 * increment nr_reclaimed here (and
					 * leave it off the LRU).
					 */
					nr_reclaimed++;
					continue;
				}
			}
		}

		if (!mapping || !__remove_mapping(mapping, page))
			goto keep_locked;

		/*
		 * At this point, we have no other references and there is
		 * no way to pick any more up (removed from LRU, removed
		 * from pagecache). Can use non-atomic bitops now (and
		 * we obviously don't have to worry about waking up a process
		 * waiting on the page lock, because there are no references.
		 */
		__clear_page_locked(page);
free_it:
		nr_reclaimed++;
		if (!pagevec_add(&freed_pvec, page)) {
			__pagevec_free(&freed_pvec);
			pagevec_reinit(&freed_pvec);
		}
		continue;

cull_mlocked:
		if (PageSwapCache(page))
			try_to_free_swap(page);
		unlock_page(page);
		putback_lru_page(page);
		continue;

activate_locked:
		/* Not a candidate for swapping, so reclaim swap space. */
		if (PageSwapCache(page) && vm_swap_full())
			try_to_free_swap(page);
		VM_BUG_ON(PageActive(page));
		SetPageActive(page);
		pgactivate++;
keep_locked:
		unlock_page(page);
keep:
		list_add(&page->lru, &ret_pages);
		VM_BUG_ON(PageLRU(page) || PageUnevictable(page));
	}
	list_splice(&ret_pages, page_list);
	if (pagevec_count(&freed_pvec))
		__pagevec_free(&freed_pvec);
	count_vm_events(PGACTIVATE, pgactivate);
	return nr_reclaimed;
}

/* LRU Isolation modes. */
#define ISOLATE_INACTIVE 0	/* Isolate inactive pages. */
#define ISOLATE_ACTIVE 1	/* Isolate active pages. */
#define ISOLATE_BOTH 2		/* Isolate both active and inactive pages. */

/*
 * Attempt to remove the specified page from its LRU.  Only take this page
 * if it is of the appropriate PageActive status.  Pages which are being
 * freed elsewhere are also ignored.
 *
 * page:	page to consider
 * mode:	one of the LRU isolation modes defined above
 *
 * returns 0 on success, -ve errno on failure.
 */
int __isolate_lru_page(struct page *page, int mode, int file)
{
	int ret = -EINVAL;

	/* Only take pages on the LRU. */
	if (!PageLRU(page))
		return ret;

	/*
	 * When checking the active state, we need to be sure we are
	 * dealing with comparible boolean values.  Take the logical not
	 * of each.
	 */
	if (mode != ISOLATE_BOTH && (!PageActive(page) != !mode))
		return ret;

	if (mode != ISOLATE_BOTH && page_is_file_cache(page) != file)
		return ret;

	/*
	 * When this function is being called for lumpy reclaim, we
	 * initially look into all LRU pages, active, inactive and
	 * unevictable; only give shrink_page_list evictable pages.
	 */
	if (PageUnevictable(page))
		return ret;

	ret = -EBUSY;

	if (likely(get_page_unless_zero(page))) {
		/*
		 * Be careful not to clear PageLRU until after we're
		 * sure the page is not being freed elsewhere -- the
		 * page release code relies on it.
		 */
		ClearPageLRU(page);
		ret = 0;
	}

	return ret;
}

/*
 * zone->lru_lock is heavily contended.  Some of the functions that
 * shrink the lists perform better by taking out a batch of pages
 * and working on them outside the LRU lock.
 *
 * For pagecache intensive workloads, this function is the hottest
 * spot in the kernel (apart from copy_*_user functions).
 *
 * Appropriate locks must be held before calling this function.
 *
 * @nr_to_scan:	The number of pages to look through on the list.
 * @src:	The LRU list to pull pages off.
 * @dst:	The temp list to put pages on to.
 * @scanned:	The number of pages that were scanned.
 * @order:	The caller's attempted allocation order
 * @mode:	One of the LRU isolation modes
 * @file:	True [1] if isolating file [!anon] pages
 *
 * returns how many pages were moved onto *@dst.
 */
static unsigned long isolate_lru_pages(unsigned long nr_to_scan,
		struct list_head *src, struct list_head *dst,
		unsigned long *scanned, int order, int mode, int file)
{
	unsigned long nr_taken = 0;
	unsigned long scan;

	for (scan = 0; scan < nr_to_scan && !list_empty(src); scan++) {
		struct page *page;
		unsigned long pfn;
		unsigned long end_pfn;
		unsigned long page_pfn;
		int zone_id;

		page = lru_to_page(src);
		prefetchw_prev_lru_page(page, src, flags);

		VM_BUG_ON(!PageLRU(page));

		switch (__isolate_lru_page(page, mode, file)) {
		case 0:
			list_move(&page->lru, dst);
			mem_cgroup_del_lru(page);
			nr_taken++;
			break;

		case -EBUSY:
			/* else it is being freed elsewhere */
			list_move(&page->lru, src);
			mem_cgroup_rotate_lru_list(page, page_lru(page));
			continue;

		default:
			BUG();
		}

		if (!order)
			continue;

		/*
		 * Attempt to take all pages in the order aligned region
		 * surrounding the tag page.  Only take those pages of
		 * the same active state as that tag page.  We may safely
		 * round the target page pfn down to the requested order
		 * as the mem_map is guarenteed valid out to MAX_ORDER,
		 * where that page is in a different zone we will detect
		 * it from its zone id and abort this block scan.
		 */
		zone_id = page_zone_id(page);
		page_pfn = page_to_pfn(page);
		pfn = page_pfn & ~((1 << order) - 1);
		end_pfn = pfn + (1 << order);
		for (; pfn < end_pfn; pfn++) {
			struct page *cursor_page;

			/* The target page is in the block, ignore it. */
			if (unlikely(pfn == page_pfn))
				continue;

			/* Avoid holes within the zone. */
			if (unlikely(!pfn_valid_within(pfn)))
				break;

			cursor_page = pfn_to_page(pfn);

			/* Check that we have not crossed a zone boundary. */
			if (unlikely(page_zone_id(cursor_page) != zone_id))
				continue;

			/*
			 * If we don't have enough swap space, reclaiming of
			 * anon page which don't already have a swap slot is
			 * pointless.
			 */
			if (nr_swap_pages <= 0 && PageAnon(cursor_page) &&
					!PageSwapCache(cursor_page))
				continue;

			if (__isolate_lru_page(cursor_page, mode, file) == 0) {
				list_move(&cursor_page->lru, dst);
				mem_cgroup_del_lru(cursor_page);
				nr_taken++;
				scan++;
			}
		}
	}

	*scanned = scan;
	return nr_taken;
}

static unsigned long isolate_pages_global(unsigned long nr,
					struct list_head *dst,
					unsigned long *scanned, int order,
					int mode, struct zone *z,
					struct mem_cgroup *mem_cont,
					int active, int file)
{
	int lru = LRU_BASE;
	if (active)
		lru += LRU_ACTIVE;
	if (file)
		lru += LRU_FILE;
	return isolate_lru_pages(nr, &z->lru[lru].list, dst, scanned, order,
								mode, file);
}

/*
 * clear_active_flags() is a helper for shrink_active_list(), clearing
 * any active bits from the pages in the list.
 */
static unsigned long clear_active_flags(struct list_head *page_list,
					unsigned int *count)
{
	int nr_active = 0;
	int lru;
	struct page *page;

	list_for_each_entry(page, page_list, lru) {
		lru = page_lru_base_type(page);
		if (PageActive(page)) {
			lru += LRU_ACTIVE;
			ClearPageActive(page);
			nr_active++;
		}
		count[lru]++;
	}

	return nr_active;
}

/**
 * isolate_lru_page - tries to isolate a page from its LRU list
 * @page: page to isolate from its LRU list
 *
 * Isolates a @page from an LRU list, clears PageLRU and adjusts the
 * vmstat statistic corresponding to whatever LRU list the page was on.
 *
 * Returns 0 if the page was removed from an LRU list.
 * Returns -EBUSY if the page was not on an LRU list.
 *
 * The returned page will have PageLRU() cleared.  If it was found on
 * the active list, it will have PageActive set.  If it was found on
 * the unevictable list, it will have the PageUnevictable bit set. That flag
 * may need to be cleared by the caller before letting the page go.
 *
 * The vmstat statistic corresponding to the list on which the page was
 * found will be decremented.
 *
 * Restrictions:
 * (1) Must be called with an elevated refcount on the page. This is a
 *     fundamentnal difference from isolate_lru_pages (which is called
 *     without a stable reference).
 * (2) the lru_lock must not be held.
 * (3) interrupts must be enabled.
 */
int isolate_lru_page(struct page *page)
{
	int ret = -EBUSY;

	if (PageLRU(page)) {
		struct zone *zone = page_zone(page);

		spin_lock_irq(&zone->lru_lock);
		if (PageLRU(page) && get_page_unless_zero(page)) {
			int lru = page_lru(page);
			ret = 0;
			ClearPageLRU(page);

			del_page_from_lru_list(zone, page, lru);
		}
		spin_unlock_irq(&zone->lru_lock);
	}
	return ret;
}

/*
 * Are there way too many processes in the direct reclaim path already?
 */
static int too_many_isolated(struct zone *zone, int file,
		struct scan_control *sc)
{
	unsigned long inactive, isolated;

	if (current_is_kswapd())
		return 0;

	if (!scanning_global_lru(sc))
		return 0;

	if (file) {
		inactive = zone_page_state(zone, NR_INACTIVE_FILE);
		isolated = zone_page_state(zone, NR_ISOLATED_FILE);
	} else {
		inactive = zone_page_state(zone, NR_INACTIVE_ANON);
		isolated = zone_page_state(zone, NR_ISOLATED_ANON);
	}

	return isolated > inactive;
}

/*
 * Returns true if the caller should wait to clean dirty/writeback pages.
 *
 * If we are direct reclaiming for contiguous pages and we do not reclaim
 * everything in the list, try again and wait for writeback IO to complete.
 * This will stall high-order allocations noticeably. Only do that when really
 * need to free the pages under high memory pressure.
 */
static inline bool should_reclaim_stall(unsigned long nr_taken,
					unsigned long nr_freed,
					int priority,
					int lumpy_reclaim,
					struct scan_control *sc)
{
	int lumpy_stall_priority;

	/* kswapd should not stall on sync IO */
	if (current_is_kswapd())
		return false;

	/* Only stall on lumpy reclaim */
	if (!lumpy_reclaim)
		return false;

	/* If we have relaimed everything on the isolated list, no stall */
	if (nr_freed == nr_taken)
		return false;

	/*
	 * For high-order allocations, there are two stall thresholds.
	 * High-cost allocations stall immediately where as lower
	 * order allocations such as stacks require the scanning
	 * priority to be much higher before stalling.
	 */
	if (sc->order > PAGE_ALLOC_COSTLY_ORDER)
		lumpy_stall_priority = DEF_PRIORITY;
	else
		lumpy_stall_priority = DEF_PRIORITY / 3;

	return priority <= lumpy_stall_priority;
}

/*
 * shrink_inactive_list() is a helper for shrink_zone().  It returns the number
 * of reclaimed pages
 */
static unsigned long shrink_inactive_list(unsigned long max_scan,
			struct zone *zone, struct scan_control *sc,
			int priority, int file)
{
	LIST_HEAD(page_list);
	struct pagevec pvec;
	unsigned long nr_scanned = 0;
	unsigned long nr_reclaimed = 0;
	struct zone_reclaim_stat *reclaim_stat = get_reclaim_stat(zone, sc);
	int lumpy_reclaim = 0;

	while (unlikely(too_many_isolated(zone, file, sc))) {
		congestion_wait(BLK_RW_ASYNC, HZ/10);

		/* We are about to die and free our memory. Return now. */
		if (fatal_signal_pending(current))
			return SWAP_CLUSTER_MAX;
	}

	/*
	 * If we need a large contiguous chunk of memory, or have
	 * trouble getting a small set of contiguous pages, we
	 * will reclaim both active and inactive pages.
	 *
	 * We use the same threshold as pageout congestion_wait below.
	 */
	if (sc->order > PAGE_ALLOC_COSTLY_ORDER)
		lumpy_reclaim = 1;
	else if (sc->order && priority < DEF_PRIORITY - 2)
		lumpy_reclaim = 1;

	pagevec_init(&pvec, 1);

	lru_add_drain();
	spin_lock_irq(&zone->lru_lock);
	do {
		struct page *page;
		unsigned long nr_taken;
		unsigned long nr_scan;
		unsigned long nr_freed;
		unsigned long nr_active;
		unsigned int count[NR_LRU_LISTS] = { 0, };
		int mode = lumpy_reclaim ? ISOLATE_BOTH : ISOLATE_INACTIVE;
		unsigned long nr_anon;
		unsigned long nr_file;

		nr_taken = sc->isolate_pages(sc->swap_cluster_max,
			     &page_list, &nr_scan, sc->order, mode,
				zone, sc->mem_cgroup, 0, file);

		if (scanning_global_lru(sc)) {
			zone->pages_scanned += nr_scan;
			if (current_is_kswapd())
				__count_zone_vm_events(PGSCAN_KSWAPD, zone,
						       nr_scan);
			else
				__count_zone_vm_events(PGSCAN_DIRECT, zone,
						       nr_scan);
		}

		if (nr_taken == 0)
			goto done;

		nr_active = clear_active_flags(&page_list, count);
		__count_vm_events(PGDEACTIVATE, nr_active);

		__mod_zone_page_state(zone, NR_ACTIVE_FILE,
						-count[LRU_ACTIVE_FILE]);
		__mod_zone_page_state(zone, NR_INACTIVE_FILE,
						-count[LRU_INACTIVE_FILE]);
		__mod_zone_page_state(zone, NR_ACTIVE_ANON,
						-count[LRU_ACTIVE_ANON]);
		__mod_zone_page_state(zone, NR_INACTIVE_ANON,
						-count[LRU_INACTIVE_ANON]);

		nr_anon = count[LRU_ACTIVE_ANON] + count[LRU_INACTIVE_ANON];
		nr_file = count[LRU_ACTIVE_FILE] + count[LRU_INACTIVE_FILE];
		__mod_zone_page_state(zone, NR_ISOLATED_ANON, nr_anon);
		__mod_zone_page_state(zone, NR_ISOLATED_FILE, nr_file);

		reclaim_stat->recent_scanned[0] += count[LRU_INACTIVE_ANON];
		reclaim_stat->recent_scanned[0] += count[LRU_ACTIVE_ANON];
		reclaim_stat->recent_scanned[1] += count[LRU_INACTIVE_FILE];
		reclaim_stat->recent_scanned[1] += count[LRU_ACTIVE_FILE];

		spin_unlock_irq(&zone->lru_lock);

		nr_scanned += nr_scan;
		nr_freed = shrink_page_list(&page_list, sc, PAGEOUT_IO_ASYNC);

		/* Check if we should syncronously wait for writeback */
		if (should_reclaim_stall(nr_taken, nr_freed, priority,
					lumpy_reclaim, sc)) {
			congestion_wait(BLK_RW_ASYNC, HZ/10);

			/*
			 * The attempt at page out may have made some
			 * of the pages active, mark them inactive again.
			 */
			nr_active = clear_active_flags(&page_list, count);
			count_vm_events(PGDEACTIVATE, nr_active);

			nr_freed += shrink_page_list(&page_list, sc,
							PAGEOUT_IO_SYNC);
		}

		nr_reclaimed += nr_freed;

		local_irq_disable();
		if (current_is_kswapd())
			__count_vm_events(KSWAPD_STEAL, nr_freed);
		__count_zone_vm_events(PGSTEAL, zone, nr_freed);

		spin_lock(&zone->lru_lock);
		/*
		 * Put back any unfreeable pages.
		 */
		while (!list_empty(&page_list)) {
			int lru;
			page = lru_to_page(&page_list);
			VM_BUG_ON(PageLRU(page));
			list_del(&page->lru);
			if (unlikely(!page_evictable(page, NULL))) {
				spin_unlock_irq(&zone->lru_lock);
				putback_lru_page(page);
				spin_lock_irq(&zone->lru_lock);
				continue;
			}
			SetPageLRU(page);
			lru = page_lru(page);
			add_page_to_lru_list(zone, page, lru);
			if (is_active_lru(lru)) {
				int file = is_file_lru(lru);
				reclaim_stat->recent_rotated[file]++;
			}
			if (!pagevec_add(&pvec, page)) {
				spin_unlock_irq(&zone->lru_lock);
				__pagevec_release(&pvec);
				spin_lock_irq(&zone->lru_lock);
			}
		}
		__mod_zone_page_state(zone, NR_ISOLATED_ANON, -nr_anon);
		__mod_zone_page_state(zone, NR_ISOLATED_FILE, -nr_file);

  	} while (nr_scanned < max_scan);

done:
	spin_unlock_irq(&zone->lru_lock);
	pagevec_release(&pvec);
	return nr_reclaimed;
}

/*
 * We are about to scan this zone at a certain priority level.  If that priority
 * level is smaller (ie: more urgent) than the previous priority, then note
 * that priority level within the zone.  This is done so that when the next
 * process comes in to scan this zone, it will immediately start out at this
 * priority level rather than having to build up its own scanning priority.
 * Here, this priority affects only the reclaim-mapped threshold.
 */
static inline void note_zone_scanning_priority(struct zone *zone, int priority)
{
	if (priority < zone->prev_priority)
		zone->prev_priority = priority;
}

/*
 * This moves pages from the active list to the inactive list.
 *
 * We move them the other way if the page is referenced by one or more
 * processes, from rmap.
 *
 * If the pages are mostly unmapped, the processing is fast and it is
 * appropriate to hold zone->lru_lock across the whole operation.  But if
 * the pages are mapped, the processing is slow (page_referenced()) so we
 * should drop zone->lru_lock around each page.  It's impossible to balance
 * this, so instead we remove the pages from the LRU while processing them.
 * It is safe to rely on PG_active against the non-LRU pages in here because
 * nobody will play with that bit on a non-LRU page.
 *
 * The downside is that we have to touch page->_count against each page.
 * But we had to alter page->flags anyway.
 */

static void move_active_pages_to_lru(struct zone *zone,
				     struct list_head *list,
				     enum lru_list lru)
{
	unsigned long pgmoved = 0;
	struct pagevec pvec;
	struct page *page;

	pagevec_init(&pvec, 1);

	while (!list_empty(list)) {
		page = lru_to_page(list);

		VM_BUG_ON(PageLRU(page));
		SetPageLRU(page);

		list_move(&page->lru, &zone->lru[lru].list);
		mem_cgroup_add_lru_list(page, lru);
		pgmoved++;

		if (!pagevec_add(&pvec, page) || list_empty(list)) {
			spin_unlock_irq(&zone->lru_lock);
			if (buffer_heads_over_limit)
				pagevec_strip(&pvec);
			__pagevec_release(&pvec);
			spin_lock_irq(&zone->lru_lock);
		}
	}
	__mod_zone_page_state(zone, NR_LRU_BASE + lru, pgmoved);
	if (!is_active_lru(lru))
		__count_vm_events(PGDEACTIVATE, pgmoved);
}

static void shrink_active_list(unsigned long nr_pages, struct zone *zone,
			struct scan_control *sc, int priority, int file)
{
	unsigned long nr_taken;
	unsigned long pgscanned;
	unsigned long vm_flags;
	LIST_HEAD(l_hold);	/* The pages which were snipped off */
	LIST_HEAD(l_active);
	LIST_HEAD(l_inactive);
	struct page *page;
	struct zone_reclaim_stat *reclaim_stat = get_reclaim_stat(zone, sc);
	unsigned long nr_rotated = 0;

	lru_add_drain();
	spin_lock_irq(&zone->lru_lock);
	nr_taken = sc->isolate_pages(nr_pages, &l_hold, &pgscanned, sc->order,
					ISOLATE_ACTIVE, zone,
					sc->mem_cgroup, 1, file);
	/*
	 * zone->pages_scanned is used for detect zone's oom
	 * mem_cgroup remembers nr_scan by itself.
	 */
	if (scanning_global_lru(sc)) {
		zone->pages_scanned += pgscanned;
	}
	reclaim_stat->recent_scanned[file] += nr_taken;

	__count_zone_vm_events(PGREFILL, zone, pgscanned);
	if (file)
		__mod_zone_page_state(zone, NR_ACTIVE_FILE, -nr_taken);
	else
		__mod_zone_page_state(zone, NR_ACTIVE_ANON, -nr_taken);
	__mod_zone_page_state(zone, NR_ISOLATED_ANON + file, nr_taken);
	spin_unlock_irq(&zone->lru_lock);

	while (!list_empty(&l_hold)) {
		cond_resched();
		page = lru_to_page(&l_hold);
		list_del(&page->lru);

		if (unlikely(!page_evictable(page, NULL))) {
			putback_lru_page(page);
			continue;
		}

		/* page_referenced clears PageReferenced */
		if (page_mapping_inuse(page) &&
		    page_referenced(page, 0, sc->mem_cgroup, &vm_flags)) {
			nr_rotated++;
			/*
			 * Identify referenced, file-backed active pages and
			 * give them one more trip around the active list. So
			 * that executable code get better chances to stay in
			 * memory under moderate memory pressure.  Anon pages
			 * are not likely to be evicted by use-once streaming
			 * IO, plus JVM can create lots of anon VM_EXEC pages,
			 * so we ignore them here.
			 */
			if ((vm_flags & VM_EXEC) && page_is_file_cache(page)) {
				list_add(&page->lru, &l_active);
				continue;
			}
		}

		ClearPageActive(page);	/* we are de-activating */
		list_add(&page->lru, &l_inactive);
	}

	/*
	 * Move pages back to the lru list.
	 */
	spin_lock_irq(&zone->lru_lock);
	/*
	 * Count referenced pages from currently used mappings as rotated,
	 * even though only some of them are actually re-activated.  This
	 * helps balance scan pressure between file and anonymous pages in
	 * get_scan_ratio.
	 */
	reclaim_stat->recent_rotated[file] += nr_rotated;

	move_active_pages_to_lru(zone, &l_active,
						LRU_ACTIVE + file * LRU_FILE);
	move_active_pages_to_lru(zone, &l_inactive,
						LRU_BASE   + file * LRU_FILE);
	__mod_zone_page_state(zone, NR_ISOLATED_ANON + file, -nr_taken);
	spin_unlock_irq(&zone->lru_lock);
}

static int inactive_anon_is_low_global(struct zone *zone)
{
	unsigned long active, inactive;

	active = zone_page_state(zone, NR_ACTIVE_ANON);
	inactive = zone_page_state(zone, NR_INACTIVE_ANON);

	if (inactive * zone->inactive_ratio < active)
		return 1;

	return 0;
}

/**
 * inactive_anon_is_low - check if anonymous pages need to be deactivated
 * @zone: zone to check
 * @sc:   scan control of this context
 *
 * Returns true if the zone does not have enough inactive anon pages,
 * meaning some active anon pages need to be deactivated.
 */
static int inactive_anon_is_low(struct zone *zone, struct scan_control *sc)
{
	int low;

	if (scanning_global_lru(sc))
		low = inactive_anon_is_low_global(zone);
	else
		low = mem_cgroup_inactive_anon_is_low(sc->mem_cgroup);
	return low;
}

static int inactive_file_is_low_global(struct zone *zone)
{
	unsigned long active, inactive;

	active = zone_page_state(zone, NR_ACTIVE_FILE);
	inactive = zone_page_state(zone, NR_INACTIVE_FILE);

	return ((inactive * 100)/(inactive + active) < inactive_file_ratio);
}

/**
 * inactive_file_is_low - check if file pages need to be deactivated
 * @zone: zone to check
 * @sc:   scan control of this context
 *
 * When the system is doing streaming IO, memory pressure here
 * ensures that active file pages get deactivated, until more
 * than half of the file pages are on the inactive list.
 *
 * Once we get to that situation, protect the system's working
 * set from being evicted by disabling active file page aging.
 *
 * This uses a different ratio than the anonymous pages, because
 * the page cache uses a use-once replacement algorithm.
 */
static int inactive_file_is_low(struct zone *zone, struct scan_control *sc)
{
	int low;

	if (scanning_global_lru(sc))
		low = inactive_file_is_low_global(zone);
	else
		low = mem_cgroup_inactive_file_is_low(sc->mem_cgroup);
	return low;
}

static int inactive_list_is_low(struct zone *zone, struct scan_control *sc,
				int file)
{
	if (file)
		return inactive_file_is_low(zone, sc);
	else
		return inactive_anon_is_low(zone, sc);
}

static unsigned long shrink_list(enum lru_list lru, unsigned long nr_to_scan,
	struct zone *zone, struct scan_control *sc, int priority)
{
	int file = is_file_lru(lru);

	if (is_active_lru(lru)) {
		if (inactive_list_is_low(zone, sc, file))
		    shrink_active_list(nr_to_scan, zone, sc, priority, file);
		return 0;
	}

	return shrink_inactive_list(nr_to_scan, zone, sc, priority, file);
}

/*
 * Determine how aggressively the anon and file LRU lists should be
 * scanned.  The relative value of each set of LRU lists is determined
 * by looking at the fraction of the pages scanned we did rotate back
 * onto the active list instead of evict.
 *
 * percent[0] specifies how much pressure to put on ram/swap backed
 * memory, while percent[1] determines pressure on the file LRUs.
 */
static void get_scan_ratio(struct zone *zone, struct scan_control *sc,
					unsigned long *percent)
{
	unsigned long anon, file, free;
	unsigned long anon_prio, file_prio;
	unsigned long ap, fp;
	struct zone_reclaim_stat *reclaim_stat = get_reclaim_stat(zone, sc);

	anon  = zone_nr_lru_pages(zone, sc, LRU_ACTIVE_ANON) +
		zone_nr_lru_pages(zone, sc, LRU_INACTIVE_ANON);
	file  = zone_nr_lru_pages(zone, sc, LRU_ACTIVE_FILE) +
		zone_nr_lru_pages(zone, sc, LRU_INACTIVE_FILE);

	if (scanning_global_lru(sc)) {
		free  = zone_page_state(zone, NR_FREE_PAGES);
		/* If we have very few page cache pages,
		   force-scan anon pages. */
		if (unlikely(file + free <= high_wmark_pages(zone))) {
			percent[0] = 100;
			percent[1] = 0;
			return;
		}
	}

	/*
	 * OK, so we have swap space and a fair amount of page cache
	 * pages.  We use the recently rotated / recently scanned
	 * ratios to determine how valuable each cache is.
	 *
	 * Because workloads change over time (and to avoid overflow)
	 * we keep these statistics as a floating average, which ends
	 * up weighing recent references more than old ones.
	 *
	 * anon in [0], file in [1]
	 */
	if (unlikely(reclaim_stat->recent_scanned[0] > anon / 4)) {
		spin_lock_irq(&zone->lru_lock);
		reclaim_stat->recent_scanned[0] /= 2;
		reclaim_stat->recent_rotated[0] /= 2;
		spin_unlock_irq(&zone->lru_lock);
	}

	if (unlikely(reclaim_stat->recent_scanned[1] > file / 4)) {
		spin_lock_irq(&zone->lru_lock);
		reclaim_stat->recent_scanned[1] /= 2;
		reclaim_stat->recent_rotated[1] /= 2;
		spin_unlock_irq(&zone->lru_lock);
	}

	/*
	 * With swappiness at 100, anonymous and file have the same priority.
	 * This scanning priority is essentially the inverse of IO cost.
	 */
	anon_prio = sc->swappiness;
	file_prio = 200 - sc->swappiness;

	/*
	 * The amount of pressure on anon vs file pages is inversely
	 * proportional to the fraction of recently scanned pages on
	 * each list that were recently referenced and in active use.
	 */
	ap = (anon_prio + 1) * (reclaim_stat->recent_scanned[0] + 1);
	ap /= reclaim_stat->recent_rotated[0] + 1;

	fp = (file_prio + 1) * (reclaim_stat->recent_scanned[1] + 1);
	fp /= reclaim_stat->recent_rotated[1] + 1;

	/* Normalize to percentages */
	percent[0] = 100 * ap / (ap + fp + 1);
	percent[1] = 100 - percent[0];
}

/*
 * Smallish @nr_to_scan's are deposited in @nr_saved_scan,
 * until we collected @swap_cluster_max pages to scan.
 */
static unsigned long nr_scan_try_batch(unsigned long nr_to_scan,
				       unsigned long *nr_saved_scan,
				       unsigned long swap_cluster_max)
{
	unsigned long nr;

	*nr_saved_scan += nr_to_scan;
	nr = *nr_saved_scan;

	if (nr >= swap_cluster_max)
		*nr_saved_scan = 0;
	else
		nr = 0;

	return nr;
}

/*
 * This is a basic per-zone page freer.  Used by both kswapd and direct reclaim.
 */
static void shrink_zone(int priority, struct zone *zone,
				struct scan_control *sc)
{
	unsigned long nr[NR_LRU_LISTS];
	unsigned long nr_to_scan;
	unsigned long percent[2];	/* anon @ 0; file @ 1 */
	enum lru_list l;
	unsigned long nr_reclaimed = sc->nr_reclaimed;
	unsigned long swap_cluster_max = sc->swap_cluster_max;
	struct zone_reclaim_stat *reclaim_stat = get_reclaim_stat(zone, sc);
	int noswap = 0;

	/* If we have no swap space, do not bother scanning anon pages. */
	if (!sc->may_swap || (nr_swap_pages <= 0)) {
		noswap = 1;
		percent[0] = 0;
		percent[1] = 100;
	} else
		get_scan_ratio(zone, sc, percent);

	for_each_evictable_lru(l) {
		int file = is_file_lru(l);
		unsigned long scan;

		scan = zone_nr_lru_pages(zone, sc, l);
		if (priority || noswap) {
			scan >>= priority;
			scan = (scan * percent[file]) / 100;
		}
		nr[l] = nr_scan_try_batch(scan,
					  &reclaim_stat->nr_saved_scan[l],
					  swap_cluster_max);
	}

	while (nr[LRU_INACTIVE_ANON] || nr[LRU_ACTIVE_FILE] ||
					nr[LRU_INACTIVE_FILE]) {
		for_each_evictable_lru(l) {
			if (nr[l]) {
				nr_to_scan = min(nr[l], swap_cluster_max);
				nr[l] -= nr_to_scan;

				nr_reclaimed += shrink_list(l, nr_to_scan,
							    zone, sc, priority);
			}
		}
		/*
		 * On large memory systems, scan >> priority can become
		 * really large. This is fine for the starting priority;
		 * we want to put equal scanning pressure on each zone.
		 * However, if the VM has a harder time of freeing pages,
		 * with multiple processes reclaiming pages, the total
		 * freeing target can get unreasonably large.
		 */
		if (nr_reclaimed > swap_cluster_max &&
			priority < DEF_PRIORITY && !current_is_kswapd())
			break;
	}

	sc->nr_reclaimed = nr_reclaimed;

	/*
	 * Even if we did not try to evict anon pages at all, we want to
	 * rebalance the anon lru active/inactive ratio.
	 */
	if (inactive_anon_is_low(zone, sc) && nr_swap_pages > 0)
		shrink_active_list(SWAP_CLUSTER_MAX, zone, sc, priority, 0);

	throttle_vm_writeout(sc->gfp_mask);
}

/*
 * This is the direct reclaim path, for page-allocating processes.  We only
 * try to reclaim pages from zones which will satisfy the caller's allocation
 * request.
 *
 * We reclaim from a zone even if that zone is over high_wmark_pages(zone).
 * Because:
 * a) The caller may be trying to free *extra* pages to satisfy a higher-order
 *    allocation or
 * b) The target zone may be at high_wmark_pages(zone) but the lower zones
 *    must go *over* high_wmark_pages(zone) to satisfy the `incremental min'
 *    zone defense algorithm.
 *
 * If a zone is deemed to be full of pinned pages then just give it a light
 * scan then give up on it.
 */
static void shrink_zones(int priority, struct zonelist *zonelist,
					struct scan_control *sc)
{
	enum zone_type high_zoneidx = gfp_zone(sc->gfp_mask);
	struct zoneref *z;
	struct zone *zone;

	sc->all_unreclaimable = 1;
	for_each_zone_zonelist_nodemask(zone, z, zonelist, high_zoneidx,
					sc->nodemask) {
		if (!populated_zone(zone))
			continue;
		/*
		 * Take care memory controller reclaiming has small influence
		 * to global LRU.
		 */
		if (scanning_global_lru(sc)) {
			if (!cpuset_zone_allowed_hardwall(zone, GFP_KERNEL))
				continue;
			note_zone_scanning_priority(zone, priority);

			if (zone_is_all_unreclaimable(zone) &&
						priority != DEF_PRIORITY)
				continue;	/* Let kswapd poll it */
			sc->all_unreclaimable = 0;
		} else {
			/*
			 * Ignore cpuset limitation here. We just want to reduce
			 * # of used pages by us regardless of memory shortage.
			 */
			sc->all_unreclaimable = 0;
			mem_cgroup_note_reclaim_priority(sc->mem_cgroup,
							priority);
		}

		shrink_zone(priority, zone, sc);
	}
}

/*
 * This is the main entry point to direct page reclaim.
 *
 * If a full scan of the inactive list fails to free enough memory then we
 * are "out of memory" and something needs to be killed.
 *
 * If the caller is !__GFP_FS then the probability of a failure is reasonably
 * high - the zone may be full of dirty or under-writeback pages, which this
 * caller can't do much about.  We kick the writeback threads and take explicit
 * naps in the hope that some of these pages can be written.  But if the
 * allocating task holds filesystem locks which prevent writeout this might not
 * work, and the allocation attempt will fail.
 *
 * returns:	0, if no pages reclaimed
 * 		else, the number of pages reclaimed
 */
static unsigned long do_try_to_free_pages(struct zonelist *zonelist,
					struct scan_control *sc)
{
	int priority;
	unsigned long ret = 0;
	unsigned long total_scanned = 0;
	struct reclaim_state *reclaim_state = current->reclaim_state;
	unsigned long lru_pages = 0;
	struct zoneref *z;
	struct zone *zone;
	enum zone_type high_zoneidx = gfp_zone(sc->gfp_mask);

	delayacct_freepages_start();

	if (scanning_global_lru(sc))
		count_vm_event(ALLOCSTALL);
	/*
	 * mem_cgroup will not do shrink_slab.
	 */
	if (scanning_global_lru(sc)) {
		for_each_zone_zonelist(zone, z, zonelist, high_zoneidx) {

			if (!cpuset_zone_allowed_hardwall(zone, GFP_KERNEL))
				continue;

			lru_pages += zone_reclaimable_pages(zone);
		}
	}

	for (priority = DEF_PRIORITY; priority >= 0; priority--) {
		sc->nr_scanned = 0;
		if (!priority)
			disable_swap_token();
		shrink_zones(priority, zonelist, sc);
		/*
		 * Don't shrink slabs when reclaiming memory from
		 * over limit cgroups
		 */
		if (scanning_global_lru(sc)) {
			shrink_slab(sc->nr_scanned, sc->gfp_mask, lru_pages);
			if (reclaim_state) {
				sc->nr_reclaimed += reclaim_state->reclaimed_slab;
				reclaim_state->reclaimed_slab = 0;
			}
		}
		total_scanned += sc->nr_scanned;
		if (sc->nr_reclaimed >= sc->swap_cluster_max) {
			ret = sc->nr_reclaimed;
			goto out;
		}

		/*
		 * Try to write back as many pages as we just scanned.  This
		 * tends to cause slow streaming writers to write data to the
		 * disk smoothly, at the dirtying rate, which is nice.   But
		 * that's undesirable in laptop mode, where we *want* lumpy
		 * writeout.  So in laptop mode, write out the whole world.
		 */
		if (total_scanned > sc->swap_cluster_max +
					sc->swap_cluster_max / 2) {
			wakeup_flusher_threads(laptop_mode ? 0 : total_scanned);
			sc->may_writepage = 1;
		}

		/* Take a nap, wait for some writeback to complete */
		if (sc->nr_scanned && priority < DEF_PRIORITY - 2)
			congestion_wait(BLK_RW_ASYNC, HZ/10);
	}
	/* top priority shrink_zones still had more to do? don't OOM, then */
	if (!sc->all_unreclaimable && scanning_global_lru(sc))
		ret = sc->nr_reclaimed;
out:
	/*
	 * Now that we've scanned all the zones at this priority level, note
	 * that level within the zone so that the next thread which performs
	 * scanning of this zone will immediately start out at this priority
	 * level.  This affects only the decision whether or not to bring
	 * mapped pages onto the inactive list.
	 */
	if (priority < 0)
		priority = 0;

	if (scanning_global_lru(sc)) {
		for_each_zone_zonelist(zone, z, zonelist, high_zoneidx) {

			if (!cpuset_zone_allowed_hardwall(zone, GFP_KERNEL))
				continue;

			zone->prev_priority = priority;
		}
	} else
		mem_cgroup_record_reclaim_priority(sc->mem_cgroup, priority);

	delayacct_freepages_end();

	return ret;
}

unsigned long try_to_free_pages(struct zonelist *zonelist, int order,
				gfp_t gfp_mask, nodemask_t *nodemask)
{
	struct scan_control sc = {
		.gfp_mask = gfp_mask,
		.may_writepage = !laptop_mode,
		.swap_cluster_max = SWAP_CLUSTER_MAX,
		.may_unmap = 1,
		.may_swap = 1,
		.swappiness = vm_swappiness,
		.order = order,
		.mem_cgroup = NULL,
		.isolate_pages = isolate_pages_global,
		.nodemask = nodemask,
	};

	return do_try_to_free_pages(zonelist, &sc);
}

#ifdef CONFIG_CGROUP_MEM_RES_CTLR

unsigned long mem_cgroup_shrink_node_zone(struct mem_cgroup *mem,
						gfp_t gfp_mask, bool noswap,
						unsigned int swappiness,
						struct zone *zone, int nid)
{
	struct scan_control sc = {
		.may_writepage = !laptop_mode,
		.may_unmap = 1,
		.may_swap = !noswap,
		.swap_cluster_max = SWAP_CLUSTER_MAX,
		.swappiness = swappiness,
		.order = 0,
		.mem_cgroup = mem,
		.isolate_pages = mem_cgroup_isolate_pages,
	};
	nodemask_t nm  = nodemask_of_node(nid);

	sc.gfp_mask = (gfp_mask & GFP_RECLAIM_MASK) |
			(GFP_HIGHUSER_MOVABLE & ~GFP_RECLAIM_MASK);
	sc.nodemask = &nm;
	sc.nr_reclaimed = 0;
	sc.nr_scanned = 0;
	/*
	 * NOTE: Although we can get the priority field, using it
	 * here is not a good idea, since it limits the pages we can scan.
	 * if we don't reclaim here, the shrink_zone from balance_pgdat
	 * will pick up pages from other mem cgroup's as well. We hack
	 * the priority and make it zero.
	 */
	shrink_zone(0, zone, &sc);
	return sc.nr_reclaimed;
}

unsigned long try_to_free_mem_cgroup_pages(struct mem_cgroup *mem_cont,
					   gfp_t gfp_mask,
					   bool noswap,
					   unsigned int swappiness)
{
	struct zonelist *zonelist;
	struct scan_control sc = {
		.may_writepage = !laptop_mode,
		.may_unmap = 1,
		.may_swap = !noswap,
		.swap_cluster_max = SWAP_CLUSTER_MAX,
		.swappiness = swappiness,
		.order = 0,
		.mem_cgroup = mem_cont,
		.isolate_pages = mem_cgroup_isolate_pages,
		.nodemask = NULL, /* we don't care the placement */
	};

	sc.gfp_mask = (gfp_mask & GFP_RECLAIM_MASK) |
			(GFP_HIGHUSER_MOVABLE & ~GFP_RECLAIM_MASK);
	zonelist = NODE_DATA(numa_node_id())->node_zonelists;
	return do_try_to_free_pages(zonelist, &sc);
}
#endif

/*
 * For kswapd, balance_pgdat() will work across all this node's zones until
 * they are all at high_wmark_pages(zone).
 *
 * Returns the number of pages which were actually freed.
 *
 * There is special handling here for zones which are full of pinned pages.
 * This can happen if the pages are all mlocked, or if they are all used by
 * device drivers (say, ZONE_DMA).  Or if they are all in use by hugetlb.
 * What we do is to detect the case where all pages in the zone have been
 * scanned twice and there has been zero successful reclaim.  Mark the zone as
 * dead and from now on, only perform a short scan.  Basically we're polling
 * the zone for when the problem goes away.
 *
 * kswapd scans the zones in the highmem->normal->dma direction.  It skips
 * zones which have free_pages > high_wmark_pages(zone), but once a zone is
 * found to have free_pages <= high_wmark_pages(zone), we scan that zone and the
 * lower zones regardless of the number of free pages in the lower zones. This
 * interoperates with the page allocator fallback scheme to ensure that aging
 * of pages is balanced across the zones.
 */
static unsigned long balance_pgdat(pg_data_t *pgdat, int order)
{
	int all_zones_ok;
	int priority;
	int i;
	unsigned long total_scanned;
	struct reclaim_state *reclaim_state = current->reclaim_state;
	struct scan_control sc = {
		.gfp_mask = GFP_KERNEL,
		.may_unmap = 1,
		.may_swap = 1,
		.swap_cluster_max = SWAP_CLUSTER_MAX,
		.swappiness = vm_swappiness,
		.order = order,
		.mem_cgroup = NULL,
		.isolate_pages = isolate_pages_global,
	};
	/*
	 * temp_priority is used to remember the scanning priority at which
	 * this zone was successfully refilled to
	 * free_pages == high_wmark_pages(zone).
	 */
	int temp_priority[MAX_NR_ZONES];

loop_again:
	total_scanned = 0;
	sc.nr_reclaimed = 0;
	sc.may_writepage = !laptop_mode;
	count_vm_event(PAGEOUTRUN);

	for (i = 0; i < pgdat->nr_zones; i++)
		temp_priority[i] = DEF_PRIORITY;

	for (priority = DEF_PRIORITY; priority >= 0; priority--) {
		int end_zone = 0;	/* Inclusive.  0 = ZONE_DMA */
		unsigned long lru_pages = 0;

		/* The swap token gets in the way of swapout... */
		if (!priority)
			disable_swap_token();

		all_zones_ok = 1;

		/*
		 * Scan in the highmem->dma direction for the highest
		 * zone which needs scanning
		 */
		for (i = pgdat->nr_zones - 1; i >= 0; i--) {
			struct zone *zone = pgdat->node_zones + i;

			if (!populated_zone(zone))
				continue;

			if (zone_is_all_unreclaimable(zone) &&
			    priority != DEF_PRIORITY)
				continue;

			/*
			 * Do some background aging of the anon list, to give
			 * pages a chance to be referenced before reclaiming.
			 */
			if (inactive_anon_is_low(zone, &sc))
				shrink_active_list(SWAP_CLUSTER_MAX, zone,
							&sc, priority, 0);

			if (!zone_watermark_ok(zone, order,
					high_wmark_pages(zone), 0, 0)) {
				end_zone = i;
				break;
			}
		}
		if (i < 0)
			goto out;

		for (i = 0; i <= end_zone; i++) {
			struct zone *zone = pgdat->node_zones + i;

			lru_pages += zone_reclaimable_pages(zone);
		}

		/*
		 * Now scan the zone in the dma->highmem direction, stopping
		 * at the last zone which needs scanning.
		 *
		 * We do this because the page allocator works in the opposite
		 * direction.  This prevents the page allocator from allocating
		 * pages behind kswapd's direction of progress, which would
		 * cause too much scanning of the lower zones.
		 */
		for (i = 0; i <= end_zone; i++) {
			struct zone *zone = pgdat->node_zones + i;
			int nr_slab;
			int nid, zid;

			if (!populated_zone(zone))
				continue;

			if (zone_is_all_unreclaimable(zone) &&
					priority != DEF_PRIORITY)
				continue;

			if (!zone_watermark_ok(zone, order,
					high_wmark_pages(zone), end_zone, 0))
				all_zones_ok = 0;
			temp_priority[i] = priority;
			sc.nr_scanned = 0;
			note_zone_scanning_priority(zone, priority);

			nid = pgdat->node_id;
			zid = zone_idx(zone);
			/*
			 * Call soft limit reclaim before calling shrink_zone.
			 * For now we ignore the return value
			 */
			mem_cgroup_soft_limit_reclaim(zone, order, sc.gfp_mask,
							nid, zid);
			/*
			 * We put equal pressure on every zone, unless one
			 * zone has way too many pages free already.
			 */
			if (!zone_watermark_ok(zone, order,
					8*high_wmark_pages(zone), end_zone, 0))
				shrink_zone(priority, zone, &sc);
			reclaim_state->reclaimed_slab = 0;
			nr_slab = shrink_slab(sc.nr_scanned, GFP_KERNEL,
						lru_pages);
			sc.nr_reclaimed += reclaim_state->reclaimed_slab;
			total_scanned += sc.nr_scanned;
			if (zone_is_all_unreclaimable(zone))
				continue;
			if (nr_slab == 0 && zone->pages_scanned >=
					(zone_reclaimable_pages(zone) * 6))
					zone_set_flag(zone,
						      ZONE_ALL_UNRECLAIMABLE);
			/*
			 * If we've done a decent amount of scanning and
			 * the reclaim ratio is low, start doing writepage
			 * even in laptop mode
			 */
			if (total_scanned > SWAP_CLUSTER_MAX * 2 &&
			    total_scanned > sc.nr_reclaimed + sc.nr_reclaimed / 2)
				sc.may_writepage = 1;
		}
		if (all_zones_ok)
			break;		/* kswapd: all done */
		/*
		 * OK, kswapd is getting into trouble.  Take a nap, then take
		 * another pass across the zones.
		 */
		if (total_scanned && priority < DEF_PRIORITY - 2)
			congestion_wait(BLK_RW_ASYNC, HZ/10);

		/*
		 * We do this so kswapd doesn't build up large priorities for
		 * example when it is freeing in parallel with allocators. It
		 * matches the direct reclaim path behaviour in terms of impact
		 * on zone->*_priority.
		 */
		if (sc.nr_reclaimed >= SWAP_CLUSTER_MAX)
			break;
	}
out:
	/*
	 * Note within each zone the priority level at which this zone was
	 * brought into a happy state.  So that the next thread which scans this
	 * zone will start out at that priority level.
	 */
	for (i = 0; i < pgdat->nr_zones; i++) {
		struct zone *zone = pgdat->node_zones + i;

		zone->prev_priority = temp_priority[i];
	}
	if (!all_zones_ok) {
		cond_resched();

		try_to_freeze();

		/*
		 * Fragmentation may mean that the system cannot be
		 * rebalanced for high-order allocations in all zones.
		 * At this point, if nr_reclaimed < SWAP_CLUSTER_MAX,
		 * it means the zones have been fully scanned and are still
		 * not balanced. For high-order allocations, there is
		 * little point trying all over again as kswapd may
		 * infinite loop.
		 *
		 * Instead, recheck all watermarks at order-0 as they
		 * are the most important. If watermarks are ok, kswapd will go
		 * back to sleep. High-order users can still perform direct
		 * reclaim if they wish.
		 */
		if (sc.nr_reclaimed < SWAP_CLUSTER_MAX)
			order = sc.order = 0;

		goto loop_again;
	}

	return sc.nr_reclaimed;
}

/*
 * The background pageout daemon, started as a kernel thread
 * from the init process.
 *
 * This basically trickles out pages so that we have _some_
 * free memory available even if there is no other activity
 * that frees anything up. This is needed for things like routing
 * etc, where we otherwise might have all activity going on in
 * asynchronous contexts that cannot page things out.
 *
 * If there are applications that are active memory-allocators
 * (most normal use), this basically shouldn't matter.
 */
static int kswapd(void *p)
{
	unsigned long order;
	pg_data_t *pgdat = (pg_data_t*)p;
	struct task_struct *tsk = current;
	DEFINE_WAIT(wait);
	struct reclaim_state reclaim_state = {
		.reclaimed_slab = 0,
	};
	const struct cpumask *cpumask = cpumask_of_node(pgdat->node_id);

	lockdep_set_current_reclaim_state(GFP_KERNEL);

	if (!cpumask_empty(cpumask))
		set_cpus_allowed_ptr(tsk, cpumask);
	current->reclaim_state = &reclaim_state;

	/*
	 * Tell the memory management that we're a "memory allocator",
	 * and that if we need more memory we should get access to it
	 * regardless (see "__alloc_pages()"). "kswapd" should
	 * never get caught in the normal page freeing logic.
	 *
	 * (Kswapd normally doesn't need memory anyway, but sometimes
	 * you need a small amount of memory in order to be able to
	 * page out something else, and this flag essentially protects
	 * us from recursively trying to free more memory as we're
	 * trying to free the first piece of memory in the first place).
	 */
	tsk->flags |= PF_MEMALLOC | PF_SWAPWRITE | PF_KSWAPD;
	set_freezable();

	order = 0;
	for ( ; ; ) {
		unsigned long new_order;

		prepare_to_wait(&pgdat->kswapd_wait, &wait, TASK_INTERRUPTIBLE);
		new_order = pgdat->kswapd_max_order;
		pgdat->kswapd_max_order = 0;
		if (order < new_order) {
			/*
			 * Don't sleep if someone wants a larger 'order'
			 * allocation
			 */
			order = new_order;
		} else {
			if (!freezing(current))
				schedule();

			order = pgdat->kswapd_max_order;
		}
		finish_wait(&pgdat->kswapd_wait, &wait);

		if (!try_to_freeze()) {
			/* We can speed up thawing tasks if we don't call
			 * balance_pgdat after returning from the refrigerator
			 */
			balance_pgdat(pgdat, order);
		}
	}
	return 0;
}

/*
 * A zone is low on free memory, so wake its kswapd task to service it.
 */
void wakeup_kswapd(struct zone *zone, int order)
{
	pg_data_t *pgdat;

	if (!populated_zone(zone))
		return;

	pgdat = zone->zone_pgdat;
	if (zone_watermark_ok(zone, order, low_wmark_pages(zone), 0, 0))
		return;
	if (pgdat->kswapd_max_order < order)
		pgdat->kswapd_max_order = order;
	if (!cpuset_zone_allowed_hardwall(zone, GFP_KERNEL))
		return;
	if (!waitqueue_active(&pgdat->kswapd_wait))
		return;
	wake_up_interruptible(&pgdat->kswapd_wait);
}

/*
 * The reclaimable count would be mostly accurate.
 * The less reclaimable pages may be
 * - mlocked pages, which will be moved to unevictable list when encountered
 * - mapped pages, which may require several travels to be reclaimed
 * - dirty pages, which is not "instantly" reclaimable
 */
unsigned long global_reclaimable_pages(void)
{
	int nr;

	nr = global_page_state(NR_ACTIVE_FILE) +
	     global_page_state(NR_INACTIVE_FILE);

	if (nr_swap_pages > 0)
		nr += global_page_state(NR_ACTIVE_ANON) +
		      global_page_state(NR_INACTIVE_ANON);

	return nr;
}

unsigned long zone_reclaimable_pages(struct zone *zone)
{
	int nr;

	nr = zone_page_state(zone, NR_ACTIVE_FILE) +
	     zone_page_state(zone, NR_INACTIVE_FILE);

	if (nr_swap_pages > 0)
		nr += zone_page_state(zone, NR_ACTIVE_ANON) +
		      zone_page_state(zone, NR_INACTIVE_ANON);

	return nr;
}

#ifdef CONFIG_HIBERNATION
/*
 * Helper function for shrink_all_memory().  Tries to reclaim 'nr_pages' pages
 * from LRU lists system-wide, for given pass and priority.
 *
 * For pass > 3 we also try to shrink the LRU lists that contain a few pages
 */
static void shrink_all_zones(unsigned long nr_pages, int prio,
				      int pass, struct scan_control *sc)
{
	struct zone *zone;
	unsigned long nr_reclaimed = 0;
	struct zone_reclaim_stat *reclaim_stat;

	for_each_populated_zone(zone) {
		enum lru_list l;

		if (zone_is_all_unreclaimable(zone) && prio != DEF_PRIORITY)
			continue;

		for_each_evictable_lru(l) {
			enum zone_stat_item ls = NR_LRU_BASE + l;
			unsigned long lru_pages = zone_page_state(zone, ls);

			/* For pass = 0, we don't shrink the active list */
			if (pass == 0 && (l == LRU_ACTIVE_ANON ||
						l == LRU_ACTIVE_FILE))
				continue;

			reclaim_stat = get_reclaim_stat(zone, sc);
			reclaim_stat->nr_saved_scan[l] +=
						(lru_pages >> prio) + 1;
			if (reclaim_stat->nr_saved_scan[l]
						>= nr_pages || pass > 3) {
				unsigned long nr_to_scan;

				reclaim_stat->nr_saved_scan[l] = 0;
				nr_to_scan = min(nr_pages, lru_pages);
				nr_reclaimed += shrink_list(l, nr_to_scan, zone,
								sc, prio);
				if (nr_reclaimed >= nr_pages) {
					sc->nr_reclaimed += nr_reclaimed;
					return;
				}
			}
		}
	}
	sc->nr_reclaimed += nr_reclaimed;
}

/*
 * Try to free `nr_pages' of memory, system-wide, and return the number of
 * freed pages.
 *
 * Rather than trying to age LRUs the aim is to preserve the overall
 * LRU order by reclaiming preferentially
 * inactive > active > active referenced > active mapped
 */
unsigned long shrink_all_memory(unsigned long nr_pages)
{
	unsigned long lru_pages, nr_slab;
	int pass;
	struct reclaim_state reclaim_state;
	struct scan_control sc = {
		.gfp_mask = GFP_KERNEL,
		.may_unmap = 0,
		.may_writepage = 1,
		.isolate_pages = isolate_pages_global,
		.nr_reclaimed = 0,
	};

	current->reclaim_state = &reclaim_state;

	lru_pages = global_reclaimable_pages();
	nr_slab = global_page_state(NR_SLAB_RECLAIMABLE);
	/* If slab caches are huge, it's better to hit them first */
	while (nr_slab >= lru_pages) {
		reclaim_state.reclaimed_slab = 0;
		shrink_slab(nr_pages, sc.gfp_mask, lru_pages);
		if (!reclaim_state.reclaimed_slab)
			break;

		sc.nr_reclaimed += reclaim_state.reclaimed_slab;
		if (sc.nr_reclaimed >= nr_pages)
			goto out;

		nr_slab -= reclaim_state.reclaimed_slab;
	}

	/*
	 * We try to shrink LRUs in 5 passes:
	 * 0 = Reclaim from inactive_list only
	 * 1 = Reclaim from active list but don't reclaim mapped
	 * 2 = 2nd pass of type 1
	 * 3 = Reclaim mapped (normal reclaim)
	 * 4 = 2nd pass of type 3
	 */
	for (pass = 0; pass < 5; pass++) {
		int prio;

		/* Force reclaiming mapped pages in the passes #3 and #4 */
		if (pass > 2)
			sc.may_unmap = 1;

		for (prio = DEF_PRIORITY; prio >= 0; prio--) {
			unsigned long nr_to_scan = nr_pages - sc.nr_reclaimed;

			sc.nr_scanned = 0;
			sc.swap_cluster_max = nr_to_scan;
			shrink_all_zones(nr_to_scan, prio, pass, &sc);
			if (sc.nr_reclaimed >= nr_pages)
				goto out;

			reclaim_state.reclaimed_slab = 0;
			shrink_slab(sc.nr_scanned, sc.gfp_mask,
				    global_reclaimable_pages());
			sc.nr_reclaimed += reclaim_state.reclaimed_slab;
			if (sc.nr_reclaimed >= nr_pages)
				goto out;

			if (sc.nr_scanned && prio < DEF_PRIORITY - 2)
				congestion_wait(BLK_RW_ASYNC, HZ / 10);
		}
	}

	/*
	 * If sc.nr_reclaimed = 0, we could not shrink LRUs, but there may be
	 * something in slab caches
	 */
	if (!sc.nr_reclaimed) {
		do {
			reclaim_state.reclaimed_slab = 0;
			shrink_slab(nr_pages, sc.gfp_mask,
				    global_reclaimable_pages());
			sc.nr_reclaimed += reclaim_state.reclaimed_slab;
		} while (sc.nr_reclaimed < nr_pages &&
				reclaim_state.reclaimed_slab > 0);
	}


out:
	current->reclaim_state = NULL;

	return sc.nr_reclaimed;
}
#endif /* CONFIG_HIBERNATION */

/* It's optimal to keep kswapds on the same CPUs as their memory, but
   not required for correctness.  So if the last cpu in a node goes
   away, we get changed to run anywhere: as the first one comes back,
   restore their cpu bindings. */
static int __devinit cpu_callback(struct notifier_block *nfb,
				  unsigned long action, void *hcpu)
{
	int nid;

	if (action == CPU_ONLINE || action == CPU_ONLINE_FROZEN) {
		for_each_node_state(nid, N_HIGH_MEMORY) {
			pg_data_t *pgdat = NODE_DATA(nid);
			const struct cpumask *mask;

			mask = cpumask_of_node(pgdat->node_id);

			if (cpumask_any_and(cpu_online_mask, mask) < nr_cpu_ids)
				/* One of our CPUs online: restore mask */
				set_cpus_allowed_ptr(pgdat->kswapd, mask);
		}
	}
	return NOTIFY_OK;
}

/*
 * This kswapd start function will be called by init and node-hot-add.
 * On node-hot-add, kswapd will moved to proper cpus if cpus are hot-added.
 */
int kswapd_run(int nid)
{
	pg_data_t *pgdat = NODE_DATA(nid);
	int ret = 0;

	if (pgdat->kswapd)
		return 0;

	pgdat->kswapd = kthread_run(kswapd, pgdat, "kswapd%d", nid);
	if (IS_ERR(pgdat->kswapd)) {
		/* failure at boot is fatal */
		BUG_ON(system_state == SYSTEM_BOOTING);
		printk("Failed to start kswapd on node %d\n",nid);
		ret = -1;
	}
	return ret;
}

static int __init kswapd_init(void)
{
	int nid;

	swap_setup();
	for_each_node_state(nid, N_HIGH_MEMORY)
 		kswapd_run(nid);
	hotcpu_notifier(cpu_callback, 0);
	return 0;
}

module_init(kswapd_init)

#ifdef CONFIG_NUMA
/*
 * Zone reclaim mode
 *
 * If non-zero call zone_reclaim when the number of free pages falls below
 * the watermarks.
 */
int zone_reclaim_mode __read_mostly;

#define RECLAIM_OFF 0
#define RECLAIM_ZONE (1<<0)	/* Run shrink_inactive_list on the zone */
#define RECLAIM_WRITE (1<<1)	/* Writeout pages during reclaim */
#define RECLAIM_SWAP (1<<2)	/* Swap pages out during reclaim */

/*
 * Priority for ZONE_RECLAIM. This determines the fraction of pages
 * of a node considered for each zone_reclaim. 4 scans 1/16th of
 * a zone.
 */
#define ZONE_RECLAIM_PRIORITY 4

/*
 * Percentage of pages in a zone that must be unmapped for zone_reclaim to
 * occur.
 */
int sysctl_min_unmapped_ratio = 1;

/*
 * If the number of slab pages in a zone grows beyond this percentage then
 * slab reclaim needs to occur.
 */
int sysctl_min_slab_ratio = 5;

static inline unsigned long zone_unmapped_file_pages(struct zone *zone)
{
	unsigned long file_mapped = zone_page_state(zone, NR_FILE_MAPPED);
	unsigned long file_lru = zone_page_state(zone, NR_INACTIVE_FILE) +
		zone_page_state(zone, NR_ACTIVE_FILE);

	/*
	 * It's possible for there to be more file mapped pages than
	 * accounted for by the pages on the file LRU lists because
	 * tmpfs pages accounted for as ANON can also be FILE_MAPPED
	 */
	return (file_lru > file_mapped) ? (file_lru - file_mapped) : 0;
}

/* Work out how many page cache pages we can reclaim in this reclaim_mode */
static long zone_pagecache_reclaimable(struct zone *zone)
{
	long nr_pagecache_reclaimable;
	long delta = 0;

	/*
	 * If RECLAIM_SWAP is set, then all file pages are considered
	 * potentially reclaimable. Otherwise, we have to worry about
	 * pages like swapcache and zone_unmapped_file_pages() provides
	 * a better estimate
	 */
	if (zone_reclaim_mode & RECLAIM_SWAP)
		nr_pagecache_reclaimable = zone_page_state(zone, NR_FILE_PAGES);
	else
		nr_pagecache_reclaimable = zone_unmapped_file_pages(zone);

	/* If we can't clean pages, remove dirty pages from consideration */
	if (!(zone_reclaim_mode & RECLAIM_WRITE))
		delta += zone_page_state(zone, NR_FILE_DIRTY);

	/* Watch for any possible underflows due to delta */
	if (unlikely(delta > nr_pagecache_reclaimable))
		delta = nr_pagecache_reclaimable;

	return nr_pagecache_reclaimable - delta;
}

/*
 * Try to free up some pages from this zone through reclaim.
 */
static int __zone_reclaim(struct zone *zone, gfp_t gfp_mask, unsigned int order)
{
	/* Minimum pages needed in order to stay on node */
	const unsigned long nr_pages = 1 << order;
	struct task_struct *p = current;
	struct reclaim_state reclaim_state;
	int priority;
	struct scan_control sc = {
		.may_writepage = !!(zone_reclaim_mode & RECLAIM_WRITE),
		.may_unmap = !!(zone_reclaim_mode & RECLAIM_SWAP),
		.may_swap = 1,
		.swap_cluster_max = max_t(unsigned long, nr_pages,
					SWAP_CLUSTER_MAX),
		.gfp_mask = gfp_mask,
		.swappiness = vm_swappiness,
		.order = order,
		.isolate_pages = isolate_pages_global,
	};
	unsigned long slab_reclaimable;

	disable_swap_token();
	cond_resched();
	/*
	 * We need to be able to allocate from the reserves for RECLAIM_SWAP
	 * and we also need to be able to write out pages for RECLAIM_WRITE
	 * and RECLAIM_SWAP.
	 */
	p->flags |= PF_MEMALLOC | PF_SWAPWRITE;
	reclaim_state.reclaimed_slab = 0;
	p->reclaim_state = &reclaim_state;

	if (zone_pagecache_reclaimable(zone) > zone->min_unmapped_pages) {
		/*
		 * Free memory by calling shrink zone with increasing
		 * priorities until we have enough memory freed.
		 */
		priority = ZONE_RECLAIM_PRIORITY;
		do {
			note_zone_scanning_priority(zone, priority);
			shrink_zone(priority, zone, &sc);
			priority--;
		} while (priority >= 0 && sc.nr_reclaimed < nr_pages);
	}

	slab_reclaimable = zone_page_state(zone, NR_SLAB_RECLAIMABLE);
	if (slab_reclaimable > zone->min_slab_pages) {
		/*
		 * shrink_slab() does not currently allow us to determine how
		 * many pages were freed in this zone. So we take the current
		 * number of slab pages and shake the slab until it is reduced
		 * by the same nr_pages that we used for reclaiming unmapped
		 * pages.
		 *
		 * Note that shrink_slab will free memory on all zones and may
		 * take a long time.
		 */
		while (shrink_slab(sc.nr_scanned, gfp_mask, order) &&
			zone_page_state(zone, NR_SLAB_RECLAIMABLE) >
				slab_reclaimable - nr_pages)
			;

		/*
		 * Update nr_reclaimed by the number of slab pages we
		 * reclaimed from this zone.
		 */
		sc.nr_reclaimed += slab_reclaimable -
			zone_page_state(zone, NR_SLAB_RECLAIMABLE);
	}

	p->reclaim_state = NULL;
	current->flags &= ~(PF_MEMALLOC | PF_SWAPWRITE);
	return sc.nr_reclaimed >= nr_pages;
}

int zone_reclaim(struct zone *zone, gfp_t gfp_mask, unsigned int order)
{
	int node_id;
	int ret;

	/*
	 * Zone reclaim reclaims unmapped file backed pages and
	 * slab pages if we are over the defined limits.
	 *
	 * A small portion of unmapped file backed pages is needed for
	 * file I/O otherwise pages read by file I/O will be immediately
	 * thrown out if the zone is overallocated. So we do not reclaim
	 * if less than a specified percentage of the zone is used by
	 * unmapped file backed pages.
	 */
	if (zone_pagecache_reclaimable(zone) <= zone->min_unmapped_pages &&
	    zone_page_state(zone, NR_SLAB_RECLAIMABLE) <= zone->min_slab_pages)
		return ZONE_RECLAIM_FULL;

	if (zone_is_all_unreclaimable(zone))
		return ZONE_RECLAIM_FULL;

	/*
	 * Do not scan if the allocation should not be delayed.
	 */
	if (!(gfp_mask & __GFP_WAIT) || (current->flags & PF_MEMALLOC))
		return ZONE_RECLAIM_NOSCAN;

	/*
	 * Only run zone reclaim on the local zone or on zones that do not
	 * have associated processors. This will favor the local processor
	 * over remote processors and spread off node memory allocations
	 * as wide as possible.
	 */
	node_id = zone_to_nid(zone);
	if (node_state(node_id, N_CPU) && node_id != numa_node_id())
		return ZONE_RECLAIM_NOSCAN;

	if (zone_test_and_set_flag(zone, ZONE_RECLAIM_LOCKED))
		return ZONE_RECLAIM_NOSCAN;

	ret = __zone_reclaim(zone, gfp_mask, order);
	zone_clear_flag(zone, ZONE_RECLAIM_LOCKED);

	if (!ret)
		count_vm_event(PGSCAN_ZONE_RECLAIM_FAILED);

	return ret;
}
#endif

/*
 * page_evictable - test whether a page is evictable
 * @page: the page to test
 * @vma: the VMA in which the page is or will be mapped, may be NULL
 *
 * Test whether page is evictable--i.e., should be placed on active/inactive
 * lists vs unevictable list.  The vma argument is !NULL when called from the
 * fault path to determine how to instantate a new page.
 *
 * Reasons page might not be evictable:
 * (1) page's mapping marked unevictable
 * (2) page is part of an mlocked VMA
 *
 */
int page_evictable(struct page *page, struct vm_area_struct *vma)
{

	if (mapping_unevictable(page_mapping(page)))
		return 0;

	if (PageMlocked(page) || (vma && is_mlocked_vma(vma, page)))
		return 0;

	return 1;
}

/**
 * check_move_unevictable_page - check page for evictability and move to appropriate zone lru list
 * @page: page to check evictability and move to appropriate lru list
 * @zone: zone page is in
 *
 * Checks a page for evictability and moves the page to the appropriate
 * zone lru list.
 *
 * Restrictions: zone->lru_lock must be held, page must be on LRU and must
 * have PageUnevictable set.
 */
static void check_move_unevictable_page(struct page *page, struct zone *zone)
{
	VM_BUG_ON(PageActive(page));

retry:
	ClearPageUnevictable(page);
	if (page_evictable(page, NULL)) {
		enum lru_list l = page_lru_base_type(page);

		__dec_zone_state(zone, NR_UNEVICTABLE);
		list_move(&page->lru, &zone->lru[l].list);
		mem_cgroup_move_lists(page, LRU_UNEVICTABLE, l);
		__inc_zone_state(zone, NR_INACTIVE_ANON + l);
		__count_vm_event(UNEVICTABLE_PGRESCUED);
	} else {
		/*
		 * rotate unevictable list
		 */
		SetPageUnevictable(page);
		list_move(&page->lru, &zone->lru[LRU_UNEVICTABLE].list);
		mem_cgroup_rotate_lru_list(page, LRU_UNEVICTABLE);
		if (page_evictable(page, NULL))
			goto retry;
	}
}

/**
 * scan_mapping_unevictable_pages - scan an address space for evictable pages
 * @mapping: struct address_space to scan for evictable pages
 *
 * Scan all pages in mapping.  Check unevictable pages for
 * evictability and move them to the appropriate zone lru list.
 */
void scan_mapping_unevictable_pages(struct address_space *mapping)
{
	pgoff_t next = 0;
	pgoff_t end   = (i_size_read(mapping->host) + PAGE_CACHE_SIZE - 1) >>
			 PAGE_CACHE_SHIFT;
	struct zone *zone;
	struct pagevec pvec;

	if (mapping->nrpages == 0)
		return;

	pagevec_init(&pvec, 0);
	while (next < end &&
		pagevec_lookup(&pvec, mapping, next, PAGEVEC_SIZE)) {
		int i;
		int pg_scanned = 0;

		zone = NULL;

		for (i = 0; i < pagevec_count(&pvec); i++) {
			struct page *page = pvec.pages[i];
			pgoff_t page_index = page->index;
			struct zone *pagezone = page_zone(page);

			pg_scanned++;
			if (page_index > next)
				next = page_index;
			next++;

			if (pagezone != zone) {
				if (zone)
					spin_unlock_irq(&zone->lru_lock);
				zone = pagezone;
				spin_lock_irq(&zone->lru_lock);
			}

			if (PageLRU(page) && PageUnevictable(page))
				check_move_unevictable_page(page, zone);
		}
		if (zone)
			spin_unlock_irq(&zone->lru_lock);
		pagevec_release(&pvec);

		count_vm_events(UNEVICTABLE_PGSCANNED, pg_scanned);
	}

}

/**
 * scan_zone_unevictable_pages - check unevictable list for evictable pages
 * @zone - zone of which to scan the unevictable list
 *
 * Scan @zone's unevictable LRU lists to check for pages that have become
 * evictable.  Move those that have to @zone's inactive list where they
 * become candidates for reclaim, unless shrink_inactive_zone() decides
 * to reactivate them.  Pages that are still unevictable are rotated
 * back onto @zone's unevictable list.
 */
#define SCAN_UNEVICTABLE_BATCH_SIZE 16UL /* arbitrary lock hold batch size */
static void scan_zone_unevictable_pages(struct zone *zone)
{
	struct list_head *l_unevictable = &zone->lru[LRU_UNEVICTABLE].list;
	unsigned long scan;
	unsigned long nr_to_scan = zone_page_state(zone, NR_UNEVICTABLE);

	while (nr_to_scan > 0) {
		unsigned long batch_size = min(nr_to_scan,
						SCAN_UNEVICTABLE_BATCH_SIZE);

		spin_lock_irq(&zone->lru_lock);
		for (scan = 0;  scan < batch_size; scan++) {
			struct page *page = lru_to_page(l_unevictable);

			if (!trylock_page(page))
				continue;

			prefetchw_prev_lru_page(page, l_unevictable, flags);

			if (likely(PageLRU(page) && PageUnevictable(page)))
				check_move_unevictable_page(page, zone);

			unlock_page(page);
		}
		spin_unlock_irq(&zone->lru_lock);

		nr_to_scan -= batch_size;
	}
}


/**
 * scan_all_zones_unevictable_pages - scan all unevictable lists for evictable pages
 *
 * A really big hammer:  scan all zones' unevictable LRU lists to check for
 * pages that have become evictable.  Move those back to the zones'
 * inactive list where they become candidates for reclaim.
 * This occurs when, e.g., we have unswappable pages on the unevictable lists,
 * and we add swap to the system.  As such, it runs in the context of a task
 * that has possibly/probably made some previously unevictable pages
 * evictable.
 */
static void scan_all_zones_unevictable_pages(void)
{
	struct zone *zone;

	for_each_zone(zone) {
		scan_zone_unevictable_pages(zone);
	}
}

/*
 * scan_unevictable_pages [vm] sysctl handler.  On demand re-scan of
 * all nodes' unevictable lists for evictable pages
 */
unsigned long scan_unevictable_pages;

int scan_unevictable_handler(struct ctl_table *table, int write,
			   void __user *buffer,
			   size_t *length, loff_t *ppos)
{
	proc_doulongvec_minmax(table, write, buffer, length, ppos);

	if (write && *(unsigned long *)table->data)
		scan_all_zones_unevictable_pages();

	scan_unevictable_pages = 0;
	return 0;
}

/*
 * per node 'scan_unevictable_pages' attribute.  On demand re-scan of
 * a specified node's per zone unevictable lists for evictable pages.
 */

static ssize_t read_scan_unevictable_node(struct sys_device *dev,
					  struct sysdev_attribute *attr,
					  char *buf)
{
	return sprintf(buf, "0\n");	/* always zero; should fit... */
}

static ssize_t write_scan_unevictable_node(struct sys_device *dev,
					   struct sysdev_attribute *attr,
					const char *buf, size_t count)
{
	struct zone *node_zones = NODE_DATA(dev->id)->node_zones;
	struct zone *zone;
	unsigned long res;
	unsigned long req = strict_strtoul(buf, 10, &res);

	if (!req)
		return 1;	/* zero is no-op */

	for (zone = node_zones; zone - node_zones < MAX_NR_ZONES; ++zone) {
		if (!populated_zone(zone))
			continue;
		scan_zone_unevictable_pages(zone);
	}
	return 1;
}


static SYSDEV_ATTR(scan_unevictable_pages, S_IRUGO | S_IWUSR,
			read_scan_unevictable_node,
			write_scan_unevictable_node);

int scan_unevictable_register_node(struct node *node)
{
	return sysdev_create_file(&node->sysdev, &attr_scan_unevictable_pages);
}

void scan_unevictable_unregister_node(struct node *node)
{
	sysdev_remove_file(&node->sysdev, &attr_scan_unevictable_pages);
}

