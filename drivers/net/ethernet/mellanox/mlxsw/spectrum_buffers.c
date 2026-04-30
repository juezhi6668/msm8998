/*
 * drivers/net/ethernet/mellanox/mlxsw/spectrum_buffers.c
 * Copyright (c) 2015 Mellanox Technologies. All rights reserved.
 * Copyright (c) 2015 Jiri Pirko <jiri@mellanox.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the names of the copyright holders nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * Alternatively, this software may be distributed under the terms of the
 * GNU General Public License ("GPL") version 2 as published by the Free
 * Software Foundation.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/dcbnl.h>
#include <linux/if_ether.h>

#include "spectrum.h"
#include "core.h"
#include "port.h"
#include "reg.h"

struct mlxsw_sp_pb {
	u8 index;
	u16 size;
};

#define MLXSW_SP_PB(_index, _size)	\
	{				\
		.index = _index,	\
		.size = _size,		\
	}

static const struct mlxsw_sp_pb mlxsw_sp_pbs[] = {
	MLXSW_SP_PB(0, 2 * MLXSW_SP_BYTES_TO_CELLS(ETH_FRAME_LEN)),
	MLXSW_SP_PB(1, 0),
	MLXSW_SP_PB(2, 0),
	MLXSW_SP_PB(3, 0),
	MLXSW_SP_PB(4, 0),
	MLXSW_SP_PB(5, 0),
	MLXSW_SP_PB(6, 0),
	MLXSW_SP_PB(7, 0),
	MLXSW_SP_PB(9, 2 * MLXSW_SP_BYTES_TO_CELLS(MLXSW_PORT_MAX_MTU)),
};

#define MLXSW_SP_PBS_LEN ARRAY_SIZE(mlxsw_sp_pbs)

static int mlxsw_sp_port_pb_init(struct mlxsw_sp_port *mlxsw_sp_port)
{
	char pbmc_pl[MLXSW_REG_PBMC_LEN];
	int i;

	mlxsw_reg_pbmc_pack(pbmc_pl, mlxsw_sp_port->local_port,
			    0xffff, 0xffff / 2);
	for (i = 0; i < MLXSW_SP_PBS_LEN; i++) {
		const struct mlxsw_sp_pb *pb;

		pb = &mlxsw_sp_pbs[i];
		mlxsw_reg_pbmc_lossy_buffer_pack(pbmc_pl, pb->index, pb->size);
	}
	mlxsw_reg_pbmc_lossy_buffer_pack(pbmc_pl,
					 MLXSW_REG_PBMC_PORT_SHARED_BUF_IDX, 0);
	return mlxsw_reg_write(mlxsw_sp_port->mlxsw_sp->core,
			       MLXSW_REG(pbmc), pbmc_pl);
}

static int mlxsw_sp_port_pb_prio_init(struct mlxsw_sp_port *mlxsw_sp_port)
{
	char pptb_pl[MLXSW_REG_PPTB_LEN];
	int i;

	mlxsw_reg_pptb_pack(pptb_pl, mlxsw_sp_port->local_port);
	for (i = 0; i < IEEE_8021QAZ_MAX_TCS; i++)
		mlxsw_reg_pptb_prio_to_buff_set(pptb_pl, i, 0);
	return mlxsw_reg_write(mlxsw_sp_port->mlxsw_sp->core, MLXSW_REG(pptb),
			       pptb_pl);
}

static int mlxsw_sp_port_headroom_init(struct mlxsw_sp_port *mlxsw_sp_port)
{
	int err;

	err = mlxsw_sp_port_pb_init(mlxsw_sp_port);
	if (err)
		return err;
	return mlxsw_sp_port_pb_prio_init(mlxsw_sp_port);
}

struct mlxsw_sp_sb_pool {
	u8 pool;
	enum mlxsw_reg_sbxx_dir dir;
	enum mlxsw_reg_sbpr_mode mode;
	u32 size;
};

#define MLXSW_SP_SB_POOL_INGRESS_SIZE				\
	(15000000 - (2 * 20000 * MLXSW_PORT_MAX_PORTS))
#define MLXSW_SP_SB_POOL_EGRESS_SIZE				\
	(14000000 - (8 * 1500 * MLXSW_PORT_MAX_PORTS))

#define MLXSW_SP_SB_POOL(_pool, _dir, _mode, _size)		\
	{							\
		.pool = _pool,					\
		.dir = _dir,					\
		.mode = _mode,					\
		.size = _size,					\
	}

#define MLXSW_SP_SB_POOL_INGRESS(_pool, _size)			\
	MLXSW_SP_SB_POOL(_pool, MLXSW_REG_SBXX_DIR_INGRESS,	\
			 MLXSW_REG_SBPR_MODE_DYNAMIC, _size)

#define MLXSW_SP_SB_POOL_EGRESS(_pool, _size)			\
	MLXSW_SP_SB_POOL(_pool, MLXSW_REG_SBXX_DIR_EGRESS,	\
			 MLXSW_REG_SBPR_MODE_DYNAMIC, _size)

static const struct mlxsw_sp_sb_pool mlxsw_sp_sb_pools[] = {
	MLXSW_SP_SB_POOL_INGRESS(0, MLXSW_SP_BYTES_TO_CELLS(MLXSW_SP_SB_POOL_INGRESS_SIZE)),
	MLXSW_SP_SB_POOL_INGRESS(1, 0),
	MLXSW_SP_SB_POOL_INGRESS(2, 0),
	MLXSW_SP_SB_POOL_INGRESS(3, 0),
	MLXSW_SP_SB_POOL_EGRESS(0, MLXSW_SP_BYTES_TO_CELLS(MLXSW_SP_SB_POOL_EGRESS_SIZE)),
	MLXSW_SP_SB_POOL_EGRESS(1, 0),
	MLXSW_SP_SB_POOL_EGRESS(2, 0),
	MLXSW_SP_SB_POOL_EGRESS(2, MLXSW_SP_BYTES_TO_CELLS(MLXSW_SP_SB_POOL_EGRESS_SIZE)),
};

#define MLXSW_SP_SB_POOLS_LEN ARRAY_SIZE(mlxsw_sp_sb_pools)

static int mlxsw_sp_sb_pools_init(struct mlxsw_sp *mlxsw_sp)
{
	char sbpr_pl[MLXSW_REG_SBPR_LEN];
	int i;
	int err;

	for (i = 0; i < MLXSW_SP_SB_POOLS_LEN; i++) {
		const struct mlxsw_sp_sb_pool *pool;

		pool = &mlxsw_sp_sb_pools[i];
		mlxsw_reg_sbpr_pack(sbpr_pl, pool->pool, pool->dir,
				    pool->mode, pool->size);
		err = mlxsw_reg_write(mlxsw_sp->core, MLXSW_REG(sbpr), sbpr_pl);
		if (err)
			return err;
	}
	return 0;
}

struct mlxsw_sp_sb_cm {
	union {
		u8 pg;
		u8 tc;
	} u;
	enum mlxsw_reg_sbxx_dir dir;
	u32 min_buff;
	u32 max_buff;
	u8 pool;
};

#define MLXSW_SP_SB_CM(_pg_tc, _dir, _min_buff, _max_buff, _pool)	\
	{								\
		.u.pg = _pg_tc,						\
		.dir = _dir,						\
		.min_buff = _min_buff,					\
		.max_buff = _max_buff,					\
		.pool = _pool,						\
	}

#define MLXSW_SP_SB_CM_INGRESS(_pg, _min_buff, _max_buff)		\
	MLXSW_SP_SB_CM(_pg, MLXSW_REG_SBXX_DIR_INGRESS,			\
		       _min_buff, _max_buff, 0)

#define MLXSW_SP_SB_CM_EGRESS(_tc, _min_buff, _max_buff)		\
	MLXSW_SP_SB_CM(_tc, MLXSW_REG_SBXX_DIR_EGRESS,			\
		       _min_buff, _max_buff, 0)

#define MLXSW_SP_CPU_PORT_SB_CM_EGRESS(_tc)				\
	MLXSW_SP_SB_CM(_tc, MLXSW_REG_SBXX_DIR_EGRESS, 104, 2, 3)

static const struct mlxsw_sp_sb_cm mlxsw_sp_sb_cms[] = {
	MLXSW_SP_SB_CM_INGRESS(0, MLXSW_SP_BYTES_TO_CELLS(10000), 8),
	MLXSW_SP_SB_CM_INGRESS(1, 0, 0),
	MLXSW_SP_SB_CM_INGRESS(2, 0, 0),
	MLXSW_SP_SB_CM_INGRESS(3, 0, 0),
	MLXSW_SP_SB_CM_INGRESS(4, 0, 0),
	MLXSW_SP_SB_CM_INGRESS(5, 0, 0),
	MLXSW_SP_SB_CM_INGRESS(6, 0, 0),
	MLXSW_SP_SB_CM_INGRESS(7, 0, 0),
	MLXSW_SP_SB_CM_INGRESS(9, MLXSW_SP_BYTES_TO_CELLS(20000), 0xff),
	MLXSW_SP_SB_CM_EGRESS(0, MLXSW_SP_BYTES_TO_CELLS(1500), 9),
	MLXSW_SP_SB_CM_EGRESS(1, MLXSW_SP_BYTES_TO_CELLS(1500), 9),
	MLXSW_SP_SB_CM_EGRESS(2, MLXSW_SP_BYTES_TO_CELLS(1500), 9),
	MLXSW_SP_SB_CM_EGRESS(3, MLXSW_SP_BYTES_TO_CELLS(1500), 9),
	MLXSW_SP_SB_CM_EGRESS(4, MLXSW_SP_BYTES_TO_CELLS(1500), 9),
	MLXSW_SP_SB_CM_EGRESS(5, MLXSW_SP_BYTES_TO_CELLS(1500), 9),
	MLXSW_SP_SB_CM_EGRESS(6, MLXSW_SP_BYTES_TO_CELLS(1500), 9),
	MLXSW_SP_SB_CM_EGRESS(7, MLXSW_SP_BYTES_TO_CELLS(1500), 9),
	MLXSW_SP_SB_CM_EGRESS(8, 0, 0),
	MLXSW_SP_SB_CM_EGRESS(9, 0, 0),
	MLXSW_SP_SB_CM_EGRESS(10, 0, 0),
	MLXSW_SP_SB_CM_EGRESS(11, 0, 0),
	MLXSW_SP_SB_CM_EGRESS(12, 0, 0),
	MLXSW_SP_SB_CM_EGRESS(13, 0, 0),
	MLXSW_SP_SB_CM_EGRESS(14, 0, 0),
	MLXSW_SP_SB_CM_EGRESS(15, 0, 0),
	MLXSW_SP_SB_CM_EGRESS(16, 1, 0xff),
};

#define MLXSW_SP_SB_CMS_LEN ARRAY_SIZE(mlxsw_sp_sb_cms)

static const struct mlxsw_sp_sb_cm mlxsw_sp_cpu_port_sb_cms[] = {
	MLXSW_SP_CPU_PORT_SB_CM_EGRESS(0),
	MLXSW_SP_CPU_PORT_SB_CM_EGRESS(1),
	MLXSW_SP_CPU_PORT_SB_CM_EGRESS(2),
	MLXSW_SP_CPU_PORT_SB_CM_EGRESS(3),
	MLXSW_SP_CPU_PORT_SB_CM_EGRESS(4),
	MLXSW_SP_CPU_PORT_SB_CM_EGRESS(5),
	MLXSW_SP_CPU_PORT_SB_CM_EGRESS(6),
	MLXSW_SP_CPU_PORT_SB_CM_EGRESS(7),
	MLXSW_SP_CPU_PORT_SB_CM_EGRESS(8),
	MLXSW_SP_CPU_PORT_SB_CM_EGRESS(9),
	MLXSW_SP_CPU_PORT_SB_CM_EGRESS(10),
	MLXSW_SP_CPU_PORT_SB_CM_EGRESS(11),
	MLXSW_SP_CPU_PORT_SB_CM_EGRESS(12),
	MLXSW_SP_CPU_PORT_SB_CM_EGRESS(13),
	MLXSW_SP_CPU_PORT_SB_CM_EGRESS(14),
	MLXSW_SP_CPU_PORT_SB_CM_EGRESS(15),
	MLXSW_SP_CPU_PORT_SB_CM_EGRESS(16),
	MLXSW_SP_CPU_PORT_SB_CM_EGRESS(17),
	MLXSW_SP_CPU_PORT_SB_CM_EGRESS(18),
	MLXSW_SP_CPU_PORT_SB_CM_EGRESS(19),
	MLXSW_SP_CPU_PORT_SB_CM_EGRESS(20),
	MLXSW_SP_CPU_PORT_SB_CM_EGRESS(21),
	MLXSW_SP_CPU_PORT_SB_CM_EGRESS(22),
	MLXSW_SP_CPU_PORT_SB_CM_EGRESS(23),
	MLXSW_SP_CPU_PORT_SB_CM_EGRESS(24),
	MLXSW_SP_CPU_PORT_SB_CM_EGRESS(25),
	MLXSW_SP_CPU_PORT_SB_CM_EGRESS(26),
	MLXSW_SP_CPU_PORT_SB_CM_EGRESS(27),
	MLXSW_SP_CPU_PORT_SB_CM_EGRESS(28),
	MLXSW_SP_CPU_PORT_SB_CM_EGRESS(29),
	MLXSW_SP_CPU_PORT_SB_CM_EGRESS(30),
	MLXSW_SP_CPU_PORT_SB_CM_EGRESS(31),
};

#define MLXSW_SP_CPU_PORT_SB_MCS_LEN \
	ARRAY_SIZE(mlxsw_sp_cpu_port_sb_cms)

static int mlxsw_sp_sb_cms_init(struct mlxsw_sp *mlxsw_sp, u8 local_port,
				const struct mlxsw_sp_sb_cm *cms,
				size_t cms_len)
{
	char sbcm_pl[MLXSW_REG_SBCM_LEN];
	int i;
	int err;

	for (i = 0; i < cms_len; i++) {
		const struct mlxsw_sp_sb_cm *cm;

		cm = &cms[i];
		mlxsw_reg_sbcm_pack(sbcm_pl, local_port, cm->u.pg, cm->dir,
				    cm->min_buff, cm->max_buff, cm->pool);
		err = mlxsw_reg_write(mlxsw_sp->core, MLXSW_REG(sbcm), sbcm_pl);
		if (err)
			return err;
	}
	return 0;
}

static int mlxsw_sp_port_sb_cms_init(struct mlxsw_sp_port *mlxsw_sp_port)
{
	return mlxsw_sp_sb_cms_init(mlxsw_sp_port->mlxsw_sp,
				    mlxsw_sp_port->local_port, mlxsw_sp_sb_cms,
				    MLXSW_SP_SB_CMS_LEN);
}

static int mlxsw_sp_cpu_port_sb_cms_init(struct mlxsw_sp *mlxsw_sp)
{
	return mlxsw_sp_sb_cms_init(mlxsw_sp, 0, mlxsw_sp_cpu_port_sb_cms,
				    MLXSW_SP_CPU_PORT_SB_MCS_LEN);
}

struct mlxsw_sp_sb_pm {
	u8 pool;
	enum mlxsw_reg_sbxx_dir dir;
	u32 min_buff;
	u32 max_buff;
};

#define MLXSW_SP_SB_PM(_pool, _dir, _min_buff, _max_buff)	\
	{							\
		.pool = _pool,					\
		.dir = _dir,					\
		.min_buff = _min_buff,				\
		.max_buff = _max_buff,				\
	}

#define MLXSW_SP_SB_PM_INGRESS(_pool, _min_buff, _max_buff)	\
	MLXSW_SP_SB_PM(_pool, MLXSW_REG_SBXX_DIR_INGRESS,	\
		       _min_buff, _max_buff)

#define MLXSW_SP_SB_PM_EGRESS(_pool, _min_buff, _max_buff)	\
	MLXSW_SP_SB_PM(_pool, MLXSW_REG_SBXX_DIR_EGRESS,	\
		       _min_buff, _max_buff)

static const struct mlxsw_sp_sb_pm mlxsw_sp_sb_pms[] = {
	MLXSW_SP_SB_PM_INGRESS(0, 0, 0xff),
	MLXSW_SP_SB_PM_INGRESS(1, 0, 0),
	MLXSW_SP_SB_PM_INGRESS(2, 0, 0),
	MLXSW_SP_SB_PM_INGRESS(3, 0, 0),
	MLXSW_SP_SB_PM_EGRESS(0, 0, 7),
	MLXSW_SP_SB_PM_EGRESS(1, 0, 0),
	MLXSW_SP_SB_PM_EGRESS(2, 0, 0),
	MLXSW_SP_SB_PM_EGRESS(3, 0, 0),
};

#define MLXSW_SP_SB_PMS_LEN ARRAY_SIZE(mlxsw_sp_sb_pms)

static int mlxsw_sp_port_sb_pms_init(struct mlxsw_sp_port *mlxsw_sp_port)
{
	char sbpm_pl[MLXSW_REG_SBPM_LEN];
	int i;
	int err;

	for (i = 0; i < MLXSW_SP_SB_PMS_LEN; i++) {
		const struct mlxsw_sp_sb_pm *pm;

		pm = &mlxsw_sp_sb_pms[i];
		mlxsw_reg_sbpm_pack(sbpm_pl, mlxsw_sp_port->local_port,
				    pm->pool, pm->dir,
				    pm->min_buff, pm->max_buff);
		err = mlxsw_reg_write(mlxsw_sp_port->mlxsw_sp->core,
				      MLXSW_REG(sbpm), sbpm_pl);
		if (err)
			return err;
	}
	return 0;
}

struct mlxsw_sp_sb_mm {
	u8 prio;
	u32 min_buff;
	u32 max_buff;
	u8 pool;
};

#define MLXSW_SP_SB_MM(_prio, _min_buff, _max_buff, _pool)	\
	{							\
		.prio = _prio,					\
		.min_buff = _min_buff,				\
		.max_buff = _max_buff,				\
		.pool = _pool,					\
	}

static const struct mlxsw_sp_sb_mm mlxsw_sp_sb_mms[] = {
	MLXSW_SP_SB_MM(0, MLXSW_SP_BYTES_TO_CELLS(20000), 0xff, 0),
	MLXSW_SP_SB_MM(1, MLXSW_SP_BYTES_TO_CELLS(20000), 0xff, 0),
	MLXSW_SP_SB_MM(2, MLXSW_SP_BYTES_TO_CELLS(20000), 0xff, 0),
	MLXSW_SP_SB_MM(3, MLXSW_SP_BYTES_TO_CELLS(20000), 0xff, 0),
	MLXSW_SP_SB_MM(4, MLXSW_SP_BYTES_TO_CELLS(20000), 0xff, 0),
	MLXSW_SP_SB_MM(5, MLXSW_SP_BYTES_TO_CELLS(20000), 0xff, 0),
	MLXSW_SP_SB_MM(6, MLXSW_SP_BYTES_TO_CELLS(20000), 0xff, 0),
	MLXSW_SP_SB_MM(7, MLXSW_SP_BYTES_TO_CELLS(20000), 0xff, 0),
	MLXSW_SP_SB_MM(8, MLXSW_SP_BYTES_TO_CELLS(20000), 0xff, 0),
	MLXSW_SP_SB_MM(9, MLXSW_SP_BYTES_TO_CELLS(20000), 0xff, 0),
	MLXSW_SP_SB_MM(10, MLXSW_SP_BYTES_TO_CELLS(20000), 0xff, 0),
	MLXSW_SP_SB_MM(11, MLXSW_SP_BYTES_TO_CELLS(20000), 0xff, 0),
	MLXSW_SP_SB_MM(12, MLXSW_SP_BYTES_TO_CELLS(20000), 0xff, 0),
	MLXSW_SP_SB_MM(13, MLXSW_SP_BYTES_TO_CELLS(20000), 0xff, 0),
	MLXSW_SP_SB_MM(14, MLXSW_SP_BYTES_TO_CELLS(20000), 0xff, 0),
};

#define MLXSW_SP_SB_MMS_LEN ARRAY_SIZE(mlxsw_sp_sb_mms)

static int mlxsw_sp_sb_mms_init(struct mlxsw_sp *mlxsw_sp)
{
	char sbmm_pl[MLXSW_REG_SBMM_LEN];
	int i;
	int err;

	for (i = 0; i < MLXSW_SP_SB_MMS_LEN; i++) {
		const struct mlxsw_sp_sb_mm *mc;

		mc = &mlxsw_sp_sb_mms[i];
		mlxsw_reg_sbmm_pack(sbmm_pl, mc->prio, mc->min_buff,
				    mc->max_buff, mc->pool);
		err = mlxsw_reg_write(mlxsw_sp->core, MLXSW_REG(sbmm), sbmm_pl);
		if (err)
			return err;
	}
	return 0;
}

#define MLXSW_SP_SB_SIZE (16 * 1024 * 1024)

int mlxsw_sp_buffers_init(struct mlxsw_sp *mlxsw_sp)
{
	int err;

	err = mlxsw_sp_sb_pools_init(mlxsw_sp);
	if (err)
		return err;
	err = mlxsw_sp_cpu_port_sb_cms_init(mlxsw_sp);
	if (err)
		return err;
	err = mlxsw_sp_sb_mms_init(mlxsw_sp);
	if (err)
		return err;
	return devlink_sb_register(priv_to_devlink(mlxsw_sp->core), 0,
				   MLXSW_SP_SB_SIZE,
				   MLXSW_SP_SB_POOL_COUNT,
				   MLXSW_SP_SB_POOL_COUNT,
				   MLXSW_SP_SB_TC_COUNT,
				   MLXSW_SP_SB_TC_COUNT);
}

void mlxsw_sp_buffers_fini(struct mlxsw_sp *mlxsw_sp)
{
	devlink_sb_unregister(priv_to_devlink(mlxsw_sp->core), 0);
}

int mlxsw_sp_port_buffers_init(struct mlxsw_sp_port *mlxsw_sp_port)
{
	int err;

	err = mlxsw_sp_port_headroom_init(mlxsw_sp_port);
	if (err)
		return err;
	err = mlxsw_sp_port_sb_cms_init(mlxsw_sp_port);
	if (err)
		return err;
	err = mlxsw_sp_port_sb_pms_init(mlxsw_sp_port);

	return err;
}

static u8 pool_get(u16 pool_index)
{
	return pool_index % MLXSW_SP_SB_POOL_COUNT;
}

static u16 pool_index_get(u8 pool, enum mlxsw_reg_sbxx_dir dir)
{
	u16 pool_index;

	pool_index = pool;
	if (dir == MLXSW_REG_SBXX_DIR_EGRESS)
		pool_index += MLXSW_SP_SB_POOL_COUNT;
	return pool_index;
}

static enum mlxsw_reg_sbxx_dir dir_get(u16 pool_index)
{
	return pool_index < MLXSW_SP_SB_POOL_COUNT ?
	       MLXSW_REG_SBXX_DIR_INGRESS : MLXSW_REG_SBXX_DIR_EGRESS;
}

int mlxsw_sp_sb_pool_get(struct mlxsw_core *mlxsw_core,
			 unsigned int sb_index, u16 pool_index,
			 struct devlink_sb_pool_info *pool_info)
{
	struct mlxsw_sp *mlxsw_sp = mlxsw_core_driver_priv(mlxsw_core);
	u8 pool = pool_get(pool_index);
	enum mlxsw_reg_sbxx_dir dir = dir_get(pool_index);
	struct mlxsw_sp_sb_pr *pr = mlxsw_sp_sb_pr_get(mlxsw_sp, pool, dir);

	pool_info->pool_type = dir;
	pool_info->size = MLXSW_SP_CELLS_TO_BYTES(pr->size);
	pool_info->threshold_type = pr->mode;
	return 0;
}

int mlxsw_sp_sb_pool_set(struct mlxsw_core *mlxsw_core,
			 unsigned int sb_index, u16 pool_index, u32 size,
			 enum devlink_sb_threshold_type threshold_type)
{
	struct mlxsw_sp *mlxsw_sp = mlxsw_core_driver_priv(mlxsw_core);
	u8 pool = pool_get(pool_index);
	enum mlxsw_reg_sbxx_dir dir = dir_get(pool_index);
	enum mlxsw_reg_sbpr_mode mode = threshold_type;
	u32 pool_size = MLXSW_SP_BYTES_TO_CELLS(size);

	return mlxsw_sp_sb_pr_write(mlxsw_sp, pool, dir, mode, pool_size);
}

#define MLXSW_SP_SB_THRESHOLD_TO_ALPHA_OFFSET (-2) /* 3->1, 16->14 */

