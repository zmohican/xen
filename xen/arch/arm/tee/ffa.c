/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * xen/arch/arm/tee/ffa.c
 *
 * Arm Firmware Framework for ARMv8-A (FF-A) mediator
 *
 * Copyright (C) 2023  Linaro Limited
 */

#include <xen/bitops.h>
#include <xen/domain_page.h>
#include <xen/errno.h>
#include <xen/init.h>
#include <xen/lib.h>
#include <xen/sched.h>
#include <xen/sizes.h>
#include <xen/types.h>

#include <asm/event.h>
#include <asm/regs.h>
#include <asm/smccc.h>
#include <asm/tee/ffa.h>
#include <asm/tee/tee.h>

/* Error codes */
#define FFA_RET_OK                      0
#define FFA_RET_NOT_SUPPORTED           -1
#define FFA_RET_INVALID_PARAMETERS      -2
#define FFA_RET_NO_MEMORY               -3
#define FFA_RET_BUSY                    -4
#define FFA_RET_INTERRUPTED             -5
#define FFA_RET_DENIED                  -6
#define FFA_RET_RETRY                   -7
#define FFA_RET_ABORTED                 -8

/* FFA_VERSION helpers */
#define FFA_VERSION_MAJOR_SHIFT         16U
#define FFA_VERSION_MAJOR_MASK          0x7FFFU
#define FFA_VERSION_MINOR_SHIFT         0U
#define FFA_VERSION_MINOR_MASK          0xFFFFU
#define MAKE_FFA_VERSION(major, minor)  \
        ((((major) & FFA_VERSION_MAJOR_MASK) << FFA_VERSION_MAJOR_SHIFT) | \
         ((minor) & FFA_VERSION_MINOR_MASK))

#define FFA_VERSION_1_0         MAKE_FFA_VERSION(1, 0)
#define FFA_VERSION_1_1         MAKE_FFA_VERSION(1, 1)
/* The minimal FF-A version of the SPMC that can be supported */
#define FFA_MIN_SPMC_VERSION    FFA_VERSION_1_1

/*
 * This is the version we want to use in communication with guests and SPs.
 * During negotiation with a guest or a SP we may need to lower it for
 * that particular guest or SP.
 */
#define FFA_MY_VERSION_MAJOR    1U
#define FFA_MY_VERSION_MINOR    1U
#define FFA_MY_VERSION          MAKE_FFA_VERSION(FFA_MY_VERSION_MAJOR, \
                                                 FFA_MY_VERSION_MINOR)

/*
 * The FF-A specification explicitly works with 4K pages as a measure of
 * memory size, for example, FFA_RXTX_MAP takes one parameter "RX/TX page
 * count" which is the number of contiguous 4K pages allocated. Xen may use
 * a different page size depending on the configuration to avoid confusion
 * with PAGE_SIZE use a special define when it's a page size as in the FF-A
 * specification.
 */
#define FFA_PAGE_SIZE                   SZ_4K

/*
 * Flags and field values used for the MSG_SEND_DIRECT_REQ/RESP:
 * BIT(31): Framework or partition message
 * BIT(7-0): Message type for frameworks messages
 */
#define FFA_MSG_FLAG_FRAMEWORK          BIT(31, U)
#define FFA_MSG_TYPE_MASK               0xFFU;
#define FFA_MSG_PSCI                    0x0U
#define FFA_MSG_SEND_VM_CREATED         0x4U
#define FFA_MSG_RESP_VM_CREATED         0x5U
#define FFA_MSG_SEND_VM_DESTROYED       0x6U
#define FFA_MSG_RESP_VM_DESTROYED       0x7U

/*
 * Flags to determine partition properties in FFA_PARTITION_INFO_GET return
 * message:
 * BIT(0): Supports receipt of direct requests
 * BIT(1): Can send direct requests
 * BIT(2): Can send and receive indirect messages
 * BIT(3): Supports receipt of notifications
 * BIT(4-5): Partition ID is a PE endpoint ID
 * BIT(6): Partition must be informed about each VM that is created by
 *         the Hypervisor
 * BIT(7): Partition must be informed about each VM that is destroyed by
 *         the Hypervisor
 * BIT(8): Partition runs in the AArch64 execution state else AArch32
 *         execution state
 */
