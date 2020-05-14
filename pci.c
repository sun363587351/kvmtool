#include "kvm/devices.h"
#include "kvm/pci.h"
#include "kvm/ioport.h"
#include "kvm/irq.h"
#include "kvm/util.h"
#include "kvm/kvm.h"

#include <linux/err.h>
#include <assert.h>

static u32 pci_config_address_bits;

/* This is within our PCI gap - in an unused area.
 * Note this is a PCI *bus address*, is used to assign BARs etc.!
 * (That's why it can still 32bit even with 64bit guests-- 64bit
 * PCI isn't currently supported.)
 */
static u32 mmio_blocks			= KVM_PCI_MMIO_AREA;
static u16 io_port_blocks		= PCI_IOPORT_START;

u16 pci_get_io_port_block(u32 size)
{
	u16 port = ALIGN(io_port_blocks, PCI_IO_SIZE);

	io_port_blocks = port + size;
	return port;
}

/*
 * BARs must be naturally aligned, so enforce this in the allocator.
 */
u32 pci_get_mmio_block(u32 size)
{
	u32 block = ALIGN(mmio_blocks, size);
	mmio_blocks = block + size;
	return block;
}

void *pci_find_cap(struct pci_device_header *hdr, u8 cap_type)
{
	u8 pos;
	struct pci_cap_hdr *cap;

	pci_for_each_cap(pos, cap, hdr) {
		if (cap->type == cap_type)
			return cap;
	}

	return NULL;
}

int pci__assign_irq(struct pci_device_header *pci_hdr)
{
	/*
	 * PCI supports only INTA#,B#,C#,D# per device.
	 *
	 * A#,B#,C#,D# are allowed for multifunctional devices so stick
	 * with A# for our single function devices.
	 */
	pci_hdr->irq_pin	= 1;
	pci_hdr->irq_line	= irq__alloc_line();

	if (!pci_hdr->irq_type)
		pci_hdr->irq_type = IRQ_TYPE_EDGE_RISING;

	return pci_hdr->irq_line;
}

static bool pci_bar_is_implemented(struct pci_device_header *pci_hdr, int bar_num)
{
	return pci__bar_size(pci_hdr, bar_num);
}

static void *pci_config_address_ptr(u16 port)
{
	unsigned long offset;
	void *base;

	offset	= port - PCI_CONFIG_ADDRESS;
	base	= &pci_config_address_bits;

	return base + offset;
}

static bool pci_config_address_out(struct ioport *ioport, struct kvm_cpu *vcpu, u16 port, void *data, int size)
{
	void *p = pci_config_address_ptr(port);

	memcpy(p, data, size);

	return true;
}

static bool pci_config_address_in(struct ioport *ioport, struct kvm_cpu *vcpu, u16 port, void *data, int size)
{
	void *p = pci_config_address_ptr(port);

	memcpy(data, p, size);

	return true;
}

static struct ioport_operations pci_config_address_ops = {
	.io_in	= pci_config_address_in,
	.io_out	= pci_config_address_out,
};

static bool pci_device_exists(u8 bus_number, u8 device_number, u8 function_number)
{
	union pci_config_address pci_config_address;

	pci_config_address.w = ioport__read32(&pci_config_address_bits);

	if (pci_config_address.bus_number != bus_number)
		return false;

	if (pci_config_address.function_number != function_number)
		return false;

	return !IS_ERR_OR_NULL(device__find_dev(DEVICE_BUS_PCI, device_number));
}

static bool pci_config_data_out(struct ioport *ioport, struct kvm_cpu *vcpu, u16 port, void *data, int size)
{
	union pci_config_address pci_config_address;

	if (size > 4)
		size = 4;

	pci_config_address.w = ioport__read32(&pci_config_address_bits);
	/*
	 * If someone accesses PCI configuration space offsets that are not
	 * aligned to 4 bytes, it uses ioports to signify that.
	 */
	pci_config_address.reg_offset = port - PCI_CONFIG_DATA;

	pci__config_wr(vcpu->kvm, pci_config_address, data, size);

	return true;
}