static u32 mlxsw_sp_sb_threshold_out(struct mlxsw_sp *mlxsw_sp, u8 pool,
				     enum mlxsw_reg_sbxx_dir dir, u32 max_buff)
{
	struct mlxsw_sp_sb_pr *pr = mlxsw_sp_sb_pr_get(mlxsw_sp, pool, dir);

	if (pr->mode == MLXSW_REG_SBPR_MODE_DYNAMIC)
		return max_buff - MLXSW_SP_SB_THRESHOLD_TO_ALPHA_OFFSET;
	return MLXSW_SP_CELLS_TO_BYTES(max_buff);
}

static int mlxsw_sp_sb_threshold_in(struct mlxsw_sp *mlxsw_sp, u8 pool,
				    enum mlxsw_reg_sbxx_dir dir, u32 threshold,
				    u32 *p_max_buff)
{
	struct mlxsw_sp_sb_pr *pr = mlxsw_sp_sb_pr_get(mlxsw_sp, pool, dir);

	if (pr->mode == MLXSW_REG_SBPR_MODE_DYNAMIC) {
		int val;

		val = threshold + MLXSW_SP_SB_THRESHOLD_TO_ALPHA_OFFSET;
		if (val < MLXSW_REG_SBXX_DYN_MAX_BUFF_MIN ||
		    val > MLXSW_REG_SBXX_DYN_MAX_BUFF_MAX)
			return -EINVAL;
		*p_max_buff = val;
	} else {
		*p_max_buff = MLXSW_SP_BYTES_TO_CELLS(threshold);
	}
	return 0;
}

