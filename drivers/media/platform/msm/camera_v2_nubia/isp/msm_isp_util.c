/* Copyright (c) 2013-2016, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include <linux/mutex.h>
#include <linux/io.h>
#include <media/v4l2-subdev.h>
#include <linux/ratelimit.h>

#include "msm.h"
#include "msm_isp_util.h"
#include "msm_isp_axi_util.h"
#include "msm_isp_stats_util.h"
#include "msm_camera_io_util.h"
#include "cam_smmu_api.h"

#define MAX_ISP_V4l2_EVENTS 100
static DEFINE_MUTEX(bandwidth_mgr_mutex);
static struct msm_isp_bandwidth_mgr isp_bandwidth_mgr;

static uint64_t msm_isp_cpp_clk_rate;

#define VFE40_8974V2_VERSION 0x1001001A
static struct msm_bus_vectors msm_isp_init_vectors[] = {
	{
		.src = MSM_BUS_MASTER_VFE,
		.dst = MSM_BUS_SLAVE_EBI_CH0,
		.ab  = 0,
		.ib  = 0,
	},
};

/* During open node request min ab/ib bus bandwidth which
*  is needed to successfully enable bus clocks
*/
static struct msm_bus_vectors msm_isp_ping_vectors[] = {
	{
		.src = MSM_BUS_MASTER_VFE,
		.dst = MSM_BUS_SLAVE_EBI_CH0,
		.ab  = MSM_ISP_MIN_AB,
		.ib  = MSM_ISP_MIN_IB,
	},
};

static struct msm_bus_vectors msm_isp_pong_vectors[] = {
	{
		.src = MSM_BUS_MASTER_VFE,
		.dst = MSM_BUS_SLAVE_EBI_CH0,
		.ab  = 0,
		.ib  = 0,
	},
};

static struct msm_bus_paths msm_isp_bus_client_config[] = {
	{
		ARRAY_SIZE(msm_isp_init_vectors),
		msm_isp_init_vectors,
	},
	{
		ARRAY_SIZE(msm_isp_ping_vectors),
		msm_isp_ping_vectors,
	},
	{
		ARRAY_SIZE(msm_isp_pong_vectors),
		msm_isp_pong_vectors,
	},
};

static struct msm_bus_scale_pdata msm_isp_bus_client_pdata = {
	msm_isp_bus_client_config,
	ARRAY_SIZE(msm_isp_bus_client_config),
	.name = "msm_camera_isp",
};

void msm_isp_print_fourcc_error(const char *origin, uint32_t fourcc_format)
{
	int i;
	char text[5];
	text[4] = '\0';
	for (i = 0; i < 4; i++) {
		text[i] = (char)(((fourcc_format) >> (i * 8)) & 0xFF);
		if ((text[i] < '0') || (text[i] > 'z')) {
			pr_err("%s: Invalid output format %d (unprintable)\n",
				origin, fourcc_format);
			return;
		}
	}
	pr_err("%s: Invalid output format %s\n",
		origin, text);
	return;
}

int msm_isp_init_bandwidth_mgr(enum msm_isp_hw_client client)
{
	int rc = 0;
	mutex_lock(&bandwidth_mgr_mutex);
	isp_bandwidth_mgr.client_info[client].active = 1;
	if (isp_bandwidth_mgr.use_count++) {
		mutex_unlock(&bandwidth_mgr_mutex);
		return rc;
	}
	isp_bandwidth_mgr.bus_client =
		msm_bus_scale_register_client(&msm_isp_bus_client_pdata);
	if (!isp_bandwidth_mgr.bus_client) {
		pr_err("%s: client register failed\n", __func__);
		mutex_unlock(&bandwidth_mgr_mutex);
		return -EINVAL;
	}

	isp_bandwidth_mgr.bus_vector_active_idx = 1;
	msm_bus_scale_client_update_request(
	   isp_bandwidth_mgr.bus_client,
	   isp_bandwidth_mgr.bus_vector_active_idx);

	mutex_unlock(&bandwidth_mgr_mutex);
	return 0;
}

int msm_isp_update_bandwidth(enum msm_isp_hw_client client,
	uint64_t ab, uint64_t ib)
{
	int i;
	struct msm_bus_paths *path;
	mutex_lock(&bandwidth_mgr_mutex);
	if (!isp_bandwidth_mgr.use_count ||
		!isp_bandwidth_mgr.bus_client) {
		pr_err("%s:error bandwidth manager inactive use_cnt:%d bus_clnt:%d\n",
			__func__, isp_bandwidth_mgr.use_count,
			isp_bandwidth_mgr.bus_client);
		mutex_unlock(&bandwidth_mgr_mutex);
		return -EINVAL;
	}

	isp_bandwidth_mgr.client_info[client].ab = ab;
	isp_bandwidth_mgr.client_info[client].ib = ib;
	ALT_VECTOR_IDX(isp_bandwidth_mgr.bus_vector_active_idx);
	path =
		&(msm_isp_bus_client_pdata.usecase[
		  isp_bandwidth_mgr.bus_vector_active_idx]);
	path->vectors[0].ab = 0;
	path->vectors[0].ib = 0;
	for (i = 0; i < MAX_ISP_CLIENT; i++) {
		if (isp_bandwidth_mgr.client_info[i].active) {
			path->vectors[0].ab +=
				isp_bandwidth_mgr.client_info[i].ab;
			path->vectors[0].ib +=
				isp_bandwidth_mgr.client_info[i].ib;
		}
	}
	msm_bus_scale_client_update_request(isp_bandwidth_mgr.bus_client,
		isp_bandwidth_mgr.bus_vector_active_idx);
	/* Insert into circular buffer */
	msm_isp_update_req_history(isp_bandwidth_mgr.bus_client,
		path->vectors[0].ab,
		path->vectors[0].ib,
		isp_bandwidth_mgr.client_info,
		sched_clock());
	mutex_unlock(&bandwidth_mgr_mutex);
	return 0;
}

void msm_isp_deinit_bandwidth_mgr(enum msm_isp_hw_client client)
{
	if (client >= MAX_ISP_CLIENT) {
		pr_err("invalid Client id %d", client);
		return;
	}
	mutex_lock(&bandwidth_mgr_mutex);
	memset(&isp_bandwidth_mgr.client_info[client], 0,
		   sizeof(struct msm_isp_bandwidth_info));
	if (--isp_bandwidth_mgr.use_count) {
		mutex_unlock(&bandwidth_mgr_mutex);
		return;
	}

	if (!isp_bandwidth_mgr.bus_client) {
		pr_err("%s:%d error: bus client invalid\n", __func__, __LINE__);
		mutex_unlock(&bandwidth_mgr_mutex);
		return;
	}

	msm_bus_scale_client_update_request(
	   isp_bandwidth_mgr.bus_client, 0);
	msm_bus_scale_unregister_client(isp_bandwidth_mgr.bus_client);
	isp_bandwidth_mgr.bus_client = 0;
	mutex_unlock(&bandwidth_mgr_mutex);
}

void msm_isp_util_get_bandwidth_stats(struct vfe_device *vfe_dev,
				      struct msm_isp_statistics *stats)
{
	stats->isp_vfe0_active = isp_bandwidth_mgr.client_info[ISP_VFE0].active;
	stats->isp_vfe0_ab = isp_bandwidth_mgr.client_info[ISP_VFE0].ab;
	stats->isp_vfe0_ib = isp_bandwidth_mgr.client_info[ISP_VFE0].ib;

	stats->isp_vfe1_active = isp_bandwidth_mgr.client_info[ISP_VFE1].active;
	stats->isp_vfe1_ab = isp_bandwidth_mgr.client_info[ISP_VFE1].ab;
	stats->isp_vfe1_ib = isp_bandwidth_mgr.client_info[ISP_VFE1].ib;

	stats->isp_cpp_active = isp_bandwidth_mgr.client_info[ISP_CPP].active;
	stats->isp_cpp_ab = isp_bandwidth_mgr.client_info[ISP_CPP].ab;
	stats->isp_cpp_ib = isp_bandwidth_mgr.client_info[ISP_CPP].ib;
	stats->last_overflow_ab = vfe_dev->msm_isp_last_overflow_ab;
	stats->last_overflow_ib = vfe_dev->msm_isp_last_overflow_ib;
	stats->vfe_clk_rate = vfe_dev->msm_isp_vfe_clk_rate;
	stats->cpp_clk_rate = msm_isp_cpp_clk_rate;
}

void msm_isp_util_update_last_overflow_ab_ib(struct vfe_device *vfe_dev)
{
	struct msm_bus_paths *path;
	 path = &(msm_isp_bus_client_pdata.usecase[
		  isp_bandwidth_mgr.bus_vector_active_idx]);
	vfe_dev->msm_isp_last_overflow_ab = path->vectors[0].ab;
	vfe_dev->msm_isp_last_overflow_ib = path->vectors[0].ib;
}

void msm_isp_util_update_clk_rate(long clock_rate)
{
	msm_isp_cpp_clk_rate = clock_rate;
}

uint32_t msm_isp_get_framedrop_period(
	enum msm_vfe_frame_skip_pattern frame_skip_pattern)
{
	switch (frame_skip_pattern) {
	case NO_SKIP:
	case EVERY_2FRAME:
	case EVERY_3FRAME:
	case EVERY_4FRAME:
	case EVERY_5FRAME:
	case EVERY_6FRAME:
	case EVERY_7FRAME:
	case EVERY_8FRAME:
		return frame_skip_pattern + 1;
	case EVERY_16FRAME:
		return 16;
		break;
	case EVERY_32FRAME:
		return 32;
		break;
	case SKIP_ALL:
		return 1;
	default:
		return 1;
	}
	return 1;
}

int msm_isp_get_clk_info(struct vfe_device *vfe_dev,
	struct platform_device *pdev, struct msm_cam_clk_info *vfe_clk_info)
{
	uint32_t count;
	int i, rc;
	uint32_t rates[VFE_CLK_INFO_MAX];

	struct device_node *of_node;
	of_node = pdev->dev.of_node;

	count = of_property_count_strings(of_node, "clock-names");

	ISP_DBG("count = %d\n", count);
	if (count == 0) {
		pr_err("no clocks found in device tree, count=%d", count);
		return 0;
	}

	if (count > VFE_CLK_INFO_MAX) {
		pr_err("invalid count=%d, max is %d\n", count,
			VFE_CLK_INFO_MAX);
		return -EINVAL;
	}

	for (i = 0; i < count; i++) {
		rc = of_property_read_string_index(of_node, "clock-names",
				i, &(vfe_clk_info[i].clk_name));
		ISP_DBG("clock-names[%d] = %s\n", i, vfe_clk_info[i].clk_name);
		if (rc < 0) {
			pr_err("%s failed %d\n", __func__, __LINE__);
			return rc;
		}
		if (0 == strcmp(vfe_clk_info[i].clk_name, "vfe_clk_src")) {
			ISP_DBG("%s: find vfe src clk index %d\n", __func__, i);
			vfe_dev->hw_info->vfe_clk_idx = i;
		}
	}
	rc = of_property_read_u32_array(of_node, "qcom,clock-rates",
		rates, count);
	if (rc < 0) {
		pr_err("%s failed %d\n", __func__, __LINE__);
		return rc;
	}
	for (i = 0; i < count; i++) {
		vfe_clk_info[i].clk_rate =
			(rates[i] == 0) ? (long)-1 : rates[i];
		ISP_DBG("clk_rate[%d] = %ld\n", i, vfe_clk_info[i].clk_rate);
	}
	vfe_dev->num_clk = count;
	return 0;
}

void msm_isp_get_timestamp(struct msm_isp_timestamp *time_stamp)
{
	struct timespec ts;
#ifdef CONFIG_BOARD_NUBIA
	ktime_get_ts(&ts);
#else
	get_monotonic_boottime(&ts);
#endif
	time_stamp->buf_time.tv_sec    = ts.tv_sec;
	time_stamp->buf_time.tv_usec   = ts.tv_nsec/1000;
	do_gettimeofday(&(time_stamp->event_time));
}

