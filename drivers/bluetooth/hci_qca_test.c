// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/device.h>
#include <kunit/test.h>
#include <linux/pm_runtime.h>
#include <linux/tty.h>

struct qca_uart_test {
	struct kunit *test;
	struct qca_serdev qcadev;
	struct qca_power power;
	struct serdev_controller *ctrl;
	struct serdev_device *serdev;
	bool opened;
	bool registered;
	bool hci_registered;
	bool controller_added;
	bool serdev_added;
	unsigned int opens;
	unsigned int closes;
	unsigned int fail_open_from;
	unsigned int closed_accesses;
	unsigned int pulses;
	unsigned int baudrate;
	bool flow_control;
	bool rts;
	u8 last_pulse;
};

static struct qca_uart_test *qca_uart_test_access(struct serdev_controller *ctrl)
{
	struct qca_uart_test *ctx = serdev_controller_get_drvdata(ctrl);
	struct hci_dev *hdev = ctx->qcadev.serdev_hu.hdev;

	if (!ctx->opened)
		ctx->closed_accesses++;
	KUNIT_EXPECT_TRUE(ctx->test, ctx->opened);
	if (hdev)
		KUNIT_EXPECT_TRUE(ctx->test, hdev->dev.kobj.state_initialized);
	return ctx;
}

static int qca_uart_test_open(struct serdev_controller *ctrl)
{
	struct qca_uart_test *ctx = serdev_controller_get_drvdata(ctrl);
	struct hci_uart *hu = &ctx->qcadev.serdev_hu;

	KUNIT_EXPECT_FALSE(ctx->test, ctx->opened);
	KUNIT_EXPECT_FALSE(ctx->test,
			   test_bit(HCI_UART_PROTO_READY, &hu->flags));
	ctx->opens++;
	if (ctx->fail_open_from && ctx->opens >= ctx->fail_open_from)
		return -EIO;

	ctx->opened = true;
	ctx->flow_control = true;
	ctx->rts = true;
	return 0;
}

static void qca_uart_test_close(struct serdev_controller *ctrl)
{
	struct qca_uart_test *ctx = serdev_controller_get_drvdata(ctrl);
	struct hci_uart *hu = &ctx->qcadev.serdev_hu;

	KUNIT_EXPECT_TRUE(ctx->test, ctx->opened);
	KUNIT_EXPECT_FALSE(ctx->test,
			   test_bit(HCI_UART_PROTO_READY, &hu->flags));
	ctx->opened = false;
	ctx->closes++;
}

static unsigned int qca_uart_test_baudrate(struct serdev_controller *ctrl,
					   unsigned int speed)
{
	struct qca_uart_test *ctx = qca_uart_test_access(ctrl);

	ctx->baudrate = speed;
	return speed;
}

static void qca_uart_test_flow_control(struct serdev_controller *ctrl, bool enable)
{
	struct qca_uart_test *ctx = qca_uart_test_access(ctrl);

	ctx->flow_control = enable;
}

static int qca_uart_test_tiocm(struct serdev_controller *ctrl,
			       unsigned int set, unsigned int clear)
{
	struct qca_uart_test *ctx = qca_uart_test_access(ctrl);

	if (set & TIOCM_RTS)
		ctx->rts = true;
	if (clear & TIOCM_RTS)
		ctx->rts = false;
	return 0;
}

static ssize_t qca_uart_test_write(struct serdev_controller *ctrl,
				   const u8 *data, size_t count)
{
	struct qca_uart_test *ctx = qca_uart_test_access(ctrl);

	KUNIT_EXPECT_EQ(ctx->test, count, (size_t)1);
	KUNIT_EXPECT_TRUE(ctx->test,
			  data[0] == QCA_WCN3990_POWERON_PULSE ||
			  data[0] == QCA_WCN3990_POWEROFF_PULSE);
	KUNIT_EXPECT_EQ(ctx->test, ctx->baudrate,
			data[0] == QCA_WCN3990_POWERON_PULSE ? 115200U : 2400U);
	KUNIT_EXPECT_FALSE(ctx->test, ctx->flow_control);
	KUNIT_EXPECT_FALSE(ctx->test, ctx->rts);
	ctx->last_pulse = data[0];
	ctx->pulses++;
	return count;
}

static void qca_uart_test_flush(struct serdev_controller *ctrl)
{
	qca_uart_test_access(ctrl);
}

