/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2017-2019 Nippon Telegraph and Telephone Corporation
 */

#include <unistd.h>
#include <string.h>

#include <rte_ether.h>
#include <rte_log.h>
#include <rte_branch_prediction.h>

#include "cmd_parser.h"

#define RTE_LOGTYPE_SPP_COMMAND_PROC RTE_LOGTYPE_USER1
#define RTE_LOGTYPE_APP RTE_LOGTYPE_USER2

/**
 * List of command action. The order of items should be same as the order of
 * enum `sppwk_action` in cmd_parser.h.
 */
const char *CMD_ACT_LIST[] = {
	"none",
	"start",
	"stop",
	"add",
	"del",
	"",  /* termination */
};

/**
 * List of classifier type. The order of items should be same as the order of
 * enum `spp_classifier_type` defined in spp_proc.h.
 */
/* TODO(yasufum) fix similar vars in command_proc.c */
const char *CLS_TYPE_LIST[] = {
	"none",
	"mac",
	"vlan",
	"",  /* termination */
};

/**
 * List of port direction. The order of items should be same as the order of
 * enum `spp_port_rxtx` in spp_vf.h.
 */
const char *PORT_DIR_LIST[] = {
	"none",
	"rx",
	"tx",
	"",  /* termination */
};

/**
 * List of port abilities. The order of items should be same as the order of
 * enum `spp_port_ability_type` in spp_vf.h.
 */
const char *PORT_ABILITY_LIST[] = {
	"none",
	"add_vlantag",
	"del_vlantag",
	"",  /* termination */
};

/* Return 1 as true if port is used with given mac_addr and vid. */
static int
is_used_with_addr(
		int vid, uint64_t mac_addr,
		enum port_type iface_type, int iface_no)
{
	struct sppwk_port_info *wk_port = get_sppwk_port(
			iface_type, iface_no);

	return ((mac_addr == wk_port->cls_attrs.mac_addr) &&
		(vid == wk_port->cls_attrs.vlantag.vid));
}

/* Return 1 as true if given port is already used. */
static int
is_added_port(enum port_type iface_type, int iface_no)
{
	struct sppwk_port_info *port = get_sppwk_port(iface_type, iface_no);
	return port->iface_type != UNDEF;
}

/**
 * Separate resource UID of combination of iface type and number and assign to
 * given argument, iface_type and iface_no. For instance, 'ring:0' is separated
 * to 'ring' and '0'. The supported types are `phy`, `vhost` and `ring`.
 */
static int
parse_resource_uid(const char *res_uid,
		    enum port_type *iface_type,
		    int *iface_no)
{
	enum port_type ptype = UNDEF;
	const char *iface_no_str = NULL;
	char *endptr = NULL;

	/**
	 * TODO(yasufum) consider this checking of zero value is recommended
	 * way, or should be changed.
	 */
	if (strncmp(res_uid, SPP_IFTYPE_NIC_STR ":",
			strlen(SPP_IFTYPE_NIC_STR)+1) == 0) {
		ptype = PHY;
		iface_no_str = &res_uid[strlen(SPP_IFTYPE_NIC_STR)+1];
	} else if (strncmp(res_uid, SPP_IFTYPE_VHOST_STR ":",
			strlen(SPP_IFTYPE_VHOST_STR)+1) == 0) {
		ptype = VHOST;
		iface_no_str = &res_uid[strlen(SPP_IFTYPE_VHOST_STR)+1];
	} else if (strncmp(res_uid, SPP_IFTYPE_RING_STR ":",
			strlen(SPP_IFTYPE_RING_STR)+1) == 0) {
		ptype = RING;
		iface_no_str = &res_uid[strlen(SPP_IFTYPE_RING_STR)+1];
	} else {
		RTE_LOG(ERR, APP, "Unexpected port type in '%s'.\n", res_uid);
		return SPP_RET_NG;
	}

	int port_id = strtol(iface_no_str, &endptr, 0);
	if (unlikely(iface_no_str == endptr) || unlikely(*endptr != '\0')) {
		RTE_LOG(ERR, APP, "No interface number in '%s'.\n", res_uid);
		return SPP_RET_NG;
	}

	*iface_type = ptype;
	*iface_no = port_id;

	RTE_LOG(DEBUG, APP, "Parsed '%s' to '%d' and '%d'.\n",
			res_uid, *iface_type, *iface_no);
	return SPP_RET_OK;
}