static inline u32 msm_isp_evt_mask_to_isp_event(u32 evt_mask)
{
	u32 evt_id = ISP_EVENT_SUBS_MASK_NONE;

	switch (evt_mask) {
	case ISP_EVENT_MASK_INDEX_STATS_NOTIFY:
		evt_id = ISP_EVENT_STATS_NOTIFY;
		break;
	case ISP_EVENT_MASK_INDEX_ERROR:
		evt_id = ISP_EVENT_ERROR;
		break;
	case ISP_EVENT_MASK_INDEX_IOMMU_P_FAULT:
		evt_id = ISP_EVENT_IOMMU_P_FAULT;
		break;
	case ISP_EVENT_MASK_INDEX_STREAM_UPDATE_DONE:
		evt_id = ISP_EVENT_STREAM_UPDATE_DONE;
		break;
	case ISP_EVENT_MASK_INDEX_REG_UPDATE:
		evt_id = ISP_EVENT_REG_UPDATE;
		break;
	case ISP_EVENT_MASK_INDEX_SOF:
		evt_id = ISP_EVENT_SOF;
		break;
	case ISP_EVENT_MASK_INDEX_BUF_DIVERT:
		evt_id = ISP_EVENT_BUF_DIVERT;
		break;
	case ISP_EVENT_MASK_INDEX_BUF_DONE:
		evt_id = ISP_EVENT_BUF_DONE;
		break;
	case ISP_EVENT_MASK_INDEX_COMP_STATS_NOTIFY:
		evt_id = ISP_EVENT_COMP_STATS_NOTIFY;
		break;
	case ISP_EVENT_MASK_INDEX_MASK_FE_READ_DONE:
		evt_id = ISP_EVENT_FE_READ_DONE;
		break;
	case ISP_EVENT_MASK_INDEX_PING_PONG_MISMATCH:
		evt_id = ISP_EVENT_PING_PONG_MISMATCH;
		break;
	case ISP_EVENT_MASK_INDEX_REG_UPDATE_MISSING:
		evt_id = ISP_EVENT_REG_UPDATE_MISSING;
		break;
	case ISP_EVENT_MASK_INDEX_BUF_FATAL_ERROR:
		evt_id = ISP_EVENT_BUF_FATAL_ERROR;
		break;
	default:
		evt_id = ISP_EVENT_SUBS_MASK_NONE;
		break;
	}

	return evt_id;
}

static inline int msm_isp_subscribe_event_mask(struct v4l2_fh *fh,
		struct v4l2_event_subscription *sub, int evt_mask_index,
		u32 evt_id, bool subscribe_flag)
{
	int rc = 0, i, interface;

	if (ISP_EVENT_MASK_INDEX_STATS_NOTIFY == evt_mask_index) {
		for (i = 0; i < MSM_ISP_STATS_MAX; i++) {
			sub->type = evt_id + i;
			if (subscribe_flag)
				rc = v4l2_event_subscribe(fh, sub,
					MAX_ISP_V4l2_EVENTS, NULL);
			else
				rc = v4l2_event_unsubscribe(fh, sub);
			if (rc != 0) {
				pr_err("%s: Subs event_type =0x%x failed\n",
					__func__, sub->type);
				return rc;
			}
		}
	} else if (ISP_EVENT_MASK_INDEX_SOF == evt_mask_index ||
		   ISP_EVENT_MASK_INDEX_REG_UPDATE == evt_mask_index ||
		   ISP_EVENT_MASK_INDEX_STREAM_UPDATE_DONE == evt_mask_index) {
		for (interface = 0; interface < VFE_SRC_MAX; interface++) {
			sub->type = evt_id | interface;
			if (subscribe_flag)
				rc = v4l2_event_subscribe(fh, sub,
					MAX_ISP_V4l2_EVENTS, NULL);
			else
				rc = v4l2_event_unsubscribe(fh, sub);
			if (rc != 0) {
				pr_err("%s: Subs event_type =0x%x failed\n",
					__func__, sub->type);
				return rc;
			}
		}
	} else {
		sub->type = evt_id;
		if (subscribe_flag)
			rc = v4l2_event_subscribe(fh, sub,
				MAX_ISP_V4l2_EVENTS, NULL);
		else
			rc = v4l2_event_unsubscribe(fh, sub);
		if (rc != 0) {
			pr_err("%s: Subs event_type =0x%x failed\n",
				__func__, sub->type);
			return rc;
		}
	}
	return rc;
}

static inline int msm_isp_process_event_subscription(struct v4l2_fh *fh,
	struct v4l2_event_subscription *sub, bool subscribe_flag)
{
	int rc = 0, evt_mask_index = 0;
	u32 evt_mask = sub->type;
	u32 evt_id = 0;

	if (ISP_EVENT_SUBS_MASK_NONE == evt_mask) {
		pr_err("%s: Subs event_type is None=0x%x\n",
			__func__, evt_mask);
		return 0;
	}

	for (evt_mask_index = ISP_EVENT_MASK_INDEX_STATS_NOTIFY;
		evt_mask_index <= ISP_EVENT_MASK_INDEX_BUF_FATAL_ERROR;
		evt_mask_index++) {
		if (evt_mask & (1<<evt_mask_index)) {
			evt_id = msm_isp_evt_mask_to_isp_event(evt_mask_index);
			rc = msm_isp_subscribe_event_mask(fh, sub,
				evt_mask_index, evt_id, subscribe_flag);
			if (rc != 0) {
				pr_err("%s: Subs event index:%d failed\n",
					__func__, evt_mask_index);
				return rc;
			}
		}
	}
	return rc;
}

int msm_isp_subscribe_event(struct v4l2_subdev *sd, struct v4l2_fh *fh,
	struct v4l2_event_subscription *sub)
{
	return msm_isp_process_event_subscription(fh, sub, true);
}

int msm_isp_unsubscribe_event(struct v4l2_subdev *sd, struct v4l2_fh *fh,
	struct v4l2_event_subscription *sub)
{
	return msm_isp_process_event_subscription(fh, sub, false);
}

static int msm_isp_get_max_clk_rate(struct vfe_device *vfe_dev, long *rate)
{
	int           clk_idx = 0;
	unsigned long max_value = ~0;
	long          round_rate = 0;

	if (!vfe_dev || !rate) {
		pr_err("%s:%d failed: vfe_dev %p rate %p\n", __func__, __LINE__,
			vfe_dev, rate);
		return -EINVAL;
	}

	*rate = 0;
	if (!vfe_dev->hw_info) {
		pr_err("%s:%d failed: vfe_dev->hw_info %p\n", __func__,
			__LINE__, vfe_dev->hw_info);
		return -EINVAL;
	}

	clk_idx = vfe_dev->hw_info->vfe_clk_idx;
	if (clk_idx >= vfe_dev->num_clk) {
		pr_err("%s:%d failed: clk_idx %d max array size %d\n",
			__func__, __LINE__, clk_idx,
			vfe_dev->num_clk);
		return -EINVAL;
	}

	round_rate = clk_round_rate(vfe_dev->vfe_clk[clk_idx], max_value);
	if (round_rate < 0) {
		pr_err("%s: Invalid vfe clock rate\n", __func__);
		return -EINVAL;
	}

	*rate = round_rate;
	return 0;
}

static int msm_isp_get_clk_rates(struct vfe_device *vfe_dev,
	struct msm_isp_clk_rates *rates)
{
	struct device_node *of_node;
	int32_t  rc = 0;
	uint32_t svs = 0, nominal = 0, turbo = 0;
	if (!vfe_dev || !rates) {
		pr_err("%s:%d failed: vfe_dev %p rates %p\n", __func__,
			__LINE__, vfe_dev, rates);
		return -EINVAL;
	}

	if (!vfe_dev->pdev) {
		pr_err("%s:%d failed: vfe_dev->pdev %p\n", __func__,
			__LINE__, vfe_dev->pdev);
		return -EINVAL;
	}

	of_node = vfe_dev->pdev->dev.of_node;

	if (!of_node) {
		pr_err("%s %d failed: of_node = %p\n", __func__,
		 __LINE__, of_node);
		return -EINVAL;
	}

	/*
	 * Many older targets dont define svs.
	 * return svs=0 for older targets.
	 */
	rc = of_property_read_u32(of_node, "max-clk-svs",
		&svs);
	if (rc < 0)
		svs = 0;

	rc = of_property_read_u32(of_node, "max-clk-nominal",
		&nominal);
	if (rc < 0 || !nominal) {
		pr_err("%s: nominal rate error\n", __func__);
		return -EINVAL;
	}

	rc = of_property_read_u32(of_node, "max-clk-turbo",
		&turbo);
	if (rc < 0 || !turbo) {
		pr_err("%s: turbo rate error\n", __func__);
			return -EINVAL;
	}
	rates->svs_rate = svs;
	rates->nominal_rate = nominal;
	rates->high_rate = turbo;
	return 0;
}

static int msm_isp_set_clk_rate(struct vfe_device *vfe_dev, long *rate)
{
	int rc = 0;
	int clk_idx = vfe_dev->hw_info->vfe_clk_idx;
	long round_rate =
		clk_round_rate(vfe_dev->vfe_clk[clk_idx], *rate);
	if (round_rate < 0) {
		pr_err("%s: Invalid vfe clock rate\n", __func__);
		return round_rate;
	}

	rc = clk_set_rate(vfe_dev->vfe_clk[clk_idx], round_rate);
	if (rc < 0) {
		pr_err("%s: Vfe set rate error\n", __func__);
		return rc;
	}
	*rate = round_rate;
	vfe_dev->msm_isp_vfe_clk_rate = round_rate;
	return 0;
}


static int msm_isp_start_fetch_engine(struct vfe_device *vfe_dev,
	void *arg)
{
	struct msm_vfe_fetch_eng_start *fe_cfg = arg;
	/*
	 * For Offline VFE, HAL expects same frame id
	 * for offline output which it requested in do_reprocess.
	 */
	vfe_dev->axi_data.src_info[VFE_PIX_0].frame_id =
		fe_cfg->frame_id;
	return vfe_dev->hw_info->vfe_ops.core_ops.
		start_fetch_eng(vfe_dev, arg);
}

void msm_isp_fetch_engine_done_notify(struct vfe_device *vfe_dev,
	struct msm_vfe_fetch_engine_info *fetch_engine_info)
{
	struct msm_isp_event_data fe_rd_done_event;
	if (!fetch_engine_info->is_busy)
		return;

	memset(&fe_rd_done_event, 0, sizeof(struct msm_isp_event_data));
	fe_rd_done_event.frame_id =
		vfe_dev->axi_data.src_info[VFE_PIX_0].frame_id;
	fe_rd_done_event.u.fetch_done.session_id =
		fetch_engine_info->session_id;
	fe_rd_done_event.u.fetch_done.stream_id = fetch_engine_info->stream_id;
	fe_rd_done_event.u.fetch_done.handle = fetch_engine_info->bufq_handle;
	fe_rd_done_event.u.fetch_done.buf_idx = fetch_engine_info->buf_idx;
	fe_rd_done_event.u.fetch_done.fd = fetch_engine_info->fd;
	fe_rd_done_event.u.fetch_done.offline_mode =
		fetch_engine_info->offline_mode;

	ISP_DBG("%s:VFE%d ISP_EVENT_FE_READ_DONE buf_idx %d\n",
		__func__, vfe_dev->pdev->id, fetch_engine_info->buf_idx);
	fetch_engine_info->is_busy = 0;
	msm_isp_send_event(vfe_dev, ISP_EVENT_FE_READ_DONE, &fe_rd_done_event);
}

static int msm_isp_cfg_pix(struct vfe_device *vfe_dev,
	struct msm_vfe_input_cfg *input_cfg)
{
	int rc = 0;
	struct msm_vfe_pix_cfg *pix_cfg = NULL;

	if (vfe_dev->axi_data.src_info[VFE_PIX_0].active) {
		pr_err("%s: pixel path is active\n", __func__);
		return -EINVAL;
	}

	pix_cfg = &input_cfg->d.pix_cfg;
	if ((pix_cfg->hvx_cmd > HVX_DISABLE) &&
		(pix_cfg->hvx_cmd <= HVX_ROUND_TRIP))
		vfe_dev->hvx_cmd = pix_cfg->hvx_cmd;
	vfe_dev->is_split = input_cfg->d.pix_cfg.is_split;

