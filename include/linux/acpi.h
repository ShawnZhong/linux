/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * acpi.h - ACPI Interface
 *
 * Copyright (C) 2001 Paul Diefenbaugh <paul.s.diefenbaugh@intel.com>
 */

#ifndef _LINUX_ACPI_H
#define _LINUX_ACPI_H

#include <linux/errno.h>
#include <linux/ioport.h>	/* for struct resource */
#include <linux/resource_ext.h>
#include <linux/device.h>
#include <linux/mod_devicetable.h>
#include <linux/property.h>
#include <linux/uuid.h>
#include <linux/node.h>

struct irq_domain;
struct irq_domain_ops;

#ifndef _LINUX
#define _LINUX
#endif
#include <acpi/acpi.h>
#include <acpi/acpi_numa.h>

#ifdef	CONFIG_ACPI

#include <linux/list.h>
#include <linux/dynamic_debug.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/fw_table.h>

#include <acpi/acpi_bus.h>
#include <acpi/acpi_drivers.h>
#include <acpi/acpi_io.h>
#include <asm/acpi.h>

#ifdef CONFIG_ACPI_TABLE_LIB
#define EXPORT_SYMBOL_ACPI_LIB(x) EXPORT_SYMBOL_NS_GPL(x, "ACPI")
#define __init_or_acpilib
#define __initdata_or_acpilib
#else
#define EXPORT_SYMBOL_ACPI_LIB(x)
#define __init_or_acpilib __init
#define __initdata_or_acpilib __initdata
#endif

static inline acpi_handle acpi_device_handle(struct acpi_device *adev)
{
	return adev ? adev->handle : NULL;
}

#define ACPI_COMPANION(dev)		to_acpi_device_node((dev)->fwnode)
#define ACPI_COMPANION_SET(dev, adev)	set_primary_fwnode(dev, (adev) ? \
	acpi_fwnode_handle(adev) : NULL)
#define ACPI_HANDLE(dev)		acpi_device_handle(ACPI_COMPANION(dev))
#define ACPI_HANDLE_FWNODE(fwnode)	\
				acpi_device_handle(to_acpi_device_node(fwnode))

static inline struct fwnode_handle *acpi_alloc_fwnode_static(void)
{
	struct fwnode_handle *fwnode;

	fwnode = kzalloc(sizeof(struct fwnode_handle), GFP_KERNEL);
	if (!fwnode)
		return NULL;

	fwnode_init(fwnode, &acpi_static_fwnode_ops);

	return fwnode;
}

static inline void acpi_free_fwnode_static(struct fwnode_handle *fwnode)
{
	if (WARN_ON(!is_acpi_static_node(fwnode)))
		return;

	kfree(fwnode);
}

static inline bool has_acpi_companion(struct device *dev)
{
	return is_acpi_device_node(dev->fwnode);
}

static inline void acpi_preset_companion(struct device *dev,
					 struct acpi_device *parent, u64 addr)
{
	ACPI_COMPANION_SET(dev, acpi_find_child_device(parent, addr, false));
}

static inline const char *acpi_dev_name(struct acpi_device *adev)
{
	return dev_name(&adev->dev);
}

struct device *acpi_get_first_physical_node(struct acpi_device *adev);

enum acpi_irq_model_id {
	ACPI_IRQ_MODEL_PIC = 0,
	ACPI_IRQ_MODEL_IOAPIC,
	ACPI_IRQ_MODEL_IOSAPIC,
	ACPI_IRQ_MODEL_PLATFORM,
	ACPI_IRQ_MODEL_GIC,
	ACPI_IRQ_MODEL_LPIC,
	ACPI_IRQ_MODEL_RINTC,
	ACPI_IRQ_MODEL_COUNT
};

extern enum acpi_irq_model_id	acpi_irq_model;

enum acpi_interrupt_id {
	ACPI_INTERRUPT_PMI	= 1,
	ACPI_INTERRUPT_INIT,
	ACPI_INTERRUPT_CPEI,
	ACPI_INTERRUPT_COUNT
};

#define	ACPI_SPACE_MEM		0

enum acpi_address_range_id {
	ACPI_ADDRESS_RANGE_MEMORY = 1,
	ACPI_ADDRESS_RANGE_RESERVED = 2,
	ACPI_ADDRESS_RANGE_ACPI = 3,
	ACPI_ADDRESS_RANGE_NVS	= 4,
	ACPI_ADDRESS_RANGE_COUNT
};


/* Table Handlers */
typedef int (*acpi_tbl_table_handler)(struct acpi_table_header *table);

/* Debugger support */

struct acpi_debugger_ops {
	int (*create_thread)(acpi_osd_exec_callback function, void *context);
	ssize_t (*write_log)(const char *msg);
	ssize_t (*read_cmd)(char *buffer, size_t length);
	int (*wait_command_ready)(bool single_step, char *buffer, size_t length);
	int (*notify_command_complete)(void);
};

struct acpi_debugger {
	const struct acpi_debugger_ops *ops;
	struct module *owner;
	struct mutex lock;
};

#ifdef CONFIG_ACPI_DEBUGGER
int __init acpi_debugger_init(void);
int acpi_register_debugger(struct module *owner,
			   const struct acpi_debugger_ops *ops);
void acpi_unregister_debugger(const struct acpi_debugger_ops *ops);
int acpi_debugger_create_thread(acpi_osd_exec_callback function, void *context);
ssize_t acpi_debugger_write_log(const char *msg);
ssize_t acpi_debugger_read_cmd(char *buffer, size_t buffer_length);
int acpi_debugger_wait_command_ready(void);
int acpi_debugger_notify_command_complete(void);
#else
static inline int acpi_debugger_init(void)
{
	return -ENODEV;
}

static inline int acpi_register_debugger(struct module *owner,
					 const struct acpi_debugger_ops *ops)
{
	return -ENODEV;
}

static inline void acpi_unregister_debugger(const struct acpi_debugger_ops *ops)
{
}

static inline int acpi_debugger_create_thread(acpi_osd_exec_callback function,
					      void *context)
{
	return -ENODEV;
}

static inline int acpi_debugger_write_log(const char *msg)
{
	return -ENODEV;
}

static inline int acpi_debugger_read_cmd(char *buffer, u32 buffer_length)
{
	return -ENODEV;
}

static inline int acpi_debugger_wait_command_ready(void)
{
	return -ENODEV;
}

static inline int acpi_debugger_notify_command_complete(void)
{
	return -ENODEV;
}
#endif

#define BAD_MADT_ENTRY(entry, end) (					    \
		(!entry) || (unsigned long)entry + sizeof(*entry) > end ||  \
		((struct acpi_subtable_header *)entry)->length < sizeof(*entry))

void __iomem *__acpi_map_table(unsigned long phys, unsigned long size);
void __acpi_unmap_table(void __iomem *map, unsigned long size);
int early_acpi_boot_init(void);
int acpi_boot_init (void);
void acpi_boot_table_prepare (void);
void acpi_boot_table_init (void);
int acpi_mps_check (void);
int acpi_numa_init (void);

int acpi_locate_initial_tables (void);
void acpi_reserve_initial_tables (void);
void acpi_table_init_complete (void);
int acpi_table_init (void);

int acpi_table_parse(char *id, acpi_tbl_table_handler handler);
int __init_or_acpilib acpi_table_parse_entries(char *id,
		unsigned long table_size, int entry_id,
		acpi_tbl_entry_handler handler, unsigned int max_entries);