/* Get component type from string of its name. */
/* TODO(yasufum) should be worker local, separated for vf and mirror. */
static enum spp_component_type
get_comp_type_from_str(const char *type_str)
{
	RTE_LOG(DEBUG, APP, "type_str is %s\n", type_str);

#ifdef SPP_VF_MODULE
	if (strncmp(type_str, CORE_TYPE_CLASSIFIER_MAC_STR,
			strlen(CORE_TYPE_CLASSIFIER_MAC_STR)+1) == 0) {
		return SPP_COMPONENT_CLASSIFIER_MAC;
	} else if (strncmp(type_str, CORE_TYPE_MERGE_STR,
			strlen(CORE_TYPE_MERGE_STR)+1) == 0) {
		return SPP_COMPONENT_MERGE;
	} else if (strncmp(type_str, CORE_TYPE_FORWARD_STR,
			strlen(CORE_TYPE_FORWARD_STR)+1) == 0) {
		return SPP_COMPONENT_FORWARD;
	}
#endif /* SPP_VF_MODULE */

#ifdef SPP_MIRROR_MODULE
	if (strncmp(type_str, SPP_TYPE_MIRROR_STR,
			strlen(SPP_TYPE_MIRROR_STR)+1) == 0)
		return SPP_COMPONENT_MIRROR;
#endif /* SPP_MIRROR_MODULE */

	return SPP_COMPONENT_UNUSE;
}

/* Format error message object and return error code for an error case. */
/* TODO(yasufum) confirm usage of `set_parse_error` and
 * `set_detailed_parse_error`, which should be used ?
 */
static inline int
set_parse_error(struct sppwk_parse_err_msg *wk_err_msg,
		const int err_code, const char *err_msg)
{
	wk_err_msg->code = err_code;

	if (likely(err_msg != NULL))
		strcpy(wk_err_msg->msg, err_msg);

	return wk_err_msg->code;
}

/* Set parse error message. */
static inline int
set_detailed_parse_error(struct sppwk_parse_err_msg *wk_err_msg,
		const char *err_msg, const char *err_details)
{
	strcpy(wk_err_msg->details, err_details);
	return set_parse_error(wk_err_msg, SPPWK_PARSE_INVALID_VALUE, err_msg);
}

/* Split command line paramis with spaces. */
/**
 * TODO(yasufum) It should be renamed because this function checks if the num
 * of params is over given max num, but this behaviour is not explicit in the
 * name of function. Or remove this checking for simplicity.
 */
static int
split_cmd_params(char *string, int max, int *argc, char *argv[])
{
	int cnt = 0;
	const char *delim = " ";
	char *argv_tok = NULL;
	char *saveptr = NULL;

	argv_tok = strtok_r(string, delim, &saveptr);
	while (argv_tok != NULL) {
		if (cnt >= max)
			return SPP_RET_NG;
		argv[cnt] = argv_tok;
		cnt++;
		argv_tok = strtok_r(NULL, delim, &saveptr);
	}
	*argc = cnt;

	return SPP_RET_OK;
}

/* Get index of given str from list. */
static int
get_list_idx(const char *str, const char *list[])
{
	int i;
	for (i = 0; list[i][0] != '\0'; i++) {
		if (strcmp(list[i], str) == 0)
			return i;
	}
	return SPP_RET_NG;
}

/**
 * Get int from given val. It validates if the val is in the range from min to
 * max given as third and fourth args. It is intended to get VLAN ID or PCP.
 */
static int
get_int_in_range(int *output, const char *arg_val, int min, int max)
{
	int ret;
	char *endptr = NULL;
	ret = strtol(arg_val, &endptr, 0);
	if (unlikely(endptr == arg_val) || unlikely(*endptr != '\0'))
		return SPP_RET_NG;
	if (unlikely(ret < min) || unlikely(ret > max))
		return SPP_RET_NG;
	*output = ret;
	return SPP_RET_OK;
}

/**
 * Get uint from given val. It validates if the val is in the range from min to
 * max given as third and fourth args. It is intended to get lcore ID.
 */
static int
get_uint_in_range(unsigned int *output, const char *arg_val, unsigned int min,
		unsigned int max)
{
	unsigned int ret;
	char *endptr = NULL;
	ret = strtoul(arg_val, &endptr, 0);
	if (unlikely(endptr == arg_val) || unlikely(*endptr != '\0'))
		return SPP_RET_NG;

	if (unlikely(ret < min) || unlikely(ret > max))
		return SPP_RET_NG;

	*output = ret;
	return SPP_RET_OK;
}

/* Parse given res UID of port and init object of struct sppwk_port_idx. */
/* TODO(yasufum) Confirm why 1st arg is not sppwk_port_idx, but void. */
/**
 * TODO(yasufum) Confirm why this func is required. Is it not enough to use
 * parse_resource_uid() ?
 */