	vfe_dev->axi_data.src_info[VFE_PIX_0].pixel_clock =
		input_cfg->input_pix_clk;
	vfe_dev->axi_data.src_info[VFE_PIX_0].input_mux =
		input_cfg->d.pix_cfg.input_mux;
	vfe_dev->axi_data.src_info[VFE_PIX_0].input_format =
		input_cfg->d.pix_cfg.input_format;
	vfe_dev->axi_data.src_info[VFE_PIX_0].sof_counter_step = 1;

	/*
	 * Fill pixel_clock into input_pix_clk so that user space
	 * can use rounded clk rate
	 */
	input_cfg->input_pix_clk =
		vfe_dev->axi_data.src_info[VFE_PIX_0].pixel_clock;

	ISP_DBG("%s: input mux is %d CAMIF %d io_format 0x%x\n", __func__,
		input_cfg->d.pix_cfg.input_mux, CAMIF,
		input_cfg->d.pix_cfg.input_format);

	if (input_cfg->d.pix_cfg.input_mux == CAMIF ||
		input_cfg->d.pix_cfg.input_mux == TESTGEN) {
		vfe_dev->axi_data.src_info[VFE_PIX_0].width =
			input_cfg->d.pix_cfg.camif_cfg.pixels_per_line;
		if (input_cfg->d.pix_cfg.camif_cfg.subsample_cfg.
			sof_counter_step > 0) {
			vfe_dev->axi_data.src_info[VFE_PIX_0].
				sof_counter_step = input_cfg->d.pix_cfg.
				camif_cfg.subsample_cfg.sof_counter_step;
		}
	} else if (input_cfg->d.pix_cfg.input_mux == EXTERNAL_READ) {
		vfe_dev->axi_data.src_info[VFE_PIX_0].width =
			input_cfg->d.pix_cfg.fetch_engine_cfg.buf_stride;
	}
	vfe_dev->hw_info->vfe_ops.core_ops.cfg_input_mux(
			vfe_dev, &input_cfg->d.pix_cfg);
	vfe_dev->hw_info->vfe_ops.core_ops.reg_update(vfe_dev, VFE_PIX_0);
	return rc;
}

static int msm_isp_cfg_rdi(struct vfe_device *vfe_dev,
	struct msm_vfe_input_cfg *input_cfg)
{
	int rc = 0;
	if (vfe_dev->axi_data.src_info[input_cfg->input_src].active) {
		pr_err("%s: RAW%d path is active\n", __func__,
			   input_cfg->input_src - VFE_RAW_0);
		return -EINVAL;
	}

	vfe_dev->axi_data.src_info[input_cfg->input_src].pixel_clock =
		input_cfg->input_pix_clk;
	vfe_dev->hw_info->vfe_ops.core_ops.cfg_rdi_reg(
		vfe_dev, &input_cfg->d.rdi_cfg, input_cfg->input_src);
	return rc;
}

int msm_isp_cfg_input(struct vfe_device *vfe_dev, void *arg)
{
	int rc = 0;
	struct msm_vfe_input_cfg *input_cfg = arg;
	long pixel_clock = 0;

	switch (input_cfg->input_src) {
	case VFE_PIX_0:
		rc = msm_isp_cfg_pix(vfe_dev, input_cfg);
		break;
	case VFE_RAW_0:
	case VFE_RAW_1:
	case VFE_RAW_2:
		rc = msm_isp_cfg_rdi(vfe_dev, input_cfg);
		break;
	default:
		pr_err("%s: Invalid input source\n", __func__);
		rc = -EINVAL;
	}

	pixel_clock = input_cfg->input_pix_clk;
	rc = msm_isp_set_clk_rate(vfe_dev,
		&pixel_clock);
	if (rc < 0) {
		pr_err("%s: clock set rate failed\n", __func__);
		return rc;
	}
	return rc;
}

static int msm_isp_set_dual_HW_master_slave_mode(
	struct vfe_device *vfe_dev, void *arg)
{
	/*
	 * This method assumes no 2 processes are accessing it simultaneously.
	 * Currently this is guaranteed by mutex lock in ioctl.
	 * If that changes, need to revisit this
	 */
	int rc = 0, i, j;
	struct msm_isp_set_dual_hw_ms_cmd *dual_hw_ms_cmd = NULL;
	struct msm_vfe_src_info *src_info = NULL;
	unsigned long flags;

	if (!vfe_dev || !arg) {
		pr_err("%s: Error! Invalid input vfe_dev %p arg %p\n",
			__func__, vfe_dev, arg);
		return -EINVAL;
	}

	dual_hw_ms_cmd = (struct msm_isp_set_dual_hw_ms_cmd *)arg;
	vfe_dev->common_data->ms_resource.dual_hw_type = DUAL_HW_MASTER_SLAVE;
	vfe_dev->vfe_ub_policy = MSM_WM_UB_EQUAL_SLICING;
	if (dual_hw_ms_cmd->primary_intf < VFE_SRC_MAX) {
		src_info = &vfe_dev->axi_data.
			src_info[dual_hw_ms_cmd->primary_intf];
		src_info->dual_hw_ms_info.dual_hw_ms_type =
			dual_hw_ms_cmd->dual_hw_ms_type;
	}

	/* No lock needed here since ioctl lock protects 2 session from race */
	if (src_info != NULL &&
		dual_hw_ms_cmd->dual_hw_ms_type == MS_TYPE_MASTER) {
		src_info->dual_hw_type = DUAL_HW_MASTER_SLAVE;
		ISP_DBG("%s: Master\n", __func__);

		src_info->dual_hw_ms_info.sof_info =
			&vfe_dev->common_data->ms_resource.master_sof_info;
		vfe_dev->common_data->ms_resource.sof_delta_threshold =
			dual_hw_ms_cmd->sof_delta_threshold;
	} else if (src_info != NULL) {
		spin_lock_irqsave(
			&vfe_dev->common_data->common_dev_data_lock,
			flags);
		src_info->dual_hw_type = DUAL_HW_MASTER_SLAVE;
		ISP_DBG("%s: Slave\n", __func__);

		for (j = 0; j < MS_NUM_SLAVE_MAX; j++) {
			if (vfe_dev->common_data->ms_resource.
				reserved_slave_mask & (1 << j))
				continue;

			vfe_dev->common_data->ms_resource.reserved_slave_mask |=
				(1 << j);
			vfe_dev->common_data->ms_resource.num_slave++;
			src_info->dual_hw_ms_info.sof_info =
				&vfe_dev->common_data->ms_resource.
				slave_sof_info[j];
			src_info->dual_hw_ms_info.slave_id = j;
			ISP_DBG("%s: Slave id %d\n", __func__, j);
			break;
		}
		spin_unlock_irqrestore(
			&vfe_dev->common_data->common_dev_data_lock,
			flags);

		if (j == MS_NUM_SLAVE_MAX) {
			pr_err("%s: Error! Cannot find free aux resource\n",
				__func__);
			return -EBUSY;
		}
	}
	ISP_DBG("%s: num_src %d\n", __func__, dual_hw_ms_cmd->num_src);
	/* This for loop is for non-primary intf to be marked with Master/Slave
	 * in order for frame id sync. But their timestamp is not saved.
	 * So no sof_info resource is allocated */
	for (i = 0; i < dual_hw_ms_cmd->num_src; i++) {
		if (dual_hw_ms_cmd->input_src[i] >= VFE_SRC_MAX) {
			pr_err("%s: Error! Invalid SRC param %d\n", __func__,
				dual_hw_ms_cmd->input_src[i]);
			return -EINVAL;
		}
		ISP_DBG("%s: src %d\n", __func__, dual_hw_ms_cmd->input_src[i]);
		src_info = &vfe_dev->axi_data.
			src_info[dual_hw_ms_cmd->input_src[i]];
		src_info->dual_hw_type = DUAL_HW_MASTER_SLAVE;
		src_info->dual_hw_ms_info.dual_hw_ms_type =
			dual_hw_ms_cmd->dual_hw_ms_type;
	}

	return rc;
}

static int msm_isp_proc_cmd_list_unlocked(struct vfe_device *vfe_dev, void *arg)
{
	int rc = 0;
	struct msm_vfe_cfg_cmd_list *proc_cmd =
		(struct msm_vfe_cfg_cmd_list *)arg;
	struct msm_vfe_cfg_cmd_list cmd, cmd_next;

	if (!vfe_dev || !arg) {
		pr_err("%s:%d failed: vfe_dev %p arg %p", __func__, __LINE__,
			vfe_dev, arg);
		return -EINVAL;
	}

	rc = msm_isp_proc_cmd(vfe_dev, &proc_cmd->cfg_cmd);
	if (rc < 0)
		pr_err("%s:%d failed: rc %d", __func__, __LINE__, rc);

	cmd = *proc_cmd;

	while (cmd.next) {
		if (cmd.next_size != sizeof(struct msm_vfe_cfg_cmd_list)) {
			pr_err("%s:%d failed: next size %u != expected %zu\n",
				__func__, __LINE__, cmd.next_size,
				sizeof(struct msm_vfe_cfg_cmd_list));
			break;
		}
		if (copy_from_user(&cmd_next, (void __user *)cmd.next,
			sizeof(struct msm_vfe_cfg_cmd_list))) {
			rc = -EFAULT;
			continue;
		}

		rc = msm_isp_proc_cmd(vfe_dev, &cmd_next.cfg_cmd);
		if (rc < 0)
			pr_err("%s:%d failed: rc %d", __func__, __LINE__, rc);

		cmd = cmd_next;
	}
	return rc;
}

#ifdef CONFIG_COMPAT
struct msm_vfe_cfg_cmd2_32 {
	uint16_t num_cfg;
	uint16_t cmd_len;
	compat_caddr_t cfg_data;
	compat_caddr_t cfg_cmd;
};

struct msm_vfe_cfg_cmd_list_32 {
	struct msm_vfe_cfg_cmd2_32   cfg_cmd;
	compat_caddr_t               next;
	uint32_t                     next_size;
};

#define VIDIOC_MSM_VFE_REG_CFG_COMPAT \
	_IOWR('V', BASE_VIDIOC_PRIVATE, struct msm_vfe_cfg_cmd2_32)
#define VIDIOC_MSM_VFE_REG_LIST_CFG_COMPAT \
	_IOWR('V', BASE_VIDIOC_PRIVATE+14, struct msm_vfe_cfg_cmd_list_32)

static void msm_isp_compat_to_proc_cmd(struct msm_vfe_cfg_cmd2 *proc_cmd,
	struct msm_vfe_cfg_cmd2_32 *proc_cmd_ptr32)
{
	proc_cmd->num_cfg = proc_cmd_ptr32->num_cfg;
	proc_cmd->cmd_len = proc_cmd_ptr32->cmd_len;
	proc_cmd->cfg_data = compat_ptr(proc_cmd_ptr32->cfg_data);
	proc_cmd->cfg_cmd = compat_ptr(proc_cmd_ptr32->cfg_cmd);
}

static int msm_isp_proc_cmd_list_compat(struct vfe_device *vfe_dev, void *arg)
{
	int rc = 0;
	struct msm_vfe_cfg_cmd_list_32 *proc_cmd =
		(struct msm_vfe_cfg_cmd_list_32 *)arg;
	struct msm_vfe_cfg_cmd_list_32 cmd, cmd_next;
	struct msm_vfe_cfg_cmd2 current_cmd;

	if (!vfe_dev || !arg) {
		pr_err("%s:%d failed: vfe_dev %p arg %p", __func__, __LINE__,
			vfe_dev, arg);
		return -EINVAL;
	}
	msm_isp_compat_to_proc_cmd(&current_cmd, &proc_cmd->cfg_cmd);
	rc = msm_isp_proc_cmd(vfe_dev, &current_cmd);
	if (rc < 0)
		pr_err("%s:%d failed: rc %d", __func__, __LINE__, rc);

	cmd = *proc_cmd;

	while (NULL != compat_ptr(cmd.next)) {
		if (cmd.next_size != sizeof(struct msm_vfe_cfg_cmd_list_32)) {
			pr_err("%s:%d failed: next size %u != expected %zu\n",
				__func__, __LINE__, cmd.next_size,
				sizeof(struct msm_vfe_cfg_cmd_list));
			break;
		}
		if (copy_from_user(&cmd_next, compat_ptr(cmd.next),
			sizeof(struct msm_vfe_cfg_cmd_list_32))) {
			rc = -EFAULT;
			continue;
		}

		msm_isp_compat_to_proc_cmd(&current_cmd, &cmd_next.cfg_cmd);
		rc = msm_isp_proc_cmd(vfe_dev, &current_cmd);
		if (rc < 0)
			pr_err("%s:%d failed: rc %d", __func__, __LINE__, rc);

		cmd = cmd_next;
	}
	return rc;
}