int __init_or_acpilib acpi_table_parse_entries_array(char *id,
		unsigned long table_size, struct acpi_subtable_proc *proc,
		int proc_num, unsigned int max_entries);
int acpi_table_parse_madt(enum acpi_madt_type id,
			  acpi_tbl_entry_handler handler,
			  unsigned int max_entries);
int __init_or_acpilib
acpi_table_parse_cedt(enum acpi_cedt_type id,
		      acpi_tbl_entry_handler_arg handler_arg, void *arg);

int acpi_parse_mcfg (struct acpi_table_header *header);
void acpi_table_print_madt_entry (struct acpi_subtable_header *madt);

#if defined(CONFIG_X86) || defined(CONFIG_LOONGARCH)
void acpi_numa_processor_affinity_init (struct acpi_srat_cpu_affinity *pa);
#else
static inline void
acpi_numa_processor_affinity_init(struct acpi_srat_cpu_affinity *pa) { }
#endif

void acpi_numa_x2apic_affinity_init(struct acpi_srat_x2apic_cpu_affinity *pa);

#if defined(CONFIG_ARM64) || defined(CONFIG_LOONGARCH)
void acpi_arch_dma_setup(struct device *dev);
#else
static inline void acpi_arch_dma_setup(struct device *dev) { }
#endif

#ifdef CONFIG_ARM64
void acpi_numa_gicc_affinity_init(struct acpi_srat_gicc_affinity *pa);
#else
static inline void
acpi_numa_gicc_affinity_init(struct acpi_srat_gicc_affinity *pa) { }
#endif

#ifdef CONFIG_RISCV
void acpi_numa_rintc_affinity_init(struct acpi_srat_rintc_affinity *pa);
#else
static inline void acpi_numa_rintc_affinity_init(struct acpi_srat_rintc_affinity *pa) { }
#endif

#ifndef PHYS_CPUID_INVALID
typedef u32 phys_cpuid_t;
#define PHYS_CPUID_INVALID (phys_cpuid_t)(-1)
#endif

static inline bool invalid_logical_cpuid(u32 cpuid)
{
	return (int)cpuid < 0;
}

static inline bool invalid_phys_cpuid(phys_cpuid_t phys_id)
{
	return phys_id == PHYS_CPUID_INVALID;
}


int __init acpi_get_madt_revision(void);

/* Validate the processor object's proc_id */
bool acpi_duplicate_processor_id(int proc_id);
/* Processor _CTS control */
struct acpi_processor_power;

#ifdef CONFIG_ACPI_PROCESSOR_CSTATE
bool acpi_processor_claim_cst_control(void);
int acpi_processor_evaluate_cst(acpi_handle handle, u32 cpu,
				struct acpi_processor_power *info);
#else
static inline bool acpi_processor_claim_cst_control(void) { return false; }
static inline int acpi_processor_evaluate_cst(acpi_handle handle, u32 cpu,
					      struct acpi_processor_power *info)
{
	return -ENODEV;
}
#endif

#ifdef CONFIG_ACPI_HOTPLUG_CPU
/* Arch dependent functions for cpu hotplug support */
int acpi_map_cpu(acpi_handle handle, phys_cpuid_t physid, u32 acpi_id,
		 int *pcpu);
int acpi_unmap_cpu(int cpu);
#endif /* CONFIG_ACPI_HOTPLUG_CPU */

acpi_handle acpi_get_processor_handle(int cpu);

#ifdef CONFIG_ACPI_HOTPLUG_IOAPIC
int acpi_get_ioapic_id(acpi_handle handle, u32 gsi_base, u64 *phys_addr);
#endif

int acpi_register_ioapic(acpi_handle handle, u64 phys_addr, u32 gsi_base);
int acpi_unregister_ioapic(acpi_handle handle, u32 gsi_base);
int acpi_ioapic_registered(acpi_handle handle, u32 gsi_base);
void acpi_irq_stats_init(void);
extern u32 acpi_irq_handled;
extern u32 acpi_irq_not_handled;
extern unsigned int acpi_sci_irq;
extern bool acpi_no_s5;
#define INVALID_ACPI_IRQ	((unsigned)-1)
static inline bool acpi_sci_irq_valid(void)
{
	return acpi_sci_irq != INVALID_ACPI_IRQ;
}

extern int sbf_port;

int acpi_register_gsi (struct device *dev, u32 gsi, int triggering, int polarity);
int acpi_gsi_to_irq (u32 gsi, unsigned int *irq);
int acpi_isa_irq_to_gsi (unsigned isa_irq, u32 *gsi);

typedef struct fwnode_handle *(*acpi_gsi_domain_disp_fn)(u32);

void acpi_set_irq_model(enum acpi_irq_model_id model,
			acpi_gsi_domain_disp_fn fn);
acpi_gsi_domain_disp_fn acpi_get_gsi_dispatcher(void);
void acpi_set_gsi_to_irq_fallback(u32 (*)(u32));

struct irq_domain *acpi_irq_create_hierarchy(unsigned int flags,
					     unsigned int size,
					     struct fwnode_handle *fwnode,
					     const struct irq_domain_ops *ops,
					     void *host_data);

#ifdef CONFIG_X86_IO_APIC
extern int acpi_get_override_irq(u32 gsi, int *trigger, int *polarity);
#else
static inline int acpi_get_override_irq(u32 gsi, int *trigger, int *polarity)
{
	return -1;
}
#endif
/*
 * This function undoes the effect of one call to acpi_register_gsi().
 * If this matches the last registration, any IRQ resources for gsi
 * are freed.
 */
void acpi_unregister_gsi (u32 gsi);

struct pci_dev;

struct acpi_prt_entry *acpi_pci_irq_lookup(struct pci_dev *dev, int pin);
int acpi_pci_irq_enable (struct pci_dev *dev);
void acpi_penalize_isa_irq(int irq, int active);
bool acpi_isa_irq_available(int irq);
#ifdef CONFIG_PCI
void acpi_penalize_sci_irq(int irq, int trigger, int polarity);
#else
static inline void acpi_penalize_sci_irq(int irq, int trigger,
					int polarity)
{
}
#endif
void acpi_pci_irq_disable (struct pci_dev *dev);

extern int ec_read(u8 addr, u8 *val);
extern int ec_write(u8 addr, u8 val);
extern int ec_transaction(u8 command,
                          const u8 *wdata, unsigned wdata_len,
                          u8 *rdata, unsigned rdata_len);
extern acpi_handle ec_get_handle(void);

extern bool acpi_is_pnp_device(struct acpi_device *);

#if defined(CONFIG_ACPI_WMI) || defined(CONFIG_ACPI_WMI_MODULE)

typedef void (*wmi_notify_handler) (union acpi_object *data, void *context);

int wmi_instance_count(const char *guid);

extern acpi_status wmi_evaluate_method(const char *guid, u8 instance,
					u32 method_id,
					const struct acpi_buffer *in,
					struct acpi_buffer *out);
extern acpi_status wmi_query_block(const char *guid, u8 instance,
					struct acpi_buffer *out);
extern acpi_status wmi_set_block(const char *guid, u8 instance,
					const struct acpi_buffer *in);
extern acpi_status wmi_install_notify_handler(const char *guid,
					wmi_notify_handler handler, void *data);
extern acpi_status wmi_remove_notify_handler(const char *guid);
extern bool wmi_has_guid(const char *guid);
extern char *wmi_get_acpi_device_uid(const char *guid);