static bool pci_config_data_in(struct ioport *ioport, struct kvm_cpu *vcpu, u16 port, void *data, int size)
{
	union pci_config_address pci_config_address;

	if (size > 4)
		size = 4;

	pci_config_address.w = ioport__read32(&pci_config_address_bits);
	/*
	 * If someone accesses PCI configuration space offsets that are not
	 * aligned to 4 bytes, it uses ioports to signify that.
	 */
	pci_config_address.reg_offset = port - PCI_CONFIG_DATA;

	pci__config_rd(vcpu->kvm, pci_config_address, data, size);

	return true;
}

static struct ioport_operations pci_config_data_ops = {
	.io_in	= pci_config_data_in,
	.io_out	= pci_config_data_out,
};

static void pci_config_command_wr(struct kvm *kvm,
				  struct pci_device_header *pci_hdr,
				  u16 new_command)
{
	int i;
	bool toggle_io, toggle_mem;

	toggle_io = (pci_hdr->command ^ new_command) & PCI_COMMAND_IO;
	toggle_mem = (pci_hdr->command ^ new_command) & PCI_COMMAND_MEMORY;

	for (i = 0; i < 6; i++) {
		if (!pci_bar_is_implemented(pci_hdr, i))
			continue;

		if (toggle_io && pci__bar_is_io(pci_hdr, i)) {
			if (__pci__io_space_enabled(new_command))
				pci_hdr->bar_activate_fn(kvm, pci_hdr, i,
							 pci_hdr->data);
			else
				pci_hdr->bar_deactivate_fn(kvm, pci_hdr, i,
							   pci_hdr->data);
		}

		if (toggle_mem && pci__bar_is_memory(pci_hdr, i)) {
			if (__pci__memory_space_enabled(new_command))
				pci_hdr->bar_activate_fn(kvm, pci_hdr, i,
							 pci_hdr->data);
			else
				pci_hdr->bar_deactivate_fn(kvm, pci_hdr, i,
							   pci_hdr->data);
		}
	}

	pci_hdr->command = new_command;
}

void pci__config_wr(struct kvm *kvm, union pci_config_address addr, void *data, int size)
{
	void *base;
	u8 bar, offset;
	struct pci_device_header *pci_hdr;
	u8 dev_num = addr.device_number;
	u32 value = 0;
	u32 mask;

	if (!pci_device_exists(addr.bus_number, dev_num, 0))
		return;

	offset = addr.w & PCI_DEV_CFG_MASK;
	base = pci_hdr = device__find_dev(DEVICE_BUS_PCI, dev_num)->data;

	if (pci_hdr->cfg_ops.write)
		pci_hdr->cfg_ops.write(kvm, pci_hdr, offset, data, size);

	/*
	 * legacy hack: ignore writes to uninitialized regions (e.g. ROM BAR).
	 * Not very nice but has been working so far.
	 */
	if (*(u32 *)(base + offset) == 0)
		return;

	if (offset == PCI_COMMAND) {
		memcpy(&value, data, size);
		pci_config_command_wr(kvm, pci_hdr, (u16)value);
		return;
	}

	bar = (offset - PCI_BAR_OFFSET(0)) / sizeof(u32);

	/*
	 * If the kernel masks the BAR, it will expect to find the size of the
	 * BAR there next time it reads from it. After the kernel reads the
	 * size, it will write the address back.
	 */
	if (bar < 6) {
		if (pci__bar_is_io(pci_hdr, bar))
			mask = (u32)PCI_BASE_ADDRESS_IO_MASK;
		else
			mask = (u32)PCI_BASE_ADDRESS_MEM_MASK;
		/*
		 * According to the PCI local bus specification REV 3.0:
		 * The number of upper bits that a device actually implements
		 * depends on how much of the address space the device will
		 * respond to. A device that wants a 1 MB memory address space
		 * (using a 32-bit base address register) would build the top
		 * 12 bits of the address register, hardwiring the other bits
		 * to 0.
		 *
		 * Furthermore, software can determine how much address space
		 * the device requires by writing a value of all 1's to the
		 * register and then reading the value back. The device will
		 * return 0's in all don't-care address bits, effectively
		 * specifying the address space required.
		 *
		 * Software computes the size of the address space with the
		 * formula S = ~B + 1, where S is the memory size and B is the
		 * value read from the BAR. This means that the BAR value that
		 * kvmtool should return is B = ~(S - 1).
		 */
		memcpy(&value, data, size);
		if (value == 0xffffffff)
			value = ~(pci__bar_size(pci_hdr, bar) - 1);
		/* Preserve the special bits. */
		value = (value & mask) | (pci_hdr->bar[bar] & ~mask);
		memcpy(base + offset, &value, size);
	} else {
		memcpy(base + offset, data, size);
	}
}

