// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>
#include <linux/suspend.h>

#include <net/bluetooth/bluetooth.h>
#include <net/bluetooth/hci_core.h>

struct suspend_test {
	struct hci_dev *hdev;
	u8 command_status;
	u8 disconnect_status;
	u8 advertising_disable_status;
	u8 inquiry_cancel_status;
	bool drop_disconnect;
	bool restart_on_cancel;
	bool cancel_saw_paused;
	int restart_result;
	unsigned int commands;
	unsigned int inquiry_commands;
	unsigned int inquiry_in_suspend;
	unsigned int disconnect_commands;
	unsigned int masked_disconnect;
	unsigned int advertising_enabled;
	unsigned int advertising_in_suspend;
};

static int restart_discovery(struct hci_dev *hdev, void *data)
{
	struct suspend_test *ctx = data;

	ctx->restart_result = hci_start_discovery_sync(hdev);
	return ctx->restart_result;
}

static int receive_event(struct hci_dev *hdev, u8 event, const void *data,
			 size_t len)
{
	struct hci_event_hdr *hdr;
	struct sk_buff *skb;

	skb = bt_skb_alloc(sizeof(*hdr) + len, GFP_KERNEL);
	if (!skb)
		return -ENOMEM;

	hdr = skb_put(skb, sizeof(*hdr));
	hdr->evt = event;
	hdr->plen = len;
	skb_put_data(skb, data, len);
	hci_skb_pkt_type(skb) = HCI_EVENT_PKT;
	return hci_recv_frame(hdev, skb);
}

static int send_command(struct hci_dev *hdev, struct sk_buff *skb)
{
	struct suspend_test *ctx = hci_get_drvdata(hdev);
	struct hci_command_hdr *hdr = (void *)skb->data;
	u16 opcode = le16_to_cpu(hdr->opcode);
	u8 *param = skb->data + sizeof(*hdr);
	struct {
		struct hci_ev_cmd_complete hdr;
		u8 status;
	} __packed complete = {
		.hdr.ncmd = 1,
		.hdr.opcode = hdr->opcode,
	};
	int err;

	ctx->commands++;
	if (opcode == HCI_OP_INQUIRY) {
		struct hci_ev_cmd_status status = {
			.ncmd = 1,
			.opcode = hdr->opcode,
		};

		ctx->inquiry_commands++;
		if (hdev->suspended)
			ctx->inquiry_in_suspend++;
		err = receive_event(hdev, HCI_EV_CMD_STATUS, &status, sizeof(status));
	} else if (opcode == HCI_OP_DISCONNECT) {
		struct hci_cp_disconnect *cp = (void *)param;
		struct hci_ev_cmd_status status = {
			.status = ctx->command_status,
			.ncmd = 1,
			.opcode = hdr->opcode,
		};
		struct hci_ev_disconn_complete disconnected = {
			.status = ctx->disconnect_status,
			.handle = cp->handle,
			.reason = HCI_ERROR_LOCAL_HOST_TERM,
		};

		ctx->disconnect_commands++;
		err = receive_event(hdev, HCI_EV_CMD_STATUS, &status,
				    sizeof(status));
		if (!err && !status.status && !ctx->drop_disconnect)
			err = receive_event(hdev, HCI_EV_DISCONN_COMPLETE,
					    &disconnected, sizeof(disconnected));
	} else {
		if (opcode == HCI_OP_INQUIRY_CANCEL) {
			ctx->cancel_saw_paused = hdev->discovery_paused;
			complete.status = ctx->inquiry_cancel_status;
			if (ctx->restart_on_cancel)
				hci_cmd_sync_queue(hdev, restart_discovery, ctx, NULL);
		}
		if (opcode == HCI_OP_SET_EVENT_MASK && !(param[0] & BIT(4)))
			ctx->masked_disconnect++;
		if (opcode == HCI_OP_LE_SET_ADV_ENABLE ||
		    opcode == HCI_OP_LE_SET_EXT_ADV_ENABLE) {
			if (param[0]) {
				ctx->advertising_enabled++;
				if (hdev->suspended)
					ctx->advertising_in_suspend++;
			} else {
				complete.status = ctx->advertising_disable_status;
			}
		}
		err = receive_event(hdev, HCI_EV_CMD_COMPLETE, &complete,
				    sizeof(complete));
	}

	kfree_skb(skb);
	return err;
}