static int msm_isp_proc_cmd_list(struct vfe_device *vfe_dev, void *arg)
{
	if (is_compat_task())
		return msm_isp_proc_cmd_list_compat(vfe_dev, arg);
	else
		return msm_isp_proc_cmd_list_unlocked(vfe_dev, arg);
}
#else /* CONFIG_COMPAT */
static int msm_isp_proc_cmd_list(struct vfe_device *vfe_dev, void *arg)
{
	return msm_isp_proc_cmd_list_unlocked(vfe_dev, arg);
}
#endif /* CONFIG_COMPAT */


static long msm_isp_ioctl_unlocked(struct v4l2_subdev *sd,
	unsigned int cmd, void *arg)
{
	long rc = 0;
	struct vfe_device *vfe_dev = v4l2_get_subdevdata(sd);

	if (!vfe_dev || !vfe_dev->vfe_base) {
		pr_err("%s:%d failed: invalid params %p\n",
			__func__, __LINE__, vfe_dev);
		if (vfe_dev)
			pr_err("%s:%d failed %p\n", __func__,
				__LINE__, vfe_dev->vfe_base);
		return -EINVAL;
	}

	/* use real time mutex for hard real-time ioctls such as
	 * buffer operations and register updates.
	 * Use core mutex for other ioctls that could take
	 * longer time to complete such as start/stop ISP streams
	 * which blocks until the hardware start/stop streaming
	 */
	ISP_DBG("%s: cmd: %d\n", __func__, _IOC_TYPE(cmd));
	switch (cmd) {
	case VIDIOC_MSM_VFE_REG_CFG: {
		mutex_lock(&vfe_dev->realtime_mutex);
		rc = msm_isp_proc_cmd(vfe_dev, arg);
		mutex_unlock(&vfe_dev->realtime_mutex);
		break;
	}
	case VIDIOC_MSM_VFE_REG_LIST_CFG: {
		mutex_lock(&vfe_dev->realtime_mutex);
		rc = msm_isp_proc_cmd_list(vfe_dev, arg);
		mutex_unlock(&vfe_dev->realtime_mutex);
		break;
	}
	case VIDIOC_MSM_ISP_REQUEST_BUF:
		/* fallthrough */
	case VIDIOC_MSM_ISP_ENQUEUE_BUF:
		/* fallthrough */
	case VIDIOC_MSM_ISP_DEQUEUE_BUF:
		/* fallthrough */
	case VIDIOC_MSM_ISP_UNMAP_BUF: {
		mutex_lock(&vfe_dev->buf_mgr->lock);
		rc = msm_isp_proc_buf_cmd(vfe_dev->buf_mgr, cmd, arg);
		mutex_unlock(&vfe_dev->buf_mgr->lock);
		break;
	}
	case VIDIOC_MSM_ISP_RELEASE_BUF: {
		if (vfe_dev->buf_mgr == NULL) {
			pr_err("%s: buf mgr NULL! rc = -1\n", __func__);
			rc = -EINVAL;
			return rc;
		}
		mutex_lock(&vfe_dev->buf_mgr->lock);
		rc = msm_isp_proc_buf_cmd(vfe_dev->buf_mgr, cmd, arg);
		mutex_unlock(&vfe_dev->buf_mgr->lock);
		break;
	}
	case VIDIOC_MSM_ISP_REQUEST_STREAM:
		mutex_lock(&vfe_dev->core_mutex);
		rc = msm_isp_request_axi_stream(vfe_dev, arg);
		mutex_unlock(&vfe_dev->core_mutex);
		break;
	case VIDIOC_MSM_ISP_RELEASE_STREAM:
		mutex_lock(&vfe_dev->core_mutex);
		rc = msm_isp_release_axi_stream(vfe_dev, arg);
		mutex_unlock(&vfe_dev->core_mutex);
		break;
	case VIDIOC_MSM_ISP_CFG_STREAM:
		mutex_lock(&vfe_dev->core_mutex);
		rc = msm_isp_cfg_axi_stream(vfe_dev, arg);
		mutex_unlock(&vfe_dev->core_mutex);
		break;
	case VIDIOC_MSM_ISP_AXI_HALT:
		mutex_lock(&vfe_dev->core_mutex);
		rc = msm_isp_axi_halt(vfe_dev, arg);
		mutex_unlock(&vfe_dev->core_mutex);
		break;
	case VIDIOC_MSM_ISP_AXI_RESET:
		mutex_lock(&vfe_dev->core_mutex);
		if (atomic_read(&vfe_dev->error_info.overflow_state)
			!= HALT_ENFORCED) {
			rc = msm_isp_stats_reset(vfe_dev);
			rc |= msm_isp_axi_reset(vfe_dev, arg);
		} else {
			pr_err_ratelimited("%s: no HW reset, halt enforced.\n",
				__func__);
		}
		mutex_unlock(&vfe_dev->core_mutex);
		break;
	case VIDIOC_MSM_ISP_AXI_RESTART:
		mutex_lock(&vfe_dev->core_mutex);
		if (atomic_read(&vfe_dev->error_info.overflow_state)
			!= HALT_ENFORCED) {
			rc = msm_isp_stats_restart(vfe_dev);
			rc |= msm_isp_axi_restart(vfe_dev, arg);
		} else {
			pr_err_ratelimited("%s: no AXI restart, halt enforced.\n",
				__func__);
		}
		mutex_unlock(&vfe_dev->core_mutex);
		break;
	case VIDIOC_MSM_ISP_INPUT_CFG:
		mutex_lock(&vfe_dev->core_mutex);
		rc = msm_isp_cfg_input(vfe_dev, arg);
		mutex_unlock(&vfe_dev->core_mutex);
		break;
	case VIDIOC_MSM_ISP_SET_DUAL_HW_MASTER_SLAVE:
		mutex_lock(&vfe_dev->core_mutex);
		rc = msm_isp_set_dual_HW_master_slave_mode(vfe_dev, arg);
		mutex_unlock(&vfe_dev->core_mutex);
		break;
	case VIDIOC_MSM_ISP_FETCH_ENG_START:
	case VIDIOC_MSM_ISP_MAP_BUF_START_FE:
		mutex_lock(&vfe_dev->core_mutex);
		rc = msm_isp_start_fetch_engine(vfe_dev, arg);
		mutex_unlock(&vfe_dev->core_mutex);
		break;
	case VIDIOC_MSM_ISP_REG_UPDATE_CMD:
		if (arg) {
			enum msm_vfe_input_src frame_src =
				*((enum msm_vfe_input_src *)arg);
			vfe_dev->hw_info->vfe_ops.core_ops.
				reg_update(vfe_dev, frame_src);
		}
		break;
	case VIDIOC_MSM_ISP_SET_SRC_STATE:
		mutex_lock(&vfe_dev->core_mutex);
		rc = msm_isp_set_src_state(vfe_dev, arg);
		mutex_unlock(&vfe_dev->core_mutex);
		break;
	case VIDIOC_MSM_ISP_REQUEST_STATS_STREAM:
		mutex_lock(&vfe_dev->core_mutex);
		rc = msm_isp_request_stats_stream(vfe_dev, arg);
		mutex_unlock(&vfe_dev->core_mutex);
		break;
	case VIDIOC_MSM_ISP_RELEASE_STATS_STREAM:
		mutex_lock(&vfe_dev->core_mutex);
		rc = msm_isp_release_stats_stream(vfe_dev, arg);
		mutex_unlock(&vfe_dev->core_mutex);
		break;
	case VIDIOC_MSM_ISP_CFG_STATS_STREAM:
		mutex_lock(&vfe_dev->core_mutex);
		rc = msm_isp_cfg_stats_stream(vfe_dev, arg);
		mutex_unlock(&vfe_dev->core_mutex);
		break;
	case VIDIOC_MSM_ISP_UPDATE_STATS_STREAM:
		mutex_lock(&vfe_dev->core_mutex);
		rc = msm_isp_update_stats_stream(vfe_dev, arg);
		mutex_unlock(&vfe_dev->core_mutex);
		break;
	case VIDIOC_MSM_ISP_UPDATE_STREAM:
		mutex_lock(&vfe_dev->core_mutex);
		rc = msm_isp_update_axi_stream(vfe_dev, arg);
		mutex_unlock(&vfe_dev->core_mutex);
		break;
	case VIDIOC_MSM_ISP_SMMU_ATTACH:
		mutex_lock(&vfe_dev->core_mutex);
		rc = msm_isp_smmu_attach(vfe_dev->buf_mgr, arg);
		mutex_unlock(&vfe_dev->core_mutex);
		break;
	case MSM_SD_NOTIFY_FREEZE:
		vfe_dev->isp_sof_debug = 0;
		vfe_dev->isp_raw0_debug = 0;
		vfe_dev->isp_raw1_debug = 0;
		vfe_dev->isp_raw2_debug = 0;
		break;
	case MSM_SD_SHUTDOWN:
		while (vfe_dev->vfe_open_cnt != 0)
			msm_isp_close_node(sd, NULL);
		break;

	default:
		pr_err_ratelimited("%s: Invalid ISP command %d\n", __func__,
				    cmd);
		rc = -EINVAL;
	}
	return rc;
}


#ifdef CONFIG_COMPAT
static long msm_isp_ioctl_compat(struct v4l2_subdev *sd,
	unsigned int cmd, void *arg)
{
	struct vfe_device *vfe_dev = v4l2_get_subdevdata(sd);
	long rc = 0;

	if (!vfe_dev || !vfe_dev->vfe_base) {
		pr_err("%s:%d failed: invalid params %p\n",
			__func__, __LINE__, vfe_dev);
		if (vfe_dev)
			pr_err("%s:%d failed %p\n", __func__,
				__LINE__, vfe_dev->vfe_base);
		return -EINVAL;
	}

	switch (cmd) {
	case VIDIOC_MSM_VFE_REG_CFG_COMPAT: {
		struct msm_vfe_cfg_cmd2 proc_cmd;
		mutex_lock(&vfe_dev->realtime_mutex);
		msm_isp_compat_to_proc_cmd(&proc_cmd,
			(struct msm_vfe_cfg_cmd2_32 *) arg);
		rc = msm_isp_proc_cmd(vfe_dev, &proc_cmd);
		mutex_unlock(&vfe_dev->realtime_mutex);
		break;
	}
	case VIDIOC_MSM_VFE_REG_LIST_CFG_COMPAT: {
		mutex_lock(&vfe_dev->realtime_mutex);
		rc = msm_isp_proc_cmd_list(vfe_dev, arg);
		mutex_unlock(&vfe_dev->realtime_mutex);
		break;
	}
	default:
		return msm_isp_ioctl_unlocked(sd, cmd, arg);
	}

	return rc;
}

long msm_isp_ioctl(struct v4l2_subdev *sd,
	unsigned int cmd, void *arg)
{
	return msm_isp_ioctl_compat(sd, cmd, arg);
}
#else /* CONFIG_COMPAT */
long msm_isp_ioctl(struct v4l2_subdev *sd,
	unsigned int cmd, void *arg)
{
	return msm_isp_ioctl_unlocked(sd, cmd, arg);
}
#endif /* CONFIG_COMPAT */