#define FFA_PART_PROP_DIRECT_REQ_RECV   BIT(0, U)
#define FFA_PART_PROP_DIRECT_REQ_SEND   BIT(1, U)
#define FFA_PART_PROP_INDIRECT_MSGS     BIT(2, U)
#define FFA_PART_PROP_RECV_NOTIF        BIT(3, U)
#define FFA_PART_PROP_IS_TYPE_MASK      (3U << 4)
#define FFA_PART_PROP_IS_PE_ID          (0U << 4)
#define FFA_PART_PROP_IS_SEPID_INDEP    (1U << 4)
#define FFA_PART_PROP_IS_SEPID_DEP      (2U << 4)
#define FFA_PART_PROP_IS_AUX_ID         (3U << 4)
#define FFA_PART_PROP_NOTIF_CREATED     BIT(6, U)
#define FFA_PART_PROP_NOTIF_DESTROYED   BIT(7, U)
#define FFA_PART_PROP_AARCH64_STATE     BIT(8, U)

/*
 * Flag used as parameter to FFA_PARTITION_INFO_GET to return partition
 * count only.
 */
#define FFA_PARTITION_INFO_GET_COUNT_FLAG BIT(0, U)

/* Function IDs */
#define FFA_ERROR                       0x84000060U
#define FFA_SUCCESS_32                  0x84000061U
#define FFA_SUCCESS_64                  0xC4000061U
#define FFA_INTERRUPT                   0x84000062U
#define FFA_VERSION                     0x84000063U
#define FFA_FEATURES                    0x84000064U
#define FFA_RX_ACQUIRE                  0x84000084U
#define FFA_RX_RELEASE                  0x84000065U
#define FFA_RXTX_MAP_32                 0x84000066U
#define FFA_RXTX_MAP_64                 0xC4000066U
#define FFA_RXTX_UNMAP                  0x84000067U
#define FFA_PARTITION_INFO_GET          0x84000068U
#define FFA_ID_GET                      0x84000069U
#define FFA_SPM_ID_GET                  0x84000085U
#define FFA_MSG_WAIT                    0x8400006BU
#define FFA_MSG_YIELD                   0x8400006CU
#define FFA_RUN                         0x8400006DU
#define FFA_MSG_SEND2                   0x84000086U
#define FFA_MSG_SEND_DIRECT_REQ_32      0x8400006FU
#define FFA_MSG_SEND_DIRECT_REQ_64      0xC400006FU
#define FFA_MSG_SEND_DIRECT_RESP_32     0x84000070U
#define FFA_MSG_SEND_DIRECT_RESP_64     0xC4000070U
#define FFA_MEM_DONATE_32               0x84000071U
#define FFA_MEM_DONATE_64               0xC4000071U
#define FFA_MEM_LEND_32                 0x84000072U
#define FFA_MEM_LEND_64                 0xC4000072U
#define FFA_MEM_SHARE_32                0x84000073U
#define FFA_MEM_SHARE_64                0xC4000073U
#define FFA_MEM_RETRIEVE_REQ_32         0x84000074U
#define FFA_MEM_RETRIEVE_REQ_64         0xC4000074U
#define FFA_MEM_RETRIEVE_RESP           0x84000075U
#define FFA_MEM_RELINQUISH              0x84000076U
#define FFA_MEM_RECLAIM                 0x84000077U
#define FFA_MEM_FRAG_RX                 0x8400007AU
#define FFA_MEM_FRAG_TX                 0x8400007BU
#define FFA_MSG_SEND                    0x8400006EU
#define FFA_MSG_POLL                    0x8400006AU

struct ffa_ctx {
    /* FF-A version used by the guest */
    uint32_t guest_vers;
};

/* Negotiated FF-A version to use with the SPMC */
static uint32_t __ro_after_init ffa_version;

static bool ffa_get_version(uint32_t *vers)
{
    const struct arm_smccc_1_2_regs arg = {
        .a0 = FFA_VERSION,
        .a1 = FFA_MY_VERSION,
    };
    struct arm_smccc_1_2_regs resp;

    arm_smccc_1_2_smc(&arg, &resp);
    if ( resp.a0 == FFA_RET_NOT_SUPPORTED )
    {
        gprintk(XENLOG_ERR, "ffa: FFA_VERSION returned not supported\n");
        return false;
    }

    *vers = resp.a0;

    return true;
}