static int suspend_test_init(struct kunit *test)
{
	struct suspend_test *ctx;
	struct hci_dev *hdev;
	int err;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	hdev = hci_alloc_dev();
	if (!hdev)
		return -ENOMEM;

	ctx->hdev = hdev;
	test->priv = ctx;
	hci_set_drvdata(hdev, ctx);
	hdev->name = "hci_suspend_test";
	hdev->send = send_command;
	hdev->hci_ver = BLUETOOTH_VER_1_2;
	hdev->acl_mtu = 1021;
	hdev->acl_pkts = 4;
	hdev->bdaddr.b[0] = 1;
	set_bit(HCI_UP, &hdev->flags);
	set_bit(HCI_RUNNING, &hdev->flags);
	hci_dev_set_flag(hdev, HCI_BREDR_ENABLED);
	hci_dev_set_flag(hdev, HCI_LE_ENABLED);
	atomic_set(&hdev->cmd_cnt, 1);

	hdev->workqueue = alloc_ordered_workqueue("hci_suspend_rx", 0);
	if (!hdev->workqueue) {
		err = -ENOMEM;
		goto free_dev;
	}
	hdev->req_workqueue = alloc_ordered_workqueue("hci_suspend_req", 0);
	if (!hdev->req_workqueue) {
		err = -ENOMEM;
		goto free_workqueue;
	}
	err = hci_register_suspend_notifier(hdev);
	if (err)
		goto free_req_workqueue;

	return 0;

free_req_workqueue:
	destroy_workqueue(hdev->req_workqueue);
free_workqueue:
	destroy_workqueue(hdev->workqueue);
free_dev:
	hci_free_dev(hdev);
	return err;
}

static void suspend_test_exit(struct kunit *test)
{
	struct suspend_test *ctx = test->priv;
	struct hci_dev *hdev = ctx->hdev;

	hci_unregister_suspend_notifier(hdev);
	clear_bit(HCI_RUNNING, &hdev->flags);
	flush_workqueue(hdev->req_workqueue);
	flush_workqueue(hdev->workqueue);
	hci_dev_lock(hdev);
	hci_conn_hash_flush(hdev);
	hci_dev_unlock(hdev);
	hci_cmd_sync_clear(hdev);
	cancel_delayed_work_sync(&hdev->cmd_timer);
	cancel_delayed_work_sync(&hdev->ncmd_timer);
	cancel_delayed_work_sync(&hdev->adv_instance_expire);
	cancel_delayed_work_sync(&hdev->le_scan_disable);
	destroy_workqueue(hdev->req_workqueue);
	destroy_workqueue(hdev->workqueue);
	kfree_skb(hdev->sent_cmd);
	kfree_skb(hdev->req_skb);
	kfree_skb(hdev->recv_event);
	hci_free_dev(hdev);
}

static void add_connection(struct kunit *test, u8 type, u8 role)
{
	struct suspend_test *ctx = test->priv;
	bdaddr_t addr = { .b = { 2 } };
	struct hci_conn *conn;

	hci_dev_lock(ctx->hdev);
	conn = hci_conn_add(ctx->hdev, type, &addr, ADDR_LE_DEV_PUBLIC, role, 1);
	if (!IS_ERR(conn))
		conn->state = BT_CONNECTED;
	hci_dev_unlock(ctx->hdev);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, conn);
}

static int notify_suspend(struct hci_dev *hdev, unsigned long action)
{
	struct notifier_block *nb = &hdev->suspend_notifier;

	return notifier_to_errno(nb->notifier_call(nb, action, NULL));
}