static int msm_isp_send_hw_cmd(struct vfe_device *vfe_dev,
	struct msm_vfe_reg_cfg_cmd *reg_cfg_cmd,
	uint32_t *cfg_data, uint32_t cmd_len)
{
	if (!vfe_dev || !reg_cfg_cmd) {
		pr_err("%s:%d failed: vfe_dev %p reg_cfg_cmd %p\n", __func__,
			__LINE__, vfe_dev, reg_cfg_cmd);
		return -EINVAL;
	}
	if ((reg_cfg_cmd->cmd_type != VFE_CFG_MASK) &&
		(!cfg_data || !cmd_len)) {
		pr_err("%s:%d failed: cmd type %d cfg_data %p cmd_len %d\n",
			__func__, __LINE__, reg_cfg_cmd->cmd_type, cfg_data,
			cmd_len);
		return -EINVAL;
	}

	/* Validate input parameters */
	switch (reg_cfg_cmd->cmd_type) {
	case VFE_WRITE:
	case VFE_READ:
	case VFE_WRITE_MB: {
		if ((reg_cfg_cmd->u.rw_info.reg_offset >
			(UINT_MAX - reg_cfg_cmd->u.rw_info.len)) ||
			((reg_cfg_cmd->u.rw_info.reg_offset +
			reg_cfg_cmd->u.rw_info.len) >
			resource_size(vfe_dev->vfe_mem))) {
			pr_err("%s:%d reg_offset %d len %d res %d\n",
				__func__, __LINE__,
				reg_cfg_cmd->u.rw_info.reg_offset,
				reg_cfg_cmd->u.rw_info.len,
				(uint32_t)resource_size(vfe_dev->vfe_mem));
			return -EINVAL;
		}

		if ((reg_cfg_cmd->u.rw_info.cmd_data_offset >
			(UINT_MAX - reg_cfg_cmd->u.rw_info.len)) ||
			((reg_cfg_cmd->u.rw_info.cmd_data_offset +
			reg_cfg_cmd->u.rw_info.len) > cmd_len)) {
			pr_err("%s:%d cmd_data_offset %d len %d cmd_len %d\n",
				__func__, __LINE__,
				reg_cfg_cmd->u.rw_info.cmd_data_offset,
				reg_cfg_cmd->u.rw_info.len, cmd_len);
			return -EINVAL;
		}
		break;
	}

	case VFE_WRITE_DMI_16BIT:
	case VFE_WRITE_DMI_32BIT:
	case VFE_WRITE_DMI_64BIT:
	case VFE_READ_DMI_16BIT:
	case VFE_READ_DMI_32BIT:
	case VFE_READ_DMI_64BIT: {
		if (reg_cfg_cmd->cmd_type == VFE_WRITE_DMI_64BIT) {
			if ((reg_cfg_cmd->u.dmi_info.hi_tbl_offset <=
				reg_cfg_cmd->u.dmi_info.lo_tbl_offset) ||
				(reg_cfg_cmd->u.dmi_info.hi_tbl_offset -
				reg_cfg_cmd->u.dmi_info.lo_tbl_offset !=
				(sizeof(uint32_t)))) {
				pr_err("%s:%d hi %d lo %d\n",
					__func__, __LINE__,
					reg_cfg_cmd->u.dmi_info.hi_tbl_offset,
					reg_cfg_cmd->u.dmi_info.hi_tbl_offset);
				return -EINVAL;
			}
			if (reg_cfg_cmd->u.dmi_info.len <= sizeof(uint32_t)) {
				pr_err("%s:%d len %d\n",
					__func__, __LINE__,
					reg_cfg_cmd->u.dmi_info.len);
				return -EINVAL;
			}
			if (((UINT_MAX -
				reg_cfg_cmd->u.dmi_info.hi_tbl_offset) <
				(reg_cfg_cmd->u.dmi_info.len -
				sizeof(uint32_t))) ||
				((reg_cfg_cmd->u.dmi_info.hi_tbl_offset +
				reg_cfg_cmd->u.dmi_info.len -
				sizeof(uint32_t)) > cmd_len)) {
				pr_err("%s:%d hi_tbl_offset %d len %d cmd %d\n",
					__func__, __LINE__,
					reg_cfg_cmd->u.dmi_info.hi_tbl_offset,
					reg_cfg_cmd->u.dmi_info.len, cmd_len);
				return -EINVAL;
			}
		}
		if ((reg_cfg_cmd->u.dmi_info.lo_tbl_offset >
			(UINT_MAX - reg_cfg_cmd->u.dmi_info.len)) ||
			((reg_cfg_cmd->u.dmi_info.lo_tbl_offset +
			reg_cfg_cmd->u.dmi_info.len) > cmd_len)) {
			pr_err("%s:%d lo_tbl_offset %d len %d cmd_len %d\n",
				__func__, __LINE__,
				reg_cfg_cmd->u.dmi_info.lo_tbl_offset,
				reg_cfg_cmd->u.dmi_info.len, cmd_len);
			return -EINVAL;
		}
		break;
	}

	default:
		break;
	}

	switch (reg_cfg_cmd->cmd_type) {
	case VFE_WRITE: {
		msm_camera_io_memcpy(vfe_dev->vfe_base +
			reg_cfg_cmd->u.rw_info.reg_offset,
			cfg_data + reg_cfg_cmd->u.rw_info.cmd_data_offset/4,
			reg_cfg_cmd->u.rw_info.len);
		break;
	}
	case VFE_WRITE_MB: {
		msm_camera_io_memcpy_mb(vfe_dev->vfe_base +
			reg_cfg_cmd->u.rw_info.reg_offset,
			cfg_data + reg_cfg_cmd->u.rw_info.cmd_data_offset/4,
			reg_cfg_cmd->u.rw_info.len);
		break;
	}
	case VFE_CFG_MASK: {
		uint32_t temp;
		bool grab_lock;
		unsigned long flags;
		if ((UINT_MAX - sizeof(temp) <
			reg_cfg_cmd->u.mask_info.reg_offset) ||
			(resource_size(vfe_dev->vfe_mem) <
			reg_cfg_cmd->u.mask_info.reg_offset +
			sizeof(temp))) {
			pr_err("%s: VFE_CFG_MASK: Invalid length\n", __func__);
			return -EINVAL;
		}
		grab_lock = vfe_dev->hw_info->vfe_ops.core_ops.
			is_module_cfg_lock_needed(reg_cfg_cmd->
			u.mask_info.reg_offset);
		if (grab_lock)
			spin_lock_irqsave(&vfe_dev->shared_data_lock, flags);
		temp = msm_camera_io_r(vfe_dev->vfe_base +
			reg_cfg_cmd->u.mask_info.reg_offset);

		temp &= ~reg_cfg_cmd->u.mask_info.mask;
		temp |= reg_cfg_cmd->u.mask_info.val;
		msm_camera_io_w(temp, vfe_dev->vfe_base +
			reg_cfg_cmd->u.mask_info.reg_offset);
		if (grab_lock)
			spin_unlock_irqrestore(&vfe_dev->shared_data_lock,
				flags);
		break;
	}
	case VFE_WRITE_DMI_16BIT:
	case VFE_WRITE_DMI_32BIT:
	case VFE_WRITE_DMI_64BIT: {
		int i;
		uint32_t *hi_tbl_ptr = NULL, *lo_tbl_ptr = NULL;
		uint32_t hi_val, lo_val, lo_val1;
		if (reg_cfg_cmd->cmd_type == VFE_WRITE_DMI_64BIT) {
			hi_tbl_ptr = cfg_data +
				reg_cfg_cmd->u.dmi_info.hi_tbl_offset/4;
		}
		lo_tbl_ptr = cfg_data +
			reg_cfg_cmd->u.dmi_info.lo_tbl_offset/4;
		if (reg_cfg_cmd->cmd_type == VFE_WRITE_DMI_64BIT)
			reg_cfg_cmd->u.dmi_info.len =
				reg_cfg_cmd->u.dmi_info.len / 2;
		for (i = 0; i < reg_cfg_cmd->u.dmi_info.len/4; i++) {
			lo_val = *lo_tbl_ptr++;
			if (reg_cfg_cmd->cmd_type == VFE_WRITE_DMI_16BIT) {
				lo_val1 = lo_val & 0x0000FFFF;
				lo_val = (lo_val & 0xFFFF0000)>>16;
				msm_camera_io_w(lo_val1, vfe_dev->vfe_base +
					vfe_dev->hw_info->dmi_reg_offset + 0x4);
			} else if (reg_cfg_cmd->cmd_type ==
					   VFE_WRITE_DMI_64BIT) {
				lo_tbl_ptr++;
				hi_val = *hi_tbl_ptr;
				hi_tbl_ptr = hi_tbl_ptr + 2;
				msm_camera_io_w(hi_val, vfe_dev->vfe_base +
					vfe_dev->hw_info->dmi_reg_offset);
			}
			msm_camera_io_w(lo_val, vfe_dev->vfe_base +
				vfe_dev->hw_info->dmi_reg_offset + 0x4);
		}
		break;
	}
	case VFE_READ_DMI_16BIT:
	case VFE_READ_DMI_32BIT:
	case VFE_READ_DMI_64BIT: {
		int i;
		uint32_t *hi_tbl_ptr = NULL, *lo_tbl_ptr = NULL;
		uint32_t hi_val, lo_val, lo_val1;
		if (reg_cfg_cmd->cmd_type == VFE_READ_DMI_64BIT) {
			hi_tbl_ptr = cfg_data +
				reg_cfg_cmd->u.dmi_info.hi_tbl_offset/4;
		}

		lo_tbl_ptr = cfg_data +
			reg_cfg_cmd->u.dmi_info.lo_tbl_offset/4;

		if (reg_cfg_cmd->cmd_type == VFE_READ_DMI_64BIT)
			reg_cfg_cmd->u.dmi_info.len =
				reg_cfg_cmd->u.dmi_info.len / 2;

		for (i = 0; i < reg_cfg_cmd->u.dmi_info.len/4; i++) {
			lo_val = msm_camera_io_r(vfe_dev->vfe_base +
					vfe_dev->hw_info->dmi_reg_offset + 0x4);

			if (reg_cfg_cmd->cmd_type == VFE_READ_DMI_16BIT) {
				lo_val1 = msm_camera_io_r(vfe_dev->vfe_base +
					vfe_dev->hw_info->dmi_reg_offset + 0x4);
				lo_val |= lo_val1 << 16;
			}
			*lo_tbl_ptr++ = lo_val;
			if (reg_cfg_cmd->cmd_type == VFE_READ_DMI_64BIT) {
				hi_val = msm_camera_io_r(vfe_dev->vfe_base +
					vfe_dev->hw_info->dmi_reg_offset);
				*hi_tbl_ptr = hi_val;
				hi_tbl_ptr += 2;
				lo_tbl_ptr++;
			}
		}
		break;
	}
	case VFE_HW_UPDATE_LOCK: {
		uint32_t update_id =
			vfe_dev->axi_data.src_info[VFE_PIX_0].last_updt_frm_id;
		if (vfe_dev->axi_data.src_info[VFE_PIX_0].frame_id != *cfg_data
			|| update_id == *cfg_data) {
			pr_err("%s hw update lock failed acq %d, cur id %u, last id %u\n",
				__func__,
				*cfg_data,
				vfe_dev->axi_data.src_info[VFE_PIX_0].frame_id,
				update_id);
			return -EINVAL;
		}
		break;
	}
	case VFE_HW_UPDATE_UNLOCK: {
		if (vfe_dev->axi_data.src_info[VFE_PIX_0].frame_id
			!= *cfg_data) {
			pr_err("hw update across frame boundary,begin id %u, end id %d\n",
				*cfg_data,
				vfe_dev->axi_data.src_info[VFE_PIX_0].frame_id);
		}
		vfe_dev->axi_data.src_info[VFE_PIX_0].last_updt_frm_id =
			vfe_dev->axi_data.src_info[VFE_PIX_0].frame_id;
		break;
	}
	case VFE_READ: {
		int i;
		uint32_t *data_ptr = cfg_data +
			reg_cfg_cmd->u.rw_info.cmd_data_offset/4;
		for (i = 0; i < reg_cfg_cmd->u.rw_info.len/4; i++) {
			if ((data_ptr < cfg_data) ||
				(UINT_MAX / sizeof(*data_ptr) <
				 (data_ptr - cfg_data)) ||
				(sizeof(*data_ptr) * (data_ptr - cfg_data) >=
				 cmd_len))
				return -EINVAL;
			*data_ptr++ = msm_camera_io_r(vfe_dev->vfe_base +
				reg_cfg_cmd->u.rw_info.reg_offset);
			reg_cfg_cmd->u.rw_info.reg_offset += 4;
		}
		break;
	}
	case GET_MAX_CLK_RATE: {
		int rc = 0;
		unsigned long rate;

		if (cmd_len != sizeof(__u32)) {
			pr_err("%s:%d failed: invalid cmd len %u exp %zu\n",
				__func__, __LINE__, cmd_len,
				sizeof(__u32));
			return -EINVAL;
		}
		rc = msm_isp_get_max_clk_rate(vfe_dev, &rate);
		if (rc < 0) {
			pr_err("%s:%d failed: rc %d\n", __func__, __LINE__, rc);
			return -EINVAL;
		}

		*(__u32 *)cfg_data = (__u32)rate;

		break;
	}
	case GET_CLK_RATES: {
		int rc = 0;
		struct msm_isp_clk_rates rates;
		struct msm_isp_clk_rates *user_data =
			(struct msm_isp_clk_rates *)cfg_data;
		if (cmd_len != sizeof(struct msm_isp_clk_rates)) {
			pr_err("%s:%d failed: invalid cmd len %u exp %zu\n",
				__func__, __LINE__, cmd_len,
				sizeof(struct msm_isp_clk_rates));
			return -EINVAL;
		}
		rc = msm_isp_get_clk_rates(vfe_dev, &rates);
		if (rc < 0) {
			pr_err("%s:%d failed: rc %d\n", __func__, __LINE__, rc);
			return -EINVAL;
		}
		user_data->svs_rate = rates.svs_rate;
		user_data->nominal_rate = rates.nominal_rate;
		user_data->high_rate = rates.high_rate;
		break;
	}
	case GET_ISP_ID: {
		uint32_t *isp_id = NULL;

		if (cmd_len < sizeof(uint32_t)) {
			pr_err("%s:%d failed: invalid cmd len %u exp %zu\n",
				__func__, __LINE__, cmd_len,
				sizeof(uint32_t));
			return -EINVAL;
		}

		isp_id = (uint32_t *)cfg_data;
		*isp_id = vfe_dev->pdev->id;
		break;
	}
	case SET_WM_UB_SIZE:
		break;
	case SET_UB_POLICY: {

		if (cmd_len < sizeof(vfe_dev->vfe_ub_policy)) {
			pr_err("%s:%d failed: invalid cmd len %u exp %zu\n",
				__func__, __LINE__, cmd_len,
				sizeof(vfe_dev->vfe_ub_policy));
			return -EINVAL;
		}
		vfe_dev->vfe_ub_policy = *cfg_data;
		break;
	}
	case GET_VFE_HW_LIMIT: {
		uint32_t *hw_limit = NULL;
		if (cmd_len < sizeof(uint32_t)) {
			pr_err("%s:%d failed: invalid cmd len %u exp %zu\n",
				__func__, __LINE__, cmd_len,
				sizeof(uint32_t));
			return -EINVAL;
		}
		hw_limit = (uint32_t *)cfg_data;
		*hw_limit = vfe_dev->vfe_hw_limit;
		break;
	}
	}
	return 0;
}

