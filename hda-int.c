/*
 * hda-emu - simple HD-audio codec emulator for debugging snd-hda-intel driver
 *
 * Emulate the communication of HD-audio codec
 *
 * Copyright (c) Takashi Iwai <tiwai@suse.de>
 *
 *  This driver is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This driver is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 */

#include <stdio.h>
#include <errno.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include "hda-types.h"
#include "hda-log.h"


#define AC_AMP_GET_LEFT			(1<<13)
#define AC_AMP_GET_RIGHT		(0<<13)
#define AC_AMP_GET_OUTPUT		(1<<15)
#define AC_AMP_GET_INPUT		(0<<15)

#define AC_AMP_SET_INDEX		(0xf<<8)
#define AC_AMP_SET_INDEX_SHIFT		8
#define AC_AMP_SET_RIGHT		(1<<12)
#define AC_AMP_SET_LEFT			(1<<13)
#define AC_AMP_SET_INPUT		(1<<14)
#define AC_AMP_SET_OUTPUT		(1<<15)

/* Audio Widget Capabilities */
#define AC_WCAP_STEREO			(1<<0)	/* stereo I/O */
#define AC_WCAP_IN_AMP			(1<<1)	/* AMP-in present */
#define AC_WCAP_OUT_AMP			(1<<2)	/* AMP-out present */
#define AC_WCAP_AMP_OVRD		(1<<3)	/* AMP-parameter override */
#define AC_WCAP_FORMAT_OVRD		(1<<4)	/* format override */
#define AC_WCAP_STRIPE			(1<<5)	/* stripe */
#define AC_WCAP_PROC_WID		(1<<6)	/* Proc Widget */
#define AC_WCAP_UNSOL_CAP		(1<<7)	/* Unsol capable */
#define AC_WCAP_CONN_LIST		(1<<8)	/* connection list */
#define AC_WCAP_DIGITAL			(1<<9)	/* digital I/O */
#define AC_WCAP_POWER			(1<<10)	/* power control */
#define AC_WCAP_LR_SWAP			(1<<11)	/* L/R swap */
#define AC_WCAP_DELAY			(0xf<<16)
#define AC_WCAP_DELAY_SHIFT		16
#define AC_WCAP_TYPE			(0xf<<20)
#define AC_WCAP_TYPE_SHIFT		20

/* Pin widget capabilies */
#define AC_PINCAP_IMP_SENSE		(1<<0)	/* impedance sense capable */
#define AC_PINCAP_TRIG_REQ		(1<<1)	/* trigger required */
#define AC_PINCAP_PRES_DETECT		(1<<2)	/* presence detect capable */
#define AC_PINCAP_HP_DRV		(1<<3)	/* headphone drive capable */
#define AC_PINCAP_OUT			(1<<4)	/* output capable */
#define AC_PINCAP_IN			(1<<5)	/* input capable */
#define AC_PINCAP_BALANCE		(1<<6)	/* balanced I/O capable */
/* Note: This LR_SWAP pincap is defined in the Realtek ALC883 specification,
 *       but is marked reserved in the Intel HDA specification.
 */
#define AC_PINCAP_LR_SWAP		(1<<7)	/* L/R swap */
/* Note: The same bit as LR_SWAP is newly defined as HDMI capability
 *       in HD-audio specification
 */
#define AC_PINCAP_HDMI			(1<<7)	/* HDMI pin */
#define AC_PINCAP_VREF			(0x37<<8)
#define AC_PINCAP_VREF_SHIFT		8
#define AC_PINCAP_EAPD			(1<<16)	/* EAPD capable */
/* Vref status (used in pin cap) */
#define AC_PINCAP_VREF_HIZ		(1<<0)	/* Hi-Z */
#define AC_PINCAP_VREF_50		(1<<1)	/* 50% */
#define AC_PINCAP_VREF_GRD		(1<<2)	/* ground */
#define AC_PINCAP_VREF_80		(1<<4)	/* 80% */
#define AC_PINCAP_VREF_100		(1<<5)	/* 100% */

/* Pin widget control - 8bit */
#define AC_PINCTL_EPT			(0x3<<0)
#define AC_PINCTL_EPT_NATIVE		0
#define AC_PINCTL_EPT_HBR		3
#define AC_PINCTL_VREFEN		(0x7<<0)
#define AC_PINCTL_VREF_HIZ		0	/* Hi-Z */
#define AC_PINCTL_VREF_50		1	/* 50% */
#define AC_PINCTL_VREF_GRD		2	/* ground */
#define AC_PINCTL_VREF_80		4	/* 80% */
#define AC_PINCTL_VREF_100		5	/* 100% */
#define AC_PINCTL_IN_EN			(1<<5)
#define AC_PINCTL_OUT_EN		(1<<6)
#define AC_PINCTL_HP_EN			(1<<7)

/*
 * widget types
 */
enum {
	AC_WID_AUD_OUT,		/* Audio Out */
	AC_WID_AUD_IN,		/* Audio In */
	AC_WID_AUD_MIX,		/* Audio Mixer */
	AC_WID_AUD_SEL,		/* Audio Selector */
	AC_WID_PIN,		/* Pin Complex */
	AC_WID_POWER,		/* Power */
	AC_WID_VOL_KNB,		/* Volume Knob */
	AC_WID_BEEP,		/* Beep Generator */
	AC_WID_VENDOR = 0x0f	/* Vendor specific */
};

/*
 */

static const struct xhda_verb_table *
find_verb(unsigned int verb, const struct xhda_verb_table *tbl)
{
	for (; tbl->verb || tbl->func; tbl++)
		if (tbl->verb == verb)
			return tbl;
	return NULL;
}

/*
 */

static int set_stream_format(struct xhda_codec *codec, struct xhda_node *node,
			     unsigned int cmd)
{
	if (!node)
		return 0;
	node->stream_format = cmd & 0xffff;
	return 0;
}

static int get_stream_format(struct xhda_codec *codec, struct xhda_node *node,
			     unsigned int cmd)
{
	if (!node)
		return 0;
	return node->stream_format;
}

