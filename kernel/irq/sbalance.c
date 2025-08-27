// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023-2024 Sultan Alsawaf <sultan@kerneltoast.com>.
 */

/**
 * DOC: SBalance description
 *
 * This is a simple IRQ balancer that polls every X number of milliseconds and
 * moves IRQs from the most interrupt-heavy CPU to the least interrupt-heavy
 * CPUs until the heaviest CPU is no longer the heaviest. IRQs are only moved
 * from one source CPU to any number of destination CPUs per balance run.
 * Balancing is skipped if the gap between the most interrupt-heavy CPU and the
 * least interrupt-heavy CPU is below the configured threshold of interrupts.
 *
 * The heaviest IRQs are targeted for migration in order to reduce the number of
 * IRQs to migrate. If moving an IRQ would reduce overall balance, then it won't
 * be migrated.
 *
 * The most interrupt-heavy CPU is calculated by scaling the number of new
 * interrupts on that CPU to the CPU's current capacity. This way, interrupt
 * heaviness takes into account factors such as thermal pressure and time spent
 * processing interrupts rather than just the sheer number of them. This also
 * makes SBalance aware of CPU asymmetry, where different CPUs can have
 * different performance capacities and be proportionally balanced.
 */

#define pr_fmt(fmt) "sbalance: " fmt

#include <linux/freezer.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/list_sort.h>
#include "../sched/sched.h"
#include "internals.h"

/* Perform IRQ balancing every POLL_MS milliseconds */
#define POLL_MS CONFIG_IRQ_SBALANCE_POLL_MSEC

/*
 * There needs to be a difference of at least this many new interrupts between
 * the heaviest and least-heavy CPUs during the last polling window in order for
 * balancing to occur. This is to avoid balancing when the system is quiet.
 *
 * This threshold is compared to the _scaled_ interrupt counts per CPU; i.e.,
 * the number of interrupts scaled to the CPU's capacity.
 */
#define IRQ_SCALED_THRESH CONFIG_IRQ_SBALANCE_THRESH

enum irq_type {
	IRQ_TYPE_NORMAL,
	IRQ_TYPE_CRITICAL,
	IRQ_TYPE_PERFORMANCE,
};

static const char * const critical_performance_irqs[] = {
	"msm_drm", "touchpanel", "dsi_ctrl", "display_rsc", "kgsl-3d0"
};

static const char * const performance_irqs[] = {
	"1d84000.ufshc", "apps_rsc", "gsi", "i2c_geni", "msm_serial_geni0",
	"glink-native-cdsp", "glink-native-adsp", "glink-native-slpi"
};

struct bal_irq {
	struct list_head node;
	struct list_head move_node;
	struct rcu_head rcu;
	struct irq_desc *desc;
	unsigned int delta_nr;
	unsigned int old_nr;
	int prev_cpu;
	enum irq_type type;
};

struct bal_domain {
	struct list_head movable_irqs;
	unsigned long old_total;
	unsigned int intrs;
	int cpu;
};

static LIST_HEAD(bal_irq_list);
static DEFINE_SPINLOCK(bal_irq_lock);
static DEFINE_PER_CPU(struct bal_domain, balance_data);
static DEFINE_PER_CPU(unsigned long, cpu_cap);
static cpumask_t cpu_exclude_mask __read_mostly;

static enum irq_type get_irq_type(struct irq_desc *desc)
{
	const char *name;
	int i;

	if (!desc->action || !desc->action->name)
		return IRQ_TYPE_NORMAL;
	name = desc->action->name;

	for (i = 0; i < ARRAY_SIZE(critical_performance_irqs); i++) {
		if (strcmp(name, critical_performance_irqs[i]) == 0)
			return IRQ_TYPE_CRITICAL;
	}
	for (i = 0; i < ARRAY_SIZE(performance_irqs); i++) {
		if (strcmp(name, performance_irqs[i]) == 0)
			return IRQ_TYPE_PERFORMANCE;
	}
	return IRQ_TYPE_NORMAL;
}

