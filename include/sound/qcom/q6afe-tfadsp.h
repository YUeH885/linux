/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __SOUND_QCOM_Q6AFE_TFADSP_H
#define __SOUND_QCOM_Q6AFE_TFADSP_H

#include <linux/notifier.h>
#include <linux/types.h>

struct device;
struct q6afe_port;

#define Q6AFE_TFADSP_RX_TOPOLOGY	0x1000B900
#define Q6AFE_TFADSP_TX_TOPOLOGY	0x1000B901
#define Q6AFE_TFADSP_MAX_MESSAGE_SIZE	452

enum q6afe_tfadsp_event {
	Q6AFE_TFADSP_EVENT_INIT = 1,
	Q6AFE_TFADSP_EVENT_CLOSE = 2,
	Q6AFE_TFADSP_EVENT_CONFIGURED = 3,
	Q6AFE_TFADSP_EVENT_RX_DISABLED = 4,
	Q6AFE_TFADSP_EVENT_TX_DISABLED = 5,
};

struct q6afe_tfadsp_event_data {
	u32 value;
};

struct q6afe_port *q6afe_tfadsp_get_port(struct device *dev,
					 int dai_id);
void q6afe_tfadsp_put_port(struct q6afe_port *port);
int q6afe_tfadsp_set_feedback(struct q6afe_port *rx, struct q6afe_port *tx);
int q6afe_tfadsp_set_topology(struct q6afe_port *port, u32 topology_id);
int q6afe_tfadsp_send_msg(struct q6afe_port *port, const void *buf,
			  size_t len);
int q6afe_tfadsp_read_msg(struct q6afe_port *port, const void *command,
			  size_t command_size, void *buf, size_t len);
bool q6afe_tfadsp_is_ready(struct q6afe_port *port);
int q6afe_tfadsp_wait_configured(struct q6afe_port *port);
int q6afe_tfadsp_register_notifier(struct q6afe_port *port,
				   struct notifier_block *nb);
int q6afe_tfadsp_unregister_notifier(struct q6afe_port *port,
				     struct notifier_block *nb);

#endif /* __SOUND_QCOM_Q6AFE_TFADSP_H */