static void disconnect_succeeds(struct kunit *test)
{
	struct suspend_test *ctx = test->priv;

	add_connection(test, ACL_LINK, HCI_ROLE_MASTER);
	KUNIT_EXPECT_EQ(test, notify_suspend(ctx->hdev, PM_SUSPEND_PREPARE), 0);
	KUNIT_EXPECT_TRUE(test, ctx->hdev->suspended);
	KUNIT_EXPECT_EQ(test, hci_conn_count(ctx->hdev), 0);
	KUNIT_EXPECT_EQ(test, ctx->masked_disconnect, 1);
	KUNIT_EXPECT_EQ(test, notify_suspend(ctx->hdev, PM_POST_SUSPEND), 0);
	KUNIT_EXPECT_FALSE(test, ctx->hdev->suspended);
}

static void expect_suspend_failure(struct kunit *test, int expected,
				   bool connection_remains)
{
	struct suspend_test *ctx = test->priv;

	add_connection(test, ACL_LINK, HCI_ROLE_MASTER);
	KUNIT_EXPECT_EQ(test, notify_suspend(ctx->hdev, PM_SUSPEND_PREPARE),
			expected);
	KUNIT_EXPECT_FALSE(test, ctx->hdev->suspended);
	KUNIT_EXPECT_FALSE(test, ctx->hdev->scanning_paused);
	KUNIT_EXPECT_EQ(test, ctx->hdev->suspend_state, BT_RUNNING);
	KUNIT_EXPECT_EQ(test, ctx->masked_disconnect, 0);
	KUNIT_EXPECT_EQ(test, hci_conn_count(ctx->hdev), connection_remains ? 1 : 0);

	ctx->drop_disconnect = false;
	ctx->command_status = 0;
	ctx->disconnect_status = 0;
	KUNIT_EXPECT_EQ(test, notify_suspend(ctx->hdev, PM_SUSPEND_PREPARE), 0);
	KUNIT_EXPECT_EQ(test, hci_conn_count(ctx->hdev), 0);
	KUNIT_EXPECT_EQ(test, notify_suspend(ctx->hdev, PM_POST_SUSPEND), 0);
}

static void disconnect_timeout(struct kunit *test)
{
	struct suspend_test *ctx = test->priv;

	ctx->drop_disconnect = true;
	expect_suspend_failure(test, -ETIMEDOUT, true);
}

static void disconnect_rejected(struct kunit *test)
{
	struct suspend_test *ctx = test->priv;

	ctx->command_status = HCI_ERROR_COMMAND_DISALLOWED;
	expect_suspend_failure(test, -bt_to_errno(ctx->command_status), true);
}

static void disconnect_completion_failed(struct kunit *test)
{
	struct suspend_test *ctx = test->priv;

	ctx->disconnect_status = HCI_ERROR_COMMAND_DISALLOWED;
	expect_suspend_failure(test, -bt_to_errno(ctx->disconnect_status), true);
}

static void disconnect_unknown_handle(struct kunit *test)
{
	struct suspend_test *ctx = test->priv;

	ctx->command_status = HCI_ERROR_UNKNOWN_CONN_ID;
	expect_suspend_failure(test, -bt_to_errno(ctx->command_status), false);
}

static void power_off_skips_completion(struct kunit *test)
{
	struct suspend_test *ctx = test->priv;
	struct hci_conn *conn;
	int err;

	add_connection(test, ACL_LINK, HCI_ROLE_MASTER);
	ctx->drop_disconnect = true;
	conn = hci_conn_hash_lookup_handle(ctx->hdev, 1);
	hci_req_sync_lock(ctx->hdev);
	err = hci_abort_conn_sync(ctx->hdev, conn, HCI_ERROR_REMOTE_POWER_OFF);
	hci_req_sync_unlock(ctx->hdev);
	KUNIT_EXPECT_EQ(test, err, 0);
	KUNIT_EXPECT_EQ(test, hci_conn_count(ctx->hdev), 0);
}