static void qca_uart_test_wait(struct serdev_controller *ctrl, long timeout)
{
	struct qca_uart_test *ctx = qca_uart_test_access(ctrl);

	KUNIT_EXPECT_EQ(ctx->test, timeout, (long)msecs_to_jiffies(100));
}

static const struct serdev_controller_ops qca_uart_test_ops = {
	.open = qca_uart_test_open,
	.close = qca_uart_test_close,
	.set_baudrate = qca_uart_test_baudrate,
	.set_flow_control = qca_uart_test_flow_control,
	.set_tiocm = qca_uart_test_tiocm,
	.write_buf = qca_uart_test_write,
	.write_flush = qca_uart_test_flush,
	.wait_until_sent = qca_uart_test_wait,
};

static void qca_uart_test_remove(struct qca_uart_test *ctx)
{
	if (!ctx->hci_registered)
		clear_bit(HCI_UART_REGISTERED, &ctx->qcadev.serdev_hu.flags);
	qca_serdev_remove(ctx->serdev);
	ctx->registered = false;
	ctx->qcadev.serdev_hu.hdev = NULL;
}

static void qca_uart_test_release(void *data)
{
	struct qca_uart_test *ctx = data;

	if (ctx->registered)
		qca_uart_test_remove(ctx);
	if (ctx->serdev_added)
		device_del(&ctx->serdev->dev);
	if (ctx->controller_added)
		device_del(&ctx->ctrl->dev);
	pm_runtime_disable(&ctx->ctrl->dev);
	ctx->ctrl->serdev = NULL;
	serdev_device_put(ctx->serdev);
	serdev_controller_put(ctx->ctrl);
}

static int qca_uart_test_init(struct kunit *test)
{
	struct qca_uart_test *ctx;
	struct hci_uart *hu;
	struct device *parent;
	int ret;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	test->priv = ctx;
	ctx->test = test;

	parent = kunit_device_register(test, "qca_uart_test");
	if (IS_ERR(parent))
		return PTR_ERR(parent);
	ctx->ctrl = serdev_controller_alloc(parent, parent, 0);
	if (!ctx->ctrl)
		return -ENOMEM;
	ctx->serdev = serdev_device_alloc(ctx->ctrl);
	if (!ctx->serdev) {
		serdev_controller_put(ctx->ctrl);
		return -ENOMEM;
	}
	ctx->ctrl->ops = &qca_uart_test_ops;
	ctx->ctrl->serdev = ctx->serdev;
	serdev_controller_set_drvdata(ctx->ctrl, ctx);
	serdev_device_set_drvdata(ctx->serdev, &ctx->qcadev);
	ret = pm_runtime_set_active(&ctx->ctrl->dev);
	if (ret) {
		serdev_device_put(ctx->serdev);
		serdev_controller_put(ctx->ctrl);
		return ret;
	}
	pm_runtime_enable(&ctx->ctrl->dev);

	ret = kunit_add_action_or_reset(test, qca_uart_test_release, ctx);
	if (ret)
		return ret;

	ctx->qcadev.btsoc_type = QCA_WCN3998;
	ctx->qcadev.bt_power = &ctx->power;
	ctx->qcadev.init_speed = 115200;
	ctx->qcadev.oper_speed = 3200000;
	hu = &ctx->qcadev.serdev_hu;
	hu->serdev = ctx->serdev;
	/* Install the real HCI callbacks without scheduling controller setup. */
	set_bit(HCI_UART_INIT_PENDING, &hu->hdev_flags);
	ret = hci_uart_register_device(hu, &qca_proto);
	if (ret)
		return ret;
	ctx->registered = true;
	hu->hdev->name = "hci_qca_test";
	hci_set_quirk(hu->hdev, HCI_QUIRK_NON_PERSISTENT_SETUP);
	ctx->opens = 0;
	return 0;
}

static void qca_uart_test_cleanup_failure(struct qca_uart_test *ctx)
{
	struct hci_uart *hu = &ctx->qcadev.serdev_hu;

	qca_power_off(hu);
	KUNIT_EXPECT_EQ(ctx->test, hu->hdev->flush(hu->hdev), 0);
	KUNIT_EXPECT_EQ(ctx->test, hu->hdev->close(hu->hdev), 0);
	qca_uart_test_remove(ctx);
	KUNIT_EXPECT_FALSE(ctx->test, ctx->opened);
	KUNIT_EXPECT_FALSE(ctx->test, ctx->power.vregs_on);
	KUNIT_EXPECT_PTR_EQ(ctx->test, hu->priv, NULL);
	KUNIT_EXPECT_EQ(ctx->test, ctx->closed_accesses, 0U);
	KUNIT_EXPECT_EQ(ctx->test,
			atomic_read(&ctx->ctrl->dev.power.usage_count), 0);
}