#endif	/* CONFIG_ACPI_WMI */

#define ACPI_VIDEO_OUTPUT_SWITCHING			0x0001
#define ACPI_VIDEO_DEVICE_POSTING			0x0002
#define ACPI_VIDEO_ROM_AVAILABLE			0x0004
#define ACPI_VIDEO_BACKLIGHT				0x0008
#define ACPI_VIDEO_BACKLIGHT_FORCE_VENDOR		0x0010
#define ACPI_VIDEO_BACKLIGHT_FORCE_VIDEO		0x0020
#define ACPI_VIDEO_OUTPUT_SWITCHING_FORCE_VENDOR	0x0040
#define ACPI_VIDEO_OUTPUT_SWITCHING_FORCE_VIDEO		0x0080
#define ACPI_VIDEO_BACKLIGHT_DMI_VENDOR			0x0100
#define ACPI_VIDEO_BACKLIGHT_DMI_VIDEO			0x0200
#define ACPI_VIDEO_OUTPUT_SWITCHING_DMI_VENDOR		0x0400
#define ACPI_VIDEO_OUTPUT_SWITCHING_DMI_VIDEO		0x0800

extern char acpi_video_backlight_string[];
extern long acpi_is_video_device(acpi_handle handle);

extern void acpi_osi_setup(char *str);
extern bool acpi_osi_is_win8(void);

#ifdef CONFIG_ACPI_THERMAL_LIB
int thermal_acpi_active_trip_temp(struct acpi_device *adev, int id, int *ret_temp);
int thermal_acpi_passive_trip_temp(struct acpi_device *adev, int *ret_temp);
int thermal_acpi_hot_trip_temp(struct acpi_device *adev, int *ret_temp);
int thermal_acpi_critical_trip_temp(struct acpi_device *adev, int *ret_temp);
#endif

#ifdef CONFIG_ACPI_HMAT
int acpi_get_genport_coordinates(u32 uid, struct access_coordinate *coord);
#else
static inline int acpi_get_genport_coordinates(u32 uid,
					       struct access_coordinate *coord)
{
	return -EOPNOTSUPP;
}
#endif

#ifdef CONFIG_ACPI_NUMA
int acpi_map_pxm_to_node(int pxm);
int acpi_get_node(acpi_handle handle);

/**
 * pxm_to_online_node - Map proximity ID to online node
 * @pxm: ACPI proximity ID
 *
 * This is similar to pxm_to_node(), but always returns an online
 * node.  When the mapped node from a given proximity ID is offline, it
 * looks up the node distance table and returns the nearest online node.
 *
 * ACPI device drivers, which are called after the NUMA initialization has
 * completed in the kernel, can call this interface to obtain their device
 * NUMA topology from ACPI tables.  Such drivers do not have to deal with
 * offline nodes.  A node may be offline when SRAT memory entry does not exist,
 * or NUMA is disabled, ex. "numa=off" on x86.
 */
static inline int pxm_to_online_node(int pxm)
{
	int node = pxm_to_node(pxm);

	return numa_map_to_online_node(node);
}
#else
static inline int pxm_to_online_node(int pxm)
{
	return 0;
}
static inline int acpi_map_pxm_to_node(int pxm)
{
	return 0;
}
static inline int acpi_get_node(acpi_handle handle)
{
	return 0;
}
#endif
extern int pnpacpi_disabled;

#define PXM_INVAL	(-1)

bool acpi_dev_resource_memory(struct acpi_resource *ares, struct resource *res);
bool acpi_dev_resource_io(struct acpi_resource *ares, struct resource *res);
bool acpi_dev_resource_address_space(struct acpi_resource *ares,
				     struct resource_win *win);
bool acpi_dev_resource_ext_address_space(struct acpi_resource *ares,
					 struct resource_win *win);
unsigned long acpi_dev_irq_flags(u8 triggering, u8 polarity, u8 shareable, u8 wake_capable);
unsigned int acpi_dev_get_irq_type(int triggering, int polarity);
bool acpi_dev_resource_interrupt(struct acpi_resource *ares, int index,
				 struct resource *res);

void acpi_dev_free_resource_list(struct list_head *list);
int acpi_dev_get_resources(struct acpi_device *adev, struct list_head *list,
			   int (*preproc)(struct acpi_resource *, void *),
			   void *preproc_data);
int acpi_dev_get_dma_resources(struct acpi_device *adev,
			       struct list_head *list);
int acpi_dev_get_memory_resources(struct acpi_device *adev, struct list_head *list);
int acpi_dev_filter_resource_type(struct acpi_resource *ares,
				  unsigned long types);

static inline int acpi_dev_filter_resource_type_cb(struct acpi_resource *ares,
						   void *arg)
{
	return acpi_dev_filter_resource_type(ares, (unsigned long)arg);
}

struct acpi_device *acpi_resource_consumer(struct resource *res);

int acpi_check_resource_conflict(const struct resource *res);

int acpi_check_region(resource_size_t start, resource_size_t n,
		      const char *name);

int acpi_resources_are_enforced(void);

#ifdef CONFIG_HIBERNATION
extern int acpi_check_s4_hw_signature;
#endif

#ifdef CONFIG_PM_SLEEP
void __init acpi_old_suspend_ordering(void);
void __init acpi_nvs_nosave(void);
void __init acpi_nvs_nosave_s3(void);
void __init acpi_sleep_no_blacklist(void);
#endif /* CONFIG_PM_SLEEP */

int acpi_register_wakeup_handler(
	int wake_irq, bool (*wakeup)(void *context), void *context);
void acpi_unregister_wakeup_handler(
	bool (*wakeup)(void *context), void *context);

struct acpi_osc_context {
	char *uuid_str;			/* UUID string */
	int rev;
	struct acpi_buffer cap;		/* list of DWORD capabilities */
	struct acpi_buffer ret;		/* free by caller if success */
};

acpi_status acpi_run_osc(acpi_handle handle, struct acpi_osc_context *context);

/* Number of _OSC capability DWORDS depends on bridge type */
#define OSC_PCI_CAPABILITY_DWORDS		3
#define OSC_CXL_CAPABILITY_DWORDS		5

/* Indexes into _OSC Capabilities Buffer (DWORDs 2 to 5 are device-specific) */
#define OSC_QUERY_DWORD				0	/* DWORD 1 */
#define OSC_SUPPORT_DWORD			1	/* DWORD 2 */
#define OSC_CONTROL_DWORD			2	/* DWORD 3 */
#define OSC_EXT_SUPPORT_DWORD			3	/* DWORD 4 */
#define OSC_EXT_CONTROL_DWORD			4	/* DWORD 5 */

/* _OSC Capabilities DWORD 1: Query/Control and Error Returns (generic) */
#define OSC_QUERY_ENABLE			0x00000001  /* input */
#define OSC_REQUEST_ERROR			0x00000002  /* return */
#define OSC_INVALID_UUID_ERROR			0x00000004  /* return */
#define OSC_INVALID_REVISION_ERROR		0x00000008  /* return */
#define OSC_CAPABILITIES_MASK_ERROR		0x00000010  /* return */

