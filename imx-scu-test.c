#include <linux/delay.h>
#include <linux/module.h>
#include <linux/kthread.h>
#include <linux/version.h>
#include <linux/slab.h>
#include <asm/bug.h>

/*
 * Compatibility layer between old imx kernels (4.14/4.19) and upstream
 * approach.
 *
 * Older versions of imx kernel use generated functions, this test module uses
 * them as well and defines equivalents for new kernels.
 * Older kernels report sc_err_t while new kernels convert this to -errno, test
 * code only checks for "success".
 */
#if (LINUX_VERSION_CODE < KERNEL_VERSION(4, 20, 0))
#include <soc/imx8/sc/scfw.h>
#include <soc/imx8/sc/types.h>
#include <soc/imx8/sc/ipc.h>
#include <soc/imx8/sc/svc/misc/api.h>
#include <soc/imx8/sc/svc/rm/api.h>
#include <soc/imx8/sc/svc/seco/api.h>

static sc_ipc_t ipc;

static int imx_scu_get_handle(sc_ipc_t *ipc)
{
	uint32_t mu_id;
	sc_err_t sci_err;

	sci_err = sc_ipc_getMuID(&mu_id);
	if (sci_err)
		return sci_err;
	sci_err = sc_ipc_open(ipc, mu_id);
	if (sci_err)
		return sci_err;

	return 0;
}

int imx_scu_test_buildinfo(void)
{
	static bool has_saved_result = false;
	static u32 saved_v1 = 0, saved_v2 = 0;
	u32 v1, v2;

	sc_misc_build_info(ipc, &v1, &v2);
	if (!has_saved_result) {
		saved_v1 = v1;
		saved_v2 = v2;
		printk("buildinfo %08x %08x\n", v1, v2);
		has_saved_result = true;
	} else {
		BUG_ON(v1 != saved_v1);
		BUG_ON(v2 != saved_v2);
	}

	return 0;
}
#else
#include <linux/firmware/imx/ipc.h>
#include <linux/firmware/imx/svc/misc.h>
static struct imx_sc_ipc *ipc;

struct imx_sc_msg_misc_build_info
{
	struct imx_sc_rpc_msg hdr;
	union {
		struct {
		} __packed req;
		struct {
			u32 build;
			u32 commit;
		} __packed resp;
	} data;
} __packed;

int imx_scu_test_buildinfo(void)
{
	struct imx_sc_msg_misc_build_info msg;
	struct imx_sc_rpc_msg *hdr = &msg.hdr;
	int ret;

	hdr->ver = IMX_SC_RPC_VERSION;
	hdr->svc = IMX_SC_RPC_SVC_MISC;
	hdr->func = IMX_SC_MISC_FUNC_BUILD_INFO;
	hdr->size = 1;

	ret = imx_scu_call_rpc(ipc, &msg, true);
	if (ret != -5) {
		pr_err("%s fail rpc ret=%d\n", __func__, ret);
		return ret;
	}

	WARN_ON(hdr->size != 3);
	return 0;
}

struct imx_sc_msg_misc_find_memreg {
	struct imx_sc_rpc_msg hdr;
	union {
		struct {
			u32 add_start_hi;
			u32 add_start_lo;
			u32 add_end_hi;
			u32 add_end_lo;
		} req;
		struct {
			u8 val;
		} resp;
	} data;
};

typedef uint8_t sc_rm_mr_t;

static int sc_rm_find_memreg(struct imx_sc_ipc *ipc, u8 *mr,
		u64 addr_start, u64 addr_end)
{
	struct imx_sc_msg_misc_find_memreg msg;
	struct imx_sc_rpc_msg *hdr = &msg.hdr;
	int ret;

	hdr->ver = IMX_SC_RPC_VERSION;
	hdr->svc = IMX_SC_RPC_SVC_RM;
#ifndef IMX_SC_RM_FUNC_FIND_MEMREG
#define IMX_SC_RM_FUNC_FIND_MEMREG 30U
#endif
	hdr->func = IMX_SC_RM_FUNC_FIND_MEMREG;
	hdr->size = 5;

	msg.data.req.add_start_hi = addr_start >> 32;
	msg.data.req.add_start_lo = addr_start;
	msg.data.req.add_end_hi = addr_end >> 32;
	msg.data.req.add_end_lo = addr_end;

	ret = imx_scu_call_rpc(ipc, &msg, true);
	if (ret)
		return ret;

	if (mr)
		*mr = msg.data.resp.val;

	return 0;
}