void sbalance_desc_add(struct irq_desc *desc)
{
	struct bal_irq *bi = kmalloc(sizeof(*bi), GFP_KERNEL);
	if (WARN_ON(!bi)) return;
	*bi = (typeof(*bi)){ .desc = desc, .type = IRQ_TYPE_NORMAL };
	spin_lock(&bal_irq_lock);
	list_add_tail_rcu(&bi->node, &bal_irq_list);
	spin_unlock(&bal_irq_lock);
}

void sbalance_desc_del(struct irq_desc *desc)
{
	struct bal_irq *bi;
	spin_lock(&bal_irq_lock);
	list_for_each_entry(bi, &bal_irq_list, node) {
		if (bi->desc == desc) {
			list_del_rcu(&bi->node);
			kfree_rcu(bi, rcu);
			break;
		}
	}
	spin_unlock(&bal_irq_lock);
}

static int bal_irq_move_node_cmp(void *priv, struct list_head *lhs_p, struct list_head *rhs_p)
{
	const struct bal_irq *lhs = list_entry(lhs_p, typeof(*lhs), move_node);
	const struct bal_irq *rhs = list_entry(rhs_p, typeof(*rhs), move_node);
	return rhs->delta_nr - lhs->delta_nr;
}

/* Returns false if this IRQ should be totally ignored for this balancing run */
static bool update_irq_data(struct bal_irq *bi, int *cpu)
{
	struct irq_desc *desc = bi->desc;
	unsigned int nr;

	/*
	 * Get the CPU which currently has this IRQ affined. Due to hardware and
	 * irqchip driver quirks, a previously set affinity may not match the
	 * actual affinity of the IRQ. Therefore, we check the last CPU that the
	 * IRQ fired upon in order to determine its actual affinity.
	 */
	*cpu = READ_ONCE(desc->last_cpu);
	if (*cpu >= nr_cpu_ids) return false;
	nr = *per_cpu_ptr(desc->kstat_irqs, *cpu);
	/*
	 * Calculate the number of new interrupts from this IRQ. It is assumed
	 * that the IRQ has been running on the same CPU since the last
	 * balancing run. This might not hold true if the IRQ was moved by
	 * someone else since the last balancing run, or if the CPU this IRQ was
	 * previously running on has since gone offline.
	 */
	if (nr <= bi->old_nr) {
		bi->old_nr = nr;
		return false;
	}
	bi->delta_nr = nr - bi->old_nr;
	bi->old_nr = nr;
	return true;
}

static int move_irq_to_cpu(struct bal_irq *bi, int cpu)
{
	struct irq_desc *desc = bi->desc;
	int prev_cpu, ret;

	/* Set the affinity if it wasn't changed since we looked at it */
	raw_spin_lock_irq(&desc->lock);
	prev_cpu = cpumask_first(desc->irq_common_data.affinity);
	if (prev_cpu == bi->prev_cpu) {
		ret = irq_set_affinity_locked(&desc->irq_data, cpumask_of(cpu),
									  false);
	} else {
		bi->prev_cpu = prev_cpu;
		ret = -EINVAL;
	}
	raw_spin_unlock_irq(&desc->lock);
	if (!ret) {
		/* Update the old interrupt count using the new CPU */
		bi->old_nr = *per_cpu_ptr(desc->kstat_irqs, cpu);
		pr_debug("Moved IRQ%d (CPU%d -> CPU%d)\n",
				 irq_desc_get_irq(desc), prev_cpu, cpu);
	}
	return ret;
}

static unsigned int scale_intrs(unsigned int intrs, int cpu)
{
	/* Scale the number of interrupts to this CPU's current capacity */
	return intrs * SCHED_CAPACITY_SCALE / per_cpu(cpu_cap, cpu);
}

/* Returns true if IRQ balancing should stop */
static bool find_min_bd(const cpumask_t *mask, unsigned int max_intrs, struct bal_domain **min_bd)
{
	unsigned int intrs, min_intrs = UINT_MAX;
	struct bal_domain *bd;
	int cpu;
	for_each_cpu(cpu, mask) {
		bd = per_cpu_ptr(&balance_data, cpu);
		intrs = scale_intrs(bd->intrs, bd->cpu);

		/* Terminate when the formerly-max CPU isn't the max anymore */
		if (intrs > max_intrs) return true;

		/* Don't consider moving IRQs to this CPU if it's excluded */
		if (cpumask_test_cpu(cpu, &cpu_exclude_mask)) continue;

		/* Find the CPU with the lowest relative number of interrupts */
		if (intrs < min_intrs) {
			min_intrs = intrs;
			*min_bd = bd;
		}
	}

	/* No CPUs available to move IRQs onto */
	if (min_intrs == UINT_MAX) return true;

	/* Don't balance if IRQs are already balanced evenly enough */
	return max_intrs - min_intrs < IRQ_SCALED_THRESH;
}