static int
parse_port_uid(void *output, const char *arg_val)
{
	int ret;
	struct sppwk_port_idx *port = output;
	ret = parse_resource_uid(arg_val, &port->iface_type, &port->iface_no);
	if (unlikely(ret != 0)) {
		RTE_LOG(ERR, SPP_COMMAND_PROC,
				"Invalid resource UID '%s'.\n", arg_val);
		return SPP_RET_NG;
	}
	return SPP_RET_OK;
}

/* Parse given lcore ID. */
static int
parse_lcore_id(void *output, const char *arg_val)
{
	int ret;
	ret = get_uint_in_range(output, arg_val, 0, RTE_MAX_LCORE-1);
	if (unlikely(ret < 0)) {
		RTE_LOG(ERR, SPP_COMMAND_PROC,
				"Invalid lcore id '%s'.\n", arg_val);
		return SPP_RET_NG;
	}
	return SPP_RET_OK;
}

/* Parse given action in `component` command. */
static int
parse_comp_action(void *output, const char *arg_val,
		int allow_override __attribute__ ((unused)))
{
	int ret;
	/* Get index of registered commands. */
	ret = get_list_idx(arg_val, CMD_ACT_LIST);
	if (unlikely(ret <= 0)) {
		RTE_LOG(ERR, SPP_COMMAND_PROC,
				"Given invalid cmd `%s`.\n",
				arg_val);
		return SPP_RET_NG;
	}

	if (unlikely(ret != SPPWK_ACT_START) &&
			unlikely(ret != SPPWK_ACT_STOP)) {
		RTE_LOG(ERR, SPP_COMMAND_PROC,
				"Unknown component action. val=%s\n",
				arg_val);
		return SPP_RET_NG;
	}

	*(int *)output = ret;
	return SPP_RET_OK;
}

/* Parse given name of `arg_val` in `component` command. */
static int
parse_comp_name(void *output, const char *arg_val,
		int allow_override __attribute__ ((unused)))
{
	int ret;
	struct sppwk_cmd_comp *component = output;

	/* Parsing the name is required only for action `start`. */
	if (component->wk_action == SPPWK_ACT_START) {
		/* Check if lcore is already used. */
		ret = spp_get_component_id(arg_val);  /* Get lcore ID. */
		if (unlikely(ret >= 0)) {
			RTE_LOG(ERR, SPP_COMMAND_PROC,
					"Comp name '%s' is already used.\n",
					arg_val);
			return SPP_RET_NG;
		}
	}

	if (strlen(arg_val) >= SPPWK_VAL_BUFSZ)
		return SPP_RET_NG;

	strcpy(component->name, arg_val);
	return SPP_RET_OK;
}

/* Parse given lcore ID of `arg_val` in `component` command. */
static int
parse_comp_lcore_id(void *output, const char *arg_val,
				int allow_override __attribute__ ((unused)))
{
	struct sppwk_cmd_comp *component = output;

	/* Parsing lcore is required only for action `start`. */
	if (component->wk_action != SPPWK_ACT_START)
		return SPP_RET_OK;

	return parse_lcore_id(&component->core, arg_val);
}

/**
 * Parse given type of component of `arg_val` in `component` command.
 * Return OK code if succeeded, or  NG code.
 */
static int
parse_comp_type(void *output, const char *arg_val,
		int allow_override __attribute__ ((unused)))
{
	enum spp_component_type comp_type;
	struct sppwk_cmd_comp *component = output;

	/* Parsing comp type is required only for action `start`. */
	if (component->wk_action != SPPWK_ACT_START)
		return SPP_RET_OK;

	comp_type = get_comp_type_from_str(arg_val);
	if (unlikely(comp_type <= 0)) {
		RTE_LOG(ERR, SPP_COMMAND_PROC,
				"Unknown component type '%s'.\n",
				arg_val);
		return SPP_RET_NG;
	}

	component->type = comp_type;
	return SPP_RET_OK;
}

/* Parse given action for port of `arg_val` in `port` command. */
static int
parse_port_action(void *output, const char *arg_val,
		int allow_override __attribute__ ((unused)))
{
	int ret;
	/* Get index of registered commands. */
	ret = get_list_idx(arg_val, CMD_ACT_LIST);
	if (unlikely(ret <= 0)) {
		RTE_LOG(ERR, SPP_COMMAND_PROC,
				"Unknown port action. val=%s\n",
				arg_val);
		return SPP_RET_NG;
	}

	/* TODO(yasufum) fix not explicit checking this condition. */
	if (unlikely(ret != SPPWK_ACT_ADD) &&
			unlikely(ret != SPPWK_ACT_DEL)) {
		RTE_LOG(ERR, SPP_COMMAND_PROC,
				"Unknown port action. val=%s\n",
				arg_val);
		return SPP_RET_NG;
	}

	*(int *)output = ret;  /* TODO(yasufum) confirm the statement is OK. */
	return SPP_RET_OK;
}