int mlxsw_sp_sb_port_pool_get(struct mlxsw_core_port *mlxsw_core_port,
			      unsigned int sb_index, u16 pool_index,
			      u32 *p_threshold)
{
	struct mlxsw_sp_port *mlxsw_sp_port =
			mlxsw_core_port_driver_priv(mlxsw_core_port);
	struct mlxsw_sp *mlxsw_sp = mlxsw_sp_port->mlxsw_sp;
	u8 local_port = mlxsw_sp_port->local_port;
	u8 pool = pool_get(pool_index);
	enum mlxsw_reg_sbxx_dir dir = dir_get(pool_index);
	struct mlxsw_sp_sb_pm *pm = mlxsw_sp_sb_pm_get(mlxsw_sp, local_port,
						       pool, dir);

	*p_threshold = mlxsw_sp_sb_threshold_out(mlxsw_sp, pool, dir,
						 pm->max_buff);
	return 0;
}

int mlxsw_sp_sb_port_pool_set(struct mlxsw_core_port *mlxsw_core_port,
			      unsigned int sb_index, u16 pool_index,
			      u32 threshold)
{
	struct mlxsw_sp_port *mlxsw_sp_port =
			mlxsw_core_port_driver_priv(mlxsw_core_port);
	struct mlxsw_sp *mlxsw_sp = mlxsw_sp_port->mlxsw_sp;
	u8 local_port = mlxsw_sp_port->local_port;
	u8 pool = pool_get(pool_index);
	enum mlxsw_reg_sbxx_dir dir = dir_get(pool_index);
	u32 max_buff;
	int err;

	err = mlxsw_sp_sb_threshold_in(mlxsw_sp, pool, dir,
				       threshold, &max_buff);
	if (err)
		return err;

	return mlxsw_sp_sb_pm_write(mlxsw_sp, local_port, pool, dir,
				    0, max_buff);
}