static void scanning_connection_needs_no_disconnect(struct kunit *test)
{
	struct suspend_test *ctx = test->priv;
	struct hci_conn *conn;

	add_connection(test, LE_LINK, HCI_ROLE_MASTER);
	conn = hci_conn_hash_lookup_handle(ctx->hdev, 1);
	conn->state = BT_CONNECT;
	set_bit(HCI_CONN_SCANNING, &conn->flags);
	KUNIT_EXPECT_EQ(test, notify_suspend(ctx->hdev, PM_SUSPEND_PREPARE), 0);
	KUNIT_EXPECT_EQ(test, ctx->disconnect_commands, 0);
	KUNIT_EXPECT_EQ(test, hci_conn_count(ctx->hdev), 0);
	KUNIT_EXPECT_EQ(test, notify_suspend(ctx->hdev, PM_POST_SUSPEND), 0);
}

static void advertising_pause_rejected(struct kunit *test)
{
	struct suspend_test *ctx = test->priv;

	hci_dev_set_flag(ctx->hdev, HCI_ADVERTISING);
	hci_dev_set_flag(ctx->hdev, HCI_LE_ADV);
	ctx->advertising_disable_status = HCI_ERROR_COMMAND_DISALLOWED;
	KUNIT_EXPECT_EQ(test, notify_suspend(ctx->hdev, PM_SUSPEND_PREPARE),
			-bt_to_errno(ctx->advertising_disable_status));
	KUNIT_EXPECT_FALSE(test, ctx->hdev->suspended);
	KUNIT_EXPECT_FALSE(test, ctx->hdev->advertising_paused);
	KUNIT_EXPECT_EQ(test, ctx->masked_disconnect, 0);
}

static void advertising_resume(struct kunit *test, bool extended,
			       bool was_advertising)
{
	struct suspend_test *ctx = test->priv;
	struct hci_dev *hdev = ctx->hdev;

	if (extended)
		hdev->le_features[1] |= HCI_LE_EXT_ADV;
	hci_dev_set_flag(hdev, HCI_ADVERTISING);
	if (was_advertising)
		hci_dev_set_flag(hdev, HCI_LE_ADV);
	add_connection(test, LE_LINK, HCI_ROLE_SLAVE);

	KUNIT_EXPECT_EQ(test, notify_suspend(hdev, PM_SUSPEND_PREPARE), 0);
	flush_workqueue(hdev->req_workqueue);
	KUNIT_EXPECT_EQ(test, ctx->advertising_enabled, 0);
	KUNIT_EXPECT_TRUE(test, hdev->advertising_paused);
	KUNIT_EXPECT_EQ(test, notify_suspend(hdev, PM_POST_SUSPEND), 0);
	KUNIT_EXPECT_FALSE(test, hdev->advertising_paused);
	KUNIT_EXPECT_TRUE(test, hci_dev_test_flag(hdev, HCI_LE_ADV));
	KUNIT_EXPECT_GT(test, ctx->advertising_enabled, 0);
	KUNIT_EXPECT_EQ(test, ctx->advertising_in_suspend, 0);
}

static void legacy_advertising_resume(struct kunit *test)
{
	advertising_resume(test, false, true);
}

static void connected_advertising_resume(struct kunit *test)
{
	advertising_resume(test, false, false);
}

static void extended_advertising_resume(struct kunit *test)
{
	advertising_resume(test, true, false);
}

static void discovery_is_paused_before_cancel(struct kunit *test)
{
	struct suspend_test *ctx = test->priv;
	struct hci_dev *hdev = ctx->hdev;

	hdev->discovery.type = DISCOV_TYPE_BREDR;
	hdev->discovery.state = DISCOVERY_FINDING;
	set_bit(HCI_INQUIRY, &hdev->flags);
	ctx->restart_on_cancel = true;
	KUNIT_ASSERT_EQ(test, notify_suspend(hdev, PM_SUSPEND_PREPARE), 0);
	flush_workqueue(hdev->req_workqueue);
	KUNIT_EXPECT_TRUE(test, ctx->cancel_saw_paused);
	KUNIT_EXPECT_EQ(test, ctx->restart_result, -EBUSY);
	KUNIT_EXPECT_EQ(test, ctx->inquiry_commands, 0);
	KUNIT_EXPECT_FALSE(test, test_bit(HCI_INQUIRY, &hdev->flags));
	KUNIT_ASSERT_EQ(test, notify_suspend(hdev, PM_POST_SUSPEND), 0);
	KUNIT_EXPECT_EQ(test, ctx->inquiry_commands, 1);
	KUNIT_EXPECT_EQ(test, ctx->inquiry_in_suspend, 0);
	KUNIT_EXPECT_FALSE(test, hdev->discovery_paused);
}