/* Parse given port uid in port command. */
static int
parse_port(void *output, const char *arg_val, int allow_override)
{
	int ret;
	struct sppwk_port_idx tmp_port;
	struct sppwk_cmd_port *port = output;

	ret = parse_port_uid(&tmp_port, arg_val);
	if (ret < SPP_RET_OK)
		return SPP_RET_NG;

	/* If action is `add`, check the port is already used for rx and tx. */
	if (allow_override == 0) {
		if ((port->wk_action == SPPWK_ACT_ADD) &&
				(spp_check_used_port(tmp_port.iface_type,
						tmp_port.iface_no,
						SPP_PORT_RXTX_RX) >= 0) &&
				(spp_check_used_port(tmp_port.iface_type,
						tmp_port.iface_no,
						SPP_PORT_RXTX_TX) >= 0)) {
			RTE_LOG(ERR, SPP_COMMAND_PROC,
				"Port `%s` is already used.\n",
				arg_val);
			return SPP_RET_NG;
		}
	}

	port->port.iface_type = tmp_port.iface_type;
	port->port.iface_no   = tmp_port.iface_no;
	return SPP_RET_OK;
}

/* Parse port rx and tx value. */
static int
parse_port_rxtx(void *output, const char *arg_val, int allow_override)
{
	int ret = SPP_RET_OK;
	struct sppwk_cmd_port *port = output;

	ret = get_list_idx(arg_val, PORT_DIR_LIST);
	if (unlikely(ret <= 0)) {
		RTE_LOG(ERR, SPP_COMMAND_PROC, "Unknown port rxtx. val=%s\n",
				arg_val);
		return SPP_RET_NG;
	}

	/* add vlantag command check */
	if (allow_override == 0) {
		if ((port->wk_action == SPPWK_ACT_ADD) &&
				(spp_check_used_port(port->port.iface_type,
					port->port.iface_no, ret) >= 0)) {
			RTE_LOG(ERR, SPP_COMMAND_PROC,
				"Port in used. (port command) val=%s\n",
				arg_val);
			return SPP_RET_NG;
		}
	}

	port->rxtx = ret;
	return SPP_RET_OK;
}

/* Parse comp name for `port` command. */
/* TODO(yasufum) confirm why parsing comp name "for port cmd" is required. */
static int
parse_comp_name_portcmd(void *output, const char *arg_val,
				int allow_override __attribute__ ((unused)))
{
	int ret = SPP_RET_OK;

	/* Check if lcore is already used. */
	ret = spp_get_component_id(arg_val);  /* Get lcore ID. */
	if (unlikely(ret < SPP_RET_OK)) {
		RTE_LOG(ERR, SPP_COMMAND_PROC,
				"Unknown component name. val=%s\n", arg_val);
		return SPP_RET_NG;
	}

	if (strlen(arg_val) >= SPPWK_VAL_BUFSZ)
		return SPP_RET_NG;

	strcpy(output, arg_val);
	return SPP_RET_OK;
}

/* Parse vlan operation for port command. */
/* TODO(yasufum) add desc for how to be used. */
/* TODO(yasufum) add desc for what is port ability. */
static int
parse_port_vlan_ops(void *output, const char *arg_val,
		int allow_override __attribute__ ((unused)))
{
	int ret;
	struct sppwk_cmd_port *port = output;
	struct spp_port_ability *ability = &port->ability;

	switch (ability->ops) {
	case SPPWK_PORT_ABL_OPS_NONE:
		ret = get_list_idx(arg_val, PORT_ABILITY_LIST);
		if (unlikely(ret <= 0)) {
			RTE_LOG(ERR, SPP_COMMAND_PROC,
					"Unknown port ability. val=%s\n",
					arg_val);
			return SPP_RET_NG;
		}
		ability->ops  = ret;
		ability->rxtx = port->rxtx;
		break;
	case SPPWK_PORT_ABL_OPS_ADD_VLANTAG:
		/* Nothing to do. */
		break;
	default:
		/* Not used. */
		break;
	}

	return SPP_RET_OK;
}

/* Parse VLAN ID  for port command. */
static int
parse_port_vid(void *output, const char *arg_val,
		int allow_override __attribute__ ((unused)))
{
	int vlan_id;
	struct sppwk_cmd_port *port = output;
	struct spp_port_ability *ability = &port->ability;

	switch (ability->ops) {
	case SPPWK_PORT_ABL_OPS_ADD_VLANTAG:
		vlan_id = get_int_in_range(&ability->data.vlantag.vid,
			arg_val, 0, ETH_VLAN_ID_MAX);
		if (unlikely(vlan_id < SPP_RET_OK)) {
			RTE_LOG(ERR, SPP_COMMAND_PROC,
					"Invalid `%s` for parsing VLAN ID.\n",
					arg_val);
			return SPP_RET_NG;
		}
		ability->data.vlantag.pcp = -1;
		break;
	default:
		/* Not used. */
		break;
	}

	return SPP_RET_OK;
}

