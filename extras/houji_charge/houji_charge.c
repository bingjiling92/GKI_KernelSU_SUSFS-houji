// SPDX-License-Identifier: GPL-2.0
/*
 * houji_charge - Xiaomi 14 (houji) kernel-side charge boost driver
 *
 * 双模式中的内核模式：开机自动解除厂商充电限制，日常维持 40-50W 充电功率，
 * 并内置温度闭环保护与 ChargingSus 自恢复。
 *
 * 行为：
 *  1. 开机延迟初始化：写 qcom-battery 关键节点（restrict_cur=0 解除 1A
 *     限制、smart_chg/night_charging 关闭、constant_power=目标功率）。
 *  2. 周期守护（15s）：
 *     - 电池温度 >= 45.0C：降档 constant_power=30W 并恢复 thermal_remove=0；
 *     - 电池温度 <= 40.0C：恢复目标功率并 thermal_remove=1；
 *     - status=Charging 但 ibat 为正（实际放电/暂停）连续 2 个周期：
 *       input_suspend 1->0 脉冲，强制充电链路重新枚举。
 *  3. /proc/houji_charge：读状态、写 enable / target_power 运行期调档。
 *
 * 目标功率单位与 vendor constant_power 节点一致（cW，4500 = 45.0W）。
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/delayed_work.h>
#include <linux/fs.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/string.h>
#include <linux/slab.h>

#define QCBAT	"/sys/class/qcom-battery/"
#define PSYBAT	"/sys/class/power_supply/battery/"

#define DEF_TARGET_POWER	4500	/* 45W：40-50W 日常档中值 */
#define HOT_TEMP		450	/* 45.0C，单位 0.1C */
#define COOL_TEMP		400	/* 40.0C */
#define HOT_DROP_POWER		3000	/* 高温降档 30W */

static int target_power = DEF_TARGET_POWER;
static bool boost_enable = true;
module_param(target_power, int, 0644);
module_param(boost_enable, bool, 0644);

static struct delayed_work boost_work;
static int sus_streak;

static int bat_write(const char *rel, const char *val)
{
	struct file *f;
	loff_t pos = 0;
	ssize_t n;
	char path[128];

	snprintf(path, sizeof(path), "%s%s", QCBAT, rel);
	f = filp_open(path, O_WRONLY, 0);
	if (IS_ERR(f))
		return PTR_ERR(f);
	n = kernel_write(f, val, strlen(val), &pos);
	filp_close(f, NULL);
	return n < 0 ? (int)n : 0;
}

static int psy_write(const char *rel, const char *val)
{
	struct file *f;
	loff_t pos = 0;
	ssize_t n;
	char path[128];

	snprintf(path, sizeof(path), "%s%s", PSYBAT, rel);
	f = filp_open(path, O_WRONLY, 0);
	if (IS_ERR(f))
		return PTR_ERR(f);
	n = kernel_write(f, val, strlen(val), &pos);
	filp_close(f, NULL);
	return n < 0 ? (int)n : 0;
}

static int bat_read(const char *rel, char *buf, size_t sz)
{
	struct file *f;
	loff_t pos = 0;
	ssize_t n;
	char path[128];

	snprintf(path, sizeof(path), "%s%s", QCBAT, rel);
	f = filp_open(path, O_RDONLY, 0);
	if (IS_ERR(f))
		return PTR_ERR(f);
	n = kernel_read(f, buf, sz - 1, &pos);
	filp_close(f, NULL);
	if (n < 0)
		return (int)n;
	buf[n] = '\0';
	return (int)n;
}

static int psy_read(const char *rel, char *buf, size_t sz)
{
	struct file *f;
	loff_t pos = 0;
	ssize_t n;
	char path[128];

	snprintf(path, sizeof(path), "%s%s", PSYBAT, rel);
	f = filp_open(path, O_RDONLY, 0);
	if (IS_ERR(f))
		return PTR_ERR(f);
	n = kernel_read(f, buf, sz - 1, &pos);
	filp_close(f, NULL);
	if (n < 0)
		return (int)n;
	buf[n] = '\0';
	return (int)n;
}

static void apply_target(void)
{
	char pwr[16];

	snprintf(pwr, sizeof(pwr), "%d", target_power);
	bat_write("restrict_cur", "0");
	bat_write("smart_chg", "0");
	bat_write("night_charging", "0");
	bat_write("handle_stop_charging", "0");
	bat_write("charge_control_en", "0");
	bat_write("constant_power", pwr);
	bat_write("thermal_remove", "1");
}