/* Platform-Wide Capabilities _OSC: Capabilities DWORD 2: Support Field */
#define OSC_SB_PAD_SUPPORT			0x00000001
#define OSC_SB_PPC_OST_SUPPORT			0x00000002
#define OSC_SB_PR3_SUPPORT			0x00000004
#define OSC_SB_HOTPLUG_OST_SUPPORT		0x00000008
#define OSC_SB_APEI_SUPPORT			0x00000010
#define OSC_SB_CPC_SUPPORT			0x00000020
#define OSC_SB_CPCV2_SUPPORT			0x00000040
#define OSC_SB_PCLPI_SUPPORT			0x00000080
#define OSC_SB_OSLPI_SUPPORT			0x00000100
#define OSC_SB_FAST_THERMAL_SAMPLING_SUPPORT	0x00000200
#define OSC_SB_OVER_16_PSTATES_SUPPORT		0x00000400
#define OSC_SB_GED_SUPPORT			0x00000800
#define OSC_SB_CPC_DIVERSE_HIGH_SUPPORT		0x00001000
#define OSC_SB_IRQ_RESOURCE_SOURCE_SUPPORT	0x00002000
#define OSC_SB_CPC_FLEXIBLE_ADR_SPACE		0x00004000
#define OSC_SB_GENERIC_INITIATOR_SUPPORT	0x00020000
#define OSC_SB_NATIVE_USB4_SUPPORT		0x00040000
#define OSC_SB_BATTERY_CHARGE_LIMITING_SUPPORT	0x00080000
#define OSC_SB_PRM_SUPPORT			0x00200000
#define OSC_SB_FFH_OPR_SUPPORT			0x00400000

extern bool osc_sb_apei_support_acked;
extern bool osc_pc_lpi_support_confirmed;
extern bool osc_sb_native_usb4_support_confirmed;
extern bool osc_sb_cppc2_support_acked;
extern bool osc_cpc_flexible_adr_space_confirmed;

/* USB4 Capabilities */
#define OSC_USB_USB3_TUNNELING			0x00000001
#define OSC_USB_DP_TUNNELING			0x00000002
#define OSC_USB_PCIE_TUNNELING			0x00000004
#define OSC_USB_XDOMAIN				0x00000008

extern u32 osc_sb_native_usb4_control;

/* PCI Host Bridge _OSC: Capabilities DWORD 2: Support Field */
#define OSC_PCI_EXT_CONFIG_SUPPORT		0x00000001
#define OSC_PCI_ASPM_SUPPORT			0x00000002
#define OSC_PCI_CLOCK_PM_SUPPORT		0x00000004
#define OSC_PCI_SEGMENT_GROUPS_SUPPORT		0x00000008
#define OSC_PCI_MSI_SUPPORT			0x00000010
#define OSC_PCI_EDR_SUPPORT			0x00000080
#define OSC_PCI_HPX_TYPE_3_SUPPORT		0x00000100

/* PCI Host Bridge _OSC: Capabilities DWORD 3: Control Field */
#define OSC_PCI_EXPRESS_NATIVE_HP_CONTROL	0x00000001
#define OSC_PCI_SHPC_NATIVE_HP_CONTROL		0x00000002
#define OSC_PCI_EXPRESS_PME_CONTROL		0x00000004
#define OSC_PCI_EXPRESS_AER_CONTROL		0x00000008
#define OSC_PCI_EXPRESS_CAPABILITY_CONTROL	0x00000010
#define OSC_PCI_EXPRESS_LTR_CONTROL		0x00000020
#define OSC_PCI_EXPRESS_DPC_CONTROL		0x00000080

/* CXL _OSC: Capabilities DWORD 4: Support Field */
#define OSC_CXL_1_1_PORT_REG_ACCESS_SUPPORT	0x00000001
#define OSC_CXL_2_0_PORT_DEV_REG_ACCESS_SUPPORT	0x00000002
#define OSC_CXL_PROTOCOL_ERR_REPORTING_SUPPORT	0x00000004
#define OSC_CXL_NATIVE_HP_SUPPORT		0x00000008

/* CXL _OSC: Capabilities DWORD 5: Control Field */
#define OSC_CXL_ERROR_REPORTING_CONTROL		0x00000001

static inline u32 acpi_osc_ctx_get_pci_control(struct acpi_osc_context *context)
{
	u32 *ret = context->ret.pointer;

	return ret[OSC_CONTROL_DWORD];
}

static inline u32 acpi_osc_ctx_get_cxl_control(struct acpi_osc_context *context)
{
	u32 *ret = context->ret.pointer;

	return ret[OSC_EXT_CONTROL_DWORD];
}

#define ACPI_GSB_ACCESS_ATTRIB_QUICK		0x00000002
#define ACPI_GSB_ACCESS_ATTRIB_SEND_RCV         0x00000004
#define ACPI_GSB_ACCESS_ATTRIB_BYTE		0x00000006
#define ACPI_GSB_ACCESS_ATTRIB_WORD		0x00000008
#define ACPI_GSB_ACCESS_ATTRIB_BLOCK		0x0000000A
#define ACPI_GSB_ACCESS_ATTRIB_MULTIBYTE	0x0000000B
#define ACPI_GSB_ACCESS_ATTRIB_WORD_CALL	0x0000000C
#define ACPI_GSB_ACCESS_ATTRIB_BLOCK_CALL	0x0000000D
#define ACPI_GSB_ACCESS_ATTRIB_RAW_BYTES	0x0000000E
#define ACPI_GSB_ACCESS_ATTRIB_RAW_PROCESS	0x0000000F

/* Enable _OST when all relevant hotplug operations are enabled */
#if defined(CONFIG_ACPI_HOTPLUG_CPU) &&			\
	defined(CONFIG_ACPI_HOTPLUG_MEMORY) &&		\
	defined(CONFIG_ACPI_CONTAINER)
#define ACPI_HOTPLUG_OST
#endif

/* _OST Source Event Code (OSPM Action) */
#define ACPI_OST_EC_OSPM_SHUTDOWN		0x100
#define ACPI_OST_EC_OSPM_EJECT			0x103
#define ACPI_OST_EC_OSPM_INSERTION		0x200

/* _OST General Processing Status Code */
#define ACPI_OST_SC_SUCCESS			0x0
#define ACPI_OST_SC_NON_SPECIFIC_FAILURE	0x1
#define ACPI_OST_SC_UNRECOGNIZED_NOTIFY		0x2

/* _OST OS Shutdown Processing (0x100) Status Code */
#define ACPI_OST_SC_OS_SHUTDOWN_DENIED		0x80
#define ACPI_OST_SC_OS_SHUTDOWN_IN_PROGRESS	0x81
#define ACPI_OST_SC_OS_SHUTDOWN_COMPLETED	0x82
#define ACPI_OST_SC_OS_SHUTDOWN_NOT_SUPPORTED	0x83

/* _OST Ejection Request (0x3, 0x103) Status Code */
#define ACPI_OST_SC_EJECT_NOT_SUPPORTED		0x80
#define ACPI_OST_SC_DEVICE_IN_USE		0x81
#define ACPI_OST_SC_DEVICE_BUSY			0x82
#define ACPI_OST_SC_EJECT_DEPENDENCY_BUSY	0x83
#define ACPI_OST_SC_EJECT_IN_PROGRESS		0x84

/* _OST Insertion Request (0x200) Status Code */
#define ACPI_OST_SC_INSERT_IN_PROGRESS		0x80
#define ACPI_OST_SC_DRIVER_LOAD_FAILURE		0x81
#define ACPI_OST_SC_INSERT_NOT_SUPPORTED	0x82