static int set_amp(struct xhda_node *node,
		   struct xhda_amp_vals *vals, unsigned int idx,
		   unsigned int ampcap, unsigned int val)
{
	unsigned int nsteps = (ampcap >> 8) & 0xff;
	unsigned int has_mute = (ampcap >> 31) & 1;
	unsigned int ampval;

	ampval = val & 0xff;
	if ((ampval & ~0x80) > nsteps) {
		hda_log(HDA_LOG_ERR, "invalid amp value 0x%x (nsteps = 0x%x), NID=0x%x\n",
			ampval, nsteps, node->nid);
		ampval = (ampval & 0x80) | (nsteps - 1);
	}
	if ((ampval & 0x80) && !has_mute) {
		hda_log(HDA_LOG_ERR, "turn on non-existing mute, NODE=0x%x\n",
			node->nid);
		ampval &= ~0x80;
	}
	if (val & AC_AMP_SET_LEFT)
		vals->vals[idx][0] = ampval;
	if (val & AC_AMP_SET_RIGHT)
		vals->vals[idx][1] = ampval;
	return 0;
}

static int par_amp_in_cap(struct xhda_codec *codec, struct xhda_node *node,
			  unsigned int cmd);
static int par_amp_out_cap(struct xhda_codec *codec, struct xhda_node *node,
			   unsigned int cmd);

static int set_amp_gain_mute(struct xhda_codec *codec, struct xhda_node *node,
			     unsigned int cmd)
{
	unsigned int ampval;
	unsigned int idx, type;

	if (!node)
		return 0;
	ampval = cmd & 0xffff;
	idx = (ampval & AC_AMP_SET_INDEX) >> AC_AMP_SET_INDEX_SHIFT;
	type = (node->wcaps & AC_WCAP_TYPE) >> AC_WCAP_TYPE_SHIFT;
	if (ampval & AC_AMP_SET_OUTPUT) {
		if (!(node->wcaps & AC_WCAP_OUT_AMP))
			hda_log(HDA_LOG_ERR, "no output-amp for node 0x%x\n",
				node->nid);
		if (idx) {
			if (type != AC_WID_PIN ||
			    !codec->pin_amp_workaround) {
				hda_log(HDA_LOG_ERR,
					"invalid amp index %d for output\n",
					idx);
				idx = 0;
			} else if (idx >= node->num_nodes) {
				hda_log(HDA_LOG_ERR,
					"invalid pin out-amp index %d "
					"(conns=%d)\n",
					idx, node->num_nodes);
				idx = 0;
			}
		}
		set_amp(node, &node->amp_out_vals, idx,
			par_amp_out_cap(codec, node, 0),
			ampval);
	}
	if (ampval & AC_AMP_SET_INPUT) {
		if (!(node->wcaps & AC_WCAP_IN_AMP))
			hda_log(HDA_LOG_ERR, "no input-amp for node 0x%x\n",
				node->nid);
		type = (node->wcaps & AC_WCAP_TYPE) >> AC_WCAP_TYPE_SHIFT;
		if ((type == AC_WID_PIN && idx != 0) ||
		    (type != AC_WID_PIN && idx >= node->num_nodes)) {
			hda_log(HDA_LOG_ERR,
				"invalid amp index %d (conns=%d)\n", idx,
				node->num_nodes);
			idx = 0;
		}
		set_amp(node, &node->amp_in_vals, idx,
			par_amp_in_cap(codec, node, 0),
			ampval);
	}
	return 0;
}

static int get_amp(struct xhda_amp_vals *vals, unsigned int idx,
		   unsigned int ampval)
{
	if (ampval & AC_AMP_GET_LEFT)
		return vals->vals[idx][0];
	else
		return vals->vals[idx][1];
}

static int get_amp_gain_mute(struct xhda_codec *codec, struct xhda_node *node,
			     unsigned int cmd)
{
	unsigned int ampval;
	unsigned int idx, type;

	if (!node)
		return 0;
	ampval = cmd & 0xffff;
	idx = ampval & 0x1f;
	type = (node->wcaps & AC_WCAP_TYPE) >> AC_WCAP_TYPE_SHIFT;
	if (ampval & AC_AMP_GET_OUTPUT) {
		if (!(node->wcaps & AC_WCAP_OUT_AMP))
			hda_log(HDA_LOG_ERR, "no output-amp for node 0x%x\n",
				node->nid);
		if (idx) {
			if (type != AC_WID_PIN ||
			    !codec->pin_amp_workaround) {
				hda_log(HDA_LOG_ERR,
					"invalid amp index %d for output\n",
					idx);
				idx = 0;
			} else if (idx >= node->num_nodes) {
				hda_log(HDA_LOG_ERR,
					"invalid pin out-amp index %d "
					"(conns=%d)\n",
					idx, node->num_nodes);
				idx = 0;
			}
		}
		return get_amp(&node->amp_out_vals, idx, ampval);
	} else {
		if (!(node->wcaps & AC_WCAP_IN_AMP))
			hda_log(HDA_LOG_ERR, "no input-amp for node 0x%x\n",
				node->nid);
		if ((type == AC_WID_PIN && idx != 0) ||
		    (type != AC_WID_PIN && idx >= node->num_nodes)) {
			hda_log(HDA_LOG_ERR,
				"invalid amp index %d (conns=%d)\n", idx,
				node->num_nodes);
			idx = 0;
		}
		return get_amp(&node->amp_in_vals, idx, ampval);
	}
}

static int set_connect_sel(struct xhda_codec *codec, struct xhda_node *node,
			   unsigned int cmd)
{
	unsigned int wid_type;
	unsigned int sel;

	if (!node)
		return 0;
	wid_type = (node->wcaps & AC_WCAP_TYPE) >> AC_WCAP_TYPE_SHIFT;
	if (wid_type == AC_WID_AUD_MIX || wid_type == AC_WID_VOL_KNB) {
		hda_log(HDA_LOG_ERR, "invalid connect select for node 0x%x\n",
			node->nid);
		return 0;
	}
	sel = cmd & 0xff;
	if (sel >= node->num_nodes)
		hda_log(HDA_LOG_ERR,
			"invalid connection index %d (conns=%d), NID=0x%x\n",
			sel, node->num_nodes, node->nid);
	else
		node->curr_conn = sel;
	return 0;
}

static int get_connect_sel(struct xhda_codec *codec, struct xhda_node *node,
			   unsigned int cmd)
{
	if (!node)
		return 0;
	return node->curr_conn;
}