/* Parse PCP for port command */
static int
parse_port_pcp(void *output, const char *arg_val,
		int allow_override __attribute__ ((unused)))
{
	int pcp;
	struct sppwk_cmd_port *port = output;
	struct spp_port_ability *ability = &port->ability;

	switch (ability->ops) {
	case SPPWK_PORT_ABL_OPS_ADD_VLANTAG:
		pcp = get_int_in_range(&ability->data.vlantag.pcp,
				arg_val, 0, SPP_VLAN_PCP_MAX);
		if (unlikely(pcp < SPP_RET_OK)) {
			RTE_LOG(ERR, SPP_COMMAND_PROC,
					"Invalid `%s`for parsing PCP.\n",
					arg_val);
			return SPP_RET_NG;
		}
		break;
	default:
		/* Not used. */
		break;
	}

	return SPP_RET_OK;
}

/* Parse mac address string. */
static int
parse_mac_addr(void *output, const char *arg_val,
		int allow_override __attribute__ ((unused)))
{
	int64_t res;
	const char *str_val = arg_val;

	/* If given value is the default, use dummy address instead. */
	if (unlikely(strcmp(str_val, SPP_DEFAULT_CLASSIFIED_SPEC_STR) == 0))
		str_val = SPP_DEFAULT_CLASSIFIED_DMY_ADDR_STR;

	/* Check if the given value is valid. */
	res = sppwk_convert_mac_str_to_int64(str_val);
	if (unlikely(res < SPP_RET_OK)) {
		RTE_LOG(ERR, SPP_COMMAND_PROC,
				"Invalid MAC address `%s`.\n", str_val);
		return SPP_RET_NG;
	}

	strcpy((char *)output, str_val);
	return SPP_RET_OK;
}

/**
 * Parse given action for getting index of actions for `classifier_table`
 * command.
 */
static int
parse_cls_action(void *output, const char *arg_val,
		int allow_override __attribute__ ((unused)))
{
	int idx;
	idx = get_list_idx(arg_val, CMD_ACT_LIST);
	if (unlikely(idx <= 0)) {
		RTE_LOG(ERR, SPP_COMMAND_PROC,
				"Failed to get index for action `%s`.\n",
				arg_val);
		return SPP_RET_NG;
	}

	if (unlikely(idx != SPPWK_ACT_ADD) &&
			unlikely(idx != SPPWK_ACT_DEL)) {
		RTE_LOG(ERR, SPP_COMMAND_PROC,
				"Unknown action `%s` for port.\n",
				arg_val);
		return SPP_RET_NG;
	}

	*(int *)output = idx;
	return SPP_RET_OK;
}

/* Parse cls type and get index for classifier_table command. */
static int
parse_cls_type(void *output, const char *arg_val,
		int allow_override __attribute__ ((unused)))
{
	int idx;
	idx = get_list_idx(arg_val, CLS_TYPE_LIST);
	if (unlikely(idx <= 0)) {
		RTE_LOG(ERR, SPP_COMMAND_PROC,
				"Unknown classifier type. val=%s\n",
				arg_val);
		return SPP_RET_NG;
	}

	*(int *)output = idx;
	return SPP_RET_OK;
}

/* Parse VLAN ID for classifier_table command. */
static int
parse_cls_vid(void *output, const char *arg_val,
		int allow_override __attribute__ ((unused)))
{
	int idx;
	idx = get_int_in_range(output, arg_val, 0, ETH_VLAN_ID_MAX);
	if (unlikely(idx < SPP_RET_OK)) {
		RTE_LOG(ERR, SPP_COMMAND_PROC, "Invalid VLAN ID `%s`.\n",
				arg_val);
		return SPP_RET_NG;
	}
	return SPP_RET_OK;
}