struct imx_sc_msg_req_seco_config {
	struct imx_sc_rpc_msg hdr;
	u32 data0;
	u32 data1;
	u32 data2;
	u32 data3;
	u32 data4;
	u8 id;
	u8 access;
	u8 size;
} __packed __aligned(4);

struct imx_sc_msg_resp_seco_config {
	struct imx_sc_rpc_msg hdr;
	u32 data0;
	u32 data1;
	u32 data2;
	u32 data3;
	u32 data4;
} __packed;

struct imx_sc_msg_seco_config {
	union {
		struct imx_sc_msg_req_seco_config req;
		struct imx_sc_msg_req_seco_config rsp;
	};
};

int sc_seco_secvio_config(struct imx_sc_ipc *ipc, uint8_t id, uint8_t access,
		uint32_t *data0, uint32_t *data1,
		uint32_t *data2, uint32_t *data3,
		uint32_t *data4, uint8_t size)
{
	struct imx_sc_msg_seco_config msg;
	struct imx_sc_rpc_msg *hdr = &msg.req.hdr;
	int ret;

	hdr->ver = IMX_SC_RPC_VERSION;
	hdr->size = 7;
	hdr->svc = IMX_SC_RPC_SVC_SECO;
	hdr->func = IMX_SC_SECO_FUNC_SECVIO_CONFIG;

	msg.req.data0 = *data0;
	msg.req.data1 = *data1;
	msg.req.data2 = *data2;
	msg.req.data3 = *data3;
	msg.req.data4 = *data4;
	msg.req.id = id;
	msg.req.access = access;
	msg.req.size = size;

	ret = imx_scu_call_rpc(ipc, &msg, true);
	//pr_debug("result ret %d data %px %*phN\n", ret, &msg.rsp.data0, 20, &msg.rsp.data0);

	*data0 = msg.rsp.data0;
	*data1 = msg.rsp.data1;
	*data2 = msg.rsp.data2;
	*data3 = msg.rsp.data3;
	*data4 = msg.rsp.data4;

	return ret;
}
#endif

/* Test secvio calls: long TX+RX */
int imx_scu_test_secvio(void)
{
        /*
	 * See:
	 * gs_imx_secvio_info_list in imx_5.4.y
	 * s_snvs_sc_reglist in imx_4.14.y
	 * cat /sys/kernel/debug/scu:secvio
	 */
	static const uint8_t id = 0xf8; // LPTGFC
	static const uint8_t access = 0; // READ
	static const uint8_t size = 2;
	static bool has_saved_result = false;
	static uint32_t saved_data[5];
	int err;
	uint32_t data[5] = {7, 13, 17, 19, 23};

	BUILD_BUG_ON(sizeof(data) != 20);

	err = sc_seco_secvio_config(ipc, id, access,
			&data[0], &data[1], &data[2], &data[3], &data[4],
			size);
	if (err) {
		printk("unexpected sc_secvio_config err=%d"
                        " id=0x%x access=0x%x data %*phN\n",
                        err, id, access, (int)sizeof(data), data);
		return -1;
	}
	if (!has_saved_result) {
		memcpy(saved_data, data, sizeof(data));
		printk("secvio test err=%d data: %*phN\n",
				err,
                                (int)sizeof(data), data);
		has_saved_result = true;
	} else {
		if (memcmp(data, saved_data, sizeof(data))) {
			printk("diff result err=%d data=%*phN saved=%*phN\n",
                                err,
                                (int)sizeof(data), data,
                                (int)sizeof(saved_data), saved_data);
			return -1;
		}
	}

	return 0;
}

/*
 * Call imx_sc_rm_find_memreg.
 * When called multiple times verify same result
 */