static void qca_uart_regulator_open_failure(struct kunit *test)
{
	struct qca_uart_test *ctx = test->priv;
	struct hci_uart *hu = &ctx->qcadev.serdev_hu;

	ctx->fail_open_from = 1;
	KUNIT_EXPECT_EQ(test, qca_power_on(hu->hdev), -EIO);
	KUNIT_EXPECT_FALSE(test, test_bit(HCI_UART_PROTO_READY, &hu->flags));
	KUNIT_EXPECT_TRUE(test, ctx->power.vregs_on);
	qca_uart_test_cleanup_failure(ctx);
	KUNIT_EXPECT_EQ(test, ctx->closes, 1U);
	KUNIT_EXPECT_EQ(test, ctx->pulses, 0U);
}

static void qca_uart_sync_reopen_failure(struct kunit *test)
{
	struct qca_uart_test *ctx = test->priv;
	struct hci_uart *hu = &ctx->qcadev.serdev_hu;

	ctx->power.vregs_on = true;
	ctx->fail_open_from = 1;
	KUNIT_EXPECT_EQ(test, qca_regulator_init(hu), -EIO);
	KUNIT_EXPECT_FALSE(test, test_bit(HCI_UART_PROTO_READY, &hu->flags));
	qca_uart_test_cleanup_failure(ctx);
	KUNIT_EXPECT_EQ(test, ctx->closes, 1U);
	KUNIT_EXPECT_EQ(test, ctx->pulses, 2U);
}

static void qca_uart_retry_open_failure(struct kunit *test)
{
	struct qca_uart_test *ctx = test->priv;
	struct hci_uart *hu = &ctx->qcadev.serdev_hu;

	ctx->fail_open_from = 2;
	KUNIT_EXPECT_EQ(test, qca_setup(hu), -EIO);
	KUNIT_EXPECT_FALSE(test, test_bit(HCI_UART_PROTO_READY, &hu->flags));
	KUNIT_EXPECT_FALSE(test, ctx->power.vregs_on);
	KUNIT_EXPECT_EQ(test, ctx->opens, 3U);
	qca_uart_test_cleanup_failure(ctx);
	KUNIT_EXPECT_EQ(test, ctx->closes, 2U);
	KUNIT_EXPECT_EQ(test, ctx->pulses, 2U);
}

static void qca_uart_open_after_failure(struct kunit *test)
{
	struct qca_uart_test *ctx = test->priv;
	struct hci_uart *hu = &ctx->qcadev.serdev_hu;

	ctx->fail_open_from = 1;
	KUNIT_EXPECT_EQ(test, qca_power_on(hu->hdev), -EIO);
	qca_power_off(hu);
	KUNIT_EXPECT_EQ(test, hu->hdev->flush(hu->hdev), 0);
	KUNIT_EXPECT_EQ(test, hu->hdev->close(hu->hdev), 0);

	ctx->fail_open_from = 0;
	KUNIT_ASSERT_EQ(test, hu->hdev->open(hu->hdev), 0);
	KUNIT_ASSERT_EQ(test, qca_power_on(hu->hdev), 0);
	KUNIT_EXPECT_TRUE(test, test_bit(HCI_UART_PROTO_READY, &hu->flags));
	KUNIT_EXPECT_TRUE(test, ctx->power.vregs_on);
	KUNIT_EXPECT_TRUE(test, ctx->opened);
	KUNIT_EXPECT_EQ(test, ctx->closed_accesses, 0U);
	KUNIT_EXPECT_EQ(test, atomic_read(&ctx->ctrl->dev.power.usage_count), 1);
}

static void qca_uart_remove_powered_controller(struct kunit *test)
{
	struct qca_uart_test *ctx = test->priv;
	struct hci_uart *hu = &ctx->qcadev.serdev_hu;

	KUNIT_ASSERT_EQ(test, qca_power_on(hu->hdev), 0);
	qca_uart_test_remove(ctx);
	KUNIT_EXPECT_FALSE(test, ctx->power.vregs_on);
	KUNIT_EXPECT_FALSE(test, ctx->opened);
	KUNIT_EXPECT_PTR_EQ(test, hu->priv, NULL);
	KUNIT_EXPECT_EQ(test, ctx->closed_accesses, 0U);
	KUNIT_EXPECT_EQ(test, ctx->pulses, 3U);
	KUNIT_EXPECT_EQ(test, ctx->last_pulse, (u8)QCA_WCN3990_POWEROFF_PULSE);
	KUNIT_EXPECT_EQ(test, atomic_read(&ctx->ctrl->dev.power.usage_count), 0);
}