static void discovery_rejects_late_requests(struct kunit *test)
{
	struct suspend_test *ctx = test->priv;
	struct hci_dev *hdev = ctx->hdev;
	unsigned int commands;
	int types[] = { DISCOV_TYPE_BREDR, DISCOV_TYPE_LE, DISCOV_TYPE_INTERLEAVED };
	int i;

	KUNIT_ASSERT_EQ(test, notify_suspend(hdev, PM_SUSPEND_PREPARE), 0);
	commands = ctx->commands;
	hci_req_sync_lock(hdev);
	for (i = 0; i < ARRAY_SIZE(types); i++) {
		hdev->discovery.type = types[i];
		KUNIT_EXPECT_EQ(test, hci_start_discovery_sync(hdev), -EBUSY);
	}
	/* The LE scan timer and the legacy inquiry API bypass discovery start. */
	KUNIT_EXPECT_EQ(test, hci_inquiry_sync(hdev, 4, 0), -EBUSY);
	hci_req_sync_unlock(hdev);
	KUNIT_EXPECT_EQ(test, ctx->commands, commands);
	KUNIT_EXPECT_EQ(test, notify_suspend(hdev, PM_POST_SUSPEND), 0);
}

static void discovery_cancel_failure_aborts_suspend(struct kunit *test)
{
	struct suspend_test *ctx = test->priv;
	struct hci_dev *hdev = ctx->hdev;

	hdev->discovery.type = DISCOV_TYPE_BREDR;
	hdev->discovery.state = DISCOVERY_FINDING;
	set_bit(HCI_INQUIRY, &hdev->flags);
	ctx->inquiry_cancel_status = HCI_ERROR_COMMAND_DISALLOWED;
	KUNIT_EXPECT_EQ(test, notify_suspend(hdev, PM_SUSPEND_PREPARE),
			-bt_to_errno(ctx->inquiry_cancel_status));
	KUNIT_EXPECT_TRUE(test, ctx->cancel_saw_paused);
	KUNIT_EXPECT_FALSE(test, hdev->suspended);
	KUNIT_EXPECT_FALSE(test, hdev->discovery_paused);
	KUNIT_EXPECT_TRUE(test, test_bit(HCI_INQUIRY, &hdev->flags));
	ctx->inquiry_cancel_status = 0;
	KUNIT_ASSERT_EQ(test, notify_suspend(hdev, PM_SUSPEND_PREPARE), 0);
	KUNIT_EXPECT_EQ(test, notify_suspend(hdev, PM_POST_SUSPEND), 0);
}

static struct kunit_case suspend_cases[] = {
	KUNIT_CASE(disconnect_succeeds),
	KUNIT_CASE(disconnect_timeout),
	KUNIT_CASE(disconnect_rejected),
	KUNIT_CASE(disconnect_completion_failed),
	KUNIT_CASE(disconnect_unknown_handle),
	KUNIT_CASE(power_off_skips_completion),
	KUNIT_CASE(scanning_connection_needs_no_disconnect),
	KUNIT_CASE(advertising_pause_rejected),
	KUNIT_CASE(legacy_advertising_resume),
	KUNIT_CASE(connected_advertising_resume),
	KUNIT_CASE(extended_advertising_resume),
	KUNIT_CASE(discovery_is_paused_before_cancel),
	KUNIT_CASE(discovery_rejects_late_requests),
	KUNIT_CASE(discovery_cancel_failure_aborts_suspend),
	{}
};

static struct kunit_suite suspend_suite = {
	.name = "bluetooth-suspend",
	.init = suspend_test_init,
	.exit = suspend_test_exit,
	.test_cases = suspend_cases,
};
kunit_test_suite(suspend_suite);