int msm_isp_proc_cmd(struct vfe_device *vfe_dev, void *arg)
{
	int rc = 0, i;
	struct msm_vfe_cfg_cmd2 *proc_cmd = arg;
	struct msm_vfe_reg_cfg_cmd *reg_cfg_cmd;
	uint32_t *cfg_data = NULL;

	if (!proc_cmd->num_cfg) {
		pr_err("%s: Passed num_cfg as 0\n", __func__);
		return -EINVAL;
	}

	reg_cfg_cmd = kzalloc(sizeof(struct msm_vfe_reg_cfg_cmd)*
		proc_cmd->num_cfg, GFP_KERNEL);
	if (!reg_cfg_cmd) {
		pr_err("%s: reg_cfg alloc failed\n", __func__);
		rc = -ENOMEM;
		goto reg_cfg_failed;
	}

	if (copy_from_user(reg_cfg_cmd,
		(void __user *)(proc_cmd->cfg_cmd),
		sizeof(struct msm_vfe_reg_cfg_cmd) * proc_cmd->num_cfg)) {
		rc = -EFAULT;
		goto copy_cmd_failed;
	}

	if (proc_cmd->cmd_len > 0) {
		cfg_data = kzalloc(proc_cmd->cmd_len, GFP_KERNEL);
		if (!cfg_data) {
			pr_err("%s: cfg_data alloc failed\n", __func__);
			rc = -ENOMEM;
			goto cfg_data_failed;
		}

		if (copy_from_user(cfg_data,
			(void __user *)(proc_cmd->cfg_data),
			proc_cmd->cmd_len)) {
			rc = -EFAULT;
			goto copy_cmd_failed;
		}
	}

	for (i = 0; i < proc_cmd->num_cfg; i++)
		rc = msm_isp_send_hw_cmd(vfe_dev, &reg_cfg_cmd[i],
			cfg_data, proc_cmd->cmd_len);

	if (copy_to_user(proc_cmd->cfg_data,
			cfg_data, proc_cmd->cmd_len)) {
		rc = -EFAULT;
		goto copy_cmd_failed;
	}

copy_cmd_failed:
	kfree(cfg_data);
cfg_data_failed:
	kfree(reg_cfg_cmd);
reg_cfg_failed:
	return rc;
}

int msm_isp_send_event(struct vfe_device *vfe_dev,
	uint32_t event_type,
	struct msm_isp_event_data *event_data)
{
	struct v4l2_event isp_event;
	memset(&isp_event, 0, sizeof(struct v4l2_event));
	isp_event.id = 0;
	isp_event.type = event_type;
	memcpy(&isp_event.u.data[0], event_data,
		sizeof(struct msm_isp_event_data));
	v4l2_event_queue(vfe_dev->subdev.sd.devnode, &isp_event);
	return 0;
}

#define CAL_WORD(width, M, N) ((width * M + N - 1) / N)

int msm_isp_cal_word_per_line(uint32_t output_format,
	uint32_t pixel_per_line)
{
	int val = -1;
	switch (output_format) {
	case V4L2_PIX_FMT_SBGGR8:
	case V4L2_PIX_FMT_SGBRG8:
	case V4L2_PIX_FMT_SGRBG8:
	case V4L2_PIX_FMT_SRGGB8:
	case V4L2_PIX_FMT_QBGGR8:
	case V4L2_PIX_FMT_QGBRG8:
	case V4L2_PIX_FMT_QGRBG8:
	case V4L2_PIX_FMT_QRGGB8:
	case V4L2_PIX_FMT_JPEG:
	case V4L2_PIX_FMT_META:
	case V4L2_PIX_FMT_GREY:
		val = CAL_WORD(pixel_per_line, 1, 8);
		break;
	case V4L2_PIX_FMT_SBGGR10:
	case V4L2_PIX_FMT_SGBRG10:
	case V4L2_PIX_FMT_SGRBG10:
	case V4L2_PIX_FMT_SRGGB10:
		val = CAL_WORD(pixel_per_line, 5, 32);
		break;
	case V4L2_PIX_FMT_SBGGR12:
	case V4L2_PIX_FMT_SGBRG12:
	case V4L2_PIX_FMT_SGRBG12:
	case V4L2_PIX_FMT_SRGGB12:
		val = CAL_WORD(pixel_per_line, 3, 16);
		break;
	case V4L2_PIX_FMT_SBGGR14:
	case V4L2_PIX_FMT_SGBRG14:
	case V4L2_PIX_FMT_SGRBG14:
	case V4L2_PIX_FMT_SRGGB14:
		val = CAL_WORD(pixel_per_line, 7, 32);
		break;
	case V4L2_PIX_FMT_QBGGR10:
	case V4L2_PIX_FMT_QGBRG10:
	case V4L2_PIX_FMT_QGRBG10:
	case V4L2_PIX_FMT_QRGGB10:
		val = CAL_WORD(pixel_per_line, 1, 6);
		break;
	case V4L2_PIX_FMT_QBGGR12:
	case V4L2_PIX_FMT_QGBRG12:
	case V4L2_PIX_FMT_QGRBG12:
	case V4L2_PIX_FMT_QRGGB12:
		val = CAL_WORD(pixel_per_line, 1, 5);
		break;
	case V4L2_PIX_FMT_QBGGR14:
	case V4L2_PIX_FMT_QGBRG14:
	case V4L2_PIX_FMT_QGRBG14:
	case V4L2_PIX_FMT_QRGGB14:
		val = CAL_WORD(pixel_per_line, 1, 4);
		break;
	case V4L2_PIX_FMT_NV12:
	case V4L2_PIX_FMT_NV21:
	case V4L2_PIX_FMT_NV14:
	case V4L2_PIX_FMT_NV41:
	case V4L2_PIX_FMT_NV16:
	case V4L2_PIX_FMT_NV61:
		val = CAL_WORD(pixel_per_line, 1, 8);
		break;
	case V4L2_PIX_FMT_YUYV:
	case V4L2_PIX_FMT_YVYU:
	case V4L2_PIX_FMT_UYVY:
	case V4L2_PIX_FMT_VYUY:
		val = CAL_WORD(pixel_per_line, 2, 8);
	break;
	case V4L2_PIX_FMT_P16BGGR10:
	case V4L2_PIX_FMT_P16GBRG10:
	case V4L2_PIX_FMT_P16GRBG10:
	case V4L2_PIX_FMT_P16RGGB10:
		val = CAL_WORD(pixel_per_line, 1, 4);
	break;
	case V4L2_PIX_FMT_NV24:
	case V4L2_PIX_FMT_NV42:
		val = CAL_WORD(pixel_per_line, 1, 8);
	break;
		/*TD: Add more image format*/
	default:
		msm_isp_print_fourcc_error(__func__, output_format);
		break;
	}
	return val;
}

enum msm_isp_pack_fmt msm_isp_get_pack_format(uint32_t output_format)
{
	switch (output_format) {
	case V4L2_PIX_FMT_SBGGR8:
	case V4L2_PIX_FMT_SGBRG8:
	case V4L2_PIX_FMT_SGRBG8:
	case V4L2_PIX_FMT_SRGGB8:
	case V4L2_PIX_FMT_SBGGR10:
	case V4L2_PIX_FMT_SGBRG10:
	case V4L2_PIX_FMT_SGRBG10:
	case V4L2_PIX_FMT_SRGGB10:
	case V4L2_PIX_FMT_SBGGR12:
	case V4L2_PIX_FMT_SGBRG12:
	case V4L2_PIX_FMT_SGRBG12:
	case V4L2_PIX_FMT_SRGGB12:
	case V4L2_PIX_FMT_SBGGR14:
	case V4L2_PIX_FMT_SGBRG14:
	case V4L2_PIX_FMT_SGRBG14:
	case V4L2_PIX_FMT_SRGGB14:
		return MIPI;
	case V4L2_PIX_FMT_QBGGR8:
	case V4L2_PIX_FMT_QGBRG8:
	case V4L2_PIX_FMT_QGRBG8:
	case V4L2_PIX_FMT_QRGGB8:
	case V4L2_PIX_FMT_QBGGR10:
	case V4L2_PIX_FMT_QGBRG10:
	case V4L2_PIX_FMT_QGRBG10:
	case V4L2_PIX_FMT_QRGGB10:
	case V4L2_PIX_FMT_QBGGR12:
	case V4L2_PIX_FMT_QGBRG12:
	case V4L2_PIX_FMT_QGRBG12:
	case V4L2_PIX_FMT_QRGGB12:
	case V4L2_PIX_FMT_QBGGR14:
	case V4L2_PIX_FMT_QGBRG14:
	case V4L2_PIX_FMT_QGRBG14:
	case V4L2_PIX_FMT_QRGGB14:
		return QCOM;
	case V4L2_PIX_FMT_P16BGGR10:
	case V4L2_PIX_FMT_P16GBRG10:
	case V4L2_PIX_FMT_P16GRBG10:
	case V4L2_PIX_FMT_P16RGGB10:
		return PLAIN16;
	default:
		msm_isp_print_fourcc_error(__func__, output_format);
		break;
	}
	return -EINVAL;
}