static int get_connect_list(struct xhda_codec *codec, struct xhda_node *node,
			    unsigned int cmd)
{
	unsigned int idx, i;
	unsigned int val;
	if (!node)
		return 0;
	idx = cmd & 0xff;
	if (idx >= node->num_nodes)
		return 0;
	val = 0;
	for (i = 0; i < 4; i++, idx++) {
		if (idx >= node->num_nodes)
			break;
		val |= node->node[idx] << (i * 8);
	}
	return val;
}

static int set_proc_state(struct xhda_codec *codec, struct xhda_node *node,
			  unsigned int cmd)
{
	node->proc_state = cmd & 0xff;
	return 0;
}

static int get_proc_state(struct xhda_codec *codec, struct xhda_node *node,
			  unsigned int cmd)
{
	return node->proc_state;
}

static int set_power_state(struct xhda_codec *codec, struct xhda_node *node,
			   unsigned int cmd)
{
	if (!node)
		return 0;
	if (node->nid != 0x01 &&
	    !(node->wcaps & AC_WCAP_POWER))
		return 0;
	node->power_setting = cmd & 0x0f;
	node->power_current = node->power_setting;
	return 0;
}

static int get_power_state(struct xhda_codec *codec, struct xhda_node *node,
			   unsigned int cmd)
{
	if (!node)
		return 0;
	return node->power_setting | (node->power_current << 4);
}

static int set_sdi_select(struct xhda_codec *codec, struct xhda_node *node,
			  unsigned int cmd)
{
	node->sdi = cmd & 0x0f;
	return 0;
}

static int get_sdi_select(struct xhda_codec *codec, struct xhda_node *node,
			  unsigned int cmd)
{
	return node->sdi;
}

static int set_channel_streamid(struct xhda_codec *codec,
				struct xhda_node *node, unsigned int cmd)
{
	if (!node)
		return 0;
	node->streamid = cmd & 0xff;
	return 0;
}

static int get_channel_streamid(struct xhda_codec *codec,
				struct xhda_node *node, unsigned int cmd)
{
	if (!node)
		return 0;
	return node->streamid;
}

void hda_verify_pin_ctl(struct xhda_node *node, int log,
			unsigned int *sanified_pinctl)
{
	int pinctl = node->pinctl;

	/* sanity checks */
	if ((node->pinctl & AC_PINCTL_OUT_EN) &&
	    !(node->pincap & AC_PINCAP_OUT)) {
		if (log)
			hda_log(HDA_LOG_ERR,
				"setting OUT_EN to pin 0x%x without caps\n",
				node->nid);
		pinctl &= ~AC_PINCTL_OUT_EN;
	}
	if ((node->pinctl & AC_PINCTL_HP_EN) &&
	    !(node->pincap & AC_PINCAP_HP_DRV)) {
		if (log)
			hda_log(HDA_LOG_ERR,
				"setting HP_EN to pin 0x%x without caps\n",
				node->nid);
		pinctl &= ~AC_PINCTL_HP_EN;
	}
	if ((node->pinctl & AC_PINCTL_IN_EN) &&
	    !(node->pincap & AC_PINCAP_IN)) {
		if (log)
			hda_log(HDA_LOG_ERR,
				"setting IN_EN to pin 0x%x without caps\n",
				node->nid);
		pinctl &= ~AC_PINCTL_IN_EN;
	}

	if (node->pinctl & AC_PINCTL_IN_EN) {
		unsigned int cap_set, cap_check;
		const char *vref;
		switch (node->pinctl & AC_PINCTL_VREFEN) {
		case AC_PINCTL_VREF_HIZ:
			cap_check = AC_PINCAP_VREF_HIZ;
			vref = "HIZ";
			break;
		case AC_PINCTL_VREF_50:
			cap_check = AC_PINCAP_VREF_50;
			vref = "50";
			break;
		case AC_PINCTL_VREF_GRD:
			cap_check = AC_PINCAP_VREF_GRD;
			vref = "GRD";
			break;
		case AC_PINCTL_VREF_80:
			cap_check = AC_PINCAP_VREF_80;
			vref = "80";
			break;
		case AC_PINCTL_VREF_100:
			cap_check = AC_PINCAP_VREF_100;
			vref = "100";
			break;
		default:
			cap_check = 0;
			break;
		}
		cap_set = (node->pincap & AC_PINCAP_VREF) >> AC_PINCAP_VREF_SHIFT;
		if (!cap_set)
			cap_set = AC_PINCAP_VREF_HIZ;
		if (cap_check && !(cap_set & cap_check)) {
			if (log)
				hda_log(HDA_LOG_ERR,
					"setting VREF %s to pin 0x%x without caps 0x%x\n",
					vref, node->nid, node->pincap);
			pinctl &= ~AC_PINCTL_VREFEN;
		}
	}

	if (sanified_pinctl)
		*sanified_pinctl = pinctl;
}

static int set_pin_ctl(struct xhda_codec *codec, struct xhda_node *node,
		       unsigned int cmd)
{
	if (!node)
		return 0;
	node->pinctl = cmd & 0xff;

	hda_verify_pin_ctl(node, 1, NULL);

	return 0;
}

static int get_pin_ctl(struct xhda_codec *codec, struct xhda_node *node,
		       unsigned int cmd)
{
	if (!node)
		return 0;
	return node->pinctl;
}

static int set_unsol_enable(struct xhda_codec *codec, struct xhda_node *node,
			    unsigned int cmd)
{
	if (!node)
		return 0;
	node->unsol = cmd & 0xff;
	return 0;
}

static int get_unsol_resp(struct xhda_codec *codec, struct xhda_node *node,
			  unsigned int cmd)
{
	if (!node) {
		hda_log(HDA_LOG_ERR, "Invalid unsol widget");
		return 0;
	}
	return node->unsol;
}

static int exec_pin_sense(struct xhda_codec *codec, struct xhda_node *node,
			  unsigned int cmd)
{
	return 0;
}

static int get_pin_sense(struct xhda_codec *codec, struct xhda_node *node,
			  unsigned int cmd)
{
	if (!node) {
		hda_log(HDA_LOG_ERR, "Invalid pin node\n");
		return 0;
	}
	return node->jack_state ? (1 << 31) : 0;
}