void pci__config_rd(struct kvm *kvm, union pci_config_address addr, void *data, int size)
{
	u8 offset;
	struct pci_device_header *pci_hdr;
	u8 dev_num = addr.device_number;

	if (pci_device_exists(addr.bus_number, dev_num, 0)) {
		pci_hdr = device__find_dev(DEVICE_BUS_PCI, dev_num)->data;
		offset = addr.w & PCI_DEV_CFG_MASK;

		if (pci_hdr->cfg_ops.read)
			pci_hdr->cfg_ops.read(kvm, pci_hdr, offset, data, size);

		memcpy(data, (void *)pci_hdr + offset, size);
	} else {
		memset(data, 0xff, size);
	}
}

static void pci_config_mmio_access(struct kvm_cpu *vcpu, u64 addr, u8 *data,
				   u32 len, u8 is_write, void *kvm)
{
	union pci_config_address cfg_addr;

	addr			-= KVM_PCI_CFG_AREA;
	cfg_addr.w		= (u32)addr;
	cfg_addr.enable_bit	= 1;

	if (len > 4)
		len = 4;

	if (is_write)
		pci__config_wr(kvm, cfg_addr, data, len);
	else
		pci__config_rd(kvm, cfg_addr, data, len);
}

struct pci_device_header *pci__find_dev(u8 dev_num)
{
	struct device_header *hdr = device__find_dev(DEVICE_BUS_PCI, dev_num);

	if (IS_ERR_OR_NULL(hdr))
		return NULL;

	return hdr->data;
}

int pci__register_bar_regions(struct kvm *kvm, struct pci_device_header *pci_hdr,
			      bar_activate_fn_t bar_activate_fn,
			      bar_deactivate_fn_t bar_deactivate_fn, void *data)
{
	int i, r;

	assert(bar_activate_fn && bar_deactivate_fn);

	pci_hdr->bar_activate_fn = bar_activate_fn;
	pci_hdr->bar_deactivate_fn = bar_deactivate_fn;
	pci_hdr->data = data;

	for (i = 0; i < 6; i++) {
		if (!pci_bar_is_implemented(pci_hdr, i))
			continue;

		if (pci__bar_is_io(pci_hdr, i) &&
		    pci__io_space_enabled(pci_hdr)) {
			r = bar_activate_fn(kvm, pci_hdr, i, data);
			if (r < 0)
				return r;
		}

		if (pci__bar_is_memory(pci_hdr, i) &&
		    pci__memory_space_enabled(pci_hdr)) {
			r = bar_activate_fn(kvm, pci_hdr, i, data);
			if (r < 0)
				return r;
		}
	}

	return 0;
}

int pci__init(struct kvm *kvm)
{
	int r;

	r = ioport__register(kvm, PCI_CONFIG_DATA + 0, &pci_config_data_ops, 4, NULL);
	if (r < 0)
		return r;

	r = ioport__register(kvm, PCI_CONFIG_ADDRESS + 0, &pci_config_address_ops, 4, NULL);
	if (r < 0)
		goto err_unregister_data;

	r = kvm__register_mmio(kvm, KVM_PCI_CFG_AREA, PCI_CFG_SIZE, false,
			       pci_config_mmio_access, kvm);
	if (r < 0)
		goto err_unregister_addr;

	return 0;

err_unregister_addr:
	ioport__unregister(kvm, PCI_CONFIG_ADDRESS);
err_unregister_data:
	ioport__unregister(kvm, PCI_CONFIG_DATA);
	return r;
}
dev_base_init(pci__init);

int pci__exit(struct kvm *kvm)
{
	ioport__unregister(kvm, PCI_CONFIG_DATA);
	ioport__unregister(kvm, PCI_CONFIG_ADDRESS);

	return 0;
}
dev_base_exit(pci__exit);