extern void balance_irqs(void)
{
	static cpumask_t cpus;
	struct bal_domain *bd, *max_bd = NULL, *min_bd = NULL;
	unsigned int intrs, max_intrs;
	bool moved_irq = false;
	struct bal_irq *bi;
	cpumask_t balance_mask;
	int cpu;

	cpus_read_lock();
	rcu_read_lock();

	/* Find the available CPUs for balancing, if there are any */
	cpumask_copy(&cpus, cpu_active_mask);
	if (unlikely(cpumask_weight(&cpus) <= 1))
		goto unlock;

	for_each_cpu(cpu, &cpus) {
		/*
		 * Get the current capacity for each CPU. This is adjusted for
		 * time spent processing IRQs, RT-task time, and thermal
		 * pressure. We don't exclude time spent processing IRQs when
		 * balancing because balancing is only done using interrupt
		 * counts rather than time spent in interrupts. That way, time
		 * spent processing each interrupt is considered when balancing.
		 */
		per_cpu(cpu_cap, cpu) = cpu_rq(cpu)->cpu_capacity;

		/* Get the number of new interrupts on this CPU */
		bd = per_cpu_ptr(&balance_data, cpu);
		bd->intrs = kstat_cpu_irqs_sum(cpu) - bd->old_total;
		bd->old_total += bd->intrs;
	}

	list_for_each_entry_rcu(bi, &bal_irq_list, node) {
		bi->type = get_irq_type(bi->desc);

		if (bi->type == IRQ_TYPE_CRITICAL) {
			const struct cpumask *crit_mask = cpumask_of(7);
			if (!cpumask_equal(bi->desc->irq_common_data.affinity, crit_mask))
				irq_force_affinity(irq_desc_get_irq(bi->desc), crit_mask);
			continue; // Those IRQ's won't be balanced
		}

		if (bi->type == IRQ_TYPE_PERFORMANCE) {
			static struct cpumask perf_mask;
			cpumask_clear(&perf_mask);
			cpumask_set_cpu(4, &perf_mask);
			cpumask_set_cpu(5, &perf_mask);
			cpumask_set_cpu(6, &perf_mask);
			if (!cpumask_equal(bi->desc->irq_common_data.affinity, &perf_mask))
				irq_force_affinity(irq_desc_get_irq(bi->desc), &perf_mask);
			continue; // Those IRQ's won't be balanced
		}

		/* Consider this IRQ for balancing if it's movable */
		if (!__irq_can_set_affinity(bi->desc)) continue;

		if (!update_irq_data(bi, &cpu)) continue;

		/* Ignore for this run if the IRQ isn't on the expected CPU */
		if (cpu != bi->prev_cpu) {
			bi->prev_cpu = cpu;
			continue;
		}

		/* Add this IRQ to its CPU's list of movable IRQs */
		bd = per_cpu_ptr(&balance_data, cpu);
		list_add_tail(&bi->move_node, &bd->movable_irqs);
	}

	/* Find the most interrupt-heavy CPU with movable IRQs */
	while (1) {
		max_intrs = 0;
		for_each_cpu(cpu, &cpus) {
			if (cpu == 7) continue;
			bd = per_cpu_ptr(&balance_data, cpu);
			intrs = scale_intrs(bd->intrs, bd->cpu);
			if (intrs > max_intrs) {
				max_intrs = intrs;
				max_bd = bd;
			}
		}

		/* No balancing to do if there aren't any movable IRQs */
		if (unlikely(!max_intrs || !max_bd)) goto unlock;

		/* Ensure the heaviest CPU has IRQs which can be moved away */
		if (!list_empty(&max_bd->movable_irqs)) break;

		try_next_heaviest:
		/*
		 * If the heaviest CPU has no movable IRQs then it can neither
		 * receive IRQs nor give IRQs. Exclude it from balancing so the
		 * remaining CPUs can be balanced, if there are any.
		 */
		if (cpumask_weight(&cpus) == 2) goto unlock;
		__cpumask_clear_cpu(max_bd->cpu, &cpus);
	}

	cpumask_copy(&balance_mask, &cpus);
	cpumask_clear_cpu(7, &balance_mask);

	/* Find the CPU with the lowest relative interrupt count */
	if (find_min_bd(&balance_mask, max_intrs, &min_bd)) goto unlock;

	/* Sort movable IRQs in descending order of number of new interrupts */
	list_sort(NULL, &max_bd->movable_irqs, bal_irq_move_node_cmp);

	/* Push IRQs away from the heaviest CPU to the least-heavy CPUs */
	list_for_each_entry(bi, &max_bd->movable_irqs, move_node) {
		intrs = scale_intrs(min_bd->intrs + bi->delta_nr, min_bd->cpu);
		if (intrs >= max_intrs) continue;

		/* Try to migrate this IRQ, or skip it if migration fails */
		if (move_irq_to_cpu(bi, min_bd->cpu)) continue;

		/* Keep track of whether or not any IRQs are moved */
		moved_irq = true;

		/*
		 * Update the counts and recalculate the max scaled count. The
		 * balance domain's delta interrupt count could be lower than
		 * the sum of new interrupts counted for each IRQ, since they're
		 * measured using different counters.
		 */
		min_bd->intrs += bi->delta_nr;
		max_bd->intrs -= min(bi->delta_nr, max_bd->intrs);
		max_intrs = scale_intrs(max_bd->intrs, max_bd->cpu);

		/* Recheck for the least-heavy CPU since it may have changed */
		if (find_min_bd(&balance_mask, max_intrs, &min_bd)) break;
	}

	/*
	 * If the heaviest CPU has movable IRQs which can't actually be moved,
	 * then ignore it and try balancing the next heaviest CPU.
	 */
	if (!moved_irq) goto try_next_heaviest;

	unlock:
	rcu_read_unlock();
	cpus_read_unlock();

	/* Reset each balance domain for the next run */
	for_each_possible_cpu(cpu) {
		bd = per_cpu_ptr(&balance_data, cpu);
		INIT_LIST_HEAD(&bd->movable_irqs);
		bd->intrs = 0;
	}
}