static int set_eapd_btl(struct xhda_codec *codec, struct xhda_node *node,
			unsigned int cmd)
{
	if (!node) {
		hda_log(HDA_LOG_ERR, "Invalid EAPD node\n");
		return 0;
	}
	node->eapd = cmd & 0xff;
	if ((cmd & 0x02) && !(node->pincap & AC_PINCAP_EAPD))
		hda_log(HDA_LOG_ERR, "EAPD command to non-capable pin 0x%x\n",
			node->nid);
	return 0;
}

static int get_eapd_btl(struct xhda_codec *codec, struct xhda_node *node,
			unsigned int cmd)
{
	if (!node) {
		hda_log(HDA_LOG_ERR, "Invalid EAPD node\n");
		return 0;
	}
	return node->eapd;
}

static int set_digi_cvt_1(struct xhda_codec *codec, struct xhda_node *node,
			  unsigned int cmd)
{
	if (!node) {
		hda_log(HDA_LOG_ERR, "Invalid DIGI1 node\n");
		return 0;
	}
	node->dig_conv = cmd & 0xff;
	return 0;
}

static int set_digi_cvt_2(struct xhda_codec *codec, struct xhda_node *node,
			  unsigned int cmd)
{
	if (!node) {
		hda_log(HDA_LOG_ERR, "Invalid DIGI2 node\n");
		return 0;
	}
	node->dig_category = cmd & 0xff;
	return 0;
}

static int get_digi_cvt(struct xhda_codec *codec, struct xhda_node *node,
			unsigned int cmd)
{
	if (!node) {
		hda_log(HDA_LOG_ERR, "Invalid DIGI node\n");
		return 0;
	}
	return (node->dig_category << 8) | node->dig_conv;
}

static int set_config_def_0(struct xhda_codec *codec, struct xhda_node *node,
			    unsigned int cmd)
{
	if (!node)
		return 0;
	node->pin_default &= ~0xff;
	node->pin_default |= (cmd & 0xff);
	return 0;
}

static int set_config_def_1(struct xhda_codec *codec, struct xhda_node *node,
			    unsigned int cmd)
{
	if (!node)
		return 0;
	node->pin_default &= ~0xff00;
	node->pin_default |= (cmd & 0xff) << 8;
	return 0;
}

static int set_config_def_2(struct xhda_codec *codec, struct xhda_node *node,
			    unsigned int cmd)
{
	if (!node)
		return 0;
	node->pin_default &= ~0xff0000;
	node->pin_default |= (cmd & 0xff) << 16;
	return 0;
}

static int set_config_def_3(struct xhda_codec *codec, struct xhda_node *node,
			    unsigned int cmd)
{
	if (!node)
		return 0;
	node->pin_default &= ~0xff000000;
	node->pin_default |= (cmd & 0xff) << 24;
	return 0;
}

static int get_config_default(struct xhda_codec *codec, struct xhda_node *node,
			unsigned int cmd)
{
	if (!node)
		return 0;
	return node->pin_default;
}

static int get_ssid(struct xhda_codec *codec, struct xhda_node *node,
		    unsigned int cmd)
{
	return codec->subsystem_id;
}

static int set_codec_reset(struct xhda_codec *codec, struct xhda_node *node,
			   unsigned int cmd)
{
	hda_log(HDA_LOG_INFO, "CODEC RESET\n");
	return 0;
}

static int set_coef_index(struct xhda_codec *codec, struct xhda_node *node,
			  unsigned int cmd)
{
	node->coef_idx = cmd & 0xffff;
	return 0;
}

static int get_coef_index(struct xhda_codec *codec, struct xhda_node *node,
			  unsigned int cmd)
{
	return node->coef_idx;
}

static struct xhda_coef_table *find_proc_coef(struct xhda_node *node,
					      unsigned int idx)
{
	struct xhda_coef_table *tbl;
	for (tbl = node->coef_tbl; tbl; tbl = tbl->next) {
		if (tbl->idx == idx)
			return tbl;
	}
	return NULL;
}

static struct xhda_coef_table *create_proc_coef(struct xhda_node *node,
						unsigned int idx)
{
	struct xhda_coef_table *tbl;
	tbl = xalloc(sizeof(*tbl));
	tbl->idx = idx;
	tbl->next = node->coef_tbl;
	node->coef_tbl = tbl;
	return tbl;
}

static int set_proc_coef(struct xhda_codec *codec, struct xhda_node *node,
			 unsigned int cmd)
{
	struct xhda_coef_table *tbl;
	tbl = find_proc_coef(node, node->coef_idx);
	if (!tbl) {
		tbl = create_proc_coef(node, node->coef_idx);
		if (!tbl)
			return 0;
	}
	tbl->value = cmd & 0xffff;
	node->coef_idx++;
	return 0;
}

static int get_proc_coef(struct xhda_codec *codec, struct xhda_node *node,
			 unsigned int cmd)
{
	struct xhda_coef_table *tbl;
	tbl = find_proc_coef(node, node->coef_idx);
	node->coef_idx++;
	return tbl ? tbl->value : 0;
}

static int set_beep_ctl(struct xhda_codec *codec, struct xhda_node *node,
			unsigned int cmd)
{
	node->beep_div = cmd & 0xff;
	return 0;
}

static int get_beep_ctl(struct xhda_codec *codec, struct xhda_node *node,
			unsigned int cmd)
{
	return node->beep_div;
}

static int set_volknob_ctl(struct xhda_codec *codec, struct xhda_node *node,
			unsigned int cmd)
{
	node->volknob_ctl = cmd & 0xff;
	return 0;
}

static int get_volknob_ctl(struct xhda_codec *codec, struct xhda_node *node,
			unsigned int cmd)
{
	return node->volknob_ctl;
}

static int set_gpio_data(struct xhda_codec *codec, struct xhda_node *node,
			 unsigned int cmd)
{
	node->gpio_data = cmd & 0xff;
	return 0;
}

static int get_gpio_data(struct xhda_codec *codec, struct xhda_node *node,
			 unsigned int cmd)
{
	return node->gpio_data;
}

static int set_gpio_mask(struct xhda_codec *codec, struct xhda_node *node,
			 unsigned int cmd)
{
	node->gpio_mask = cmd & 0xff;
	return 0;
}

static int get_gpio_mask(struct xhda_codec *codec, struct xhda_node *node,
			 unsigned int cmd)
{
	return node->gpio_mask;
}