static int qca_uart_test_skip_setup(struct hci_dev *hdev)
{
	return 0;
}

static void qca_uart_remove_registered_controller(struct kunit *test)
{
	struct qca_uart_test *ctx = test->priv;
	struct hci_uart *hu = &ctx->qcadev.serdev_hu;
	struct hci_dev *hdev = hu->hdev;
	int ret;

	KUNIT_ASSERT_EQ(test, device_add(&ctx->ctrl->dev), 0);
	ctx->controller_added = true;
	KUNIT_ASSERT_EQ(test, dev_set_name(&ctx->serdev->dev, "qca_uart_test_serial"), 0);
	KUNIT_ASSERT_EQ(test, device_add(&ctx->serdev->dev), 0);
	ctx->serdev_added = true;
	hci_set_quirk(hdev, HCI_QUIRK_RAW_DEVICE);
	hdev->setup = qca_uart_test_skip_setup;
	ret = hci_register_dev(hdev);
	KUNIT_ASSERT_GE(test, ret, 0);
	ctx->hci_registered = true;
	set_bit(HCI_UART_REGISTERED, &hu->flags);
	flush_work(&hdev->power_on);
	cancel_delayed_work_sync(&hdev->power_off);
	/* Model an open raw controller without HCI initialization commands. */
	KUNIT_ASSERT_EQ(test, hdev->open(hdev), 0);
	hci_dev_hold(hdev);
	set_bit(HCI_UP, &hdev->flags);
	set_bit(HCI_RUNNING, &hdev->flags);
	qca_uart_remove_powered_controller(test);
}

static void qca_uart_ignores_power_pulse_bytes(struct kunit *test)
{
	struct qca_uart_test *ctx = test->priv;
	struct hci_uart *hu = &ctx->qcadev.serdev_hu;
	struct qca_data *qca = hu->priv;
	const u8 noise[] = { 0x00, 0x00, 0xc0 };
	const u8 event[] = { HCI_EVENT_PKT, HCI_EV_VENDOR, 2, 0x92, 1 };

	set_bit(HCI_UART_REGISTERED, &hu->flags);
	KUNIT_EXPECT_EQ(test, qca_recv(hu, noise, sizeof(noise)), (int)sizeof(noise));
	KUNIT_EXPECT_PTR_EQ(test, qca->rx_skb, NULL);
	clear_bit(QCA_BT_OFF, &qca->flags);
	set_bit(QCA_DROP_VENDOR_EVENT, &qca->flags);
	KUNIT_EXPECT_EQ(test, qca_recv(hu, event, sizeof(event)), (int)sizeof(event));
	KUNIT_EXPECT_TRUE(test, completion_done(&qca->drop_ev_comp));
	KUNIT_EXPECT_PTR_EQ(test, qca->rx_skb, NULL);
	clear_bit(QCA_DROP_VENDOR_EVENT, &qca->flags);
}

static void qca_uart_fragmented_baudrate_event(struct kunit *test)
{
	struct qca_uart_test *ctx = test->priv;
	struct hci_uart *hu = &ctx->qcadev.serdev_hu;
	struct qca_data *qca = hu->priv;
	const u8 event[] = { HCI_EVENT_PKT, HCI_EV_VENDOR, 2, 0x92, 1 };
	unsigned int split;

	set_bit(QCA_DROP_VENDOR_EVENT, &qca->flags);
	for (split = 1; split < sizeof(event); split++) {
		reinit_completion(&qca->drop_ev_comp);
		qca->rx_skb = h4_recv_buf(hu, NULL, event, split,
					  qca_recv_pkts,
					  ARRAY_SIZE(qca_recv_pkts));
		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, qca->rx_skb);
		KUNIT_EXPECT_FALSE(test, completion_done(&qca->drop_ev_comp));
		qca->rx_skb = h4_recv_buf(hu, qca->rx_skb, event + split,
					  sizeof(event) - split, qca_recv_pkts,
					  ARRAY_SIZE(qca_recv_pkts));
		KUNIT_ASSERT_PTR_EQ(test, qca->rx_skb, NULL);
		KUNIT_EXPECT_TRUE(test, completion_done(&qca->drop_ev_comp));
	}
	clear_bit(QCA_DROP_VENDOR_EVENT, &qca->flags);
}