struct process_timer {
	struct timer_list timer;
	struct task_struct *task;
};

static void process_timeout(struct timer_list *t)
{
	struct process_timer *timeout = from_timer(timeout, t, timer);

	wake_up_process(timeout->task);
}

static void sbalance_wait(long poll_jiffies)
{
	struct process_timer timer;
	/*
	 * Open code freezable_schedule_timeout_interruptible() in order to
	 * make the timer deferrable, so that it doesn't kick CPUs out of idle.
	 */
	freezer_do_not_count();
	__set_current_state(TASK_IDLE);
	timer.task = current;
	timer_setup_on_stack(&timer.timer, process_timeout, TIMER_DEFERRABLE);
	timer.timer.expires = jiffies + poll_jiffies;
	add_timer(&timer.timer);
	schedule();
	del_singleshot_timer_sync(&timer.timer);
	destroy_timer_on_stack(&timer.timer);
	freezer_count();
}

static int __noreturn sbalance_thread(void *data)
{
	long poll_jiffies = msecs_to_jiffies(POLL_MS);
	struct bal_domain *bd;
	int cpu;

	/* Parse the list of CPUs to exclude, if any */
	if (cpulist_parse(CONFIG_SBALANCE_EXCLUDE_CPUS, &cpu_exclude_mask))
		cpu_exclude_mask = CPU_MASK_NONE;

	/* Initialize the data used for balancing */
	for_each_possible_cpu(cpu) {
		bd = per_cpu_ptr(&balance_data, cpu);
		INIT_LIST_HEAD(&bd->movable_irqs);
		bd->cpu = cpu;
	}

	set_freezable();
	while (1) {
		sbalance_wait(poll_jiffies);
		balance_irqs();
	}
}

static int __init sbalance_init(void)
{
	BUG_ON(IS_ERR(kthread_run(sbalance_thread, NULL, "sbalanced")));
	return 0;
}
late_initcall(sbalance_init);