enum acpi_predicate {
	all_versions,
	less_than_or_equal,
	equal,
	greater_than_or_equal,
};

/* Table must be terminted by a NULL entry */
struct acpi_platform_list {
	char	oem_id[ACPI_OEM_ID_SIZE+1];
	char	oem_table_id[ACPI_OEM_TABLE_ID_SIZE+1];
	u32	oem_revision;
	char	*table;
	enum acpi_predicate pred;
	char	*reason;
	u32	data;
};
int acpi_match_platform_list(const struct acpi_platform_list *plat);

extern void acpi_early_init(void);
extern void acpi_subsystem_init(void);

extern int acpi_nvs_register(__u64 start, __u64 size);

extern int acpi_nvs_for_each_region(int (*func)(__u64, __u64, void *),
				    void *data);

const struct acpi_device_id *acpi_match_acpi_device(const struct acpi_device_id *ids,
						    const struct acpi_device *adev);

const struct acpi_device_id *acpi_match_device(const struct acpi_device_id *ids,
					       const struct device *dev);

const void *acpi_device_get_match_data(const struct device *dev);
extern bool acpi_driver_match_device(struct device *dev,
				     const struct device_driver *drv);
int acpi_device_uevent_modalias(const struct device *, struct kobj_uevent_env *);
int acpi_device_modalias(struct device *, char *, int);

struct platform_device *acpi_create_platform_device(struct acpi_device *,
						    const struct property_entry *);
#define ACPI_PTR(_ptr)	(_ptr)

static inline void acpi_device_set_enumerated(struct acpi_device *adev)
{
	adev->flags.visited = true;
}

static inline void acpi_device_clear_enumerated(struct acpi_device *adev)
{
	adev->flags.visited = false;
}

enum acpi_reconfig_event  {
	ACPI_RECONFIG_DEVICE_ADD = 0,
	ACPI_RECONFIG_DEVICE_REMOVE,
};

int acpi_reconfig_notifier_register(struct notifier_block *nb);
int acpi_reconfig_notifier_unregister(struct notifier_block *nb);

#ifdef CONFIG_ACPI_GTDT
int acpi_gtdt_init(struct acpi_table_header *table, int *platform_timer_count);
int acpi_gtdt_map_ppi(int type);
bool acpi_gtdt_c3stop(int type);
int acpi_arch_timer_mem_init(struct arch_timer_mem *timer_mem, int *timer_count);
#endif

#ifndef ACPI_HAVE_ARCH_SET_ROOT_POINTER
static __always_inline void acpi_arch_set_root_pointer(u64 addr)
{
}
#endif

#ifndef ACPI_HAVE_ARCH_GET_ROOT_POINTER
static __always_inline u64 acpi_arch_get_root_pointer(void)
{
	return 0;
}
#endif

int acpi_get_local_u64_address(acpi_handle handle, u64 *addr);
int acpi_get_local_address(acpi_handle handle, u32 *addr);
const char *acpi_get_subsystem_id(acpi_handle handle);

#ifdef CONFIG_ACPI_MRRM
int acpi_mrrm_max_mem_region(void);
#endif

#else	/* !CONFIG_ACPI */

#define acpi_disabled 1

#define ACPI_COMPANION(dev)		(NULL)
#define ACPI_COMPANION_SET(dev, adev)	do { } while (0)
#define ACPI_HANDLE(dev)		(NULL)
#define ACPI_HANDLE_FWNODE(fwnode)	(NULL)

/* Get rid of the -Wunused-variable for adev */
#define acpi_dev_uid_match(adev, uid2)			(adev && false)
#define acpi_dev_hid_uid_match(adev, hid2, uid2)	(adev && false)

struct fwnode_handle;

static inline bool acpi_dev_found(const char *hid)
{
	return false;
}

static inline bool acpi_dev_present(const char *hid, const char *uid, s64 hrv)
{
	return false;
}

struct acpi_device;

static inline int acpi_dev_uid_to_integer(struct acpi_device *adev, u64 *integer)
{
	return -ENODEV;
}

static inline struct acpi_device *
acpi_dev_get_first_match_dev(const char *hid, const char *uid, s64 hrv)
{
	return NULL;
}

static inline bool acpi_reduced_hardware(void)
{
	return false;
}

static inline void acpi_dev_put(struct acpi_device *adev) {}

static inline bool is_acpi_node(const struct fwnode_handle *fwnode)
{
	return false;
}

static inline bool is_acpi_device_node(const struct fwnode_handle *fwnode)
{
	return false;
}

static inline struct acpi_device *to_acpi_device_node(const struct fwnode_handle *fwnode)
{
	return NULL;
}

static inline bool is_acpi_data_node(const struct fwnode_handle *fwnode)
{
	return false;
}

static inline struct acpi_data_node *to_acpi_data_node(const struct fwnode_handle *fwnode)
{
	return NULL;
}

static inline bool acpi_data_node_match(const struct fwnode_handle *fwnode,
					const char *name)
{
	return false;
}

static inline struct fwnode_handle *acpi_fwnode_handle(struct acpi_device *adev)
{
	return NULL;
}

static inline acpi_handle acpi_device_handle(struct acpi_device *adev)
{
	return NULL;
}

static inline bool has_acpi_companion(struct device *dev)
{
	return false;
}

static inline void acpi_preset_companion(struct device *dev,
					 struct acpi_device *parent, u64 addr)
{
}

static inline const char *acpi_dev_name(struct acpi_device *adev)
{
	return NULL;
}

static inline struct device *acpi_get_first_physical_node(struct acpi_device *adev)
{
	return NULL;
}

static inline void acpi_early_init(void) { }
static inline void acpi_subsystem_init(void) { }

static inline int early_acpi_boot_init(void)
{
	return 0;
}
static inline int acpi_boot_init(void)
{
	return 0;
}

static inline void acpi_boot_table_prepare(void)
{
}

static inline void acpi_boot_table_init(void)
{
}

static inline int acpi_mps_check(void)
{
	return 0;
}

static inline int acpi_check_resource_conflict(struct resource *res)
{
	return 0;
}

static inline int acpi_check_region(resource_size_t start, resource_size_t n,
				    const char *name)
{
	return 0;
}

struct acpi_table_header;
static inline int acpi_table_parse(char *id,
				int (*handler)(struct acpi_table_header *))
{
	return -ENODEV;
}

static inline int acpi_nvs_register(__u64 start, __u64 size)
{
	return 0;
}

static inline int acpi_nvs_for_each_region(int (*func)(__u64, __u64, void *),
					   void *data)
{
	return 0;
}

struct acpi_device_id;

static inline const struct acpi_device_id *acpi_match_acpi_device(
	const struct acpi_device_id *ids, const struct acpi_device *adev)
{
	return NULL;
}

static inline const struct acpi_device_id *acpi_match_device(
	const struct acpi_device_id *ids, const struct device *dev)
{
	return NULL;
}

static inline const void *acpi_device_get_match_data(const struct device *dev)
{
	return NULL;
}

static inline bool acpi_driver_match_device(struct device *dev,
					    const struct device_driver *drv)
{
	return false;
}

static inline bool acpi_check_dsm(acpi_handle handle, const guid_t *guid,
				  u64 rev, u64 funcs)
{
	return false;
}

static inline union acpi_object *acpi_evaluate_dsm(acpi_handle handle,
						   const guid_t *guid,
						   u64 rev, u64 func,
						   union acpi_object *argv4)
{
	return NULL;
}