static int set_gpio_dir(struct xhda_codec *codec, struct xhda_node *node,
			unsigned int cmd)
{
	node->gpio_dir = cmd & 0xff;
	return 0;
}

static int get_gpio_dir(struct xhda_codec *codec, struct xhda_node *node,
			unsigned int cmd)
{
	return node->gpio_dir;
}

static int set_gpio_wake_mask(struct xhda_codec *codec, struct xhda_node *node,
			      unsigned int cmd)
{
	node->gpio_wake = cmd & 0xff;
	return 0;
}

static int get_gpio_wake_mask(struct xhda_codec *codec, struct xhda_node *node,
			      unsigned int cmd)
{
	return node->gpio_wake;
}

static int set_gpio_unsol_rsp(struct xhda_codec *codec, struct xhda_node *node,
			      unsigned int cmd)
{
	node->gpio_unsol = cmd & 0xff;
	return 0;
}

static int get_gpio_unsol_rsp(struct xhda_codec *codec, struct xhda_node *node,
			      unsigned int cmd)
{
	return node->gpio_unsol;
}

static int set_gpio_sticky_mask(struct xhda_codec *codec,
				struct xhda_node *node, unsigned int cmd)
{
	node->gpio_sticky = cmd & 0xff;
	return 0;
}

static int get_gpio_sticky_mask(struct xhda_codec *codec,
				struct xhda_node *node, unsigned int cmd)
{
	return node->gpio_sticky;
}

static int set_cvt_channel_count(struct xhda_codec *codec,
				struct xhda_node *node, unsigned int cmd)
{
	node->cvt_channel_count = cmd & 0xff;
	return 0;
}

static int get_cvt_channel_count(struct xhda_codec *codec,
				struct xhda_node *node, unsigned int cmd)
{
	return node->cvt_channel_count;
}

static int get_asp_channel_slot(struct xhda_codec *codec,
				struct xhda_node *node, unsigned int cmd)
{
	return (node->asp_channel_slot[cmd & 0xf] << 4) + (cmd & 0xf);
}

static int set_asp_channel_slot(struct xhda_codec *codec,
				struct xhda_node *node, unsigned int cmd)
{
	node->asp_channel_slot[cmd & 0xf] = (cmd & 0xf0) >> 4;
	return 0;
}

static int set_dip_index(struct xhda_codec *codec,
			 struct xhda_node *node, unsigned int cmd)
{
	return 0; /* FIXME */
}

static int set_dip_data(struct xhda_codec *codec,
			struct xhda_node *node, unsigned int cmd)
{
	return 0; /* FIXME */
}

static int get_dip_size(struct xhda_codec *codec,
			struct xhda_node *node, unsigned int cmd)
{
	if (cmd & 0x8)
		return 128; /* ELD buffer size */
	return 32; /* DIP buffer size */
}

static int set_dip_xmitctrl(struct xhda_codec *codec,
			    struct xhda_node *node, unsigned int cmd)
{
	node->dip_xmitctrl = cmd & 0xc0;
	return 0;
}

static int get_dip_xmitctrl(struct xhda_codec *codec,
			    struct xhda_node *node, unsigned int cmd)
{
	return node->dip_xmitctrl;
}


/*
 * parameters
 */
static int par_vendor_id(struct xhda_codec *codec, struct xhda_node *node,
			 unsigned int cmd)
{
	return codec->vendor_id;
}	

static int par_subsystem_id(struct xhda_codec *codec, struct xhda_node *node,
			    unsigned int cmd)
{
	return codec->subsystem_id;
}	

static int par_revision_id(struct xhda_codec *codec, struct xhda_node *node,
			   unsigned int cmd)
{
	return codec->revision_id;
}	

static int par_node_count(struct xhda_codec *codec, struct xhda_node *node,
			  unsigned int cmd)
{
	unsigned int start_nid;
	if (!node)
		return codec->num_widgets | (0x01 << 16);
	if (!codec->afg.next)
		return 0;
	start_nid = codec->afg.next->nid;
	return (codec->num_widgets - start_nid + 1) | (start_nid << 16);
}

static int par_function_type(struct xhda_codec *codec, struct xhda_node *node,
			     unsigned int cmd)
{
	if (node) {
		if (!codec->function_id && node->nid == 0x01)
			return 0x01; /* FIXME */
		if (node->nid == codec->afg.nid)
			return codec->function_id ? codec->function_id : 0x01;
		if (node->nid == codec->mfg.nid)
			return codec->modem_function_id ? codec->modem_function_id : 0x02;
	}
	return 0;
}

static int par_fg_cap(struct xhda_codec *codec, struct xhda_node *node,
		      unsigned int cmd)
{
	return 0; /* FIXME */
}

static int par_audio_widget_cap(struct xhda_codec *codec,
				struct xhda_node *node, unsigned int cmd)
{
	if (!node)
		return 0;
	return node->wcaps;
}

static int par_pcm(struct xhda_codec *codec, struct xhda_node *node,
		   unsigned int cmd)
{
	if (!node)
		return 0;
	return node->pcm.rates | (node->pcm.bits << 16);
}

static int par_stream(struct xhda_codec *codec, struct xhda_node *node,
		      unsigned int cmd)
{
	if (!node)
		return 0;
	return node->pcm.formats;
}

static int par_pin_cap(struct xhda_codec *codec, struct xhda_node *node,
		       unsigned int cmd)
{
	if (!node)
		return 0;
	return node->pincap;
}

static int par_amp_cap(struct xhda_amp_caps *node_cap,
		       struct xhda_amp_caps *afg_cap)
{
	if (node_cap->override)
		return node_cap->ofs |
			(node_cap->nsteps << 8) |
			(node_cap->stepsize << 16) |
			(node_cap->mute << 31);
	else
		return afg_cap->ofs |
			(afg_cap->nsteps << 8) |
			(afg_cap->stepsize << 16) |
			(afg_cap->mute << 31);
}

static int par_amp_in_cap(struct xhda_codec *codec, struct xhda_node *node,
			  unsigned int cmd)
{
	if (!node)
		return 0;
	return par_amp_cap(&node->amp_in_caps, &codec->afg.amp_in_caps);
}

static int par_connlist_len(struct xhda_codec *codec, struct xhda_node *node,
			    unsigned int cmd)
{
	if (!node)
		return 0;
	return node->num_nodes;
}