static void boost_do_work(struct work_struct *work)
{
	char buf[64];
	int temp = 0;
	long ibat = 0;
	bool charging = false;

	if (!boost_enable)
		goto resched;

	/* 温度闭环：>=45C 降档并交还热控，<=40C 恢复目标档 */
	if (psy_read("temp", buf, sizeof(buf)) > 0)
		temp = simple_strtol(buf, NULL, 10);

	if (temp >= HOT_TEMP) {
		bat_write("constant_power", "3000");
		bat_write("thermal_remove", "0");
	} else if (temp <= COOL_TEMP) {
		char pwr[16];

		snprintf(pwr, sizeof(pwr), "%d", target_power);
		bat_write("constant_power", pwr);
		bat_write("thermal_remove", "1");
	}

	/* ChargingSus 自恢复：显示充电中但电池电流为正（放电方向） */
	if (psy_read("status", buf, sizeof(buf)) > 0)
		charging = strncmp(buf, "Charging", 8) == 0;

	if (psy_read("current_now", buf, sizeof(buf)) > 0)
		ibat = simple_strtol(buf, NULL, 10);

	if (charging && ibat > 500000) {
		sus_streak++;
		if (sus_streak >= 2) {
			pr_info("houji_charge: charging suspended detected, pulsing input_suspend\n");
			bat_write("input_suspend", "1");
			mdelay(200);
			bat_write("input_suspend", "0");
			sus_streak = 0;
		}
	} else {
		sus_streak = 0;
	}

resched:
	schedule_delayed_work(&boost_work, msecs_to_jiffies(15000));
}

static void boost_init_work(struct work_struct *work)
{
	apply_target();
	pr_info("houji_charge: boost active, target=%d cW, enable=%d\n",
		target_power, boost_enable);
	schedule_delayed_work(&boost_work, msecs_to_jiffies(15000));
}

static int houji_proc_show(struct seq_file *m, void *v)
{
	char buf[64] = "n/a";

	psy_read("temp", buf, sizeof(buf));
	seq_printf(m, "enable=%d\ntarget_power=%d\ncurrent_temp=%s",
		   boost_enable, target_power, buf);
	if (psy_read("status", buf, sizeof(buf)) > 0)
		seq_printf(m, "\nstatus=%s", buf);
	if (psy_read("current_now", buf, sizeof(buf)) > 0)
		seq_printf(m, "\nibat=%s", buf);
	if (bat_read("restrict_cur", buf, sizeof(buf)) > 0)
		seq_printf(m, "\nrestrict_cur=%s", buf);
	seq_puts(m, "\n");
	return 0;
}

static int houji_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, houji_proc_show, NULL);
}

static ssize_t houji_proc_write(struct file *file, const char __user *buf,
				size_t count, loff_t *ppos)
{
	char kbuf[64];
	int n = min(count, sizeof(kbuf) - 1);

	if (copy_from_user(kbuf, buf, n))
		return -EFAULT;
	kbuf[n] = '\0';

	if (strncmp(kbuf, "enable=", 7) == 0)
		boost_enable = (kbuf[7] == '1');
	else if (strncmp(kbuf, "target_power=", 13) == 0) {
		int v = simple_strtol(kbuf + 13, NULL, 10);

		if (v >= 2000 && v <= 9000)
			target_power = v;
	} else if (strncmp(kbuf, "reload", 6) == 0) {
		apply_target();
	}
	return count;
}

static const struct proc_ops houji_proc_ops = {
	.proc_open = houji_proc_open,
	.proc_read = seq_read,
	.proc_write = houji_proc_write,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static int __init houji_charge_init(void)
{
	proc_create("houji_charge", 0666, NULL, &houji_proc_ops);
	INIT_DELAYED_WORK(&boost_work, boost_do_work);
	schedule_delayed_work(&boost_work, msecs_to_jiffies(10000));
	return 0;
}
late_initcall(houji_charge_init);

static void __exit houji_charge_exit(void)
{
	remove_proc_entry("houji_charge", NULL);
	cancel_delayed_work_sync(&boost_work);
}
module_exit(houji_charge_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("bingjiling92");
MODULE_DESCRIPTION("houji charge boost: unlock vendor charge limits, 40-50W daily target");