static inline union acpi_object *acpi_evaluate_dsm_typed(acpi_handle handle,
							 const guid_t *guid,
							 u64 rev, u64 func,
							 union acpi_object *argv4,
							 acpi_object_type type)
{
	return NULL;
}

static inline int acpi_device_uevent_modalias(const struct device *dev,
				struct kobj_uevent_env *env)
{
	return -ENODEV;
}

static inline int acpi_device_modalias(struct device *dev,
				char *buf, int size)
{
	return -ENODEV;
}

static inline struct platform_device *
acpi_create_platform_device(struct acpi_device *adev,
			    const struct property_entry *properties)
{
	return NULL;
}

static inline bool acpi_dma_supported(const struct acpi_device *adev)
{
	return false;
}

static inline enum dev_dma_attr acpi_get_dma_attr(struct acpi_device *adev)
{
	return DEV_DMA_NOT_SUPPORTED;
}

static inline int acpi_dma_get_range(struct device *dev, const struct bus_dma_region **map)
{
	return -ENODEV;
}

static inline int acpi_dma_configure(struct device *dev,
				     enum dev_dma_attr attr)
{
	return 0;
}

static inline int acpi_dma_configure_id(struct device *dev,
					enum dev_dma_attr attr,
					const u32 *input_id)
{
	return 0;
}

#define ACPI_PTR(_ptr)	(NULL)

static inline void acpi_device_set_enumerated(struct acpi_device *adev)
{
}

static inline void acpi_device_clear_enumerated(struct acpi_device *adev)
{
}

static inline int acpi_reconfig_notifier_register(struct notifier_block *nb)
{
	return -EINVAL;
}

static inline int acpi_reconfig_notifier_unregister(struct notifier_block *nb)
{
	return -EINVAL;
}

static inline struct acpi_device *acpi_resource_consumer(struct resource *res)
{
	return NULL;
}

static inline int acpi_get_local_address(acpi_handle handle, u32 *addr)
{
	return -ENODEV;
}

static inline const char *acpi_get_subsystem_id(acpi_handle handle)
{
	return ERR_PTR(-ENODEV);
}

static inline int acpi_register_wakeup_handler(int wake_irq,
	bool (*wakeup)(void *context), void *context)
{
	return -ENXIO;
}

static inline void acpi_unregister_wakeup_handler(
	bool (*wakeup)(void *context), void *context) { }

struct acpi_osc_context;
static inline u32 acpi_osc_ctx_get_pci_control(struct acpi_osc_context *context)
{
	return 0;
}

static inline u32 acpi_osc_ctx_get_cxl_control(struct acpi_osc_context *context)
{
	return 0;
}

static inline bool acpi_sleep_state_supported(u8 sleep_state)
{
	return false;
}

static inline acpi_handle acpi_get_processor_handle(int cpu)
{
	return NULL;
}

static inline int acpi_mrrm_max_mem_region(void)
{
	return 1;
}

#endif	/* !CONFIG_ACPI */

#ifdef CONFIG_ACPI_HMAT
int hmat_get_extended_linear_cache_size(struct resource *backing_res, int nid,
					resource_size_t *size);
#else
static inline int hmat_get_extended_linear_cache_size(struct resource *backing_res,
						      int nid, resource_size_t *size)
{
	return -EOPNOTSUPP;
}
#endif

extern void arch_post_acpi_subsys_init(void);

#ifdef CONFIG_ACPI_HOTPLUG_IOAPIC
int acpi_ioapic_add(acpi_handle root);
#else
static inline int acpi_ioapic_add(acpi_handle root) { return 0; }
#endif

#ifdef CONFIG_ACPI
void acpi_os_set_prepare_sleep(int (*func)(u8 sleep_state,
			       u32 pm1a_ctrl,  u32 pm1b_ctrl));

acpi_status acpi_os_prepare_sleep(u8 sleep_state,
				  u32 pm1a_control, u32 pm1b_control);

void acpi_os_set_prepare_extended_sleep(int (*func)(u8 sleep_state,
				        u32 val_a,  u32 val_b));

acpi_status acpi_os_prepare_extended_sleep(u8 sleep_state,
					   u32 val_a, u32 val_b);
struct acpi_s2idle_dev_ops {
	struct list_head list_node;
	void (*prepare)(void);
	void (*check)(void);
	void (*restore)(void);
};
#if defined(CONFIG_SUSPEND) && defined(CONFIG_X86)
int acpi_register_lps0_dev(struct acpi_s2idle_dev_ops *arg);
void acpi_unregister_lps0_dev(struct acpi_s2idle_dev_ops *arg);
int acpi_get_lps0_constraint(struct acpi_device *adev);
#else /* CONFIG_SUSPEND && CONFIG_X86 */
static inline int acpi_get_lps0_constraint(struct device *dev)
{
	return ACPI_STATE_UNKNOWN;
}
static inline int acpi_register_lps0_dev(struct acpi_s2idle_dev_ops *arg)
{
	return -ENODEV;
}
static inline void acpi_unregister_lps0_dev(struct acpi_s2idle_dev_ops *arg)
{
}
#endif /* CONFIG_SUSPEND && CONFIG_X86 */
void arch_reserve_mem_area(acpi_physical_address addr, size_t size);
#else
#define acpi_os_set_prepare_sleep(func, pm1a_ctrl, pm1b_ctrl) do { } while (0)
#endif

#if defined(CONFIG_ACPI) && defined(CONFIG_PM)
int acpi_dev_suspend(struct device *dev, bool wakeup);
int acpi_dev_resume(struct device *dev);
int acpi_subsys_runtime_suspend(struct device *dev);
int acpi_subsys_runtime_resume(struct device *dev);
int acpi_dev_pm_attach(struct device *dev, bool power_on);
bool acpi_storage_d3(struct device *dev);
bool acpi_dev_state_d0(struct device *dev);
#else
static inline int acpi_subsys_runtime_suspend(struct device *dev) { return 0; }
static inline int acpi_subsys_runtime_resume(struct device *dev) { return 0; }
static inline int acpi_dev_pm_attach(struct device *dev, bool power_on)
{
	return 0;
}
static inline bool acpi_storage_d3(struct device *dev)
{
	return false;
}
static inline bool acpi_dev_state_d0(struct device *dev)
{
	return true;
}
#endif

#if defined(CONFIG_ACPI) && defined(CONFIG_PM_SLEEP)
int acpi_subsys_prepare(struct device *dev);
void acpi_subsys_complete(struct device *dev);
int acpi_subsys_suspend_late(struct device *dev);
int acpi_subsys_suspend_noirq(struct device *dev);
int acpi_subsys_suspend(struct device *dev);
int acpi_subsys_freeze(struct device *dev);
int acpi_subsys_poweroff(struct device *dev);
int acpi_subsys_restore_early(struct device *dev);
#else
static inline int acpi_subsys_prepare(struct device *dev) { return 0; }
static inline void acpi_subsys_complete(struct device *dev) {}
static inline int acpi_subsys_suspend_late(struct device *dev) { return 0; }
static inline int acpi_subsys_suspend_noirq(struct device *dev) { return 0; }
static inline int acpi_subsys_suspend(struct device *dev) { return 0; }
static inline int acpi_subsys_freeze(struct device *dev) { return 0; }
static inline int acpi_subsys_poweroff(struct device *dev) { return 0; }
static inline int acpi_subsys_restore_early(struct device *dev) { return 0; }
#endif

