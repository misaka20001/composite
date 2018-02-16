#include <cobj_format.h>
#include <cos_alloc.h>
#include <cos_debug.h>
#include <cos_types.h>
#include <llprint.h>
#include <cos_defkernel_api.h>
#include <sl.h>
#include <string.h>
#include <llbooter_inv.h>

#define UNDEF_SYMBS 64
#define CMP_UTYPMEM_SZ (1 << 27)
#define THD_PRIO 5
#define THD_PERIOD 10
#define THD_BUDGET 5

int num_cobj;

/* Assembly function for sinv from new component */
extern void *__inv_test_entry(int a, int b, int c);
void boot_pgtbl_cap_transfer_at(int dst, int src, int cap_slot);
void boot_thd_cap_transfer_at(int dst, int src, int cap_slot);

struct cobj_header *hs[MAX_NUM_SPDS + 1];

#define UDP_SPDID 3

/* The booter uses this to keep track of each comp */
struct comp_cap_info {
	struct cos_compinfo    *compinfo;
	struct cos_defcompinfo *defcompinfo;
	struct usr_inv_cap      ST_user_caps[UNDEF_SYMBS];
	vaddr_t                 vaddr_user_caps; // vaddr of user caps table in comp
	vaddr_t                 addr_start;
	vaddr_t                 vaddr_mapped_in_booter;
	vaddr_t                 upcall_entry;
} new_comp_cap_info[MAX_NUM_SPDS + 1];

typedef enum {
	BOOT_COMP_FLAG_SCHED = 1,
	BOOT_COMP_FLAG_MM    = 1<<1,
} boot_comp_flag_t;

struct cos_defcompinfo new_defcompinfo[MAX_NUM_SPDS + 1];

int	        schedule[MAX_NUM_SPDS + 1];
volatile size_t sched_cur;

static vaddr_t
boot_deps_map_sect(spdid_t spdid, vaddr_t dest_daddr)
{
	vaddr_t addr = (vaddr_t)cos_page_bump_alloc(boot_info());
	assert(addr);

	if (cos_mem_alias_at(new_comp_cap_info[spdid].compinfo, dest_daddr, boot_info(), addr)) BUG();

	return addr;
}

static void
boot_comp_pgtbl_expand(size_t n_pte, pgtblcap_t pt, vaddr_t vaddr, struct cobj_header *h)
{
	size_t i;
	int tot = 0;
	/* Expand Page table, could do this faster */
	for (i = 0; i < (size_t)h->nsect; i++) {
		tot += cobj_sect_size(h, i);
	}

	if (tot > SERVICE_SIZE) {
		n_pte = tot / SERVICE_SIZE;
		if (tot % SERVICE_SIZE) n_pte++;
	}

	for (i = 0; i < n_pte; i++) {
		if (!cos_pgtbl_intern_alloc(boot_info(), pt, vaddr, SERVICE_SIZE)) BUG();
		/* Increment vaddr incase we loop again */
		vaddr += SERVICE_SIZE;
	}
}

/* Initialize just the captblcap and pgtblcap, due to hack for upcall_fn addr */
static void
boot_compinfo_init(spdid_t spdid, captblcap_t *ct, pgtblcap_t *pt, u32_t vaddr)
{
	*ct = cos_captbl_alloc(boot_info());
	assert(*ct);
	*pt = cos_pgtbl_alloc(boot_info());
	assert(*pt);

	new_comp_cap_info[spdid].defcompinfo = &new_defcompinfo[spdid];
	new_comp_cap_info[spdid].compinfo    = cos_compinfo_get(&new_defcompinfo[spdid]);
	cos_compinfo_init(cos_compinfo_get(new_comp_cap_info[spdid].defcompinfo),
			*pt, *ct, 0, (vaddr_t)vaddr, BOOT_CAPTBL_FREE, boot_info());
}

static void
boot_newcomp_sinv_alloc(spdid_t spdid)
{
	sinvcap_t sinv;
	int i = 0;
	int intr_spdid;
	void *user_cap_vaddr;
	struct cos_compinfo *interface_compinfo;
	struct cos_compinfo *newcomp_compinfo = new_comp_cap_info[spdid].compinfo;
	/* TODO: Purge rest of booter of spdid convention */
	unsigned long token = (unsigned long)spdid;

	/*
	 * Loop through all undefined symbs
	 */
	for (i = 0; i < UNDEF_SYMBS; i++) {
		if ( new_comp_cap_info[spdid].ST_user_caps[i].service_entry_inst > 0) {

			intr_spdid = new_comp_cap_info[spdid].ST_user_caps[i].invocation_count;
			interface_compinfo = new_comp_cap_info[intr_spdid].compinfo;
			user_cap_vaddr = (void *) (new_comp_cap_info[spdid].vaddr_mapped_in_booter + (new_comp_cap_info[spdid].vaddr_user_caps - new_comp_cap_info[spdid].addr_start) + (sizeof(struct usr_inv_cap) * i));

			/* Create sinv capability from client to server */
			sinv = cos_sinv_alloc(newcomp_compinfo, interface_compinfo->comp_cap,
				(vaddr_t)new_comp_cap_info[spdid].ST_user_caps[i].service_entry_inst,
				token);

			assert(sinv > 0);

			new_comp_cap_info[spdid].ST_user_caps[i].cap_no = sinv;

			/*
			 * Now that we have the sinv allocated, we can copy in the symb user
			 * cap to correct index
			 */
			memcpy(user_cap_vaddr, &new_comp_cap_info[spdid].ST_user_caps[i],
					sizeof(struct usr_inv_cap));
		}
	}
}