static int par_power_state(struct xhda_codec *codec, struct xhda_node *node,
			   unsigned int cmd)
{
	if (!node)
		return 0;
	if (node->nid == 0x01)
		return 0x0f;
	if (node->wcaps & AC_WCAP_POWER)
		return 0x0f;
	return 0; /* FIXME */
}

static int par_proc_cap(struct xhda_codec *codec, struct xhda_node *node,
			unsigned int cmd)
{
	if (!node)
		return 0;
	return node->coef_benign | (node->coef_num << 8);
}

static int par_gpio_cap(struct xhda_codec *codec, struct xhda_node *node,
			unsigned int cmd)
{
	return node->gpio_cap;
}

static int par_amp_out_cap(struct xhda_codec *codec, struct xhda_node *node,
			   unsigned int cmd)
{
	if (!node)
		return 0;
	return par_amp_cap(&node->amp_out_caps, &codec->afg.amp_out_caps);
}

static int par_vol_knb_cap(struct xhda_codec *codec, struct xhda_node *node,
			   unsigned int cmd)
{
	return 0; /* FIXME */
}

static struct xhda_verb_table par_tbl[] = {
	{ 0x00, par_vendor_id, "vendor id" },
	{ 0x01, par_subsystem_id, "subsystem_id"  },
	{ 0x02, par_revision_id, "revision_id" },
	{ 0x04, par_node_count, "node_count" },
	{ 0x05, par_function_type, "function_type" },
	{ 0x08, par_fg_cap, "FG_cap" },
	{ 0x09, par_audio_widget_cap, "audio_wid_cap" },
	{ 0x0a, par_pcm, "PCM" },
	{ 0x0b, par_stream, "stream" },
	{ 0x0c, par_pin_cap, "pin_cap" },
	{ 0x0d, par_amp_in_cap, "amp_in_cap" },
	{ 0x0e, par_connlist_len, "connect_len" },
	{ 0x0f, par_power_state, "power_state" },
	{ 0x10, par_proc_cap, "proc_cap" },
	{ 0x11, par_gpio_cap, "GPIO_cap" },
	{ 0x12, par_amp_out_cap, "amp_out_cap" },
	{ 0x13, par_vol_knb_cap, "volknob_cap" },
	{}
};

static const struct xhda_verb_table *
find_matching_param(struct xhda_codec *codec, unsigned int parm)
{
	const struct xhda_verb_table *tbl;
	tbl = find_verb(parm, par_tbl);
	if (!tbl && codec && codec->extended_parameters)
		tbl = find_verb(parm, codec->extended_parameters);
	return tbl;
}

static int get_parameters(struct xhda_codec *codec, struct xhda_node *node,
			  unsigned int cmd)
{
	const struct xhda_verb_table *tbl;

	tbl = find_matching_param(codec, cmd & 0xff);
	if (tbl && tbl->func)
		return tbl->func(codec, node, cmd);
	return 0;
}


/*
 */

static struct xhda_verb_table verb_class[] = {
	{ 2, set_stream_format, "set_stream_format" },
	{ 3, set_amp_gain_mute, "set_amp_gain_mute" },
	{ 4, set_proc_coef, "set_proc_coef" },
	{ 5, set_coef_index, "set_coef_index" },
	{ 10, get_stream_format, "get_stream_format" },
	{ 11, get_amp_gain_mute, "get_amp_gain_mute" },
	{ 12, get_proc_coef, "get_proc_coef" },
	{ 13, get_coef_index, "get_coef_index" },
	{}
};

static struct xhda_verb_table verb_tbl[] = {
	{ 0x701, set_connect_sel, "set_connect_sel" },
	{ 0x703, set_proc_state, "set_proc_state" },
	{ 0x704, set_sdi_select, "set_sdi_select" },
	{ 0x705, set_power_state, "set_power_state" },
	{ 0x706, set_channel_streamid, "set_channel_streamid" },
	{ 0x707, set_pin_ctl, "set_pin_ctl" },
	{ 0x708, set_unsol_enable, "set_unsol_enable" },
	{ 0x709, exec_pin_sense, "exec_pin_sense" },
	{ 0x70a, set_beep_ctl, "set_beep_ctl" },
	{ 0x70c, set_eapd_btl, "set_eapd_btl" },
	{ 0x70d, set_digi_cvt_1, "set_digi_cvt_1" },
	{ 0x70e, set_digi_cvt_2, "set_digi_cvt_2" },
	{ 0x70f, set_volknob_ctl, "set_volknob_ctl" },
	{ 0x715, set_gpio_data, "set_gpio_data" },
	{ 0x716, set_gpio_mask, "set_gpio_mask" },
	{ 0x717, set_gpio_dir, "set_gpio_dir" },
	{ 0x718, set_gpio_wake_mask, "set_gpio_wake_mask" },
	{ 0x719, set_gpio_unsol_rsp, "set_gpio_unsol_rsp" },
	{ 0x71a, set_gpio_sticky_mask, "set_gpio_sticky_mask" },
	{ 0x71c, set_config_def_0, "set_config_def_0" },
	{ 0x71d, set_config_def_1, "set_config_def_1" },
	{ 0x71e, set_config_def_2, "set_config_def_2" },
	{ 0x71f, set_config_def_3, "set_config_def_3" },
	{ 0x72d, set_cvt_channel_count, "set_cvt_channel_count" },
	{ 0x730, set_dip_index, "set_dip_index" },
	{ 0x731, set_dip_data, "set_dip_data" },
	{ 0x732, set_dip_xmitctrl, "set_dip_xmitctrl" },
	{ 0x734, set_asp_channel_slot, "set_asp_channel_slot" },
	{ 0x7ff, set_codec_reset, "set_codec_reset" },
	{ 0xf00, get_parameters, "get_parameters" },
	{ 0xf01, get_connect_sel, "get_connect_sel" },
	{ 0xf02, get_connect_list, "get_connect_list" },
	{ 0xf03, get_proc_state, "get_proc_state" },
	{ 0xf04, get_sdi_select, "get_sdi_select" },
	{ 0xf05, get_power_state, "get_power_state" },
	{ 0xf06, get_channel_streamid, "get_channel_streamid" },
	{ 0xf07, get_pin_ctl, "get_pin_ctl" },
	{ 0xf08, get_unsol_resp, "get_unsol_resp" },
	{ 0xf09, get_pin_sense, "get_pin_sense" },
	{ 0xf0a, get_beep_ctl, "get_beep_ctl" },
	{ 0xf0c, get_eapd_btl, "get_eapd_btl" },
	{ 0xf0d, get_digi_cvt, "get_digi_cvt" },
	{ 0xf0f, get_volknob_ctl, "get_volknob_ctl" },
	{ 0xf15, get_gpio_data, "get_gpio_data" },
	{ 0xf16, get_gpio_mask, "get_gpio_mask" },
	{ 0xf17, get_gpio_dir, "get_gpio_dir" },
	{ 0xf18, get_gpio_wake_mask, "get_gpio_wake_mask" },
	{ 0xf19, get_gpio_unsol_rsp, "get_gpio_unsol_rsp" },
	{ 0xf1a, get_gpio_sticky_mask, "get_gpio_sticky_mask" },
	{ 0xf1c, get_config_default, "get_config_default" },
	{ 0xf20, get_ssid, "get_ssid" },
	{ 0xf2d, get_cvt_channel_count, "get_cvt_channel_count" },
	{ 0xf2e, get_dip_size, "get_dip_size" },
	{ 0xf32, get_dip_xmitctrl, "get_dip_xmitctrl" },
	{ 0xf34, get_asp_channel_slot, "get_asp_channel_slot" },
	{}
};