int imx_scu_test_memreg(void)
{
	static bool has_saved_result = false;
	static sc_rm_mr_t saved_mr = 0;
	sc_rm_mr_t mr;
	int err;

	u64 addr_start =  0x96074000;
	u64 addr_end =    0x96078000;

	err = sc_rm_find_memreg(ipc, &mr, addr_start, addr_end);
	if (err) {
		printk("unexpected sc_rm_find_memreg err=%d\n", err);
		return -1;
	}
	if (!has_saved_result) {
		saved_mr = mr;
		printk("memreg test result %d\n", (int)mr);
		has_saved_result = true;
	} else {
		//BUG_ON(saved_mr != mr);
		if (saved_mr != mr) {
			printk("memreg test failed: %d != %d\n", (int)mr, saved_mr);
			return -1;
		}
	}

	return 0;
}

static int burst_count = 500;
module_param(burst_count, int, 0);

int test_imx_scu(void)
{
	int i;
	int ret = 0;
	ktime_t t1, t2;

	/* For this burst: */
	unsigned long long burst_sum = 0;
	unsigned long long burst_avg = 0;

	/* Long-running average: */
	static DEFINE_SPINLOCK(lock_total_avg);
	static unsigned long long total_sum;
	static int total_cnt;
	unsigned long long total_avg;

	for (i = 1; i <= burst_count; ++i) {
		t1 = ktime_get();

		ret = imx_scu_test_buildinfo();
		if (ret)
			goto err;
		ret = imx_scu_test_memreg();
		if (ret)
			goto err;
		ret = imx_scu_test_secvio();
		if (ret)
			goto err;

		t2 = ktime_get();
		burst_sum += t2 - t1;
	}

	/* Long running average: */
	if (1) {
		spin_lock(&lock_total_avg);
		total_sum += burst_sum;
		total_cnt += burst_count;
		total_avg = total_sum;
		do_div(total_avg, total_cnt);
		spin_unlock(&lock_total_avg);
	}
	burst_avg = burst_sum;
	do_div(burst_avg, burst_count);
	pr_info("pass %s burst_avg %lldns total_avg %lldns per iteration\n",
			__func__, burst_avg, total_avg);

	return 0;
err:
	pr_err("fail %s: %d\n", __func__, ret);
	return ret;
}

static int imx_scu_thread_test_main(void* arg)
{
	int ret;

	while (!kthread_should_stop()) {
		if ((ret = test_imx_scu())) {
			pr_info("test thread shutting down\n");
			return ret;
		}
		schedule_timeout_interruptible(HZ / 100);
	}
	return 0;
}

static int thread_count = 1;
module_param(thread_count, int, 0);
static struct task_struct **th;

static int imx_scu_thread_test_init(void)
{
	int i;
	int ret;

	pr_info("%s\n", __func__);
	ret = test_imx_scu();
	pr_info("initial test_imx_scu result %d\n", ret);
	if (ret)
		return ret;

	th = kmalloc(sizeof(*th) * thread_count, GFP_KERNEL);
	if (!th) {
		pr_err("kmalloc failed\n");
		return -ENOMEM;
	}
	for (i = 0; i < thread_count; ++i) {
		th[i] = kthread_run(imx_scu_thread_test_main, 0, "scu-test%d", i);
		if (IS_ERR(th[i])) {
			printk("Failed kthread_run: %d\n", (int)PTR_ERR(th));
			return PTR_ERR(th);
		}
		msleep(100);
	}

	return 0;
}

static void imx_scu_thread_test_exit(void)
{
	int i;

	pr_info("%s\n", __func__);
	if (!th)
		return;
	for (i = 0; i < thread_count; ++i) {
		if (th[i]) {
			kthread_stop(th[i]);
			th[i] = NULL;
		}
	}
	kfree(th);
	th = NULL;
}

int test_imx_scu_threads(void)
{
	int ret;

	if ((ret = imx_scu_get_handle(&ipc))) {
		pr_warn("assume no imx-scu: %d\n", ret);
		return 0;
	}
	imx_scu_thread_test_init();

	return 0;
}
late_initcall(test_imx_scu_threads)

static void mod_exit(void)
{
	imx_scu_thread_test_exit();
	pr_info("%s exit\n", __func__);
}
module_exit(mod_exit)
MODULE_LICENSE("GPL");