#if defined(CONFIG_ACPI_EC) && defined(CONFIG_PM_SLEEP)
void acpi_ec_mark_gpe_for_wake(void);
void acpi_ec_set_gpe_wake_mask(u8 action);
#else
static inline void acpi_ec_mark_gpe_for_wake(void) {}
static inline void acpi_ec_set_gpe_wake_mask(u8 action) {}
#endif

#ifdef CONFIG_ACPI
char *acpi_handle_path(acpi_handle handle);
__printf(3, 4)
void acpi_handle_printk(const char *level, acpi_handle handle,
			const char *fmt, ...);
void acpi_evaluation_failure_warn(acpi_handle handle, const char *name,
				  acpi_status status);
#else	/* !CONFIG_ACPI */
static inline __printf(3, 4) void
acpi_handle_printk(const char *level, void *handle, const char *fmt, ...) {}
static inline void acpi_evaluation_failure_warn(acpi_handle handle,
						const char *name,
						acpi_status status) {}
#endif	/* !CONFIG_ACPI */

#if defined(CONFIG_ACPI) && defined(CONFIG_DYNAMIC_DEBUG)
__printf(3, 4)
void __acpi_handle_debug(struct _ddebug *descriptor, acpi_handle handle, const char *fmt, ...);
#endif

/*
 * acpi_handle_<level>: Print message with ACPI prefix and object path
 *
 * These interfaces acquire the global namespace mutex to obtain an object
 * path.  In interrupt context, it shows the object path as <n/a>.
 */