/* Parse port for classifier_table command */
static int
parse_cls_port(void *cls_cmd_attr, const char *arg_val,
		int allow_override __attribute__ ((unused)))
{
	int ret = SPP_RET_OK;
	struct sppwk_cls_cmd_attrs *cls_attrs = cls_cmd_attr;
	struct sppwk_port_idx tmp_port;
	int64_t mac_addr = 0;

	ret = parse_port_uid(&tmp_port, arg_val);
	if (ret < SPP_RET_OK)
		return SPP_RET_NG;

	if (is_added_port(tmp_port.iface_type, tmp_port.iface_no) == 0) {
		RTE_LOG(ERR, SPP_COMMAND_PROC, "Port not added. val=%s\n",
				arg_val);
		return SPP_RET_NG;
	}

	if (cls_attrs->type == SPP_CLASSIFIER_TYPE_MAC)
		cls_attrs->vid = ETH_VLAN_ID_MAX;

	if (unlikely(cls_attrs->wk_action == SPPWK_ACT_ADD)) {
		if (!is_used_with_addr(ETH_VLAN_ID_MAX, 0,
				tmp_port.iface_type, tmp_port.iface_no)) {
			RTE_LOG(ERR, SPP_COMMAND_PROC, "Port in used. "
					"(classifier_table command) val=%s\n",
					arg_val);
			return SPP_RET_NG;
		}
	} else if (unlikely(cls_attrs->wk_action == SPPWK_ACT_DEL)) {
		mac_addr = sppwk_convert_mac_str_to_int64(cls_attrs->mac);
		if (mac_addr < 0)
			return SPP_RET_NG;

		if (!is_used_with_addr(cls_attrs->vid,
				(uint64_t)mac_addr,
				tmp_port.iface_type, tmp_port.iface_no)) {
			RTE_LOG(ERR, SPP_COMMAND_PROC, "Port in used. "
					"(classifier_table command) val=%s\n",
					arg_val);
			return SPP_RET_NG;
		}
	}

	cls_attrs->port.iface_type = tmp_port.iface_type;
	cls_attrs->port.iface_no   = tmp_port.iface_no;
	return SPP_RET_OK;
}

/* Attributes operator functions of command for parsing. */
struct sppwk_cmd_ops {
	const char *name;
	size_t offset;  /* Offset of struct spp_command */
	/* Pointer to operator function */
	int (*func)(void *output, const char *arg_val, int allow_override);
};

/* Used for command which takes no params, such as `status`. */
#define SPPWK_CMD_NO_PARAMS { NULL, 0, NULL }

/* A set of operator functions for parsing command. */
static struct sppwk_cmd_ops
cmd_ops_list[][SPPWK_MAX_PARAMS] = {
	{  /* classifier_table(mac) */
		{
			.name = "action",
			.offset = offsetof(struct spp_command,
					spec.cls_table.wk_action),
			.func = parse_cls_action
		},
		{
			.name = "type",
			.offset = offsetof(struct spp_command,
					spec.cls_table.type),
			.func = parse_cls_type
		},
		{
			.name = "mac address",
			.offset = offsetof(struct spp_command,
					spec.cls_table.mac),
			.func = parse_mac_addr
		},
		{
			.name = "port",
			.offset = offsetof(struct spp_command,
					spec.cls_table),
			.func = parse_cls_port
		},
		SPPWK_CMD_NO_PARAMS,
	},
	{  /* classifier_table(VLAN) */
		{
			.name = "action",
			.offset = offsetof(struct spp_command,
					spec.cls_table.wk_action),
			.func = parse_cls_action
		},
		{
			.name = "type",
			.offset = offsetof(struct spp_command,
					spec.cls_table.type),
			.func = parse_cls_type
		},
		{
			.name = "vlan id",
			.offset = offsetof(struct spp_command,
					spec.cls_table.vid),
			.func = parse_cls_vid
		},
		{
			.name = "mac address",
			.offset = offsetof(struct spp_command,
					spec.cls_table.mac),
			.func = parse_mac_addr
		},
		{
			.name = "port",
			.offset = offsetof(struct spp_command,
					spec.cls_table),
			.func = parse_cls_port
		},
		SPPWK_CMD_NO_PARAMS,
	},
	{ SPPWK_CMD_NO_PARAMS },  /* _get_client_id */
	{ SPPWK_CMD_NO_PARAMS },  /* status */
	{ SPPWK_CMD_NO_PARAMS },  /* exit */
	{  /* component */
		{
			.name = "action",
			.offset = offsetof(struct spp_command,
					spec.comp.wk_action),
			.func = parse_comp_action
		},
		{
			.name = "component name",
			.offset = offsetof(struct spp_command, spec.comp),
			.func = parse_comp_name
		},
		{
			.name = "core",
			.offset = offsetof(struct spp_command, spec.comp),
			.func = parse_comp_lcore_id
		},
		{
			.name = "component type",
			.offset = offsetof(struct spp_command, spec.comp),
			.func = parse_comp_type
		},
		SPPWK_CMD_NO_PARAMS,
	},
	{  /* port */
		{
			.name = "action",
			.offset = offsetof(struct spp_command,
					spec.port.wk_action),
			.func = parse_port_action
		},
		{
			.name = "port",
			.offset = offsetof(struct spp_command, spec.port),
			.func = parse_port
		},
		{
			.name = "port rxtx",
			.offset = offsetof(struct spp_command, spec.port),
			.func = parse_port_rxtx
		},
		{
			.name = "component name",
			.offset = offsetof(struct spp_command, spec.port.name),
			.func = parse_comp_name_portcmd
		},
		{
			.name = "port vlan operation",
			.offset = offsetof(struct spp_command, spec.port),
			.func = parse_port_vlan_ops
		},
		{
			.name = "port vid",
			.offset = offsetof(struct spp_command, spec.port),
			.func = parse_port_vid
		},
		{
			.name = "port pcp",
			.offset = offsetof(struct spp_command, spec.port),
			.func = parse_port_pcp
		},
		SPPWK_CMD_NO_PARAMS,
	},
	{ SPPWK_CMD_NO_PARAMS }, /* termination */
};