static int32_t get_ffa_ret_code(const struct arm_smccc_1_2_regs *resp)
{
    switch ( resp->a0 )
    {
    case FFA_ERROR:
        if ( resp->a2 )
            return resp->a2;
        else
            return FFA_RET_NOT_SUPPORTED;
    case FFA_SUCCESS_32:
    case FFA_SUCCESS_64:
        return FFA_RET_OK;
    default:
        return FFA_RET_NOT_SUPPORTED;
    }
}

static int32_t ffa_simple_call(uint32_t fid, register_t a1, register_t a2,
                               register_t a3, register_t a4)
{
    const struct arm_smccc_1_2_regs arg = {
        .a0 = fid,
        .a1 = a1,
        .a2 = a2,
        .a3 = a3,
        .a4 = a4,
    };
    struct arm_smccc_1_2_regs resp;

    arm_smccc_1_2_smc(&arg, &resp);

    return get_ffa_ret_code(&resp);
}

static int32_t ffa_features(uint32_t id)
{
    return ffa_simple_call(FFA_FEATURES, id, 0, 0, 0);
}

static bool check_mandatory_feature(uint32_t id)
{
    int32_t ret = ffa_features(id);

    if ( ret )
        printk(XENLOG_ERR "ffa: mandatory feature id %#x missing: error %d\n",
               id, ret);

    return !ret;
}

static uint16_t get_vm_id(const struct domain *d)
{
    /* +1 since 0 is reserved for the hypervisor in FF-A */
    return d->domain_id + 1;
}

static void set_regs(struct cpu_user_regs *regs, register_t v0, register_t v1,
                     register_t v2, register_t v3, register_t v4, register_t v5,
                     register_t v6, register_t v7)
{
        set_user_reg(regs, 0, v0);
        set_user_reg(regs, 1, v1);
        set_user_reg(regs, 2, v2);
        set_user_reg(regs, 3, v3);
        set_user_reg(regs, 4, v4);
        set_user_reg(regs, 5, v5);
        set_user_reg(regs, 6, v6);
        set_user_reg(regs, 7, v7);
}

static void set_regs_success(struct cpu_user_regs *regs, uint32_t w2,
                             uint32_t w3)
{
    set_regs(regs, FFA_SUCCESS_32, 0, w2, w3, 0, 0, 0, 0);
}

static void handle_version(struct cpu_user_regs *regs)
{
    struct domain *d = current->domain;
    struct ffa_ctx *ctx = d->arch.tee;
    uint32_t vers = get_user_reg(regs, 1);

    if ( vers < FFA_VERSION_1_1 )
        vers = FFA_VERSION_1_0;
    else
        vers = FFA_VERSION_1_1;

    ctx->guest_vers = vers;
    set_regs(regs, vers, 0, 0, 0, 0, 0, 0, 0);
}

static void handle_msg_send_direct_req(struct cpu_user_regs *regs, uint32_t fid)
{
    struct arm_smccc_1_2_regs arg = { .a0 = fid, };
    struct arm_smccc_1_2_regs resp = { };
    struct domain *d = current->domain;
    uint32_t src_dst;
    uint64_t mask;

    if ( smccc_is_conv_64(fid) )
        mask = GENMASK_ULL(63, 0);
    else
        mask = GENMASK_ULL(31, 0);

    src_dst = get_user_reg(regs, 1);
    if ( (src_dst >> 16) != get_vm_id(d) )
    {
        resp.a0 = FFA_ERROR;
        resp.a2 = FFA_RET_INVALID_PARAMETERS;
        goto out;
    }

    arg.a1 = src_dst;
    arg.a2 = get_user_reg(regs, 2) & mask;
    arg.a3 = get_user_reg(regs, 3) & mask;
    arg.a4 = get_user_reg(regs, 4) & mask;
    arg.a5 = get_user_reg(regs, 5) & mask;
    arg.a6 = get_user_reg(regs, 6) & mask;
    arg.a7 = get_user_reg(regs, 7) & mask;

    arm_smccc_1_2_smc(&arg, &resp);
    switch ( resp.a0 )
    {
    case FFA_ERROR:
    case FFA_SUCCESS_32:
    case FFA_SUCCESS_64:
    case FFA_MSG_SEND_DIRECT_RESP_32:
    case FFA_MSG_SEND_DIRECT_RESP_64:
        break;
    default:
        /* Bad fid, report back to the caller. */
        memset(&resp, 0, sizeof(resp));
        resp.a0 = FFA_ERROR;
        resp.a1 = src_dst;
        resp.a2 = FFA_RET_ABORTED;
    }

out:
    set_regs(regs, resp.a0, resp.a1 & mask, resp.a2 & mask, resp.a3 & mask,
             resp.a4 & mask, resp.a5 & mask, resp.a6 & mask, resp.a7 & mask);
}

