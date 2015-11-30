/*
 * Support KVM gust page tracking
 *
 * This feature allows us to track page access in guest. Currently, only
 * write access is tracked.
 *
 * Copyright(C) 2015 Intel Corporation.
 *
 * Author:
 *   Xiao Guangrong <guangrong.xiao@linux.intel.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 */

#include <linux/kvm_host.h>
#include <asm/kvm_host.h>
#include <asm/kvm_page_track.h>

#include "mmu.h"

static void page_track_slot_free(struct kvm_memory_slot *slot)
{
	int i;

	for (i = 0; i < KVM_PAGE_TRACK_MAX; i++)
		if (slot->arch.gfn_track[i]) {
			kvfree(slot->arch.gfn_track[i]);
			slot->arch.gfn_track[i] = NULL;
		}
}

int kvm_page_track_create_memslot(struct kvm_memory_slot *slot,
				  unsigned long npages)
{
	int  i, pages = gfn_to_index(slot->base_gfn + npages - 1,
				  slot->base_gfn, PT_PAGE_TABLE_LEVEL) + 1;

	for (i = 0; i < KVM_PAGE_TRACK_MAX; i++) {
		slot->arch.gfn_track[i] = kvm_kvzalloc(pages *
					    sizeof(*slot->arch.gfn_track[i]));
		if (!slot->arch.gfn_track[i])
			goto track_free;
	}

	return 0;

track_free:
	page_track_slot_free(slot);
	return -ENOMEM;
}

void kvm_page_track_free_memslot(struct kvm_memory_slot *free,
				 struct kvm_memory_slot *dont)
{
	if (!dont || free->arch.gfn_track != dont->arch.gfn_track)
		page_track_slot_free(free);
}

static bool check_mode(enum kvm_page_track_mode mode)
{
	if (mode < 0 || mode >= KVM_PAGE_TRACK_MAX)
		return false;

	return true;
}

static void update_gfn_track(struct kvm_memory_slot *slot, gfn_t gfn,
			     enum kvm_page_track_mode mode, int count)
{
	int index, val;

	index = gfn_to_index(gfn, slot->base_gfn, PT_PAGE_TABLE_LEVEL);

	slot->arch.gfn_track[mode][index] += count;
	val = slot->arch.gfn_track[mode][index];
	WARN_ON(val < 0);
}

void
kvm_slot_page_track_add_page_nolock(struct kvm *kvm,
				    struct kvm_memory_slot *slot, gfn_t gfn,
				    enum kvm_page_track_mode mode)
{
	WARN_ON(!check_mode(mode));

	update_gfn_track(slot, gfn, mode, 1);

	/*
	 * new track stops large page mapping for the
	 * tracked page.
	 */
	kvm_mmu_gfn_disallow_lpage(slot, gfn);

	if (mode == KVM_PAGE_TRACK_WRITE)
		if (kvm_mmu_slot_gfn_write_protect(kvm, slot, gfn))
			kvm_flush_remote_tlbs(kvm);
}

/*
 * add guest page to the tracking pool so that corresponding access on that
 * page will be intercepted.
 *
 * It should be called under the protection of kvm->srcu or kvm->slots_lock
 *
 * @kvm: the guest instance we are interested in.
 * @gfn: the guest page.
 * @mode: tracking mode, currently only write track is supported.
 */
void kvm_page_track_add_page(struct kvm *kvm, gfn_t gfn,
			     enum kvm_page_track_mode mode)
{
	struct kvm_memslots *slots;
	struct kvm_memory_slot *slot;
	int i;

	WARN_ON(!check_mode(mode));

	for (i = 0; i < KVM_ADDRESS_SPACE_NUM; i++) {
		slots = __kvm_memslots(kvm, i);
		slot = __gfn_to_memslot(slots, gfn);

		spin_lock(&kvm->mmu_lock);
		kvm_slot_page_track_add_page_nolock(kvm, slot, gfn, mode);
		spin_unlock(&kvm->mmu_lock);
	}
}