static const struct xhda_verb_table *
find_matching_verb(struct xhda_codec *codec, unsigned int verb)
{
	const struct xhda_verb_table *tbl;
	tbl = find_verb((verb >> 8) & 0xf, verb_class);
	if (!tbl) {
		tbl = find_verb(verb, verb_tbl);
		if (!tbl && codec && codec->extended_verbs)
			tbl = find_verb(verb, codec->extended_verbs);
	}
	return tbl;
}

static struct xhda_node *find_node(struct xhda_codec *codec, unsigned int nid)
{
	struct xhda_node *node;

	for (node = &codec->afg; node; node = node->next)
		if (node->nid == nid)
			return node;
	if (codec->mfg.nid) {
		for (node = &codec->mfg; node; node = node->next)
			if (node->nid == nid)
				return node;
	}
	return NULL;
}

int hda_cmd(struct xhda_codec *codec, unsigned int cmd)
{
	const struct xhda_verb_table *tbl;
	struct xhda_node *node;
	unsigned int nid = (cmd >> 20) & 0x7f;
	unsigned int verb = (cmd >> 8) & 0xfff;

	if (codec->extended_cmd) {
		int r = codec->extended_cmd(codec, cmd);
		if (r != -ENXIO)
			return r;
	}

	tbl = find_matching_verb(codec, verb);
	if (!tbl)
		return -ENXIO;
	if (!tbl->func)
		return 0;
	if (!nid)
		node = NULL;
	else {
		node = find_node(codec, nid);
		if (!node)
			return -EINVAL;
	}
	codec->rc = tbl->func(codec, node, cmd);
	return 0;
}

int hda_get_jack_state(struct xhda_codec *codec, int nid)
{
	struct xhda_node *node = find_node(codec, nid);
	if (node)
		return node->jack_state;
	return -1;
}

int hda_set_jack_state(struct xhda_codec *codec, int nid, int val)
{
	struct xhda_node *node = find_node(codec, nid);
	if (!node)
		return -1;
	val = !!val;
	if (node->jack_state != val) {
		node->jack_state = val;
		return 1;
	}
	return 0;
}

int hda_get_unsol_state(struct xhda_codec *codec, int nid)
{
	struct xhda_node *node = find_node(codec, nid);
	if (!node)
		return 0;
	return node->unsol;
}

const char *get_verb_name(struct xhda_codec *codec, unsigned int cmd)
{
	unsigned int verb = (cmd >> 8) & 0xfff;
	const struct xhda_verb_table *tbl;

	tbl = find_matching_verb(codec, verb);
	return (tbl && tbl->name) ? tbl->name : "unknown";
}

const char *get_parameter_name(struct xhda_codec *codec, unsigned int cmd)
{
	const struct xhda_verb_table *tbl;

	tbl = find_matching_param(codec, cmd & 0xff);
	return (tbl && tbl->name) ? tbl->name : "unknown";
}

void hda_set_proc_coef(struct xhda_codec *codec, int nid, int idx, int val)
{
	struct xhda_node *node = find_node(codec, nid);
	struct xhda_coef_table *tbl;
	if (!node) {
		hda_log(HDA_LOG_ERR, "Invalid NID 0x%x for proc\n", nid);
		return;
	}
	tbl = find_proc_coef(node, idx);
	if (!tbl) {
		tbl = create_proc_coef(node, idx);
		if (!tbl)
			return;
	}
	tbl->value = val;
	hda_log(HDA_LOG_INFO, "Set static coef idx 0x%x val 0x%x for NID 0x%x\n",
		tbl->idx, tbl->value, nid);
}

/*
 * for hda-decode-verb
 */

unsigned int hda_decode_verb_parm(struct xhda_codec *codec,
				  unsigned int verb, unsigned int parm)
{
	hda_log(HDA_LOG_INFO, "raw value: verb = 0x%x, parm = 0x%x\n",
		verb, parm);
	hda_log(HDA_LOG_INFO, "verbname = %s\n",
		get_verb_name(codec, verb << 8));
	if ((verb >> 8) == 0x03) { /* set_amp_gain_mute */
		unsigned int ampval = verb & 0xff;
		ampval = (ampval << 8) | parm;
		hda_log(HDA_LOG_INFO, "amp raw val = 0x%x\n", ampval);
		hda_log(HDA_LOG_INFO, "%s%s%s%sidx=%d, mute=%d, val=%d\n",
			((ampval >> 15) & 1) ? "output, " : "",
			((ampval >> 14) & 1) ? "input, " : "",
			((ampval >> 13) & 1) ? "left, " : "",
			((ampval >> 12) & 1) ? "right, " : "",
			(ampval >> 8) & 0xf,
			(ampval >> 7) & 1,
			ampval & 0x7f);
	}

	if ((verb >> 8) == 0x0b) { /* get_amp_gain_mute */
		unsigned int ampval = verb & 0xff;
		ampval = (ampval << 8) | parm;
		hda_log(HDA_LOG_INFO, "amp raw val = 0x%x\n", ampval);
		hda_log(HDA_LOG_INFO, "%s, %s, idx=%d\n",
			((ampval >> 15) & 1) ? "output" : "input",
			((ampval >> 13) & 1) ? "left" : "right",
			ampval & 0xff);
	}

	if (verb == 0xf00) {
		hda_log(HDA_LOG_INFO,
			"parameter = %s\n", get_parameter_name(codec, parm));
	}
	return 0;
}