int msm_isp_get_bit_per_pixel(uint32_t output_format)
{
	switch (output_format) {
	case V4L2_PIX_FMT_Y4:
		return 4;
	case V4L2_PIX_FMT_Y6:
		return 6;
	case V4L2_PIX_FMT_SBGGR8:
	case V4L2_PIX_FMT_SGBRG8:
	case V4L2_PIX_FMT_SGRBG8:
	case V4L2_PIX_FMT_SRGGB8:
	case V4L2_PIX_FMT_QBGGR8:
	case V4L2_PIX_FMT_QGBRG8:
	case V4L2_PIX_FMT_QGRBG8:
	case V4L2_PIX_FMT_QRGGB8:
	case V4L2_PIX_FMT_JPEG:
	case V4L2_PIX_FMT_META:
	case V4L2_PIX_FMT_NV12:
	case V4L2_PIX_FMT_NV21:
	case V4L2_PIX_FMT_NV14:
	case V4L2_PIX_FMT_NV41:
	case V4L2_PIX_FMT_YVU410:
	case V4L2_PIX_FMT_YVU420:
	case V4L2_PIX_FMT_YUYV:
	case V4L2_PIX_FMT_YYUV:
	case V4L2_PIX_FMT_YVYU:
	case V4L2_PIX_FMT_UYVY:
	case V4L2_PIX_FMT_VYUY:
	case V4L2_PIX_FMT_YUV422P:
	case V4L2_PIX_FMT_YUV411P:
	case V4L2_PIX_FMT_Y41P:
	case V4L2_PIX_FMT_YUV444:
	case V4L2_PIX_FMT_YUV555:
	case V4L2_PIX_FMT_YUV565:
	case V4L2_PIX_FMT_YUV32:
	case V4L2_PIX_FMT_YUV410:
	case V4L2_PIX_FMT_YUV420:
	case V4L2_PIX_FMT_GREY:
	case V4L2_PIX_FMT_PAL8:
	case V4L2_PIX_FMT_UV8:
	case MSM_V4L2_PIX_FMT_META:
		return 8;
	case V4L2_PIX_FMT_SBGGR10:
	case V4L2_PIX_FMT_SGBRG10:
	case V4L2_PIX_FMT_SGRBG10:
	case V4L2_PIX_FMT_SRGGB10:
	case V4L2_PIX_FMT_QBGGR10:
	case V4L2_PIX_FMT_QGBRG10:
	case V4L2_PIX_FMT_QGRBG10:
	case V4L2_PIX_FMT_QRGGB10:
	case V4L2_PIX_FMT_Y10:
	case V4L2_PIX_FMT_Y10BPACK:
	case V4L2_PIX_FMT_P16BGGR10:
	case V4L2_PIX_FMT_P16GBRG10:
	case V4L2_PIX_FMT_P16GRBG10:
	case V4L2_PIX_FMT_P16RGGB10:
		return 10;
	case V4L2_PIX_FMT_SBGGR12:
	case V4L2_PIX_FMT_SGBRG12:
	case V4L2_PIX_FMT_SGRBG12:
	case V4L2_PIX_FMT_SRGGB12:
	case V4L2_PIX_FMT_QBGGR12:
	case V4L2_PIX_FMT_QGBRG12:
	case V4L2_PIX_FMT_QGRBG12:
	case V4L2_PIX_FMT_QRGGB12:
	case V4L2_PIX_FMT_Y12:
		return 12;
	case V4L2_PIX_FMT_SBGGR14:
	case V4L2_PIX_FMT_SGBRG14:
	case V4L2_PIX_FMT_SGRBG14:
	case V4L2_PIX_FMT_SRGGB14:
	case V4L2_PIX_FMT_QBGGR14:
	case V4L2_PIX_FMT_QGBRG14:
	case V4L2_PIX_FMT_QGRBG14:
	case V4L2_PIX_FMT_QRGGB14:
		return 14;
	case V4L2_PIX_FMT_NV16:
	case V4L2_PIX_FMT_NV61:
	case V4L2_PIX_FMT_Y16:
		return 16;
	case V4L2_PIX_FMT_NV24:
	case V4L2_PIX_FMT_NV42:
		return 24;
		/*TD: Add more image format*/
	default:
		msm_isp_print_fourcc_error(__func__, output_format);
		pr_err("%s: Invalid output format %x\n",
			__func__, output_format);
		return -EINVAL;
	}
}

void msm_isp_update_error_frame_count(struct vfe_device *vfe_dev)
{
	struct msm_vfe_error_info *error_info = &vfe_dev->error_info;
	error_info->info_dump_frame_count++;
	if (error_info->info_dump_frame_count == 0)
		error_info->info_dump_frame_count++;
}


static int msm_isp_process_iommu_page_fault(struct vfe_device *vfe_dev)
{
	int rc = vfe_dev->buf_mgr->pagefault_debug_disable;

	pr_err("%s:%d] VFE%d Handle Page fault! vfe_dev %p\n", __func__,
		__LINE__,  vfe_dev->pdev->id, vfe_dev);

	msm_isp_halt_send_error(vfe_dev, ISP_EVENT_IOMMU_P_FAULT);

	if (vfe_dev->buf_mgr->pagefault_debug_disable == 0) {
		vfe_dev->buf_mgr->pagefault_debug_disable = 1;
		vfe_dev->buf_mgr->ops->buf_mgr_debug(vfe_dev->buf_mgr,
			vfe_dev->page_fault_addr);
		msm_isp_print_ping_pong_address(vfe_dev,
			vfe_dev->page_fault_addr);
		vfe_dev->hw_info->vfe_ops.axi_ops.
			read_wm_ping_pong_addr(vfe_dev);
	}
	return rc;
}

void msm_isp_process_error_info(struct vfe_device *vfe_dev)
{
	struct msm_vfe_error_info *error_info = &vfe_dev->error_info;

	if (error_info->error_count == 1 ||
		!(error_info->info_dump_frame_count % 100)) {
		vfe_dev->hw_info->vfe_ops.core_ops.
			process_error_status(vfe_dev);
		error_info->error_mask0 = 0;
		error_info->error_mask1 = 0;
		error_info->camif_status = 0;
		error_info->violation_status = 0;
	}
}

static inline void msm_isp_update_error_info(struct vfe_device *vfe_dev,
	uint32_t error_mask0, uint32_t error_mask1)
{
	vfe_dev->error_info.error_mask0 |= error_mask0;
	vfe_dev->error_info.error_mask1 |= error_mask1;
	vfe_dev->error_info.error_count++;
}

static void msm_isp_process_overflow_irq(
	struct vfe_device *vfe_dev,
	uint32_t *irq_status0, uint32_t *irq_status1)
{
	uint32_t overflow_mask;

	/* if there are no active streams - do not start recovery */
	if (!vfe_dev->axi_data.num_active_stream)
		return;

	/*Mask out all other irqs if recovery is started*/
	if (atomic_read(&vfe_dev->error_info.overflow_state) != NO_OVERFLOW) {
		uint32_t halt_restart_mask0, halt_restart_mask1;
		vfe_dev->hw_info->vfe_ops.core_ops.
		get_halt_restart_mask(&halt_restart_mask0,
			&halt_restart_mask1);
		*irq_status0 &= halt_restart_mask0;
		*irq_status1 &= halt_restart_mask1;

		return;
	}

	/*Check if any overflow bit is set*/
	vfe_dev->hw_info->vfe_ops.core_ops.
		get_overflow_mask(&overflow_mask);
	overflow_mask &= *irq_status1;

	if (overflow_mask) {
		struct msm_isp_event_data error_event;
		struct msm_vfe_axi_halt_cmd halt_cmd;

		if (vfe_dev->reset_pending == 1) {
			pr_err("%s:%d failed: overflow %x during reset\n",
				__func__, __LINE__, overflow_mask);
			/* Clear overflow bits since reset is pending */
			*irq_status1 &= ~overflow_mask;
			return;
		}

		halt_cmd.overflow_detected = 1;
		halt_cmd.stop_camif = 1;
		halt_cmd.blocking_halt = 0;

		msm_isp_axi_halt(vfe_dev, &halt_cmd);

		/*Update overflow state*/
		*irq_status0 = 0;
		*irq_status1 = 0;

		if (atomic_read(&vfe_dev->error_info.overflow_state)
			!= HALT_ENFORCED) {
			error_event.frame_id =
				vfe_dev->axi_data.src_info[VFE_PIX_0].frame_id;
			error_event.u.error_info.err_type =
				ISP_ERROR_BUS_OVERFLOW;
			msm_isp_send_event(vfe_dev,
				ISP_EVENT_ERROR, &error_event);
		}
	}
}

void msm_isp_reset_burst_count_and_frame_drop(
	struct vfe_device *vfe_dev, struct msm_vfe_axi_stream *stream_info)
{
	if (stream_info->state != ACTIVE ||
		stream_info->stream_type != BURST_STREAM) {
		return;
	}
	if (stream_info->stream_type == BURST_STREAM &&
		stream_info->num_burst_capture != 0) {
		msm_isp_reset_framedrop(vfe_dev, stream_info);
	}
}

static void msm_isp_enqueue_tasklet_cmd(struct vfe_device *vfe_dev,
	uint32_t irq_status0, uint32_t irq_status1)
{
	unsigned long flags;
	struct msm_vfe_tasklet_queue_cmd *queue_cmd = NULL;

	spin_lock_irqsave(&vfe_dev->tasklet_lock, flags);
	queue_cmd = &vfe_dev->tasklet_queue_cmd[vfe_dev->taskletq_idx];
	if (queue_cmd->cmd_used) {
		ISP_DBG("%s: Tasklet queue overflow: %d\n",
			__func__, vfe_dev->pdev->id);
		list_del(&queue_cmd->list);
	} else {
		atomic_add(1, &vfe_dev->irq_cnt);
	}
	queue_cmd->vfeInterruptStatus0 = irq_status0;
	queue_cmd->vfeInterruptStatus1 = irq_status1;
	msm_isp_get_timestamp(&queue_cmd->ts);
	queue_cmd->cmd_used = 1;
	vfe_dev->taskletq_idx = (vfe_dev->taskletq_idx + 1) %
		MSM_VFE_TASKLETQ_SIZE;
	list_add_tail(&queue_cmd->list, &vfe_dev->tasklet_q);
	spin_unlock_irqrestore(&vfe_dev->tasklet_lock, flags);
	tasklet_schedule(&vfe_dev->vfe_tasklet);
}

irqreturn_t msm_isp_process_irq(int irq_num, void *data)
{
	struct vfe_device *vfe_dev = (struct vfe_device *) data;
	uint32_t irq_status0, irq_status1;
	uint32_t error_mask0, error_mask1;

	vfe_dev->hw_info->vfe_ops.irq_ops.
		read_irq_status(vfe_dev, &irq_status0, &irq_status1);

	if ((irq_status0 == 0) && (irq_status1 == 0)) {
		ISP_DBG("%s:VFE%d irq_status0 & 1 are both 0\n",
			__func__, vfe_dev->pdev->id);
		return IRQ_HANDLED;
	}

	msm_isp_process_overflow_irq(vfe_dev,
		&irq_status0, &irq_status1);

	vfe_dev->hw_info->vfe_ops.core_ops.
		get_error_mask(&error_mask0, &error_mask1);
	error_mask0 &= irq_status0;
	error_mask1 &= irq_status1;
	irq_status0 &= ~error_mask0;
	irq_status1 &= ~error_mask1;
	if (!vfe_dev->ignore_error &&
		((error_mask0 != 0) || (error_mask1 != 0)))
		msm_isp_update_error_info(vfe_dev, error_mask0, error_mask1);

	if ((irq_status0 == 0) && (irq_status1 == 0) &&
		(!(((error_mask0 != 0) || (error_mask1 != 0)) &&
		 vfe_dev->error_info.error_count == 1))) {
		ISP_DBG("%s: error_mask0/1 & error_count are set!\n", __func__);
		return IRQ_HANDLED;
	}

	msm_isp_enqueue_tasklet_cmd(vfe_dev, irq_status0, irq_status1);

	return IRQ_HANDLED;
}