void kvm_slot_page_track_remove_page_nolock(struct kvm *kvm,
					    struct kvm_memory_slot *slot,
					    gfn_t gfn,
					    enum kvm_page_track_mode mode)
{
	WARN_ON(!check_mode(mode));

	update_gfn_track(slot, gfn, mode, -1);

	/*
	 * allow large page mapping for the tracked page
	 * after the tracker is gone.
	 */
	kvm_mmu_gfn_allow_lpage(slot, gfn);
}

/*
 * remove the guest page from the tracking pool which stops the interception
 * of corresponding access on that page. It is the opposed operation of
 * kvm_page_track_add_page().
 *
 * It should be called under the protection of kvm->srcu or kvm->slots_lock
 *
 * @kvm: the guest instance we are interested in.
 * @gfn: the guest page.
 * @mode: tracking mode, currently only write track is supported.
 */
void kvm_page_track_remove_page(struct kvm *kvm, gfn_t gfn,
				enum kvm_page_track_mode mode)
{
	struct kvm_memslots *slots;
	struct kvm_memory_slot *slot;
	int i;

	for (i = 0; i < KVM_ADDRESS_SPACE_NUM; i++) {
		slots = __kvm_memslots(kvm, i);
		slot = __gfn_to_memslot(slots, gfn);

		spin_lock(&kvm->mmu_lock);
		kvm_slot_page_track_remove_page_nolock(kvm, slot, gfn, mode);
		spin_unlock(&kvm->mmu_lock);
	}
}

/*
 * check if the corresponding access on the specified guest page is tracked.
 */
bool kvm_page_track_check_mode(struct kvm_vcpu *vcpu, gfn_t gfn,
			       enum kvm_page_track_mode mode)
{
	struct kvm_memory_slot *slot = kvm_vcpu_gfn_to_memslot(vcpu, gfn);
	int index = gfn_to_index(gfn, slot->base_gfn, PT_PAGE_TABLE_LEVEL);

	WARN_ON(!check_mode(mode));

	return !!ACCESS_ONCE(slot->arch.gfn_track[mode][index]);
}

void kvm_page_track_init(struct kvm *kvm)
{
	struct kvm_page_track_notifier_head *head;

	head = &kvm->arch.track_notifier_head;
	init_srcu_struct(&head->track_srcu);
	INIT_HLIST_HEAD(&head->track_notifier_list);
}

/*
 * register the notifier so that event interception for the tracked guest
 * pages can be received.
 */
void
kvm_page_track_register_notifier(struct kvm *kvm,
				 struct kvm_page_track_notifier_node *n)
{
	struct kvm_page_track_notifier_head *head;

	head = &kvm->arch.track_notifier_head;

	spin_lock(&kvm->mmu_lock);
	hlist_add_head_rcu(&n->node, &head->track_notifier_list);
	spin_unlock(&kvm->mmu_lock);
}

/*
 * stop receiving the event interception. It is the opposed operation of
 * kvm_page_track_register_notifier().
 */
void
kvm_page_track_unregister_notifier(struct kvm *kvm,
				   struct kvm_page_track_notifier_node *n)
{
	struct kvm_page_track_notifier_head *head;

	head = &kvm->arch.track_notifier_head;

	spin_lock(&kvm->mmu_lock);
	hlist_del_rcu(&n->node);
	spin_unlock(&kvm->mmu_lock);
	synchronize_srcu(&head->track_srcu);
}

/*
 * Notify the node that write access is intercepted and write emulation is
 * finished at this time.
 *
 * The node should figure out if the written page is the one that node is
 * interested in by itself.
 */
void kvm_page_track_write(struct kvm_vcpu *vcpu, gpa_t gpa, const u8 *new,
			  int bytes)
{
	struct kvm_page_track_notifier_head *head;
	struct kvm_page_track_notifier_node *n;
	int idx;

	head = &vcpu->kvm->arch.track_notifier_head;

	idx = srcu_read_lock(&head->track_srcu);
	hlist_for_each_entry_rcu(n, &head->track_notifier_list, node)
		if (n->track_write)
			n->track_write(vcpu, gpa, new, bytes);
	srcu_read_unlock(&head->track_srcu, idx);
}
