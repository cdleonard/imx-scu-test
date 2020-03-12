#include <linux/delay.h>
#include <linux/module.h>
#include <linux/kthread.h>
#include <linux/version.h>
#include <asm/bug.h>

#if (LINUX_VERSION_CODE <= KERNEL_VERSION(4, 19, 0))
#include <soc/imx8/sc/scfw.h>
#include <soc/imx8/sc/types.h>
#include <soc/imx8/sc/ipc.h>
#include <soc/imx8/sc/svc/misc/api.h>
#include <soc/imx8/sc/svc/rm/api.h>

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
#endif

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

#define TEST_THREAD_COUNT 1
static struct task_struct *th[TEST_THREAD_COUNT];

static int imx_scu_thread_test_init(void)
{
	int i;

	pr_info("%s\n", __func__);
	test_imx_scu();

	for (i = 0; i < TEST_THREAD_COUNT; ++i) {
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
	for (i = 0; i < TEST_THREAD_COUNT; ++i) {
		if (th[i]) {
			kthread_stop(th[i]);
			th[i] = NULL;
		}
	}
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