/* Validate given command. */
static int
parse_cmd_comp(struct sppwk_cmd_req *request, int argc, char *argv[],
		struct sppwk_parse_err_msg *wk_err_msg,
		int maxargc __attribute__ ((unused)))
{
	int ret = SPP_RET_OK;
	int ci = request->commands[0].type;
	int pi = 0;
	struct sppwk_cmd_ops *list = NULL;
	for (pi = 1; pi < argc; pi++) {
		list = &cmd_ops_list[ci][pi-1];
		ret = (*list->func)((void *)
				((char *)&request->commands[0] + list->offset),
				argv[pi], 0);
		if (unlikely(ret < 0)) {
			RTE_LOG(ERR, SPP_COMMAND_PROC,
					"Invalid value. command=%s, name=%s, "
					"index=%d, value=%s\n",
					argv[0], list->name, pi, argv[pi]);
			return set_detailed_parse_error(wk_err_msg,
					list->name, argv[pi]);
		}
	}
	return SPP_RET_OK;
}

/* Validate given command for clssfier_table. */
static int
parse_cmd_cls_table(struct sppwk_cmd_req *request, int argc, char *argv[],
		struct sppwk_parse_err_msg *wk_err_msg, int maxargc)
{
	return parse_cmd_comp(request, argc, argv, wk_err_msg, maxargc);
}

/* Validate given command for clssfier_table of vlan. */
static int
parse_cmd_cls_table_vlan(struct sppwk_cmd_req *request, int argc, char *argv[],
		struct sppwk_parse_err_msg *wk_err_msg,
		int maxargc __attribute__ ((unused)))
{
	int ret = SPP_RET_OK;
	int ci = request->commands[0].type;
	int pi = 0;
	struct sppwk_cmd_ops *list = NULL;
	for (pi = 1; pi < argc; pi++) {
		list = &cmd_ops_list[ci][pi-1];
		ret = (*list->func)((void *)
				((char *)&request->commands[0] + list->offset),
				argv[pi], 0);
		if (unlikely(ret < SPP_RET_OK)) {
			RTE_LOG(ERR, SPP_COMMAND_PROC, "Bad value. "
				"command=%s, name=%s, index=%d, value=%s\n",
					argv[0], list->name, pi, argv[pi]);
			return set_detailed_parse_error(wk_err_msg,
					list->name, argv[pi]);
		}
	}
	return SPP_RET_OK;
}

/* Validate given command for port. */
static int
parse_cmd_port(struct sppwk_cmd_req *request, int argc, char *argv[],
		struct sppwk_parse_err_msg *wk_err_msg, int maxargc)
{
	int ret = SPP_RET_OK;
	int ci = request->commands[0].type;
	int pi = 0;
	struct sppwk_cmd_ops *list = NULL;
	int flag = 0;

	/* check add vlatag */
	if (argc == maxargc)
		flag = 1;

	for (pi = 1; pi < argc; pi++) {
		list = &cmd_ops_list[ci][pi-1];
		ret = (*list->func)((void *)
				((char *)&request->commands[0] + list->offset),
				argv[pi], flag);
		if (unlikely(ret < SPP_RET_OK)) {
			RTE_LOG(ERR, SPP_COMMAND_PROC, "Bad value. "
				"command=%s, name=%s, index=%d, value=%s\n",
					argv[0], list->name, pi, argv[pi]);
			return set_detailed_parse_error(wk_err_msg,
					list->name, argv[pi]);
		}
	}
	return SPP_RET_OK;
}

/**
 * Attributes of commands for parsing. The last member of function pointer
 * is the operator function for the command.
 */
struct cmd_parse_attrs {
	const char *cmd_name;
	int nof_params_min;
	int nof_params_max;
	int (*func)(struct sppwk_cmd_req *request, int argc,
			char *argv[], struct sppwk_parse_err_msg *wk_err_msg,
			int maxargc);
};

/**
 * List of command attributes defines the name of command, number of params
 * and operator functions.
 */