static void qca_uart_receive_excludes_protocol_close(struct kunit *test)
{
	struct qca_uart_test *ctx = test->priv;
	struct hci_uart *hu = &ctx->qcadev.serdev_hu;
	struct qca_data *qca = hu->priv;
	const u8 event[] = { HCI_EVENT_PKT, HCI_EV_VENDOR, 2, 0x92, 1 };
	size_t received;

	set_bit(HCI_UART_REGISTERED, &hu->flags);
	clear_bit(QCA_BT_OFF, &qca->flags);
	set_bit(QCA_DROP_VENDOR_EVENT, &qca->flags);
	percpu_down_write(&hu->proto_lock);
	received = ctx->serdev->ops->receive_buf(ctx->serdev, event, sizeof(event));
	percpu_up_write(&hu->proto_lock);
	KUNIT_EXPECT_EQ(test, received, (size_t)0);
	KUNIT_EXPECT_FALSE(test, completion_done(&qca->drop_ev_comp));
	received = ctx->serdev->ops->receive_buf(ctx->serdev, event, sizeof(event));
	KUNIT_EXPECT_EQ(test, received, sizeof(event));
	KUNIT_EXPECT_TRUE(test, completion_done(&qca->drop_ev_comp));

	reinit_completion(&qca->drop_ev_comp);
	clear_bit(HCI_UART_PROTO_READY, &hu->flags);
	received = ctx->serdev->ops->receive_buf(ctx->serdev, event, sizeof(event));
	set_bit(HCI_UART_PROTO_READY, &hu->flags);
	KUNIT_EXPECT_EQ(test, received, (size_t)0);
	KUNIT_EXPECT_FALSE(test, completion_done(&qca->drop_ev_comp));
	clear_bit(QCA_DROP_VENDOR_EVENT, &qca->flags);
}

static void qca_uart_short_vendor_event(struct kunit *test)
{
	struct qca_uart_test *ctx = test->priv;
	struct hci_uart *hu = &ctx->qcadev.serdev_hu;
	struct qca_data *qca = hu->priv;
	struct sk_buff *skb;
	unsigned int plen;

	for (plen = 0; plen < sizeof(__be16); plen++) {
		skb = bt_skb_alloc(HCI_EVENT_HDR_SIZE + sizeof(__be16), GFP_KERNEL);
		KUNIT_ASSERT_NOT_NULL(test, skb);
		/* Tailroom resembles a dump handle but is outside the payload. */
		skb->data[0] = HCI_EV_VENDOR;
		skb->data[1] = plen;
		put_unaligned_be16(QCA_SSR_DUMP_HANDLE, skb->data + HCI_EVENT_HDR_SIZE);
		skb_put(skb, HCI_EVENT_HDR_SIZE + plen);
		hci_skb_pkt_type(skb) = HCI_EVENT_PKT;
		KUNIT_EXPECT_EQ(test, qca_recv_event(hu->hdev, skb), -ENXIO);
		KUNIT_EXPECT_FALSE(test, test_bit(QCA_SSR_TRIGGERED, &qca->flags));
		KUNIT_EXPECT_TRUE(test, skb_queue_empty(&qca->rx_memdump_q));
	}
}

static struct kunit_case qca_uart_test_cases[] = {
	KUNIT_CASE(qca_uart_regulator_open_failure),
	KUNIT_CASE(qca_uart_sync_reopen_failure),
	KUNIT_CASE(qca_uart_retry_open_failure),
	KUNIT_CASE(qca_uart_open_after_failure),
	KUNIT_CASE(qca_uart_remove_powered_controller),
	KUNIT_CASE(qca_uart_remove_registered_controller),
	KUNIT_CASE(qca_uart_ignores_power_pulse_bytes),
	KUNIT_CASE(qca_uart_fragmented_baudrate_event),
	KUNIT_CASE(qca_uart_receive_excludes_protocol_close),
	KUNIT_CASE(qca_uart_short_vendor_event),
	{}
};

static struct kunit_suite qca_uart_test_suite = {
	.name = "bluetooth-qca-uart",
	.init = qca_uart_test_init,
	.test_cases = qca_uart_test_cases,
};

kunit_test_suite(qca_uart_test_suite);