#define acpi_handle_emerg(handle, fmt, ...)				\
	acpi_handle_printk(KERN_EMERG, handle, fmt, ##__VA_ARGS__)
#define acpi_handle_alert(handle, fmt, ...)				\
	acpi_handle_printk(KERN_ALERT, handle, fmt, ##__VA_ARGS__)
#define acpi_handle_crit(handle, fmt, ...)				\
	acpi_handle_printk(KERN_CRIT, handle, fmt, ##__VA_ARGS__)
#define acpi_handle_err(handle, fmt, ...)				\
	acpi_handle_printk(KERN_ERR, handle, fmt, ##__VA_ARGS__)
#define acpi_handle_warn(handle, fmt, ...)				\
	acpi_handle_printk(KERN_WARNING, handle, fmt, ##__VA_ARGS__)
#define acpi_handle_notice(handle, fmt, ...)				\
	acpi_handle_printk(KERN_NOTICE, handle, fmt, ##__VA_ARGS__)
#define acpi_handle_info(handle, fmt, ...)				\
	acpi_handle_printk(KERN_INFO, handle, fmt, ##__VA_ARGS__)

#if defined(DEBUG)
#define acpi_handle_debug(handle, fmt, ...)				\
	acpi_handle_printk(KERN_DEBUG, handle, fmt, ##__VA_ARGS__)
#else
#if defined(CONFIG_DYNAMIC_DEBUG)
#define acpi_handle_debug(handle, fmt, ...)				\
	_dynamic_func_call(fmt, __acpi_handle_debug,			\
			   handle, pr_fmt(fmt), ##__VA_ARGS__)
#else
#define acpi_handle_debug(handle, fmt, ...)				\
({									\
	if (0)								\
		acpi_handle_printk(KERN_DEBUG, handle, fmt, ##__VA_ARGS__); \
	0;								\
})
#endif
#endif

#if defined(CONFIG_ACPI) && defined(CONFIG_GPIOLIB)
bool acpi_gpio_get_irq_resource(struct acpi_resource *ares,
				struct acpi_resource_gpio **agpio);
bool acpi_gpio_get_io_resource(struct acpi_resource *ares,
			       struct acpi_resource_gpio **agpio);
int acpi_dev_gpio_irq_wake_get_by(struct acpi_device *adev, const char *con_id, int index,
				  bool *wake_capable);
#else
static inline bool acpi_gpio_get_irq_resource(struct acpi_resource *ares,
					      struct acpi_resource_gpio **agpio)
{
	return false;
}
static inline bool acpi_gpio_get_io_resource(struct acpi_resource *ares,
					     struct acpi_resource_gpio **agpio)
{
	return false;
}
static inline int acpi_dev_gpio_irq_wake_get_by(struct acpi_device *adev, const char *con_id,
						int index, bool *wake_capable)
{
	return -ENXIO;
}
#endif

static inline int acpi_dev_gpio_irq_wake_get(struct acpi_device *adev, int index,
					     bool *wake_capable)
{
	return acpi_dev_gpio_irq_wake_get_by(adev, NULL, index, wake_capable);
}

static inline int acpi_dev_gpio_irq_get_by(struct acpi_device *adev, const char *con_id,
					   int index)
{
	return acpi_dev_gpio_irq_wake_get_by(adev, con_id, index, NULL);
}

static inline int acpi_dev_gpio_irq_get(struct acpi_device *adev, int index)
{
	return acpi_dev_gpio_irq_wake_get_by(adev, NULL, index, NULL);
}

/* Device properties */

#ifdef CONFIG_ACPI
int acpi_dev_get_property(const struct acpi_device *adev, const char *name,
			  acpi_object_type type, const union acpi_object **obj);
int __acpi_node_get_property_reference(const struct fwnode_handle *fwnode,
				const char *name, size_t index, size_t num_args,
				struct fwnode_reference_args *args);

static inline int acpi_node_get_property_reference(
				const struct fwnode_handle *fwnode,
				const char *name, size_t index,
				struct fwnode_reference_args *args)
{
	return __acpi_node_get_property_reference(fwnode, name, index,
		NR_FWNODE_REFERENCE_ARGS, args);
}

static inline bool acpi_dev_has_props(const struct acpi_device *adev)
{
	return !list_empty(&adev->data.properties);
}

struct acpi_device_properties *
acpi_data_add_props(struct acpi_device_data *data, const guid_t *guid,
		    union acpi_object *properties);

int acpi_node_prop_get(const struct fwnode_handle *fwnode, const char *propname,
		       void **valptr);

struct fwnode_handle *acpi_get_next_subnode(const struct fwnode_handle *fwnode,
					    struct fwnode_handle *child);

struct acpi_probe_entry;
typedef bool (*acpi_probe_entry_validate_subtbl)(struct acpi_subtable_header *,
						 struct acpi_probe_entry *);

#define ACPI_TABLE_ID_LEN	5

/**
 * struct acpi_probe_entry - boot-time probing entry
 * @id:			ACPI table name
 * @type:		Optional subtable type to match
 *			(if @id contains subtables)
 * @subtable_valid:	Optional callback to check the validity of
 *			the subtable
 * @probe_table:	Callback to the driver being probed when table
 *			match is successful
 * @probe_subtbl:	Callback to the driver being probed when table and
 *			subtable match (and optional callback is successful)
 * @driver_data:	Sideband data provided back to the driver
 */
struct acpi_probe_entry {
	__u8 id[ACPI_TABLE_ID_LEN];
	__u8 type;
	acpi_probe_entry_validate_subtbl subtable_valid;
	union {
		acpi_tbl_table_handler probe_table;
		acpi_tbl_entry_handler probe_subtbl;
	};
	kernel_ulong_t driver_data;
};

void arch_sort_irqchip_probe(struct acpi_probe_entry *ap_head, int nr);

#define ACPI_DECLARE_PROBE_ENTRY(table, name, table_id, subtable,	\
				 valid, data, fn)			\
	static const struct acpi_probe_entry __acpi_probe_##name	\
		__used __section("__" #table "_acpi_probe_table") = {	\
			.id = table_id,					\
			.type = subtable,				\
			.subtable_valid = valid,			\
			.probe_table = fn,				\
			.driver_data = data,				\
		}

#define ACPI_DECLARE_SUBTABLE_PROBE_ENTRY(table, name, table_id,	\
					  subtable, valid, data, fn)	\
	static const struct acpi_probe_entry __acpi_probe_##name	\
		__used __section("__" #table "_acpi_probe_table") = {	\
			.id = table_id,					\
			.type = subtable,				\
			.subtable_valid = valid,			\
			.probe_subtbl = fn,				\
			.driver_data = data,				\
		}

#define ACPI_PROBE_TABLE(name)		__##name##_acpi_probe_table
#define ACPI_PROBE_TABLE_END(name)	__##name##_acpi_probe_table_end

int __acpi_probe_device_table(struct acpi_probe_entry *start, int nr);

#define acpi_probe_device_table(t)					\
	({ 								\
		extern struct acpi_probe_entry ACPI_PROBE_TABLE(t),	\
			                       ACPI_PROBE_TABLE_END(t);	\
		__acpi_probe_device_table(&ACPI_PROBE_TABLE(t),		\
					  (&ACPI_PROBE_TABLE_END(t) -	\
					   &ACPI_PROBE_TABLE(t)));	\
	})
#else
static inline int acpi_dev_get_property(struct acpi_device *adev,
					const char *name, acpi_object_type type,
					const union acpi_object **obj)
{
	return -ENXIO;
}

static inline int
__acpi_node_get_property_reference(const struct fwnode_handle *fwnode,
				const char *name, size_t index, size_t num_args,
				struct fwnode_reference_args *args)
{
	return -ENXIO;
}

static inline int
acpi_node_get_property_reference(const struct fwnode_handle *fwnode,
				 const char *name, size_t index,
				 struct fwnode_reference_args *args)
{
	return -ENXIO;
}

static inline int acpi_node_prop_get(const struct fwnode_handle *fwnode,
				     const char *propname,
				     void **valptr)
{
	return -ENXIO;
}

static inline struct fwnode_handle *
acpi_get_next_subnode(const struct fwnode_handle *fwnode,
		      struct fwnode_handle *child)
{
	return NULL;
}

static inline struct fwnode_handle *
acpi_graph_get_next_endpoint(const struct fwnode_handle *fwnode,
			     struct fwnode_handle *prev)
{
	return ERR_PTR(-ENXIO);
}

static inline int
acpi_graph_get_remote_endpoint(const struct fwnode_handle *fwnode,
			       struct fwnode_handle **remote,
			       struct fwnode_handle **port,
			       struct fwnode_handle **endpoint)
{
	return -ENXIO;
}

#define ACPI_DECLARE_PROBE_ENTRY(table, name, table_id, subtable, valid, data, fn) \
	static const void * __acpi_table_##name[]			\
		__attribute__((unused))					\
		 = { (void *) table_id,					\
		     (void *) subtable,					\
		     (void *) valid,					\
		     (void *) fn,					\
		     (void *) data }

#define acpi_probe_device_table(t)	({ int __r = 0; __r;})
#endif

#ifdef CONFIG_ACPI_TABLE_UPGRADE
void acpi_table_upgrade(void);
#else
static inline void acpi_table_upgrade(void) { }
#endif

#if defined(CONFIG_ACPI) && defined(CONFIG_ACPI_WATCHDOG)
extern bool acpi_has_watchdog(void);
#else
static inline bool acpi_has_watchdog(void) { return false; }
#endif

#ifdef CONFIG_ACPI_SPCR_TABLE
extern bool qdf2400_e44_present;
int acpi_parse_spcr(bool enable_earlycon, bool enable_console);
#else
static inline int acpi_parse_spcr(bool enable_earlycon, bool enable_console)
{
	return 0;
}
#endif

#if IS_ENABLED(CONFIG_ACPI_GENERIC_GSI)
int acpi_irq_get(acpi_handle handle, unsigned int index, struct resource *res);
#else
static inline
int acpi_irq_get(acpi_handle handle, unsigned int index, struct resource *res)
{
	return -EINVAL;
}
#endif

#ifdef CONFIG_ACPI_LPIT
int lpit_read_residency_count_address(u64 *address);
#else
static inline int lpit_read_residency_count_address(u64 *address)
{
	return -EINVAL;
}
#endif

#ifdef CONFIG_ACPI_PROCESSOR_IDLE
#ifndef arch_get_idle_state_flags
static inline unsigned int arch_get_idle_state_flags(u32 arch_flags)
{
	return 0;
}
#endif
#endif /* CONFIG_ACPI_PROCESSOR_IDLE */

#ifdef CONFIG_ACPI_PPTT
int acpi_pptt_cpu_is_thread(unsigned int cpu);
int find_acpi_cpu_topology(unsigned int cpu, int level);
int find_acpi_cpu_topology_cluster(unsigned int cpu);
int find_acpi_cpu_topology_package(unsigned int cpu);
int find_acpi_cpu_topology_hetero_id(unsigned int cpu);
#else
static inline int acpi_pptt_cpu_is_thread(unsigned int cpu)
{
	return -EINVAL;
}
static inline int find_acpi_cpu_topology(unsigned int cpu, int level)
{
	return -EINVAL;
}
static inline int find_acpi_cpu_topology_cluster(unsigned int cpu)
{
	return -EINVAL;
}
static inline int find_acpi_cpu_topology_package(unsigned int cpu)
{
	return -EINVAL;
}
static inline int find_acpi_cpu_topology_hetero_id(unsigned int cpu)
{
	return -EINVAL;
}
#endif

void acpi_arch_init(void);

#ifdef CONFIG_ACPI_PCC
void acpi_init_pcc(void);
#else
static inline void acpi_init_pcc(void) { }
#endif

#ifdef CONFIG_ACPI_FFH
void acpi_init_ffh(void);
extern int acpi_ffh_address_space_arch_setup(void *handler_ctxt,
					     void **region_ctxt);
extern int acpi_ffh_address_space_arch_handler(acpi_integer *value,
					       void *region_context);
#else
static inline void acpi_init_ffh(void) { }
#endif

#ifdef CONFIG_ACPI
extern void acpi_device_notify(struct device *dev);
extern void acpi_device_notify_remove(struct device *dev);
#else
static inline void acpi_device_notify(struct device *dev) { }
static inline void acpi_device_notify_remove(struct device *dev) { }
#endif

static inline void acpi_use_parent_companion(struct device *dev)
{
	ACPI_COMPANION_SET(dev, ACPI_COMPANION(dev->parent));
}

#ifdef CONFIG_ACPI_HMAT
int hmat_update_target_coordinates(int nid, struct access_coordinate *coord,
				   enum access_coordinate_class access);
#else
static inline int hmat_update_target_coordinates(int nid,
						 struct access_coordinate *coord,
						 enum access_coordinate_class access)
{
	return -EOPNOTSUPP;
}
#endif

#ifdef CONFIG_ACPI_NUMA
bool acpi_node_backed_by_real_pxm(int nid);
#else
static inline bool acpi_node_backed_by_real_pxm(int nid)
{
	return false;
}
#endif

#endif	/*_LINUX_ACPI_H*/