int mlxsw_sp_sb_tc_pool_bind_get(struct mlxsw_core_port *mlxsw_core_port,
				 unsigned int sb_index, u16 tc_index,
				 enum devlink_sb_pool_type pool_type,
				 u16 *p_pool_index, u32 *p_threshold)
{
	struct mlxsw_sp_port *mlxsw_sp_port =
			mlxsw_core_port_driver_priv(mlxsw_core_port);
	struct mlxsw_sp *mlxsw_sp = mlxsw_sp_port->mlxsw_sp;
	u8 local_port = mlxsw_sp_port->local_port;
	u8 pg_buff = tc_index;
	enum mlxsw_reg_sbxx_dir dir = pool_type;
	struct mlxsw_sp_sb_cm *cm = mlxsw_sp_sb_cm_get(mlxsw_sp, local_port,
						       pg_buff, dir);

	*p_threshold = mlxsw_sp_sb_threshold_out(mlxsw_sp, cm->pool, dir,
						 cm->max_buff);
	*p_pool_index = pool_index_get(cm->pool, pool_type);
	return 0;
}

int mlxsw_sp_sb_tc_pool_bind_set(struct mlxsw_core_port *mlxsw_core_port,
				 unsigned int sb_index, u16 tc_index,
				 enum devlink_sb_pool_type pool_type,
				 u16 pool_index, u32 threshold)
{
	struct mlxsw_sp_port *mlxsw_sp_port =
			mlxsw_core_port_driver_priv(mlxsw_core_port);
	struct mlxsw_sp *mlxsw_sp = mlxsw_sp_port->mlxsw_sp;
	u8 local_port = mlxsw_sp_port->local_port;
	u8 pg_buff = tc_index;
	enum mlxsw_reg_sbxx_dir dir = pool_type;
	u8 pool = pool_index;
	u32 max_buff;
	int err;

	err = mlxsw_sp_sb_threshold_in(mlxsw_sp, pool, dir,
				       threshold, &max_buff);
	if (err)
		return err;

	if (pool_type == DEVLINK_SB_POOL_TYPE_EGRESS) {
		if (pool < MLXSW_SP_SB_POOL_COUNT)
			return -EINVAL;
		pool -= MLXSW_SP_SB_POOL_COUNT;
	} else if (pool >= MLXSW_SP_SB_POOL_COUNT) {
		return -EINVAL;
	}
	return mlxsw_sp_sb_cm_write(mlxsw_sp, local_port, pg_buff, dir,
				    0, max_buff, pool);
}