void msm_isp_do_tasklet(unsigned long data)
{
	unsigned long flags;
	struct vfe_device *vfe_dev = (struct vfe_device *) data;
	struct msm_vfe_irq_ops *irq_ops = &vfe_dev->hw_info->vfe_ops.irq_ops;
	struct msm_vfe_tasklet_queue_cmd *queue_cmd;
	struct msm_isp_timestamp ts;
	uint32_t irq_status0, irq_status1;

	if (vfe_dev->vfe_base == NULL || vfe_dev->vfe_open_cnt == 0) {
		ISP_DBG("%s: VFE%d open cnt = %d, device closed(base = %p)\n",
			__func__, vfe_dev->pdev->id, vfe_dev->vfe_open_cnt,
			vfe_dev->vfe_base);
		return;
	}

	while (atomic_read(&vfe_dev->irq_cnt)) {
		spin_lock_irqsave(&vfe_dev->tasklet_lock, flags);
		queue_cmd = list_first_entry(&vfe_dev->tasklet_q,
		struct msm_vfe_tasklet_queue_cmd, list);
		if (!queue_cmd) {
			atomic_set(&vfe_dev->irq_cnt, 0);
			spin_unlock_irqrestore(&vfe_dev->tasklet_lock, flags);
			return;
		}
		atomic_sub(1, &vfe_dev->irq_cnt);
		list_del(&queue_cmd->list);
		queue_cmd->cmd_used = 0;
		irq_status0 = queue_cmd->vfeInterruptStatus0;
		irq_status1 = queue_cmd->vfeInterruptStatus1;
		ts = queue_cmd->ts;
		spin_unlock_irqrestore(&vfe_dev->tasklet_lock, flags);
		ISP_DBG("%s: vfe_id %d status0: 0x%x status1: 0x%x\n",
			__func__, vfe_dev->pdev->id, irq_status0, irq_status1);
		irq_ops->process_reset_irq(vfe_dev,
			irq_status0, irq_status1);
		irq_ops->process_halt_irq(vfe_dev,
			irq_status0, irq_status1);
		if (atomic_read(&vfe_dev->error_info.overflow_state)
			!= NO_OVERFLOW) {
			ISP_DBG("%s: Recovery in processing, Ignore IRQs!!!\n",
				__func__);
			continue;
		}
		msm_isp_process_error_info(vfe_dev);
		irq_ops->process_stats_irq(vfe_dev,
			irq_status0, irq_status1, &ts);
		irq_ops->process_axi_irq(vfe_dev,
			irq_status0, irq_status1, &ts);
		irq_ops->process_camif_irq(vfe_dev,
			irq_status0, irq_status1, &ts);
		irq_ops->process_reg_update(vfe_dev,
			irq_status0, irq_status1, &ts);
		irq_ops->process_epoch_irq(vfe_dev,
			irq_status0, irq_status1, &ts);
	}
}

int msm_isp_set_src_state(struct vfe_device *vfe_dev, void *arg)
{
	struct msm_vfe_axi_src_state *src_state = arg;
	if (src_state->input_src >= VFE_SRC_MAX)
		return -EINVAL;
	vfe_dev->axi_data.src_info[src_state->input_src].active =
	src_state->src_active;
	vfe_dev->axi_data.src_info[src_state->input_src].frame_id =
	src_state->src_frame_id;
	return 0;
}

static void msm_vfe_iommu_fault_handler(struct iommu_domain *domain,
	struct device *dev, unsigned long iova, int flags, void *token)
{
	struct vfe_device *vfe_dev = NULL;

	if (token) {
		vfe_dev = (struct vfe_device *)token;
		vfe_dev->page_fault_addr = iova;
		if (!vfe_dev->buf_mgr || !vfe_dev->buf_mgr->ops ||
			!vfe_dev->axi_data.num_active_stream) {
			pr_err("%s:%d buf_mgr %p active strms %d\n", __func__,
				__LINE__, vfe_dev->buf_mgr,
				vfe_dev->axi_data.num_active_stream);
			goto end;
		}

		mutex_lock(&vfe_dev->core_mutex);
		if (vfe_dev->vfe_open_cnt > 0) {
			atomic_set(&vfe_dev->error_info.overflow_state,
				HALT_ENFORCED);
			msm_isp_process_iommu_page_fault(vfe_dev);
		} else {
			pr_err("%s: no handling, vfe open cnt = %d\n",
				__func__, vfe_dev->vfe_open_cnt);
		}
		mutex_unlock(&vfe_dev->core_mutex);
	} else {
		ISP_DBG("%s:%d] no token received: %p\n",
			__func__, __LINE__, token);
		goto end;
	}
end:
	return;
}

int msm_isp_open_node(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	struct vfe_device *vfe_dev = v4l2_get_subdevdata(sd);
	long rc = 0;

	ISP_DBG("%s open_cnt %u\n", __func__, vfe_dev->vfe_open_cnt);

	if (vfe_dev->common_data == NULL) {
		pr_err("%s: Error in probe. No common_data\n", __func__);
		return -EINVAL;
	}

	mutex_lock(&vfe_dev->realtime_mutex);
	mutex_lock(&vfe_dev->core_mutex);

	if (vfe_dev->vfe_open_cnt++ && vfe_dev->vfe_base) {
		mutex_unlock(&vfe_dev->core_mutex);
		mutex_unlock(&vfe_dev->realtime_mutex);
		return 0;
	}

	if (vfe_dev->vfe_base) {
		pr_err("%s:%d invalid params cnt %d base %p\n", __func__,
			__LINE__, vfe_dev->vfe_open_cnt, vfe_dev->vfe_base);
		vfe_dev->vfe_base = NULL;
	}

	vfe_dev->reset_pending = 0;
	vfe_dev->isp_sof_debug = 0;
	vfe_dev->isp_raw0_debug = 0;
	vfe_dev->isp_raw1_debug = 0;
	vfe_dev->isp_raw2_debug = 0;

	if (vfe_dev->hw_info->vfe_ops.core_ops.init_hw(vfe_dev) < 0) {
		pr_err("%s: init hardware failed\n", __func__);
		vfe_dev->vfe_open_cnt--;
		mutex_unlock(&vfe_dev->core_mutex);
		mutex_unlock(&vfe_dev->realtime_mutex);
		return -EBUSY;
	}

	memset(&vfe_dev->error_info, 0, sizeof(vfe_dev->error_info));
	atomic_set(&vfe_dev->error_info.overflow_state, NO_OVERFLOW);

	vfe_dev->hw_info->vfe_ops.core_ops.clear_status_reg(vfe_dev);

	rc = vfe_dev->hw_info->vfe_ops.core_ops.reset_hw(vfe_dev, 1, 1);
	if (rc <= 0) {
		pr_err("%s: reset timeout\n", __func__);
		vfe_dev->hw_info->vfe_ops.core_ops.release_hw(vfe_dev);
		vfe_dev->vfe_open_cnt--;
		mutex_unlock(&vfe_dev->core_mutex);
		mutex_unlock(&vfe_dev->realtime_mutex);
		return -EINVAL;
	}
	vfe_dev->vfe_hw_version = msm_camera_io_r(vfe_dev->vfe_base);
	ISP_DBG("%s: HW Version: 0x%x\n", __func__, vfe_dev->vfe_hw_version);

	vfe_dev->hw_info->vfe_ops.core_ops.init_hw_reg(vfe_dev);

	vfe_dev->buf_mgr->ops->buf_mgr_init(vfe_dev->buf_mgr,
		"msm_isp");

	memset(&vfe_dev->axi_data, 0, sizeof(struct msm_vfe_axi_shared_data));
	memset(&vfe_dev->stats_data, 0,
		sizeof(struct msm_vfe_stats_shared_data));
	memset(&vfe_dev->error_info, 0, sizeof(vfe_dev->error_info));
	memset(&vfe_dev->fetch_engine_info, 0,
		sizeof(vfe_dev->fetch_engine_info));
	vfe_dev->axi_data.hw_info = vfe_dev->hw_info->axi_hw_info;
	vfe_dev->axi_data.enable_frameid_recovery = 0;
	vfe_dev->taskletq_idx = 0;
	vfe_dev->vt_enable = 0;
	vfe_dev->reg_update_requested = 0;
	/* Register page fault handler */
	vfe_dev->buf_mgr->pagefault_debug_disable = 0;
	cam_smmu_reg_client_page_fault_handler(
			vfe_dev->buf_mgr->iommu_hdl,
			msm_vfe_iommu_fault_handler, vfe_dev);
	mutex_unlock(&vfe_dev->core_mutex);
	mutex_unlock(&vfe_dev->realtime_mutex);
	return 0;
}

#ifdef CONFIG_MSM_AVTIMER
void msm_isp_end_avtimer(void)
{
	avcs_core_disable_power_collapse(0);
}
#else
void msm_isp_end_avtimer(void)
{
	pr_err("AV Timer is not supported\n");
}
#endif

int msm_isp_close_node(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	long rc = 0;
	int wm;
	struct vfe_device *vfe_dev = v4l2_get_subdevdata(sd);
	ISP_DBG("%s E open_cnt %u\n", __func__, vfe_dev->vfe_open_cnt);
	mutex_lock(&vfe_dev->realtime_mutex);
	mutex_lock(&vfe_dev->core_mutex);

	if (!vfe_dev->vfe_open_cnt) {
		pr_err("%s invalid state open cnt %d\n", __func__,
			vfe_dev->vfe_open_cnt);
		mutex_unlock(&vfe_dev->core_mutex);
		mutex_unlock(&vfe_dev->realtime_mutex);
		return -EINVAL;
	}

	if (vfe_dev->vfe_open_cnt > 1) {
		vfe_dev->vfe_open_cnt--;
		mutex_unlock(&vfe_dev->core_mutex);
		mutex_unlock(&vfe_dev->realtime_mutex);
		return 0;
	}
	/* Unregister page fault handler */
	cam_smmu_reg_client_page_fault_handler(
		vfe_dev->buf_mgr->iommu_hdl,
		NULL, vfe_dev);

	rc = vfe_dev->hw_info->vfe_ops.axi_ops.halt(vfe_dev, 1);
	if (rc <= 0)
		pr_err("%s: halt timeout rc=%ld\n", __func__, rc);

	vfe_dev->hw_info->vfe_ops.core_ops.
		update_camif_state(vfe_dev, DISABLE_CAMIF_IMMEDIATELY);
	vfe_dev->hw_info->vfe_ops.core_ops.reset_hw(vfe_dev, 0, 0);

	/* after regular hw stop, reduce open cnt */
	vfe_dev->vfe_open_cnt--;

	/* put scratch buf in all the wm */
	for (wm = 0; wm < vfe_dev->axi_data.hw_info->num_wm; wm++) {
		msm_isp_cfg_wm_scratch(vfe_dev, wm, VFE_PING_FLAG);
		msm_isp_cfg_wm_scratch(vfe_dev, wm, VFE_PONG_FLAG);
	}
	vfe_dev->hw_info->vfe_ops.core_ops.release_hw(vfe_dev);
	vfe_dev->buf_mgr->ops->buf_mgr_deinit(vfe_dev->buf_mgr);
	if (vfe_dev->vt_enable) {
		msm_isp_end_avtimer();
		vfe_dev->vt_enable = 0;
	}
	vfe_dev->is_split = 0;

	mutex_unlock(&vfe_dev->core_mutex);
	mutex_unlock(&vfe_dev->realtime_mutex);
	return 0;
}

void msm_isp_flush_tasklet(struct vfe_device *vfe_dev)
{
	unsigned long flags;
	struct msm_vfe_tasklet_queue_cmd *queue_cmd;

	spin_lock_irqsave(&vfe_dev->tasklet_lock, flags);
	while (atomic_read(&vfe_dev->irq_cnt)) {
		queue_cmd = list_first_entry(&vfe_dev->tasklet_q,
		struct msm_vfe_tasklet_queue_cmd, list);
		if (!queue_cmd) {
			atomic_set(&vfe_dev->irq_cnt, 0);
			break;
		}
		atomic_sub(1, &vfe_dev->irq_cnt);
		list_del(&queue_cmd->list);
		queue_cmd->cmd_used = 0;
	}
	spin_unlock_irqrestore(&vfe_dev->tasklet_lock, flags);

	return;
}

void msm_isp_save_framedrop_values(struct vfe_device *vfe_dev,
				enum msm_vfe_input_src frame_src)
{
	struct msm_vfe_axi_stream *stream_info = NULL;
	uint32_t j = 0;
	unsigned long flags;

	for (j = 0; j < VFE_AXI_SRC_MAX; j++) {
		stream_info = &vfe_dev->axi_data.stream_info[j];
		if (stream_info->state != ACTIVE)
			continue;
		if (frame_src != SRC_TO_INTF(stream_info->stream_src))
			continue;

		stream_info =
			&vfe_dev->axi_data.stream_info[j];
		spin_lock_irqsave(&stream_info->lock, flags);
		stream_info->prev_framedrop_period &= ~0x80000000;
		spin_unlock_irqrestore(&stream_info->lock, flags);
	}
}