static int strmatch(const char *a, const char *b)
{
	for (; *a && *b; a++, b++) {
		if (toupper(*a) != toupper(*b))
			return 0;
	}
	return 1;
}

/*
 * for hda-encode-verb
 */

static const struct xhda_verb_table *
lookup_verb_name(const char *name, const struct xhda_verb_table *tbl)
{
	for (; tbl->verb || tbl->func; tbl++) {
		if (strmatch(name, tbl->name))
			return tbl;
	}
	return NULL;
}

int hda_encode_verb_parm(const char *verb, const char *parm,
			 unsigned int *verb_ret, unsigned int *parm_ret)
{
	const struct xhda_verb_table *tbl;

	tbl = lookup_verb_name(verb, verb_class);
	if (tbl)
		*verb_ret = tbl->verb << 8;
	else {
		tbl = lookup_verb_name(verb, verb_tbl);
		if (tbl)
			*verb_ret = tbl->verb;
		else {
			errno = 0;
			*verb_ret = strtoul(verb, NULL, 0);
			if (errno)
				return -errno;
		}
	}

	tbl = lookup_verb_name(parm, par_tbl);
	if (tbl)
		*parm_ret = tbl->verb;
	else {
		errno = 0;
		*parm_ret = strtoul(parm, NULL, 0);
		if (errno)
			return -errno;
	}
	return 0;
}

/*
 * routing
 */

static struct xhda_route_list *create_route(struct xhda_node **route,
					    int depth, int revert)
{
	struct xhda_route_list *list = xalloc(sizeof(*list));
	int i;

	list->num_nodes = depth;
	for (i = 0; i < depth; i++) {
		int j = revert ? (depth - i - 1) : i;
		list->node[j] = route[i];
	}
	return list;
}

static int is_active_pin(struct xhda_node *node, unsigned int ctl_bit)
{
	unsigned int cfg;
	cfg = node->pin_default >> 30;
	if (cfg == 1)
		return 0;
	cfg = node->pinctl;
	if (!(cfg & ctl_bit))
		return 0;
	return 1;
}

static struct xhda_route_list *
routes_connected_to(struct xhda_codec *codec, struct xhda_node *node,
		    int depth, struct xhda_node **route,
		    unsigned int flags, struct xhda_route_list *list)
{
	int i, type;

	if (depth > MAX_ROUTE_DEPTH)
		return list;
	route[depth] = node;
	type = (node->wcaps & AC_WCAP_TYPE) >> AC_WCAP_TYPE_SHIFT;
	for (i = 0; i < node->num_nodes; i++) {
		struct xhda_node *src;
		int src_type;

		if (!(flags & SHOW_ALL) && type != AC_WID_AUD_MIX &&
		    i != node->curr_conn)
			continue;

		src = find_node(codec, node->node[i]);
		if (!src)
			continue;
		src_type = (src->wcaps & AC_WCAP_TYPE) >> AC_WCAP_TYPE_SHIFT;
		if (src_type == AC_WID_AUD_OUT || src_type == AC_WID_PIN) {
			struct xhda_route_list *nlist;
			if (src_type == AC_WID_PIN &&
			    !(flags & SHOW_INACTIVE) &&
			    !is_active_pin(src, 1 << 5)) /* IN_EN */
				continue;
			route[depth + 1] = src;
			nlist = create_route(route, depth + 2, 1);
			nlist->next = list;
			list = nlist;
			continue;
		}
		list = routes_connected_to(codec, src, depth + 1, route,
					   flags, list);
	}
	return list;
}

struct xhda_route_list *
hda_routes_connected_to(struct xhda_codec *codec, int nid, unsigned int flags)
{
	struct xhda_node *route[MAX_ROUTE_DEPTH + 1];
	struct xhda_node *node;

	node = find_node(codec, nid);
	if (!node)
		return NULL;
	return routes_connected_to(codec, node, 0, route, flags, NULL);
}

static struct xhda_route_list *
routes_connected_from(struct xhda_codec *codec, struct xhda_node *node,
		      int depth, struct xhda_node **route,
		      unsigned int flags, struct xhda_route_list *list)
{
	int i, type;
	struct xhda_node *dest;

	if (depth > MAX_ROUTE_DEPTH)
		return list;
	route[depth] = node;

	for (dest = &codec->afg; dest; dest = dest->next) {
		if (dest == node)
			continue;
		type = (dest->wcaps & AC_WCAP_TYPE) >> AC_WCAP_TYPE_SHIFT;
		for (i = 0; i < dest->num_nodes; i++) {
			if (dest->node[i] != node->nid)
				continue;
			if (!(flags & SHOW_ALL) && type != AC_WID_AUD_MIX &&
			    i != dest->curr_conn)
				continue;

			route[depth + 1] = dest;
			if (type == AC_WID_AUD_IN || type == AC_WID_PIN) {
				struct xhda_route_list *nlist;
				if (type == AC_WID_PIN &&
				    !(flags & SHOW_INACTIVE) &&
				    !is_active_pin(dest, 1 << 6)) /* OUT_EN */
					continue;
				route[depth + 1] = dest;
				nlist = create_route(route, depth + 2, 0);
				nlist->next = list;
				list = nlist;
				continue;
			}
			list = routes_connected_from(codec, dest, depth + 1,
						     route, flags, list);
		}
	}
	return list;
}

struct xhda_route_list *
hda_routes_connected_from(struct xhda_codec *codec, int nid, unsigned int flags)
{
	struct xhda_node *route[MAX_ROUTE_DEPTH + 1];
	struct xhda_node *node;

	node = find_node(codec, nid);
	if (!node)
		return NULL;
	return routes_connected_from(codec, node, 0, route, flags, NULL);
}

void hda_free_route_lists(struct xhda_route_list *list)
{
	struct xhda_route_list *next;
	for (; list; list = next) {
		next = list->next;
		free(list);
	}
}

