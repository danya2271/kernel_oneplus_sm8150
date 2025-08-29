/*
 * Obtain energy cost data from DT and populate relevant scheduler data
 * structures.
 *
 * Copyright (C) 2015 ARM Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#define pr_fmt(fmt) "sched-energy: " fmt

#include <linux/gfp.h>
#include <linux/of.h>
#include <linux/printk.h>
#include <linux/sched.h>
#include <linux/sched/topology.h>
#include <linux/sched/energy.h>
#include <linux/stddef.h>
#include <linux/arch_topology.h>
#include <linux/cpu.h>
#include <linux/pm_opp.h>
#include <linux/platform_device.h>

#include "sched.h"

struct sched_group_energy *sge_array[NR_CPUS][NR_SD_LEVELS];

void check_max_cap_vs_cpu_scale(int cpu, struct sched_group_energy *sge)
{
	unsigned long max_cap, cpu_scale;

	max_cap = sge->cap_states[sge->nr_cap_states - 1].cap;
	cpu_scale = topology_get_cpu_scale(cpu);

	if (max_cap == cpu_scale)
		return;

	pr_debug("CPU%d max energy model capacity=%ld != cpu_scale=%ld\n", cpu,
		max_cap, cpu_scale);
}

void init_sched_energy_costs(void)
{
}

static int sched_energy_probe(struct platform_device *pdev)
{
	return 0;
}

static const struct of_device_id of_sched_energy_dt[] = {
	{
		.compatible = "sched-energy",
	},
	{ }
};

static struct platform_driver energy_driver = {
	.driver = {
		.name = "sched-energy",
		.of_match_table = of_sched_energy_dt,
	},
	.probe = sched_energy_probe,
};

static int __init sched_energy_init(void)
{
	return platform_driver_register(&energy_driver);
}
subsys_initcall(sched_energy_init);