static bool ffa_handle_call(struct cpu_user_regs *regs)
{
    uint32_t fid = get_user_reg(regs, 0);
    struct domain *d = current->domain;
    struct ffa_ctx *ctx = d->arch.tee;

    if ( !ctx )
        return false;

    switch ( fid )
    {
    case FFA_VERSION:
        handle_version(regs);
        return true;
    case FFA_ID_GET:
        set_regs_success(regs, get_vm_id(d), 0);
        return true;
    case FFA_MSG_SEND_DIRECT_REQ_32:
    case FFA_MSG_SEND_DIRECT_REQ_64:
        handle_msg_send_direct_req(regs, fid);
        return true;

    default:
        gprintk(XENLOG_ERR, "ffa: unhandled fid 0x%x\n", fid);
        return false;
    }
}

static int ffa_domain_init(struct domain *d)
{
    struct ffa_ctx *ctx;

    if ( !ffa_version )
        return -ENODEV;
     /*
      * We can't use that last possible domain ID or get_vm_id() would cause
      * an overflow.
      */
    if ( d->domain_id >= UINT16_MAX)
        return -ERANGE;

    ctx = xzalloc(struct ffa_ctx);
    if ( !ctx )
        return -ENOMEM;

    d->arch.tee = ctx;

    return 0;
}

/* This function is supposed to undo what ffa_domain_init() has done */
static int ffa_domain_teardown(struct domain *d)
{
    struct ffa_ctx *ctx = d->arch.tee;

    if ( !ctx )
        return 0;

    XFREE(d->arch.tee);

    return 0;
}

static int ffa_relinquish_resources(struct domain *d)
{
    return 0;
}

static bool ffa_probe(void)
{
    uint32_t vers;
    unsigned int major_vers;
    unsigned int minor_vers;

    /*
     * FF-A often works in units of 4K pages and currently it's assumed
     * that we can map memory using that granularity. See also the comment
     * above the FFA_PAGE_SIZE define.
     *
     * It is possible to support a PAGE_SIZE larger than 4K in Xen, but
     * until that is fully handled in this code make sure that we only use
     * 4K page sizes.
     */
    BUILD_BUG_ON(PAGE_SIZE != FFA_PAGE_SIZE);

    /*
     * psci_init_smccc() updates this value with what's reported by EL-3
     * or secure world.
     */
    if ( smccc_ver < ARM_SMCCC_VERSION_1_2 )
    {
        printk(XENLOG_ERR
               "ffa: unsupported SMCCC version %#x (need at least %#x)\n",
               smccc_ver, ARM_SMCCC_VERSION_1_2);
        return false;
    }

    if ( !ffa_get_version(&vers) )
        return false;

    if ( vers < FFA_MIN_SPMC_VERSION || vers > FFA_MY_VERSION )
    {
        printk(XENLOG_ERR "ffa: Incompatible version %#x found\n", vers);
        return false;
    }

    major_vers = (vers >> FFA_VERSION_MAJOR_SHIFT) & FFA_VERSION_MAJOR_MASK;
    minor_vers = vers & FFA_VERSION_MINOR_MASK;
    printk(XENLOG_INFO "ARM FF-A Mediator version %u.%u\n",
           FFA_MY_VERSION_MAJOR, FFA_MY_VERSION_MINOR);
    printk(XENLOG_INFO "ARM FF-A Firmware version %u.%u\n",
           major_vers, minor_vers);

    /*
     * At the moment domains must support the same features used by Xen.
     * TODO: Rework the code to allow domain to use a subset of the
     * features supported.
     */
    if ( !check_mandatory_feature(FFA_MSG_SEND_DIRECT_REQ_32) )
        return false;

    ffa_version = vers;

    return true;
}

static const struct tee_mediator_ops ffa_ops =
{
    .probe = ffa_probe,
    .domain_init = ffa_domain_init,
    .domain_teardown = ffa_domain_teardown,
    .relinquish_resources = ffa_relinquish_resources,
    .handle_call = ffa_handle_call,
};

REGISTER_TEE_MEDIATOR(ffa, "FF-A", XEN_DOMCTL_CONFIG_TEE_FFA, &ffa_ops);