static void
boot_newcomp_definfo_init(spdid_t spdid, compcap_t cc, boot_comp_flag_t comp_flags)
{

	struct cos_defcompinfo *defci     = cos_defcompinfo_curr_get();
	struct cos_aep_info *   sched_aep = cos_sched_aep_get(defci);
	struct cos_aep_info *   child_aep = cos_sched_aep_get(new_comp_cap_info[spdid].defcompinfo);
	struct cos_compinfo *   child_ci  = cos_compinfo_get(new_comp_cap_info[spdid].defcompinfo);
	int i = 0;

	child_ci->comp_cap = cc;
	child_aep->thd = cos_initthd_alloc(boot_info, child_ci->comp_cap);
	while (schedule[i] != 0) i++;
	schedule[i] = child_aep->thd;

	if (comp_flags == BOOT_COMP_FLAG_SCHED) {
		assert(child_aep->thd);
		child_aep->tc = cos_tcap_alloc(boot_info());
		assert(child_aep->tc);

		child_aep->rcv = cos_arcv_alloc(boot_info(), child_aep->thd, child_aep->tc,
				boot_info()->comp_cap, sched_aep->rcv);
		assert(child_aep->rcv);
	} else {
		child_aep->tc  = sched_aep->tc;
		child_aep->rcv = sched_aep->rcv;
	}

	child_aep->fn   = NULL;
	child_aep->data = NULL;
}

static void
boot_newschedcomp_cap_init(spdid_t spdid, captblcap_t ct, pgtblcap_t pt, compcap_t cc) 
{
	struct cos_aep_info *   child_aep = cos_sched_aep_get(new_comp_cap_info[spdid].defcompinfo);
	pgtblcap_t              untype_pt;

	untype_pt = cos_pgtbl_alloc(boot_info());
	assert(untype_pt);
	cos_meminfo_init(&(new_comp_cap_info[spdid].compinfo->mi), BOOT_MEM_KM_BASE,
			CMP_UTYPMEM_SZ, untype_pt);

	cos_cap_cpy_at(new_comp_cap_info[spdid].compinfo, BOOT_CAPTBL_SELF_INITHW_BASE,
			boot_info(), BOOT_CAPTBL_SELF_INITHW_BASE);
	cos_cap_cpy_at(new_comp_cap_info[spdid].compinfo, BOOT_CAPTBL_SELF_CT,
			boot_info(), ct);
	cos_cap_cpy_at(new_comp_cap_info[spdid].compinfo, BOOT_CAPTBL_SELF_UNTYPED_PT,
			boot_info(), untype_pt);

	cos_cap_cpy_at(new_comp_cap_info[spdid].compinfo, BOOT_CAPTBL_SELF_PT, boot_info(), pt);
	cos_cap_cpy_at(new_comp_cap_info[spdid].compinfo, BOOT_CAPTBL_SELF_COMP, boot_info(), cc);

	/* Populate untyped memory */
	cos_meminfo_alloc(new_comp_cap_info[spdid].compinfo, BOOT_MEM_KM_BASE, CMP_UTYPMEM_SZ);

	cos_cap_cpy_at(new_comp_cap_info[spdid].compinfo, BOOT_CAPTBL_SELF_INITTHD_BASE,
			boot_info(), child_aep->thd);
	cos_cap_cpy_at(new_comp_cap_info[spdid].compinfo, BOOT_CAPTBL_SELF_INITTCAP_BASE,
			boot_info(), child_aep->tc);
	cos_cap_cpy_at(new_comp_cap_info[spdid].compinfo, BOOT_CAPTBL_SELF_INITRCV_BASE,
			boot_info(), child_aep->rcv);
}