static struct cmd_parse_attrs cmd_attr_list[] = {
	{ "classifier_table", 5, 5, parse_cmd_cls_table },
	{ "classifier_table", 6, 6, parse_cmd_cls_table_vlan },
	{ "_get_client_id", 1, 1, NULL },
	{ "status", 1, 1, NULL },
	{ "exit", 1, 1, NULL },
	{ "component", 3, 5, parse_cmd_comp },
	{ "port", 5, 8, parse_cmd_port },
	{ "", 0, 0, NULL }  /* termination */
};

/* Parse command for SPP worker. */
static int
parse_wk_cmd(struct sppwk_cmd_req *request,
		const char *request_str,
		struct sppwk_parse_err_msg *wk_err_msg)
{
	int ret = SPP_RET_OK;
	int is_valid_nof_params = 1;  /* for checking nof params in range. */
	struct cmd_parse_attrs *list = NULL;
	int i = 0;
	/**
	 * TODO(yasufum) The name of `argc` and `argv` should be renamed because
	 * it is used for the num of params and param itself, not for arguments.
	 * It is so misunderstandable for maintainance.
	 */
	int argc = 0;
	char *argv[SPPWK_MAX_PARAMS];
	char tmp_str[SPPWK_MAX_PARAMS*SPPWK_VAL_BUFSZ];
	memset(argv, 0x00, sizeof(argv));
	memset(tmp_str, 0x00, sizeof(tmp_str));

	strcpy(tmp_str, request_str);
	/**
	 * TODO(yasufum) As described in the definition of
	 * `split_cmd_params()`, the name and usage of this function should
	 * be refactored because it is no meaning to check the num of params
	 * here. The checking is not explicit in the name of func, and checking
	 * itself is done in the next step as following. No need to do here.
	 */
	ret = split_cmd_params(tmp_str, SPPWK_MAX_PARAMS, &argc, argv);
	if (ret < SPP_RET_OK) {
		RTE_LOG(ERR, SPP_COMMAND_PROC,
				"Num of params should be less than %d. "
				"request_str=%s\n",
				SPPWK_MAX_PARAMS, request_str);
		return set_parse_error(wk_err_msg, SPPWK_PARSE_WRONG_FORMAT,
				NULL);
	}
	RTE_LOG(DEBUG, SPP_COMMAND_PROC, "Decode array. num=%d\n", argc);

	for (i = 0; cmd_attr_list[i].cmd_name[0] != '\0'; i++) {
		list = &cmd_attr_list[i];
		if (strcmp(argv[0], list->cmd_name) != 0)
			continue;

		if (unlikely(argc < list->nof_params_min) ||
				unlikely(list->nof_params_max < argc)) {
			is_valid_nof_params = 0;
			continue;
		}

		request->commands[0].type = i;
		if (list->func != NULL)
			return (*list->func)(request, argc, argv, wk_err_msg,
							list->nof_params_max);

		return SPP_RET_OK;
	}

	/**
	 * Failed to parse command because of invalid nof params or
	 * unknown command.
	 */
	if (is_valid_nof_params == 0) {
		RTE_LOG(ERR, SPP_COMMAND_PROC,
				"Number of parmas is out of range. "
				"request_str=%s\n", request_str);
		return set_parse_error(wk_err_msg, SPPWK_PARSE_WRONG_FORMAT,
				NULL);
	}

	RTE_LOG(ERR, SPP_COMMAND_PROC,
			"Unknown command '%s' and request_str=%s\n",
			argv[0], request_str);
	return set_detailed_parse_error(wk_err_msg, "command", argv[0]);
}

/* Parse request of non null terminated string. */
int
sppwk_parse_req(
		struct sppwk_cmd_req *request,
		const char *request_str, size_t request_str_len,
		struct sppwk_parse_err_msg *wk_err_msg)
{
	int ret = SPP_RET_NG;
	int i;

	/* decode request */
	request->num_command = 1;
	ret = parse_wk_cmd(request, request_str, wk_err_msg);
	if (unlikely(ret != SPP_RET_OK)) {
		RTE_LOG(ERR, SPP_COMMAND_PROC,
				"Cannot decode command request. "
				"ret=%d, request_str=%.*s\n",
				ret, (int)request_str_len, request_str);
		return ret;
	}
	request->num_valid_command = 1;

	/* check getter command */
	for (i = 0; i < request->num_valid_command; ++i) {
		switch (request->commands[i].type) {
		case SPPWK_CMDTYPE_CLIENT_ID:
			request->is_requested_client_id = 1;
			break;
		case SPPWK_CMDTYPE_STATUS:
			request->is_requested_status = 1;
			break;
		case SPPWK_CMDTYPE_EXIT:
			request->is_requested_exit = 1;
			break;
		default:
			/* nothing to do */
			break;
		}
	}

	return ret;
}