static void
boot_newcomp_create(spdid_t spdid, struct cos_compinfo *comp_info, boot_comp_flag_t comp_flags)
{
	compcap_t      cc;
	captblcap_t    ct = new_comp_cap_info[spdid].compinfo->captbl_cap;
	pgtblcap_t     pt = new_comp_cap_info[spdid].compinfo->pgtbl_cap;
	sinvcap_t      sinv;
	struct sl_thd *thd;

	cc = cos_comp_alloc(boot_info(), ct, pt, (vaddr_t)new_comp_cap_info[spdid].upcall_entry);

	assert(cc);
	/* manually laying out struct due to upcall addr calculation */
	boot_newcomp_definfo_init(spdid, cc, comp_flags);

	/* Create sinv capability from Components to Booter */
	sinv = cos_sinv_alloc(boot_info(), boot_info()->comp_cap, (vaddr_t)__inv_test_entry,
			(unsigned long)spdid);

	assert(sinv);

	cos_cap_cpy_at(new_comp_cap_info[spdid].compinfo, BOOT_CAPTBL_SINV_CAP, boot_info(), sinv);

	boot_newcomp_sinv_alloc(spdid);

	//boot_newschedcomp_cap_init(spdid, ct, pt, cc);
	//if (is_sched || is_shdmem || spdid == 2) boot_newschedcomp_cap_init(spdid, ct, pt, cc);
	if (comp_flags) {
		boot_newschedcomp_cap_init(spdid, ct, pt, cc);

		thd = sl_thd_comp_init(new_comp_cap_info[spdid].defcompinfo, comp_flags);
		assert(thd);
		sl_thd_param_set(thd, sched_param_pack(SCHEDP_PRIO, THD_PRIO));
	}

	if (comp_flags == BOOT_COMP_FLAG_SCHED) {
		sl_thd_param_set(thd, sched_param_pack(SCHEDP_BUDGET, THD_BUDGET * 1000));
		sl_thd_param_set(thd, sched_param_pack(SCHEDP_WINDOW, THD_PERIOD * 1000));
	}

}

static void
boot_bootcomp_init(void)
{
	cos_defcompinfo_init();

	cos_meminfo_init(&(boot_info()->mi), BOOT_MEM_KM_BASE, COS_MEM_KERN_PA_SZ,
			BOOT_CAPTBL_SELF_UNTYPED_PT);
	sl_init(SL_MIN_PERIOD_US);
}

static void
boot_done(void)
{
//	cos_thd_switch(schedule[sched_cur]);
	sl_sched_loop();
}

static int
boot_check_scheduler(char *comp_name)
{
	int ret;
	const char prefix[] = "sl_";

	ret = strncmp(comp_name, prefix, sizeof(prefix)-1);

	return !ret;
}

static int
boot_check_shdmem(char *comp_name)
{
	int ret;
	const char prefix[] = "shmem";

	ret = strncmp(comp_name, prefix, sizeof(prefix)-1);

	return !ret;
}

void
boot_thd_done(void)
{
	printc("Done booting\n\n");
	while(1) sl_thd_block(0);
}

void
boot_pgtbl_cap_transfer_at(int dst, int src, int cap_slot)
{
	return cos_cap_cpy_at(new_comp_cap_info[dst].compinfo, cap_slot, boot_info(),
			new_comp_cap_info[src].compinfo->pgtbl_cap);
}

void
boot_thd_cap_transfer_at(int dst, int src, int cap_slot)
{
	return cos_cap_cpy_at(new_comp_cap_info[dst].compinfo, cap_slot, boot_info(),
			new_comp_cap_info[src].compinfo->comp_cap);
}


long
boot_sinv_fn(boot_hyp_op_t op, void *arg1, void *arg2, void *arg3)
{
	long ret = -ENOTSUP;

	switch (op) {
	case INIT_DONE:
		/*
		 * DEPRECATED:
		 * Should not be needed as llbooter is not scheduler
		 * Kept for the purpose of older test code like llbooter_pong.sh
		 */
		boot_thd_done();
		break;
	case BOOT_HYP_PGTBL_CAP:
		/* arg1: dst comp, arg2: src comp, arg3: cap_slot */
		ret = boot_pgtbl_cap_transfer_at((int)arg1, (int)arg2, (int)arg3);
		break;
	case BOOT_HYP_NUM_COMPS:
		/* num_cobj doesn't include the booter itself */
		ret = num_cobj;
		break;
	case BOOT_HYP_SINV_CAP:
		printc("Sinv cap request not implemented\n");
		break;
	case BOOT_HYP_CAP_FRONTIER:
		/* arg1: spdid */
		ret = new_comp_cap_info[(int)arg1].compinfo->cap_frontier;
		break;
	case BOOT_HYP_COMP_CAP:
		/* arg1: dst comp spdid, arg2: src comp spdid, arg3: cap_slot */
		ret = boot_comp_cap_transfer((int)arg1, (int)arg2, (int)arg3);
		break;
	}

	return ret;
}
